import os
import struct
import subprocess
import sys


V3_ENVELOPE_MAGIC = 0x56435354
V3_VERSION = 3
V3_IDENTITY_MAGIC = 0x33435354
V3_WAL_COMMIT_MAGIC = 0x33435754
FNV64_OFFSET = 1469598103934665603
FNV64_PRIME = 1099511628211
MASK64 = (1 << 64) - 1
PAGE_SIZE = 4096
NODE_TYPE_OFFSET = 0
IS_ROOT_OFFSET = 1
NODE_INTERNAL = 0
NODE_LEAF = 1


def find_executable(base_dir):
    candidates = [
        os.path.join(base_dir, 'build', 'Debug', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb'),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def remove_database(db_file):
    suffixes = ('', '.wal', '.free', '.schema', '.schema.wal', '.idxepoch')
    for suffix in suffixes:
        path = db_file + suffix
        if os.path.exists(path):
            os.remove(path)


def run_commands(executable, db_file, commands):
    process = subprocess.Popen(
        [executable, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, stderr = process.communicate(input=commands)
    return process.returncode, stdout, stderr


def fnv64(data):
    value = FNV64_OFFSET
    for byte in data:
        value ^= byte
        value = (value * FNV64_PRIME) & MASK64
    return value


def forge_committed_root_wal(schema_bytes, root_page_num):
    if len(schema_bytes) < 28:
        raise AssertionError('schema catalog is shorter than the V3 envelope')
    magic, version, total_size, shape_size, identity_size = struct.unpack_from('<IIIII', schema_bytes, 0)
    if magic != V3_ENVELOPE_MAGIC or version != V3_VERSION:
        raise AssertionError('expected a V3 schema catalog')
    if total_size != len(schema_bytes) or 20 + shape_size + identity_size + 8 != len(schema_bytes):
        raise AssertionError('unexpected V3 schema envelope length')

    forged = bytearray(schema_bytes)
    shape_offset = 20
    identity_offset = shape_offset + shape_size
    checksum_offset = identity_offset + identity_size

    # Shape begins num_tables,num_views,then first table name. Root is offset 40.
    if shape_size < 44:
        raise AssertionError('schema shape too short for first root')
    struct.pack_into('<I', forged, shape_offset + 40, root_page_num)

    if identity_size < 40:
        raise AssertionError('V3 identity block too short')
    identity_magic, identity_version, declared_identity, num_tables = struct.unpack_from(
        '<IIII', forged, identity_offset
    )
    if identity_magic != V3_IDENTITY_MAGIC or identity_version != V3_VERSION or num_tables < 1:
        raise AssertionError('unexpected V3 identity block')
    if declared_identity != identity_size:
        raise AssertionError('V3 identity length mismatch')

    # First identity entry: table_id:u32, root:u32, generation:u64.
    struct.pack_into('<I', forged, identity_offset + 16 + 4, root_page_num)
    identity_checksum_offset = identity_offset + identity_size - 8
    struct.pack_into('<Q', forged, identity_checksum_offset,
                     fnv64(forged[identity_offset:identity_checksum_offset]))
    struct.pack_into('<Q', forged, checksum_offset, fnv64(forged[:checksum_offset]))
    return bytes(forged) + struct.pack('<I', V3_WAL_COMMIT_MAGIC)


def find_non_root_tree_page(db_file):
    with open(db_file, 'rb') as handle:
        data = handle.read()
    if len(data) % PAGE_SIZE != 0:
        raise AssertionError('database file is not page aligned')

    for page_num in range(len(data) // PAGE_SIZE):
        page = data[page_num * PAGE_SIZE:(page_num + 1) * PAGE_SIZE]
        if page[NODE_TYPE_OFFSET] in (NODE_INTERNAL, NODE_LEAF) and page[IS_ROOT_OFFSET] == 0:
            return page_num
    raise AssertionError('expected a non-root B+ tree page after forcing a split')


def reject_forged_wal(executable, db_file, schema_file, schema_wal,
                       healthy_main, bad_root, description):
    forged_wal = forge_committed_root_wal(healthy_main, bad_root)
    with open(schema_wal, 'wb') as handle:
        handle.write(forged_wal)

    rc, stdout, stderr = run_commands(executable, db_file, '.exit\n')
    if rc != 0:
        print(f'FAIL: {description} prevented reopen.')
        print(stdout)
        print(stderr)
        sys.exit(1)
    if 'preserving main catalog' not in stdout:
        print(f'FAIL: expected {description} rejection diagnostic.')
        print(stdout)
        sys.exit(1)
    if os.path.exists(schema_wal):
        print(f'FAIL: rejected {description} WAL was not removed.')
        sys.exit(1)

    with open(schema_file, 'rb') as handle:
        recovered_main = handle.read()
    if recovered_main != healthy_main:
        print(f'FAIL: {description} WAL overwrote the healthy main schema catalog.')
        sys.exit(1)


def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    executable = find_executable(base_dir)
    if not executable:
        print('Could not find the tinydb executable.')
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), 'test_schema_catalog_wal_root_guard.db')
    schema_file = db_file + '.schema'
    schema_wal = db_file + '.schema.wal'
    remove_database(db_file)

    commands = ['CREATE TABLE products (id INT, price INT);']
    commands.extend(
        f'INSERT INTO products VALUES ({row_id}, {row_id * 10});'
        for row_id in range(1, 41)
    )
    commands.append('.exit')
    rc, stdout, stderr = run_commands(executable, db_file, '\n'.join(commands) + '\n')
    if rc != 0 or 'Error:' in stdout or 'Syntax error' in stdout:
        print('FAIL: failed to create a split database with a persisted schema catalog.')
        print(stdout)
        print(stderr)
        sys.exit(1)
    if not os.path.exists(schema_file):
        print('FAIL: CREATE TABLE did not persist a schema catalog.')
        sys.exit(1)

    with open(schema_file, 'rb') as handle:
        healthy_main = handle.read()

    reject_forged_wal(executable, db_file, schema_file, schema_wal,
                      healthy_main, 0xFFFFFFFF, 'impossible-root schema')

    child_page = find_non_root_tree_page(db_file)
    reject_forged_wal(executable, db_file, schema_file, schema_wal,
                      healthy_main, child_page, 'non-root-page schema')

    rc, stdout, stderr = run_commands(
        executable, db_file,
        'SELECT COUNT(*) FROM products;\nPRAGMA integrity_check;\n.exit\n',
    )
    if rc != 0:
        print('FAIL: database did not reopen after invalid WAL removal.')
        print(stdout)
        print(stderr)
        sys.exit(1)
    if '40' not in stdout or 'ok' not in stdout:
        print('FAIL: healthy catalog/data were not preserved after WAL rejection.')
        print(stdout)
        sys.exit(1)

    print(
        'PASS: checksum-valid V3 schema WAL roots outside the file or targeting existing '
        'non-root B+ tree pages are discarded before they can replace the healthy catalog.'
    )
    remove_database(db_file)


if __name__ == '__main__':
    run_test()

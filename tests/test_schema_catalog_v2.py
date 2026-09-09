import glob
import os
import struct
import subprocess
import sys


SCHEMA_MAGIC = 0x4D435354
SCHEMA_V1 = 1
V3_ENVELOPE_MAGIC = 0x56435354
V3_VERSION = 3
V3_WAL_COMMIT_MAGIC = 0x33435754
FNV64_OFFSET = 1469598103934665603
FNV64_PRIME = 1099511628211
MASK64 = (1 << 64) - 1


def find_binary(base_dir, name):
    candidates = [
        os.path.join(base_dir, "build", "Debug", name + ".exe"),
        os.path.join(base_dir, "build", "Release", name + ".exe"),
        os.path.join(base_dir, "build", name + ".exe"),
        os.path.join(base_dir, "build", name),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def cleanup(db_file):
    for path in glob.glob(db_file + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_file, commands, check=True):
    result = subprocess.run(
        [executable, db_file],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        timeout=120,
    )
    output = result.stdout + result.stderr
    if check and result.returncode != 0:
        raise AssertionError(output)
    return result.returncode, output


def fnv64(data):
    value = FNV64_OFFSET
    for byte in data:
        value ^= byte
        value = (value * FNV64_PRIME) & MASK64
    return value


def parse_v3(data):
    if len(data) < 28:
        raise AssertionError("schema catalog too short for V3 envelope")
    magic, version, total_size, shape_size, identity_size = struct.unpack_from("<IIIII", data, 0)
    if magic != V3_ENVELOPE_MAGIC or version != V3_VERSION:
        raise AssertionError(f"unexpected schema header magic/version: {magic:#x}/{version}")
    if total_size != len(data) or 20 + shape_size + identity_size + 8 != len(data):
        raise AssertionError("V3 schema envelope length does not match header")
    checksum_offset = len(data) - 8
    checksum = struct.unpack_from("<Q", data, checksum_offset)[0]
    if fnv64(data[:checksum_offset]) != checksum:
        raise AssertionError("V3 schema envelope checksum does not match")
    return data[20:20 + shape_size], data[20 + shape_size:checksum_offset]


def require(output, marker):
    if marker not in output:
        raise AssertionError(f"missing marker {marker!r}\n{output}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    tinydb = find_binary(repo_root, "tinydb")
    v1_probe = find_binary(repo_root, "tinydb_schema_catalog_v1_probe")
    if tinydb is None or v1_probe is None:
        print("FAIL: Could not find schema catalog test binaries.")
        sys.exit(1)

    db_file = os.path.join(os.path.dirname(__file__), "test_schema_catalog_v2.db")
    schema_file = db_file + ".schema"
    wal_file = db_file + ".schema.wal"
    cleanup(db_file)

    try:
        _, setup = run_session(
            tinydb,
            db_file,
            [
                "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);",
                "INSERT INTO archive VALUES (7, 'archived', 'archive@example.com');",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        require(setup, "ok")
        if not os.path.exists(schema_file):
            raise AssertionError("schema catalog was not created")

        with open(schema_file, "rb") as handle:
            initial_v3 = handle.read()
        parse_v3(initial_v3)
        if os.path.exists(wal_file):
            raise AssertionError("committed schema WAL was not removed")

        _, reopened = run_session(
            tinydb, db_file,
            ["SELECT COUNT(*) FROM archive;", ".schema archive", ".exit"],
        )
        require(reopened, "db > 1\nExecuted.")
        require(reopened, "Table: archive")

        # Prove legacy V1 remains readable and that the next DDL save upgrades it
        # directly to the production V3 envelope without changing table data.
        downgrade = subprocess.run([v1_probe, db_file], capture_output=True, text=True, timeout=120)
        downgrade_output = downgrade.stdout + downgrade.stderr
        if downgrade.returncode != 0:
            raise AssertionError(downgrade_output)
        require(downgrade_output, "SCHEMA_CATALOG_V1_OK")
        with open(schema_file, "rb") as handle:
            legacy_prefix = handle.read(8)
        legacy_magic, legacy_version = struct.unpack("<II", legacy_prefix)
        if legacy_magic != SCHEMA_MAGIC or legacy_version != SCHEMA_V1:
            raise AssertionError("V1 probe did not write a legacy catalog")

        _, legacy_read = run_session(
            tinydb, db_file,
            ["SELECT COUNT(*) FROM archive;", "CREATE VIEW archive_view AS SELECT * FROM archive;", ".exit"],
        )
        require(legacy_read, "db > 1\nExecuted.")
        with open(schema_file, "rb") as handle:
            upgraded_bytes = handle.read()
        parse_v3(upgraded_bytes)

        # A fully committed V3 WAL must recover the main catalog before open.
        committed_wal = upgraded_bytes + struct.pack("<I", V3_WAL_COMMIT_MAGIC)
        with open(wal_file, "wb") as handle:
            handle.write(committed_wal)
        with open(schema_file, "wb") as handle:
            handle.write(upgraded_bytes[:7])

        _, recovered = run_session(tinydb, db_file, ["SELECT COUNT(*) FROM archive;", ".exit"])
        require(recovered, "Schema catalog V3 recovery complete.")
        require(recovered, "db > 1\nExecuted.")
        if os.path.exists(wal_file):
            raise AssertionError("recovered V3 schema WAL was not removed")
        with open(schema_file, "rb") as handle:
            recovered_bytes = handle.read()
        if recovered_bytes != upgraded_bytes:
            raise AssertionError("V3 WAL recovery did not restore the committed catalog")
        parse_v3(recovered_bytes)

        # Corrupt committed WAL bytes: it must never replace the healthy main.
        corrupted_wal = bytearray(committed_wal)
        corrupted_wal[28] ^= 0x5A
        with open(wal_file, "wb") as handle:
            handle.write(corrupted_wal)
        with open(schema_file, "wb") as handle:
            handle.write(upgraded_bytes)
        _, ignored_wal = run_session(tinydb, db_file, ["SELECT COUNT(*) FROM archive;", ".exit"])
        require(ignored_wal, "db > 1\nExecuted.")
        if os.path.exists(wal_file):
            raise AssertionError("invalid V3 schema WAL was not removed")
        with open(schema_file, "rb") as handle:
            after_bad_wal = handle.read()
        if after_bad_wal != upgraded_bytes:
            raise AssertionError("corrupt V3 WAL overwrote healthy main catalog")

        # Corrupt the authoritative main envelope itself: open must fail closed.
        corrupted_main = bytearray(upgraded_bytes)
        corrupted_main[28] ^= 0x33
        with open(schema_file, "wb") as handle:
            handle.write(corrupted_main)
        corrupt_code, corrupt_output = run_session(
            tinydb, db_file, ["SELECT COUNT(*) FROM archive;"], check=False
        )
        if corrupt_code == 0:
            raise AssertionError(
                "corrupted main V3 schema catalog did not make database open fail closed\n" + corrupt_output
            )
        require(corrupt_output, "Ignoring invalid V3 schema catalog.")
        require(corrupt_output, "Error: schema catalog could not be loaded safely.")
        require(corrupt_output, "Unable to open database.")
        if "db >" in corrupt_output:
            raise AssertionError("REPL became available after corrupt V3 schema catalog detection")

        with open(schema_file, "wb") as handle:
            handle.write(upgraded_bytes)
        _, final = run_session(
            tinydb, db_file,
            ["SELECT COUNT(*) FROM archive;", "PRAGMA integrity_check;", ".exit"],
        )
        require(final, "db > 1\nExecuted.")
        require(final, "ok")

        print(
            "PASS: schema catalog reads legacy V1, upgrades production writes to the checksummed "
            "V3 shape+identity envelope, recovers committed V3 WAL, ignores corrupt WAL without "
            "overwriting main state, and fails closed on corrupt authoritative V3 metadata."
        )
    finally:
        cleanup(db_file)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)

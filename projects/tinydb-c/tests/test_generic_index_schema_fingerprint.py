import glob
import os
import struct
import subprocess
import sys


FNV_OFFSET = 1469598103934665603
FNV_PRIME = 1099511628211
SNAPSHOT_MAGIC = 0x47495231
SNAPSHOT_V2 = 2
SCHEMA_FINGERPRINT_OFFSET = 28
SCHEMA_FINGERPRINT_SIZE = 8


def find_tinydb(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    raise AssertionError("Could not find tinydb executable")


def cleanup(db_path):
    for path in glob.glob(db_path + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def run_session(executable, db_path, commands):
    result = subprocess.run(
        [executable, db_path],
        input="\n".join(commands) + "\n",
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise AssertionError(output)
    return output


def projected_ids(output):
    values = []
    for raw_line in output.splitlines():
        line = raw_line.strip()
        if line.startswith("db > "):
            line = line[5:].strip()
        if line.isdigit():
            values.append(int(line))
    return values


def fnv64(payload):
    value = FNV_OFFSET
    for byte in payload:
        value ^= byte
        value = (value * FNV_PRIME) & 0xFFFFFFFFFFFFFFFF
    return value


def read_snapshot(path):
    with open(path, "rb") as handle:
        payload = handle.read()
    if len(payload) < 44:
        raise AssertionError(f"snapshot is unexpectedly short: {len(payload)}")
    magic, version = struct.unpack_from("<II", payload, 0)
    if magic != SNAPSHOT_MAGIC:
        raise AssertionError(f"unexpected snapshot magic {magic:#x}")
    stored_checksum = struct.unpack_from("<Q", payload, len(payload) - 8)[0]
    actual_checksum = fnv64(payload[:-8])
    if stored_checksum != actual_checksum:
        raise AssertionError(
            f"snapshot checksum mismatch: stored={stored_checksum:#x} actual={actual_checksum:#x}"
        )
    return bytearray(payload), version


def rewrite_checksum(payload):
    checksum = fnv64(payload[:-8])
    struct.pack_into("<Q", payload, len(payload) - 8, checksum)


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_tinydb(repo_root)
    db_path = os.path.join(
        os.path.dirname(__file__), "test_generic_index_schema_fingerprint.db"
    )
    cleanup(db_path)

    try:
        first = run_session(
            executable,
            db_path,
            [
                "CREATE TABLE wide_docs (id INT, left_text VARCHAR(145), right_text VARCHAR(145));",
                "INSERT INTO wide_docs VALUES (10, 'left-a', 'right-a');",
                "INSERT INTO wide_docs VALUES (20, 'left-b', 'right-b');",
                "INSERT INTO wide_docs VALUES (30, 'left-c', 'right-c');",
                "CREATE INDEX idx_wide_right ON wide_docs (right_text);",
                "SELECT id FROM wide_docs WHERE right_text = 'right-b';",
                ".exit",
            ],
        )
        if 20 not in projected_ids(first):
            raise AssertionError(first)

        snapshots = glob.glob(db_path + "*.range")
        if len(snapshots) != 1:
            raise AssertionError(f"expected one persistent range snapshot, got {snapshots!r}")
        snapshot_path = snapshots[0]

        payload, version = read_snapshot(snapshot_path)
        if version != SNAPSHOT_V2:
            raise AssertionError(f"new snapshots must use V2, found version {version}")
        original_fingerprint = struct.unpack_from(
            "<Q", payload, SCHEMA_FINGERPRINT_OFFSET
        )[0]
        if original_fingerprint == 0:
            raise AssertionError("schema layout fingerprint must be non-zero")

        # Preserve a fully valid checksum and every other identity field, but
        # replace only the schema identity. The loader must treat this as a
        # semantic cache miss and rebuild from the authoritative table rows.
        tampered_fingerprint = original_fingerprint ^ 0x9E3779B97F4A7C15
        struct.pack_into(
            "<Q", payload, SCHEMA_FINGERPRINT_OFFSET, tampered_fingerprint
        )
        rewrite_checksum(payload)
        with open(snapshot_path, "wb") as handle:
            handle.write(payload)

        second = run_session(
            executable,
            db_path,
            [
                "SELECT id FROM wide_docs WHERE right_text = 'right-b';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        if 20 not in projected_ids(second) or "ok" not in second:
            raise AssertionError(second)

        rebuilt, rebuilt_version = read_snapshot(snapshot_path)
        rebuilt_fingerprint = struct.unpack_from(
            "<Q", rebuilt, SCHEMA_FINGERPRINT_OFFSET
        )[0]
        if rebuilt_version != SNAPSHOT_V2:
            raise AssertionError("semantic-staleness rebuild must publish a V2 snapshot")
        if rebuilt_fingerprint != original_fingerprint:
            raise AssertionError(
                "checksum-valid snapshot with a wrong schema fingerprint was reused instead of rebuilt"
            )

        # Materialize the exact old V1 wire shape by removing the V2 schema
        # fingerprint field. V1 remains a safe cache miss: it must never be
        # interpreted using V2 offsets, and the next indexed query should
        # transparently rebuild it as V2.
        legacy = bytearray(rebuilt[:SCHEMA_FINGERPRINT_OFFSET])
        struct.pack_into("<I", legacy, 4, 1)
        legacy.extend(rebuilt[SCHEMA_FINGERPRINT_OFFSET + SCHEMA_FINGERPRINT_SIZE : -8])
        legacy.extend(b"\x00" * 8)
        rewrite_checksum(legacy)
        with open(snapshot_path, "wb") as handle:
            handle.write(legacy)

        legacy_payload, legacy_version = read_snapshot(snapshot_path)
        if legacy_version != 1:
            raise AssertionError("failed to construct a legacy V1 snapshot fixture")
        if len(legacy_payload) + SCHEMA_FINGERPRINT_SIZE != len(rebuilt):
            raise AssertionError("legacy V1 fixture has an unexpected wire length")

        third = run_session(
            executable,
            db_path,
            [
                "SELECT id FROM wide_docs WHERE right_text >= 'right-b';",
                "PRAGMA integrity_check;",
                ".exit",
            ],
        )
        ids = projected_ids(third)
        if 20 not in ids or 30 not in ids or "ok" not in third:
            raise AssertionError(third)

        final_payload, final_version = read_snapshot(snapshot_path)
        if final_version != SNAPSHOT_V2:
            raise AssertionError("legacy V1 snapshot must be rebuilt into V2 on demand")
        final_fingerprint = struct.unpack_from(
            "<Q", final_payload, SCHEMA_FINGERPRINT_OFFSET
        )[0]
        if final_fingerprint != original_fingerprint:
            raise AssertionError("V1 upgrade rebuilt with the wrong schema identity")

        print(
            "PASS: persistent generic index snapshots carry deterministic schema-layout identity; "
            "checksum-valid semantic staleness and legacy V1 snapshots are rejected as cache misses "
            "and rebuilt safely from wide payload rows."
        )
    finally:
        cleanup(db_path)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)

import os
import subprocess
import sys


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb_record_payload_probe.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_record_payload_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_record_payload_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_record_payload_probe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)
    if probe is None:
        raise AssertionError("Could not find tinydb_record_payload_probe executable")

    result = subprocess.run(
        [probe],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=30,
    )
    output = result.stdout + result.stderr
    if result.returncode != 0:
        raise AssertionError(output)
    if "RECORD_PAYLOAD_OK" not in output:
        raise AssertionError("missing RECORD_PAYLOAD_OK marker\n" + output)

    print("PASS: logical schema-sized record payload and legacy slot packing verified")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)

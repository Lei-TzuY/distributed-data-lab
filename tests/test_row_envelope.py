import os
import subprocess
import sys


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb_row_envelope_probe.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_row_envelope_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_row_envelope_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_row_envelope_probe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)
    if probe is None:
        raise AssertionError("Could not find tinydb_row_envelope_probe executable")

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

    marker = "PASS: compact row-envelope V2 stores VARCHAR fields at actual length"
    if marker not in output:
        raise AssertionError(f"missing compact-envelope success marker\n{output}")

    print(
        "PASS: compact row-envelope V2 stores actual VARCHAR lengths, round-trips "
        "through canonical logical payloads, preserves V1 decode compatibility, and "
        "fails closed on malformed compact metadata"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)

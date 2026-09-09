import os
import subprocess
import sys


def find_probe(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb_leaf_format_discriminator_probe.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb_leaf_format_discriminator_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_leaf_format_discriminator_probe.exe"),
        os.path.join(repo_root, "build", "tinydb_leaf_format_discriminator_probe"),
    ]
    for path in candidates:
        if os.path.exists(path):
            return path
    return None


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    probe = find_probe(repo_root)
    if probe is None:
        raise AssertionError("Could not find tinydb_leaf_format_discriminator_probe executable")

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
    if "LEAF_FORMAT_DISCRIMINATOR_OK" not in output:
        raise AssertionError("missing discriminator success marker\n" + output)

    for marker in ("v1=yes", "v2=yes", "disjoint=yes", "corrupt_fail_closed=yes"):
        if marker not in output:
            raise AssertionError(f"missing {marker}\n{output}")

    print("PASS: leaf format discriminator separates V1, V2, and corrupt markers")


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)

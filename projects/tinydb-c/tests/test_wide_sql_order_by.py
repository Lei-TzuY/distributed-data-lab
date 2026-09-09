import os
import subprocess
import sys
import tempfile


def find_executable(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb"),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def run_commands(executable, db_path, commands):
    process = subprocess.run(
        [executable, db_path],
        input=commands,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=90,
    )
    return process.returncode, process.stdout + process.stderr


def clean_line(line):
    line = line.strip()
    while line.startswith("db > "):
        line = line[5:].strip()
    return line


def tuple_ids(output):
    ids = []
    for raw in output.splitlines():
        line = clean_line(raw)
        if line.startswith("(") and line.endswith(")") and "," in line:
            try:
                ids.append(int(line[1:line.index(",")]))
            except ValueError:
                pass
    return ids


def scalar_uints(output):
    values = []
    for raw in output.splitlines():
        line = clean_line(raw)
        if line.isdigit():
            values.append(int(line))
    return values


def scalar_text(output):
    values = []
    for raw in output.splitlines():
        line = clean_line(raw)
        if line.startswith("left-"):
            values.append(line)
    return values


def require_ok(executable, db_path, sql):
    rc, output = run_commands(executable, db_path, sql + "\n.exit\n")
    if rc != 0 or "Error:" in output:
        raise AssertionError(output)
    return output


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_executable(repo_root)
    if executable is None:
        raise AssertionError("could not find tinydb executable")

    with tempfile.TemporaryDirectory(prefix="tinydb-wide-order-") as temp_dir:
        db_path = os.path.join(temp_dir, "wide-order.db")
        setup = [
            "CREATE TABLE wide_order (id INT, left_text VARCHAR, right_text VARCHAR);",
            "INSERT INTO wide_order VALUES (30, 'left-30', 'delta');",
            "INSERT INTO wide_order VALUES (10, 'left-10', 'echo');",
            "INSERT INTO wide_order VALUES (50, 'left-50', 'alpha');",
            "INSERT INTO wide_order VALUES (20, 'left-20', 'charlie');",
            "INSERT INTO wide_order VALUES (40, 'left-40', 'bravo');",
        ]
        output = require_ok(executable, db_path, "\n".join(setup))
        if "schema-sized" in output.lower() and "error" in output.lower():
            raise AssertionError(output)

        output = require_ok(
            executable,
            db_path,
            "SELECT * FROM wide_order ORDER BY id ASC;",
        )
        if tuple_ids(output) != [10, 20, 30, 40, 50]:
            raise AssertionError("wide ASC order mismatch:\n" + output)

        output = require_ok(
            executable,
            db_path,
            "SELECT id FROM wide_order ORDER BY id DESC LIMIT 3;",
        )
        if scalar_uints(output) != [50, 40, 30]:
            raise AssertionError("wide DESC LIMIT mismatch:\n" + output)

        output = require_ok(
            executable,
            db_path,
            "SELECT id FROM wide_order ORDER BY right_text ASC;",
        )
        if scalar_uints(output) != [50, 40, 20, 30, 10]:
            raise AssertionError("wide VARCHAR ORDER BY mismatch:\n" + output)

        output = require_ok(
            executable,
            db_path,
            "SELECT left_text FROM wide_order ORDER BY right_text DESC LIMIT 2 OFFSET 1;",
        )
        if scalar_text(output) != ["left-30", "left-20"]:
            raise AssertionError("wide VARCHAR DESC LIMIT/OFFSET mismatch:\n" + output)

        output = require_ok(
            executable,
            db_path,
            "SELECT left_text FROM wide_order "
            "WHERE id >= 20 AND id <= 40 "
            "ORDER BY id DESC LIMIT 2 OFFSET 1;",
        )
        if scalar_text(output) != ["left-30", "left-20"]:
            raise AssertionError("wide filtered ORDER BY mismatch:\n" + output)

        output = require_ok(
            executable,
            db_path,
            "SELECT id FROM wide_order "
            "WHERE (id = 10 OR id = 50) ORDER BY right_text ASC;",
        )
        if scalar_uints(output) != [50, 10]:
            raise AssertionError("wide boolean schema-column ORDER BY mismatch:\n" + output)

        # Reopen is implicit because each command launches a fresh process.
        output = require_ok(
            executable,
            db_path,
            "SELECT id FROM wide_order ORDER BY right_text DESC OFFSET 4;",
        )
        if scalar_uints(output) != [50]:
            raise AssertionError("wide schema-column ORDER BY reopen/offset mismatch:\n" + output)

        output = require_ok(executable, db_path, "PRAGMA integrity_check;")
        if "ok" not in output.lower():
            raise AssertionError("wide ORDER BY integrity check failed:\n" + output)

        legacy_path = os.path.join(temp_dir, "legacy-order.db")
        legacy_setup = (
            "INSERT INTO users VALUES (1, 'u1', 'u1@example.com');\n"
            "INSERT INTO users VALUES (2, 'u2', 'u2@example.com');\n"
            "INSERT INTO users VALUES (3, 'u3', 'u3@example.com');"
        )
        require_ok(executable, legacy_path, legacy_setup)
        output = require_ok(
            executable,
            legacy_path,
            "SELECT * FROM users ORDER BY id DESC LIMIT 2;",
        )
        if tuple_ids(output) != [3, 2]:
            raise AssertionError("legacy ORDER BY route regressed:\n" + output)

    print(
        "PASS: wide SQL ORDER BY supports schema-aware INT/VARCHAR keys, boolean WHERE, "
        "projection, LIMIT/OFFSET, reopen, integrity checks, and preserves the legacy users route"
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)

import glob
import os
import shutil
import subprocess
import sys
import tempfile
import time


def find_executable(repo_root):
    candidates = [
        os.path.join(repo_root, "build", "Debug", "tinydb.exe"),
        os.path.join(repo_root, "build", "Release", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb.exe"),
        os.path.join(repo_root, "build", "tinydb"),
    ]
    return next((path for path in candidates if os.path.exists(path)), None)


def run_commands(executable, db_file, commands, timeout=90):
    return subprocess.run(
        [executable, db_file],
        input=commands,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=timeout,
    )


def cleanup(db_file):
    for path in glob.glob(db_file + "*"):
        try:
            os.remove(path)
        except OSError:
            pass


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    executable = find_executable(repo_root)
    if executable is None:
        raise AssertionError("Could not find tinydb executable")

    temp_dir = tempfile.mkdtemp(prefix="tinydb-multi-root-crash-")
    db_file = os.path.join(temp_dir, "multi_root.db")

    try:
        seed_commands = [
            "CREATE TABLE archive (id INT, username VARCHAR, email VARCHAR);\n"
        ]
        for row_id in range(1, 11):
            seed_commands.append(
                f"INSERT INTO users VALUES ({row_id}, 'stable-user-{row_id}', 'u{row_id}@stable.test');\n"
            )
            seed_commands.append(
                f"INSERT INTO archive VALUES ({row_id}, 'stable-archive-{row_id}', 'a{row_id}@stable.test');\n"
            )
        seed_commands.append(".exit\n")
        seeded = run_commands(executable, db_file, "".join(seed_commands))
        assert seeded.returncode == 0, seeded.stdout + "\n" + seeded.stderr
        assert "Error:" not in seeded.stdout, seeded.stdout

        commands = ["BEGIN;\n"]
        for row_id in range(1000, 1300):
            commands.append(
                f"INSERT INTO users VALUES ({row_id}, 'ghost-user-{row_id}', 'gu{row_id}@test');\n"
            )
            commands.append(
                f"INSERT INTO archive VALUES ({row_id}, 'ghost-archive-{row_id}', 'ga{row_id}@test');\n"
            )

        process = subprocess.Popen(
            [executable, db_file],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            text=True,
        )
        assert process.stdin is not None
        process.stdin.write("".join(commands))
        process.stdin.flush()
        time.sleep(2.0)
        process.kill()
        process.wait(timeout=10)

        reopened = run_commands(
            executable,
            db_file,
            "SELECT COUNT(*) FROM users;\n"
            "SELECT COUNT(*) FROM archive;\n"
            "SELECT * FROM users WHERE id >= 1000 LIMIT 1;\n"
            "SELECT * FROM archive WHERE id >= 1000 LIMIT 1;\n"
            "PRAGMA integrity_check;\n"
            ".exit\n",
        )
        assert reopened.returncode == 0, reopened.stdout + "\n" + reopened.stderr
        assert reopened.stdout.count("db > 10\nExecuted.") >= 2, reopened.stdout
        assert "ghost-user-" not in reopened.stdout, reopened.stdout
        assert "ghost-archive-" not in reopened.stdout, reopened.stdout
        assert "ok" in reopened.stdout.lower(), reopened.stdout

        print("PASS: no-steal crash atomicity spans independent table roots")
    finally:
        cleanup(db_file)
        shutil.rmtree(temp_dir, ignore_errors=True)


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)

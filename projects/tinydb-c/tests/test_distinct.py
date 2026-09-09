import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_distinct.db"

def run_db(commands):
    input_data = "\n".join(commands) + "\n.exit\n"
    proc = subprocess.Popen(
        [EXE, DB_FILE],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    stdout, stderr = proc.communicate(input_data)
    return stdout

def extract_result_lines(raw_out):
    lines = []
    for line in raw_out.splitlines():
        line = line.replace("db > ", "").strip()
        if line and line != "Executed.":
            lines.append(line)
    return lines

def cleanup():
    for f in os.listdir("."):
        if f.startswith(DB_FILE):
            try:
                os.remove(f)
            except OSError:
                pass

class TestDistinct(unittest.TestCase):
    def setUp(self):
        cleanup()

    def tearDown(self):
        cleanup()

    def test_distinct_usernames(self):
        out = run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 alice alice2@test.com",
            "INSERT 4 charlie charlie@test.com",
            "SELECT DISTINCT username FROM users;"
        ])

        lines = extract_result_lines(out)
        self.assertEqual(lines, ["alice", "bob", "charlie"])

    def test_distinct_emails_with_where(self):
        out = run_db([
            "INSERT 1 user1 same@test.com",
            "INSERT 2 user2 same@test.com",
            "INSERT 3 user3 diff@test.com",
            "SELECT DISTINCT email FROM users WHERE id >= 1;"
        ])

        lines = extract_result_lines(out)
        self.assertEqual(lines, ["same@test.com", "diff@test.com"])

    def test_distinct_with_limit(self):
        out = run_db([
            "INSERT 1 u1 same@test.com",
            "INSERT 2 u2 same@test.com",
            "INSERT 3 u3 other@test.com",
            "INSERT 4 u4 next@test.com",
            "SELECT DISTINCT email FROM users LIMIT 2;"
        ])

        lines = extract_result_lines(out)
        self.assertEqual(lines, ["same@test.com", "other@test.com"])

    def test_distinct_full_rows(self):
        out = run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "SELECT DISTINCT * FROM users;"
        ])

        lines = extract_result_lines(out)
        self.assertEqual(lines, ["(1, alice, alice@test.com)", "(2, bob, bob@test.com)"])

if __name__ == "__main__":
    unittest.main()

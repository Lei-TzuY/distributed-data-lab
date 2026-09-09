import os
import subprocess
import unittest

def find_executable():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb.exe"),
        os.path.join(base_dir, "build", "Release", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb"),
    ]
    return next((path for path in candidates if os.path.exists(path)), candidates[-1])


EXE = find_executable()
DB_FILE = "test_groupby.db"

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

def cleanup():
    for f in os.listdir("."):
        if f.startswith(DB_FILE):
            try:
                os.remove(f)
            except OSError:
                pass

class TestGroupByHaving(unittest.TestCase):
    def setUp(self):
        cleanup()

    def tearDown(self):
        cleanup()

    def test_group_by_email_count(self):
        out = run_db([
            "INSERT INTO users VALUES (1, 'alice', 'work@example.com');",
            "INSERT INTO users VALUES (2, 'bob', 'work@example.com');",
            "INSERT INTO users VALUES (3, 'charlie', 'personal@example.com');",
            "SELECT email, COUNT(*) FROM users GROUP BY email;"
        ])

        self.assertIn("work@example.com | 2", out)
        self.assertIn("personal@example.com | 1", out)

    def test_group_by_having_filter(self):
        out = run_db([
            "INSERT INTO users VALUES (1, 'alice', 'work@example.com');",
            "INSERT INTO users VALUES (2, 'bob', 'work@example.com');",
            "INSERT INTO users VALUES (3, 'charlie', 'personal@example.com');",
            "EXPLAIN SELECT email, COUNT(*) FROM users GROUP BY email HAVING COUNT(*) > 1;",
            "SELECT email, COUNT(*) FROM users GROUP BY email HAVING COUNT(*) > 1;"
        ])

        self.assertIn("PLAN: GROUP BY AGGREGATION SCAN (GROUP BY email, HAVING FILTER)", out)
        self.assertIn("work@example.com | 2", out)
        self.assertNotIn("personal@example.com | 1", out)

    def test_group_by_username_max(self):
        out = run_db([
            "INSERT INTO users VALUES (10, 'alice', 'a1@test.com');",
            "INSERT INTO users VALUES (20, 'alice', 'a2@test.com');",
            "INSERT INTO users VALUES (15, 'bob', 'b@test.com');",
            "SELECT username, MAX(id) FROM users GROUP BY username;"
        ])

        self.assertIn("alice | 20", out)
        self.assertIn("bob | 15", out)

    def test_group_by_where_and_having(self):
        out = run_db([
            "INSERT INTO users VALUES (1, 'u1', 'e1@test.com');",
            "INSERT INTO users VALUES (2, 'u2', 'e1@test.com');",
            "INSERT INTO users VALUES (3, 'u3', 'e2@test.com');",
            "SELECT email, COUNT(*) FROM users WHERE id >= 2 GROUP BY email HAVING COUNT(*) >= 1;"
        ])

        self.assertIn("e1@test.com | 1", out)
        self.assertIn("e2@test.com | 1", out)

if __name__ == "__main__":
    unittest.main()

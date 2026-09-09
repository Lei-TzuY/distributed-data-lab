import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_subquery.db"

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

class TestSubquery(unittest.TestCase):
    def setUp(self):
        cleanup()

    def tearDown(self):
        cleanup()

    def test_where_id_in_subquery(self):
        out = run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 charlie charlie@test.com",
            "INSERT 4 david david@test.com",
            "SELECT * FROM users WHERE id IN (SELECT id FROM users WHERE id > 2);"
        ])

        self.assertNotIn("(1, alice", out)
        self.assertNotIn("(2, bob", out)
        self.assertIn("(3, charlie, charlie@test.com)", out)
        self.assertIn("(4, david, david@test.com)", out)

    def test_where_exists_subquery(self):
        out_exists = run_db([
            "INSERT 1 alice alice@test.com",
            "SELECT * FROM users WHERE EXISTS (SELECT id FROM users WHERE id = 1);"
        ])
        self.assertIn("(1, alice, alice@test.com)", out_exists)

        cleanup()

        out_not_exists = run_db([
            "INSERT 1 alice alice@test.com",
            "SELECT * FROM users WHERE EXISTS (SELECT id FROM users WHERE id = 999);"
        ])
        self.assertNotIn("(1, alice", out_not_exists)

    def test_subquery_with_limit(self):
        out = run_db([
            "INSERT 1 user1 u1@test.com",
            "INSERT 2 user2 u2@test.com",
            "INSERT 3 user3 u3@test.com",
            "SELECT * FROM users WHERE id IN (SELECT id FROM users WHERE id >= 1) LIMIT 2;"
        ])

        self.assertIn("(1, user1", out)
        self.assertIn("(2, user2", out)
        self.assertNotIn("(3, user3", out)

if __name__ == "__main__":
    unittest.main()

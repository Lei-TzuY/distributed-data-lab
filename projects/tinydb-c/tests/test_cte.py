import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_cte.db"

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

class TestCTE(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 charlie charlie@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_with_cte_query(self):
        out = run_db([
            "WITH user_cte AS (SELECT * FROM users WHERE username = 'alice') SELECT * FROM user_cte;"
        ])

        self.assertIn("(1, alice, alice@test.com)", out)
        self.assertNotIn("bob", out)
        self.assertNotIn("charlie", out)

    def test_with_cte_with_limit(self):
        out = run_db([
            "WITH all_users AS (SELECT * FROM users) SELECT * FROM all_users LIMIT 2;"
        ])

        self.assertIn("(1, alice, alice@test.com)", out)
        self.assertIn("(2, bob, bob@test.com)", out)
        self.assertNotIn("charlie", out)

if __name__ == "__main__":
    unittest.main()

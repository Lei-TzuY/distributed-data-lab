import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_not.db"

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

class TestNotPredicates(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 charlie charlie@test.com",
            "INSERT 4 david david@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_not_in(self):
        out = run_db([
            "SELECT * FROM users WHERE id NOT IN (1, 3);"
        ])

        self.assertIn("(2, bob, bob@test.com)", out)
        self.assertIn("(4, david, david@test.com)", out)
        self.assertNotIn("alice", out)
        self.assertNotIn("charlie", out)

    def test_not_like(self):
        out = run_db([
            "SELECT * FROM users WHERE username NOT LIKE '%a%';"
        ])

        self.assertIn("(2, bob, bob@test.com)", out)
        self.assertNotIn("alice", out)
        self.assertNotIn("charlie", out)
        self.assertNotIn("david", out)

if __name__ == "__main__":
    unittest.main()

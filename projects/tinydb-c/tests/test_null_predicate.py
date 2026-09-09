import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_null.db"

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

class TestNullPredicate(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob NULL",
            "INSERT 3 charlie charlie@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_is_null(self):
        out = run_db([
            "SELECT * FROM users WHERE email IS NULL;"
        ])

        self.assertIn("(2, bob, NULL)", out)
        self.assertNotIn("alice", out)
        self.assertNotIn("charlie", out)

    def test_is_not_null(self):
        out = run_db([
            "SELECT * FROM users WHERE email IS NOT NULL;"
        ])

        self.assertIn("(1, alice, alice@test.com)", out)
        self.assertIn("(3, charlie, charlie@test.com)", out)
        self.assertNotIn("(2, bob", out)

if __name__ == "__main__":
    unittest.main()

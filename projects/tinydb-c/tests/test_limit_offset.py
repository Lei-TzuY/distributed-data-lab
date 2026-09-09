import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_offset.db"

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

class TestLimitOffset(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 charlie charlie@test.com",
            "INSERT 4 david david@test.com",
            "INSERT 5 eve eve@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_limit_offset_pagination(self):
        out = run_db([
            "SELECT * FROM users LIMIT 2 OFFSET 1;"
        ])

        self.assertNotIn("alice", out)
        self.assertIn("(2, bob, bob@test.com)", out)
        self.assertIn("(3, charlie, charlie@test.com)", out)
        self.assertNotIn("david", out)
        self.assertNotIn("eve", out)

    def test_offset_only(self):
        out = run_db([
            "SELECT * FROM users OFFSET 3;"
        ])

        self.assertNotIn("alice", out)
        self.assertNotIn("bob", out)
        self.assertNotIn("charlie", out)
        self.assertIn("(4, david, david@test.com)", out)
        self.assertIn("(5, eve, eve@test.com)", out)

if __name__ == "__main__":
    unittest.main()

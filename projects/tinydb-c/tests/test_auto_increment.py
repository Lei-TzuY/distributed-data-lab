import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_auto_increment.db"

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

class TestAutoIncrement(unittest.TestCase):
    def setUp(self):
        cleanup()

    def tearDown(self):
        cleanup()

    def test_auto_increment_ids(self):
        out = run_db([
            "INSERT alice alice@test.com",
            "INSERT bob bob@test.com",
            "INSERT charlie charlie@test.com",
            "SELECT * FROM users;"
        ])

        self.assertIn("(1, alice, alice@test.com)", out)
        self.assertIn("(2, bob, bob@test.com)", out)
        self.assertIn("(3, charlie, charlie@test.com)", out)

    def test_mixed_explicit_and_auto_increment(self):
        out = run_db([
            "INSERT 10 alice alice@test.com",
            "INSERT bob bob@test.com",
            "SELECT * FROM users;"
        ])

        self.assertIn("(10, alice, alice@test.com)", out)
        self.assertIn("(11, bob, bob@test.com)", out)

if __name__ == "__main__":
    unittest.main()

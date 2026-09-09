import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_ilike.db"

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

class TestIlike(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 Alice Alice@test.com",
            "INSERT 2 Bob Bob@test.com",
            "INSERT 3 CHARLIE charlie@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_ilike_case_insensitive(self):
        out = run_db([
            "SELECT * FROM users WHERE username ILIKE '%alice%';"
        ])

        self.assertIn("(1, Alice, Alice@test.com)", out)
        self.assertNotIn("Bob", out)
        self.assertNotIn("CHARLIE", out)

    def test_ilike_uppercase_pattern(self):
        out = run_db([
            "SELECT * FROM users WHERE username ILIKE '%CHAR%';"
        ])

        self.assertIn("(3, CHARLIE, charlie@test.com)", out)
        self.assertNotIn("Alice", out)

if __name__ == "__main__":
    unittest.main()

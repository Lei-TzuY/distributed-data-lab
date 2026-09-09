import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_strfn.db"

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

class TestStringFunctions(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 Alice Alice@test.com",
            "INSERT 2 Bob Bob@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_length(self):
        out = run_db([
            "SELECT LENGTH(username) FROM users WHERE id = 1;"
        ])

        self.assertIn("5", out)

    def test_upper(self):
        out = run_db([
            "SELECT UPPER(username) FROM users WHERE id = 1;"
        ])

        self.assertIn("ALICE", out)

    def test_lower(self):
        out = run_db([
            "SELECT LOWER(username) FROM users WHERE id = 2;"
        ])

        self.assertIn("bob", out)

if __name__ == "__main__":
    unittest.main()

import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_window.db"

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

class TestWindowFunctions(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice dev@company.com",
            "INSERT 2 bob dev@company.com",
            "INSERT 3 charlie sales@company.com"
        ])

    def tearDown(self):
        cleanup()

    def test_row_number_over(self):
        out = run_db([
            "SELECT ROW_NUMBER() OVER () FROM users;"
        ])

        self.assertIn("1 | (1, alice, dev@company.com)", out)
        self.assertIn("2 | (2, bob, dev@company.com)", out)
        self.assertIn("3 | (3, charlie, sales@company.com)", out)

    def test_row_number_partition_by(self):
        out = run_db([
            "SELECT ROW_NUMBER() OVER (PARTITION BY email) FROM users;"
        ])

        self.assertIn("1 | (1, alice, dev@company.com)", out)
        self.assertIn("2 | (2, bob, dev@company.com)", out)
        self.assertIn("1 | (3, charlie, sales@company.com)", out)

if __name__ == "__main__":
    unittest.main()

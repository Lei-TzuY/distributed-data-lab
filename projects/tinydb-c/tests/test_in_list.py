import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_inlist.db"

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

class TestInList(unittest.TestCase):
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

    def test_in_list_literal(self):
        out = run_db([
            "SELECT * FROM users WHERE id IN (1, 3, 5);"
        ])

        self.assertIn("(1, alice, alice@test.com)", out)
        self.assertIn("(3, charlie, charlie@test.com)", out)
        self.assertIn("(5, eve, eve@test.com)", out)
        self.assertNotIn("bob", out)
        self.assertNotIn("david", out)

if __name__ == "__main__":
    unittest.main()

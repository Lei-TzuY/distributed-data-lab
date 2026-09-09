import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_fts.db"

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

class TestFullTextSearch(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice@company.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 charlie charlie@company.com"
        ])

    def tearDown(self):
        cleanup()

    def test_fts_match_keyword(self):
        out = run_db([
            "SELECT * FROM users WHERE MATCH 'company';"
        ])

        self.assertIn("(1, alice, alice@company.com)", out)
        self.assertIn("(3, charlie, charlie@company.com)", out)
        self.assertNotIn("bob", out)

    def test_fts_column_match_keyword(self):
        out = run_db([
            "SELECT * FROM users WHERE username MATCH 'alice';"
        ])

        self.assertIn("(1, alice, alice@company.com)", out)
        self.assertNotIn("bob", out)
        self.assertNotIn("charlie", out)

if __name__ == "__main__":
    unittest.main()

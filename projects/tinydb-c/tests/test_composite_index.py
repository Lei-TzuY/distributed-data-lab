import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_comp_index.db"

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

class TestCompositeIndex(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 alice alice@company.com"
        ])

    def tearDown(self):
        cleanup()

    def test_create_composite_index(self):
        out = run_db([
            "CREATE INDEX idx_user_email ON users(username, email);",
            "PRAGMA index_list;"
        ])

        self.assertIn("idx_user_email", out)

    def test_composite_index_lookup(self):
        out = run_db([
            "CREATE INDEX idx_user_email ON users(username, email);",
            "SELECT * FROM users WHERE username = 'alice' AND email = 'alice@test.com';"
        ])

        self.assertIn("(1, alice, alice@test.com)", out)
        self.assertNotIn("alice@company.com", out)

if __name__ == "__main__":
    unittest.main()

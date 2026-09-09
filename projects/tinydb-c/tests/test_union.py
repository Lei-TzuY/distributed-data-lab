import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_union.db"

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

class TestUnion(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 charlie charlie@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_union_all(self):
        out = run_db([
            "SELECT * FROM users WHERE id = 1 UNION ALL SELECT * FROM users WHERE id = 2;"
        ])

        self.assertIn("(1, alice, alice@test.com)", out)
        self.assertIn("(2, bob, bob@test.com)", out)
        self.assertNotIn("charlie", out)

    def test_union_combine(self):
        out = run_db([
            "SELECT * FROM users WHERE id = 1 UNION SELECT * FROM users WHERE id = 3;"
        ])

        self.assertIn("(1, alice, alice@test.com)", out)
        self.assertIn("(3, charlie, charlie@test.com)", out)
        self.assertNotIn("bob", out)

if __name__ == "__main__":
    unittest.main()

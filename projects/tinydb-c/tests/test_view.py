import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_view.db"

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

class TestView(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 charlie charlie@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_create_and_query_view(self):
        out = run_db([
            "CREATE VIEW alice_view AS SELECT * FROM users WHERE username = 'alice';",
            "SELECT * FROM alice_view;"
        ])

        self.assertIn("View 'alice_view' created.", out)
        self.assertIn("(1, alice, alice@test.com)", out)
        self.assertNotIn("bob", out)

    def test_drop_view(self):
        out = run_db([
            "CREATE VIEW test_view AS SELECT * FROM users;",
            "DROP VIEW test_view;"
        ])

        self.assertIn("View 'test_view' created.", out)
        self.assertIn("View 'test_view' dropped.", out)

if __name__ == "__main__":
    unittest.main()

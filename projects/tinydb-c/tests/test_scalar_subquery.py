import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_scalarsub.db"

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

class TestScalarSubquery(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 charlie charlie@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_scalar_subquery_projection(self):
        out = run_db([
            "SELECT id, (SELECT count(*) FROM users) FROM users;"
        ])

        self.assertIn("1 | 3", out)
        self.assertIn("2 | 3", out)
        self.assertIn("3 | 3", out)

if __name__ == "__main__":
    unittest.main()

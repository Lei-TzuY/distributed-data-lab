import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_mathfn.db"

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

class TestMathFunctions(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 charlie charlie@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_abs(self):
        out = run_db([
            "SELECT ABS(id) FROM users WHERE id = 1;"
        ])

        self.assertIn("1", out)

    def test_mod(self):
        out = run_db([
            "SELECT MOD(id, 2) FROM users WHERE id = 3;"
        ])

        self.assertIn("1", out)

        out2 = run_db([
            "SELECT MOD(id, 2) FROM users WHERE id = 2;"
        ])

        self.assertIn("0", out2)

if __name__ == "__main__":
    unittest.main()

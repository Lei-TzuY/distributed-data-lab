import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_concat.db"

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

class TestConcatFunction(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_concat(self):
        out = run_db([
            "SELECT CONCAT(username, email) FROM users WHERE id = 1;"
        ])

        self.assertIn("alicealice@test.com", out)

if __name__ == "__main__":
    unittest.main()

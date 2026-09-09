import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_multisort.db"

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

class TestMultiSort(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db([
            "INSERT 1 alice alice1@test.com",
            "INSERT 2 bob bob1@test.com",
            "INSERT 3 alice alice2@test.com",
            "INSERT 4 charlie charlie1@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_multi_column_order_by(self):
        out = run_db([
            "SELECT * FROM users ORDER BY username ASC, id DESC;"
        ])

        # Expect alice with id 3 before alice with id 1, then bob (2), then charlie (4)
        lines = [line.replace("db > ", "").strip() for line in out.split("\n") if "(" in line]
        self.assertEqual(len(lines), 4)
        self.assertIn("3, alice", lines[0])
        self.assertIn("1, alice", lines[1])
        self.assertIn("2, bob", lines[2])
        self.assertIn("4, charlie", lines[3])

if __name__ == "__main__":
    unittest.main()

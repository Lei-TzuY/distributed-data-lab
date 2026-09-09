import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_fk_cascade.db"

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

class TestForeignKeyCascade(unittest.TestCase):
    def setUp(self):
        cleanup()

    def tearDown(self):
        cleanup()

    def test_on_delete_cascade_clause_parsing(self):
        out = run_db([
            "CREATE TABLE orders (id int, user_id int REFERENCES users(id) ON DELETE CASCADE);",
            ".tables"
        ])

        self.assertIn("orders", out)

    def test_on_delete_cascade_execution(self):
        out = run_db([
            "CREATE TABLE orders (id int, user_id int REFERENCES users(id) ON DELETE CASCADE);",
            "INSERT 1 alice alice@test.com",
            "DELETE FROM users WHERE id = 1;"
        ])

        self.assertIn("Foreign key ON DELETE CASCADE triggered", out)

if __name__ == "__main__":
    unittest.main()

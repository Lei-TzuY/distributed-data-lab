import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_catalog.db"

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

class TestSqliteMaster(unittest.TestCase):
    def setUp(self):
        cleanup()

    def tearDown(self):
        cleanup()

    def test_sqlite_master_query(self):
        out = run_db([
            "CREATE TABLE products (id INT, name VARCHAR);",
            "CREATE VIEW v_users AS SELECT * FROM users;",
            "SELECT * FROM sqlite_master;"
        ])

        self.assertIn("(table, users, users, 0,", out)
        self.assertIn("(table, products, products,", out)
        self.assertIn("(view, v_users, v_users, 0,", out)

if __name__ == "__main__":
    unittest.main()

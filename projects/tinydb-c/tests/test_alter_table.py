import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_alter_table.db"

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

class TestAlterTable(unittest.TestCase):
    def setUp(self):
        cleanup()

    def tearDown(self):
        cleanup()

    def test_rename_table(self):
        out = run_db([
            "CREATE TABLE products (name varchar, price int);",
            ".tables",
            "ALTER TABLE products RENAME TO items;",
            ".tables"
        ])

        self.assertIn("products", out.split("ALTER")[0])
        self.assertIn("items", out.split("renamed")[1])
        self.assertIn("Table 'products' renamed to 'items'.", out)

    def test_rename_users_table(self):
        out = run_db([
            "ALTER TABLE users RENAME TO people;",
            ".tables"
        ])

        self.assertIn("Table 'users' renamed to 'people'.", out)
        self.assertIn("people", out)

    def test_rename_nonexistent(self):
        out = run_db([
            "ALTER TABLE nonexistent RENAME TO something;"
        ])

        self.assertIn("Error: Table 'nonexistent' not found.", out)

    def test_rename_duplicate(self):
        out = run_db([
            "CREATE TABLE t1 (a int);",
            "CREATE TABLE t2 (b int);",
            "ALTER TABLE t1 RENAME TO t2;"
        ])

        self.assertIn("Error: Table 't2' already exists.", out)

    def test_add_column(self):
        out = run_db([
            "CREATE TABLE products (name varchar, price int);",
            "ALTER TABLE products ADD COLUMN quantity int;",
            "PRAGMA table_info;"
        ])

        self.assertIn("Column 'quantity' added to table 'products'.", out)

    def test_add_column_rejects_users(self):
        out = run_db([
            "ALTER TABLE users ADD COLUMN phone varchar;"
        ])

        self.assertIn("Error: Cannot add columns to built-in 'users' table (fixed row layout).", out)

    def test_add_column_nonexistent_table(self):
        out = run_db([
            "ALTER TABLE ghost ADD COLUMN x int;"
        ])

        self.assertIn("Error: Table 'ghost' not found.", out)

    def test_add_duplicate_column(self):
        out = run_db([
            "CREATE TABLE products (name varchar, price int);",
            "ALTER TABLE products ADD COLUMN name varchar;"
        ])

        self.assertIn("Error: Column 'name' already exists in table 'products'.", out)

if __name__ == "__main__":
    unittest.main()

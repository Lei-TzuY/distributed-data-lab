import os
import subprocess
import unittest

def find_executable():
    base_dir = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    candidates = [
        os.path.join(base_dir, "build", "Debug", "tinydb.exe"),
        os.path.join(base_dir, "build", "Release", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb.exe"),
        os.path.join(base_dir, "build", "tinydb"),
    ]
    return next((path for path in candidates if os.path.exists(path)), candidates[-1])


EXE = find_executable()
DB_FILE = "test_generic_idx.db"

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

class TestGenericSecondaryIndex(unittest.TestCase):
    def setUp(self):
        cleanup()

    def tearDown(self):
        cleanup()

    def test_create_and_query_email_index(self):
        out = run_db([
            "INSERT INTO users VALUES (1, 'alice', 'alice@example.com');",
            "INSERT INTO users VALUES (2, 'bob', 'bob@example.com');",
            "INSERT INTO users VALUES (3, 'charlie', 'charlie@example.com');",
            "CREATE INDEX idx_users_email ON users(email);",
            "PRAGMA index_list;",
            "EXPLAIN SELECT * FROM users WHERE email = 'bob@example.com';",
            "SELECT * FROM users WHERE email = 'bob@example.com';"
        ])

        self.assertIn("idx_users_email", out)
        self.assertIn("PLAN: SECONDARY INDEX LOOKUP (email = 'bob@example.com')", out)
        self.assertIn("(2, bob, bob@example.com)", out)

    def test_multiple_secondary_indexes(self):
        out = run_db([
            "INSERT INTO users VALUES (10, 'user10', 'email10@test.com');",
            "INSERT INTO users VALUES (20, 'user20', 'email20@test.com');",
            "CREATE INDEX idx_users_username ON users(username);",
            "CREATE INDEX idx_users_email ON users(email);",
            "PRAGMA index_list;",
            "SELECT * FROM users WHERE username = 'user20';",
            "SELECT * FROM users WHERE email = 'email10@test.com';"
        ])

        self.assertIn("idx_users_username", out)
        self.assertIn("idx_users_email", out)
        self.assertIn("(20, user20, email20@test.com)", out)
        self.assertIn("(10, user10, email10@test.com)", out)

    def test_drop_generic_index(self):
        out1 = run_db([
            "INSERT INTO users VALUES (1, 'u1', 'e1@test.com');",
            "CREATE INDEX idx_users_email ON users(email);",
            "DROP INDEX idx_users_email;",
            "PRAGMA index_list;",
            "EXPLAIN SELECT * FROM users WHERE email = 'e1@test.com';"
        ])

        self.assertIn("(no indexes found)", out1)
        self.assertIn("PLAN: FULL TABLE SCAN (email = 'e1@test.com')", out1)

    def test_durability_across_reopen(self):
        run_db([
            "INSERT INTO users VALUES (5, 'dave', 'dave@domain.org');",
            "CREATE INDEX idx_users_email ON users(email);"
        ])

        out2 = run_db([
            "PRAGMA index_list;",
            "EXPLAIN SELECT * FROM users WHERE email = 'dave@domain.org';",
            "SELECT * FROM users WHERE email = 'dave@domain.org';"
        ])

        self.assertIn("idx_users_email", out2)
        self.assertIn("PLAN: SECONDARY INDEX LOOKUP (email = 'dave@domain.org')", out2)
        self.assertIn("(5, dave, dave@domain.org)", out2)

if __name__ == "__main__":
    unittest.main()

import os
import subprocess
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_vacuum.db"
BACKUP_FILE = "test_backup.db"

def run_db(db_name, commands):
    input_data = "\n".join(commands) + "\n.exit\n"
    proc = subprocess.Popen(
        [EXE, db_name],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    stdout, stderr = proc.communicate(input_data)
    return stdout

def cleanup():
    for f in os.listdir("."):
        if f.startswith(DB_FILE) or f.startswith(BACKUP_FILE):
            try:
                os.remove(f)
            except OSError:
                pass

class TestVacuumInto(unittest.TestCase):
    def setUp(self):
        cleanup()
        run_db(DB_FILE, [
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_vacuum_into_backup(self):
        out = run_db(DB_FILE, [
            f"VACUUM INTO '{BACKUP_FILE}';"
        ])

        self.assertIn(f"Database backed up to '{BACKUP_FILE}'.", out)
        self.assertTrue(os.path.exists(BACKUP_FILE))

        # Query the backup database file directly
        backup_out = run_db(BACKUP_FILE, [
            "SELECT * FROM users;"
        ])

        self.assertIn("(1, alice, alice@test.com)", backup_out)
        self.assertIn("(2, bob, bob@test.com)", backup_out)

if __name__ == "__main__":
    unittest.main()

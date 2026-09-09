import os
import subprocess
import threading
import unittest

EXE = os.path.abspath(os.path.join("build", "Debug", "tinydb.exe"))
DB_FILE = "test_rw_lock.db"

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

class TestRWLock(unittest.TestCase):
    def setUp(self):
        cleanup()
        # Pre-populate database with rows
        run_db([
            "INSERT 1 alice alice@test.com",
            "INSERT 2 bob bob@test.com",
            "INSERT 3 charlie charlie@test.com"
        ])

    def tearDown(self):
        cleanup()

    def test_concurrent_read_threads(self):
        results = []
        errors = []

        def reader_thread(thread_id):
            try:
                out = run_db([
                    f"SELECT * FROM users WHERE id = {thread_id};"
                ])
                results.append(out)
            except Exception as e:
                errors.append(str(e))

        threads = []
        for i in range(1, 4):
            t = threading.Thread(target=reader_thread, args=(i,))
            threads.append(t)
            t.start()

        for t in threads:
            t.join()

        self.assertEqual(len(errors), 0)
        self.assertEqual(len(results), 3)

    def test_buffer_pool_stats_after_concurrency(self):
        out = run_db([
            "SELECT * FROM users;",
            ".buffer_pool"
        ])

        self.assertIn("=== Buffer Pool Manager Statistics ===", out)
        self.assertIn("Capacity", out)
        self.assertIn("Hits", out)

if __name__ == "__main__":
    unittest.main()

import subprocess
import os
import sys

def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    possible_paths = [
        os.path.join(base_dir, 'build', 'Debug', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'Release', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb.exe'),
        os.path.join(base_dir, 'build', 'tinydb')
    ]
    
    executable_path = None
    for path in possible_paths:
        if os.path.exists(path):
            executable_path = path
            break
            
    if not executable_path:
        print("Could not find the tinydb executable.")
        sys.exit(1)
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_checkpoint.db')
    wal_file = db_file + ".wal"
    if os.path.exists(db_file):
        os.remove(db_file)
    if os.path.exists(wal_file):
        os.remove(wal_file)

    commands = (
        "insert 1 user1 u1@test.com\n"
        "insert 2 user2 u2@test.com\n"
        "checkpoint;\n"
        ".exit\n"
    )
    
    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    stdout, stderr = process.communicate(input=commands)
    
    if process.returncode != 0:
        print(f"FAIL: Checkpoint test failed with return code {process.returncode}")
        sys.exit(1)

    if "Checkpoint complete." not in stdout:
        print("FAIL: Expected 'Checkpoint complete.' in stdout.")
        print(stdout)
        sys.exit(1)

    if os.path.exists(wal_file):
        print("FAIL: WAL file still exists after explicit CHECKPOINT.")
        sys.exit(1)

    print("PASS: Explicit CHECKPOINT statement correctly flushed WAL to database file!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

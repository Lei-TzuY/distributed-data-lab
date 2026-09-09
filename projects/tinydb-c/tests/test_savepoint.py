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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_savepoint.db')
    if os.path.exists(db_file):
        os.remove(db_file)

    commands = (
        "begin;\n"
        "insert 10 user10 u10@test.com\n"
        "savepoint sp1;\n"
        "insert 20 user20 u20@test.com\n"
        "savepoint sp2;\n"
        "insert 30 user30 u30@test.com\n"
        "rollback to savepoint sp2;\n"  # row 30 rolled back, row 10 and 20 remain
        "select * from users;\n"
        "rollback to savepoint sp1;\n"  # row 20 rolled back, row 10 remains
        "select * from users;\n"
        "commit;\n"
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
        print(f"FAIL: Savepoint test failed with return code {process.returncode}")
        sys.exit(1)

    if "(10, user10, u10@test.com)" not in stdout:
        print("FAIL: Expected row 10 to remain after rollback to savepoint sp1.")
        print(stdout)
        sys.exit(1)

    if "(20, user20, u20@test.com)" in stdout and stdout.count("(20, user20") > 1:
        print("FAIL: Row 20 should have been rolled back by rollback to sp1.")
        print(stdout)
        sys.exit(1)

    if "(30, user30, u30@test.com)" in stdout:
        print("FAIL: Row 30 should have been rolled back by rollback to sp2.")
        print(stdout)
        sys.exit(1)

    print("PASS: Transaction Savepoints (SAVEPOINT, ROLLBACK TO) work correctly!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

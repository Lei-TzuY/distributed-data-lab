import subprocess
import os
import sys

def run_test():
    base_dir = os.path.join(os.path.dirname(__file__), '..')
    possible_paths = [
        os.path.join(base_dir, 'build', 'Debug', 'tinydb.exe'),
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
        
    print(f"Testing executable: {executable_path}")
    
    db_file = os.path.join(os.path.dirname(__file__), 'test_vm.db')
    wal_file = db_file + ".wal"
    for path in (db_file, wal_file):
        if os.path.exists(path):
            os.remove(path)

    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    commands = "insert 1 user test@example.com\nselect\nunrecognized\n.exit\n"
    stdout, stderr = process.communicate(input=commands)
    
    if "(1, user, test@example.com)" not in stdout:
        print("FAIL: Expected inserted row.")
        sys.exit(1)
        
    if "Unrecognized keyword at start of 'unrecognized'." not in stdout:
        print("FAIL: Expected unrecognized keyword error.")
        sys.exit(1)
        
    if "Executed." not in stdout:
        print("FAIL: Expected Executed message.")
        sys.exit(1)
        
    print("PASS: VM handled insert and select correctly.")

    for path in (db_file, wal_file):
        if os.path.exists(path):
            os.remove(path)

if __name__ == "__main__":
    run_test()

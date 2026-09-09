import subprocess
import os
import sys
import time

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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_wal.db')
    wal_file = db_file + ".wal"
    if os.path.exists(db_file):
        os.remove(db_file)
    if os.path.exists(wal_file):
        os.remove(wal_file)
        
    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    process.stdin.write("insert 1 wal_user wal@example.com\n")
    process.stdin.flush()
    time.sleep(0.5) 
    
    process.kill()
    process.wait()
    
    if not os.path.exists(wal_file):
        print("FAIL: WAL file was not created! Auto-commit failed.")
        sys.exit(1)
        
    process2 = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    stdout, stderr = process2.communicate(input="select\n.exit\n")
    
    if "WAL file found. Recovering..." not in stdout:
        print("FAIL: Did not print recovery message.")
        sys.exit(1)
        
    if "(1, wal_user, wal@example.com)" not in stdout:
        print("FAIL: Recovered data not found in select.")
        sys.exit(1)
        
    print("PASS: WAL Crash Recovery successfully restored the data!")

    if os.path.exists(db_file):
        os.remove(db_file)
    if os.path.exists(wal_file):
        os.remove(wal_file)

if __name__ == "__main__":
    run_test()

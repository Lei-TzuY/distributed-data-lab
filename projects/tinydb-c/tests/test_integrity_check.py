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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_integrity.db')
    if os.path.exists(db_file):
        os.remove(db_file)

    commands = ""
    for i in range(1, 25):
        commands += f"insert {i} user{i} u{i}@test.com\n"
    commands += "pragma integrity_check;\n.check\n.exit\n"
    
    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    stdout, stderr = process.communicate(input=commands)
    
    if process.returncode != 0:
        print(f"FAIL: Integrity check test failed with return code {process.returncode}")
        sys.exit(1)

    if stdout.count("ok") < 2:
        print("FAIL: Expected 'ok' output from PRAGMA integrity_check and .check.")
        print(stdout)
        sys.exit(1)

    print("PASS: B+ Tree and Pager PRAGMA integrity_check verified successfully!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

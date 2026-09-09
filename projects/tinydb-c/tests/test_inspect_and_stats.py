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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_inspect.db')
    if os.path.exists(db_file):
        os.remove(db_file)

    commands = (
        "insert 1 user1 user1@test.com\n"
        "insert 2 user2 user2@test.com\n"
        ".constants\n"
        ".stats\n"
        ".page 0\n"
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
        print(f"FAIL: REPL failed with code {process.returncode}")
        sys.exit(1)

    if "PAGE_SIZE: 4096" not in stdout or "LEAF_NODE_MAX_CELLS:" not in stdout:
        print("FAIL: Expected engine constants output in stdout.")
        print(stdout)
        sys.exit(1)

    if "Total Pages:" not in stdout or "Total Rows: 2" not in stdout:
        print("FAIL: Expected statistics output in stdout.")
        print(stdout)
        sys.exit(1)

    if "--- Page 0 Details ---" not in stdout or "Type: LEAF" not in stdout:
        print("FAIL: Expected page details inspection output.")
        print(stdout)
        sys.exit(1)

    print("PASS: Meta commands .constants, .stats, and .page 0 work correctly!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

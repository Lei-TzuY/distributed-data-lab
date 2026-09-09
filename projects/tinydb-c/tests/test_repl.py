import subprocess
import os
import sys

def run_test():
    # Find the executable
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
        print("Could not find the tinydb executable. Did you build it?")
        sys.exit(1)
        
    print(f"Testing executable: {executable_path}")
    
    db_file = os.path.join(os.path.dirname(__file__), 'test_repl.db')
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
    
    stdout, stderr = process.communicate(input=".exit\n")
    
    if process.returncode != 0:
        print(f"FAIL: Expected return code 0, got {process.returncode}")
        sys.exit(1)
        
    if "db >" not in stdout:
        print("FAIL: Expected prompt 'db >' in output.")
        print(f"Output was:\n{stdout}")
        sys.exit(1)
        
    print("PASS: REPL exited successfully on '.exit' command.")

    for path in (db_file, wal_file):
        if os.path.exists(path):
            os.remove(path)

if __name__ == "__main__":
    run_test()

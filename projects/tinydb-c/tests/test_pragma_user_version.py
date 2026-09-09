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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_pragma.db')
    if os.path.exists(db_file):
        os.remove(db_file)

    commands_set = (
        "pragma user_version;\n"
        "pragma user_version = 42;\n"
        "pragma user_version;\n"
        ".exit\n"
    )
    
    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    stdout, stderr = process.communicate(input=commands_set)
    
    if process.returncode != 0:
        print(f"FAIL: PRAGMA user_version test failed with return code {process.returncode}")
        sys.exit(1)

    if "0" not in stdout or "42" not in stdout:
        print("FAIL: Expected 0 initially and 42 after setting user_version.")
        print(stdout)
        sys.exit(1)

    # Re-open database to test durability across restarts
    commands_get = "pragma user_version;\n.exit\n"
    process2 = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    stdout2, stderr2 = process2.communicate(input=commands_get)

    if "42" not in stdout2:
        print("FAIL: PRAGMA user_version was not persisted to disk.")
        print(stdout2)
        sys.exit(1)

    print("PASS: PRAGMA user_version read, write, and persistence verified!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

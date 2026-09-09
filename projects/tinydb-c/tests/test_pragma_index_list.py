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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_idx_list.db')
    if os.path.exists(db_file):
        os.remove(db_file)

    commands = (
        "create index idx_users_username on users(username);\n"
        "pragma index_list;\n"
        "drop index idx_users_username;\n"
        "pragma index_list;\n"
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
        print(f"FAIL: PRAGMA index_list test failed with return code {process.returncode}")
        sys.exit(1)

    if "idx_users_username" not in stdout or "(no indexes found)" not in stdout:
        print("FAIL: PRAGMA index_list output verification failed.")
        print(stdout)
        sys.exit(1)

    print("PASS: PRAGMA index_list secondary index inspection verified!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

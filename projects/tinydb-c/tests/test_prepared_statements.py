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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_prep.db')
    if os.path.exists(db_file):
        try:
            os.remove(db_file)
        except Exception:
            pass

    commands = (
        "insert 1 user1 u1@test.com\n"
        "insert 2 user2 u2@test.com\n"
        "prepare stmt1 from select * from users where id = ?;\n"
        "execute stmt1 using 2;\n"
        ".exit\n"
    )
    
    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding='utf-8',
        errors='ignore'
    )
    
    stdout, stderr = process.communicate(input=commands)
    
    if process.returncode != 0:
        print(f"FAIL: Prepared statement test failed with return code {process.returncode}")
        print("STDOUT:", stdout)
        print("STDERR:", stderr)
        sys.exit(1)

    if "Statement 'stmt1' prepared." not in stdout or "(2, user2, u2@test.com)" not in stdout:
        print("FAIL: Expected prepared statement confirmation and query output.")
        print("STDOUT:", stdout)
        sys.exit(1)

    print("PASS: Prepared Statement Parameter Binding verified successfully!")

    if os.path.exists(db_file):
        try:
            os.remove(db_file)
        except Exception:
            pass

if __name__ == "__main__":
    run_test()

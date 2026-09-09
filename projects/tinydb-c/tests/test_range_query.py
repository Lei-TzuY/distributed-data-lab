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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_range.db')
    if os.path.exists(db_file):
        os.remove(db_file)

    commands = ""
    for i in range(1, 30):
        commands += f"insert {i} user{i} u{i}@test.com\n"
    commands += "select * from users where id >= 10 and id <= 15;\n.exit\n"
    
    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    stdout, stderr = process.communicate(input=commands)
    
    if process.returncode != 0:
        print(f"FAIL: Range query test failed with return code {process.returncode}")
        sys.exit(1)

    for val in range(10, 16):
        if f"({val}, user{val}, u{val}@test.com)" not in stdout:
            print(f"FAIL: Expected row {val} in range query result.")
            print(stdout)
            sys.exit(1)

    if "(9, user9, u9@test.com)" in stdout or "(16, user16, u16@test.com)" in stdout:
        print("FAIL: Range query returned out-of-range rows.")
        print(stdout)
        sys.exit(1)

    print("PASS: B+ Tree primary key range query (WHERE id >= A AND id <= B) verified!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

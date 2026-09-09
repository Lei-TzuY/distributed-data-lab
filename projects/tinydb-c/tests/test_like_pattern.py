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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_like.db')
    if os.path.exists(db_file):
        os.remove(db_file)

    commands = (
        "insert 1 alice alice@gmail.com\n"
        "insert 2 bob bob@yahoo.com\n"
        "insert 3 alex alex@gmail.com\n"
        "select * from users where username like 'al%';\n"
        "select * from users where email like '%@gmail.com';\n"
        "select * from users where username like 'b_b';\n"
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
        print(f"FAIL: LIKE test failed with return code {process.returncode}")
        sys.exit(1)

    if "(1, alice, alice@gmail.com)" not in stdout:
        print("FAIL: Expected (1, alice, alice@gmail.com) in stdout.")
        print(stdout)
        sys.exit(1)

    if "(3, alex, alex@gmail.com)" not in stdout:
        print("FAIL: Expected (3, alex, alex@gmail.com) in stdout for LIKE 'al%'.")
        print(stdout)
        sys.exit(1)

    if "(2, bob, bob@yahoo.com)" not in stdout:
        print("FAIL: Expected (2, bob, bob@yahoo.com) in stdout for LIKE 'b_b'.")
        print(stdout)
        sys.exit(1)

    print("PASS: SQL LIKE pattern matching ('%' and '_') verified successfully!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

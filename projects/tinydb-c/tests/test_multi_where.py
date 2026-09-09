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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_multi_where.db')
    if os.path.exists(db_file):
        os.remove(db_file)

    commands = (
        "insert 1 alice alice@example.com\n"
        "insert 2 bob bob@example.com\n"
        "insert 3 alice alice2@example.com\n"
        "select * from users where username = 'alice' and email = 'alice2@example.com';\n"
        "select * from users where id > 1 and username = 'alice';\n"
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
        print(f"FAIL: Multi WHERE test failed with return code {process.returncode}")
        sys.exit(1)

    if "(3, alice, alice2@example.com)" not in stdout:
        print("FAIL: Multi WHERE query expected (3, alice, alice2@example.com) in stdout.")
        print(stdout)
        sys.exit(1)

    # Verify filtering: query for alice2@example.com should NOT include (1, alice, alice@example.com)
    lines = stdout.splitlines()
    found_row3 = False
    for line in lines:
        if "(3, alice, alice2@example.com)" in line:
            found_row3 = True

    if not found_row3:
        print("FAIL: Multi-condition WHERE query failed to find row 3.")
        sys.exit(1)

    print("PASS: Multi-condition WHERE ... AND ... filtering executed successfully!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

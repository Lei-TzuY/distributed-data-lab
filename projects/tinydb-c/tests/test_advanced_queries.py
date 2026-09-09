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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_queries.db')
    if os.path.exists(db_file):
        os.remove(db_file)

    commands = (
        "insert 10 user1 dev@company.com\n"
        "insert 20 user2 ops@company.com\n"
        "insert 30 user3 dev@company.com\n"
        "select sum(id) from users;\n"
        "select avg(id) from users;\n"
        "select * from users where email = 'dev@company.com';\n"
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
        print(f"FAIL: Query test process failed with return code {process.returncode}")
        sys.exit(1)

    # SUM = 10 + 20 + 30 = 60
    if "60" not in stdout:
        print("FAIL: Expected SUM(id) = 60 in output.")
        print(stdout)
        sys.exit(1)

    # AVG = 60 / 3 = 20
    if "20" not in stdout:
        print("FAIL: Expected AVG(id) = 20 in output.")
        print(stdout)
        sys.exit(1)

    # WHERE email = 'dev@company.com' -> rows 10 and 30
    if "(10, user1, dev@company.com)" not in stdout or "(30, user30, dev@company.com)" not in stdout and "(30, user3, dev@company.com)" not in stdout:
        print("FAIL: Expected email filter results in output.")
        print(stdout)
        sys.exit(1)

    print("PASS: Advanced query aggregates SUM, AVG, and WHERE email filter work correctly!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

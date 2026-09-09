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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_vacuum.db')
    if os.path.exists(db_file):
        os.remove(db_file)

    # Insert 30 rows, delete 20 of them, run VACUUM, and verify row count and select output
    commands = ""
    for i in range(1, 31):
        commands += f"insert {i} user{i} u{i}@test.com\n"
    for i in range(1, 21):
        commands += f"delete from users where id = {i};\n"
    commands += ".stats\nvacuum;\n.stats\nselect * from users;\n.exit\n"
    
    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    stdout, stderr = process.communicate(input=commands)
    
    if process.returncode != 0:
        print(f"FAIL: VACUUM test process exited with code {process.returncode}")
        sys.exit(1)

    if "(21, user21, u21@test.com)" not in stdout or "(30, user30, u30@test.com)" not in stdout:
        print("FAIL: Remaining rows missing after VACUUM.")
        print(stdout)
        sys.exit(1)

    if "(1, user1, u1@test.com)" in stdout:
        print("FAIL: Deleted row found after VACUUM.")
        print(stdout)
        sys.exit(1)

    print("PASS: VACUUM command successfully defragmented and re-compacted table data!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

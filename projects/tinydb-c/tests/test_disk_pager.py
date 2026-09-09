import subprocess
import os
import sys

def run_test():
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
        print("Could not find the tinydb executable.")
        sys.exit(1)
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_pager.db')
    if os.path.exists(db_file):
        os.remove(db_file)
        
    # First process: insert data
    process1 = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    commands1 = "insert 1 user1 user1@example.com\n.exit\n"
    process1.communicate(input=commands1)
    
    # Second process: select data
    process2 = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    commands2 = "select\n.exit\n"
    stdout2, stderr2 = process2.communicate(input=commands2)
    
    if "(1, user1, user1@example.com)" not in stdout2:
        print("FAIL: Expected row 1 not found after reopening.")
        sys.exit(1)
        
    print("PASS: Disk Pager persists data correctly.")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

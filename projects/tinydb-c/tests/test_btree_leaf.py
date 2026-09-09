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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_btree.db')
    if os.path.exists(db_file):
        os.remove(db_file)
        
    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    commands = """insert 3 user3 user3@example.com
insert 1 user1 user1@example.com
insert 2 user2 user2@example.com
insert 2 user2_dup user2_dup@example.com
select
.exit
"""
    stdout, stderr = process.communicate(input=commands)
    
    if "Error: Duplicate key." not in stdout:
        print("FAIL: Expected duplicate key error.")
        sys.exit(1)
        
    idx1 = stdout.find("(1, user1, user1@example.com)")
    idx2 = stdout.find("(2, user2, user2@example.com)")
    idx3 = stdout.find("(3, user3, user3@example.com)")
    
    if idx1 == -1 or idx2 == -1 or idx3 == -1:
        print("FAIL: Missing rows in select output.")
        sys.exit(1)
        
    if not (idx1 < idx2 < idx3):
        print("FAIL: Rows are not sorted! B-Tree leaf insertion failed.")
        sys.exit(1)
        
    print("PASS: B-Tree Leaf Node maintains sorted order and prevents duplicates.")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

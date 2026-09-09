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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_split.db')
    if os.path.exists(db_file):
        os.remove(db_file)
        
    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    commands = ""
    for i in range(1, 16):
        commands += f"insert {i} user{i} person{i}@example.com\n"
    commands += ".btree\n.exit\n"
    
    stdout, stderr = process.communicate(input=commands)
    
    if "internal (size 1)" not in stdout:
        print("FAIL: Expected an internal node to be created.")
        print(stdout)
        sys.exit(1)
        
    if stdout.count("- leaf") < 2:
        print("FAIL: Expected at least two leaf nodes.")
        print(stdout)
        sys.exit(1)
        
    print("PASS: B-Tree internal node splitting logic functions properly!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

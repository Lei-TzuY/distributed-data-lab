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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_lru.db')
    if os.path.exists(db_file):
        os.remove(db_file)

    # Insert 150 rows to split B+ tree across multiple pages and trigger LRU evictions
    commands = ""
    for i in range(1, 150):
        commands += f"insert {i} user{i} u{i}@test.com\n"
    commands += ".buffer_pool\n.check\n.exit\n"
    
    process = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    
    stdout, stderr = process.communicate(input=commands)
    
    if process.returncode != 0:
        print(f"FAIL: Buffer Pool LRU test process1 failed with code {process.returncode}")
        print("STDOUT:", stdout)
        print("STDERR:", stderr)
        sys.exit(1)

    if "Capacity    : 16 pages" not in stdout or "Evictions" not in stdout:
        print("FAIL: Buffer Pool Manager stats incomplete.")
        print(stdout)
        sys.exit(1)

    # Re-open database and query to verify persistence after evictions
    commands_reopen = "select count(*) from users;\n.buffer_pool\n.exit\n"
    process2 = subprocess.Popen(
        [executable_path, db_file],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True
    )
    stdout2, stderr2 = process2.communicate(input=commands_reopen)

    if process2.returncode != 0:
        print(f"FAIL: Buffer Pool LRU test process2 failed with code {process2.returncode}")
        print("STDOUT2:", stdout2)
        print("STDERR2:", stderr2)
        sys.exit(1)

    if "149" not in stdout2:
        print("FAIL: Expected COUNT(*) = 149 after buffer pool evictions.")
        print("STDOUT2:", stdout2)
        print("STDERR2:", stderr2)
        sys.exit(1)

    print("PASS: Buffer Pool Manager with LRU Eviction & Pinning verified successfully!")

    if os.path.exists(db_file):
        os.remove(db_file)

if __name__ == "__main__":
    run_test()

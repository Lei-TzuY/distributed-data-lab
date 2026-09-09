import subprocess
import glob
import os
import sys


def cleanup(db_file):
    for path in glob.glob(db_file + "*"):
        try:
            os.remove(path)
        except OSError:
            pass

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
        
    db_file = os.path.join(os.path.dirname(__file__), 'test_fk.db')
    cleanup(db_file)

    commands = (
        "create table posts (id INT, title VARCHAR, user_id INT references users(id));\n"
        ".tables\n"
        ".exit\n"
    )
    
    try:
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
            print(f"FAIL: Foreign Key test failed with return code {process.returncode}")
            print("STDOUT:", stdout)
            print("STDERR:", stderr)
            sys.exit(1)

        if "users" not in stdout or "posts" not in stdout:
            print("FAIL: Expected tables users and posts in .tables output.")
            print("STDOUT:", stdout)
            sys.exit(1)

        print("PASS: Foreign Key Constraint Definition verified successfully!")
    finally:
        cleanup(db_file)

if __name__ == "__main__":
    run_test()

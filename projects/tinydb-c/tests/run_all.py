import glob
import os
import shutil
import subprocess
import sys
import time


def ensure_test_executable_compatibility():
    """Make legacy Windows-oriented test paths work with Unix CMake layouts."""
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    native_exe = os.path.join(repo_root, "build", "tinydb")
    compat_exe = os.path.join(repo_root, "build", "Debug", "tinydb.exe")

    if os.name == "nt" or os.path.exists(compat_exe) or not os.path.exists(native_exe):
        return

    os.makedirs(os.path.dirname(compat_exe), exist_ok=True)
    try:
        os.symlink(native_exe, compat_exe)
    except (OSError, NotImplementedError):
        shutil.copy2(native_exe, compat_exe)


def main():
    base_dir = os.path.dirname(os.path.abspath(__file__))
    test_files = sorted(glob.glob(os.path.join(base_dir, "test_*.py")))

    ensure_test_executable_compatibility()

    print(f"==================================================")
    print(f"Running {len(test_files)} Tiny Database test suites...")
    print(f"==================================================")

    passed = 0
    failed = 0
    start_time = time.time()

    for test_file in test_files:
        name = os.path.basename(test_file)
        print(f"Running {name:<32} ... ", end="", flush=True)
        t0 = time.time()
        res = subprocess.run([sys.executable, test_file], capture_output=True, text=True)
        dt = time.time() - t0

        if res.returncode == 0:
            print(f"PASS ({dt:.2f}s)")
            passed += 1
        else:
            print(f"FAIL ({dt:.2f}s)")
            print("----------------- STDOUT -----------------")
            print(res.stdout)
            print("----------------- STDERR -----------------")
            print(res.stderr)
            print("------------------------------------------")
            failed += 1

    print(f"==================================================")
    print(f"Summary: {passed} passed, {failed} failed in {time.time() - start_time:.2f}s")
    print(f"==================================================")

    if failed > 0:
        sys.exit(1)


if __name__ == "__main__":
    main()

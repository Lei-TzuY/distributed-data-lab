import os
import subprocess
import sys


def run_test():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    script = os.path.join(repo_root, "tools", "recovery_stress.py")

    result = subprocess.run(
        [
            sys.executable,
            script,
            "--iterations",
            "3",
            "--rows-per-round",
            "2",
            "--seed",
            "20260826",
            "--settle-seconds",
            "0.35",
        ],
        capture_output=True,
        text=True,
    )

    if result.returncode != 0:
        print("FAIL: recovery stress runner exited unsuccessfully")
        print("----------------- STDOUT -----------------")
        print(result.stdout)
        print("----------------- STDERR -----------------")
        print(result.stderr)
        sys.exit(1)

    if "RECOVERY_STRESS_OK" not in result.stdout:
        print("FAIL: recovery stress runner did not report success")
        print(result.stdout)
        sys.exit(1)

    if "committed=6" not in result.stdout or "rejected_uncommitted=6" not in result.stdout:
        print("FAIL: recovery stress runner reported unexpected row totals")
        print(result.stdout)
        sys.exit(1)

    if result.stdout.count("integrity=ok") != 3:
        print("FAIL: expected one integrity checkpoint per stress round")
        print(result.stdout)
        sys.exit(1)

    print("PASS: repeated committed/uncommitted crash recovery remains atomic and structurally valid.")


if __name__ == "__main__":
    run_test()

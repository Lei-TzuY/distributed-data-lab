#!/usr/bin/env python3
"""Repeated crash/recovery stress testing for TinyDB.

The runner keeps one database alive across many crash cycles and verifies two
important durability invariants after every cycle:

1. transactions that returned successfully from COMMIT survive an immediate
   hard process exit without a graceful database close/checkpoint;
2. rows written inside a transaction that never reached COMMIT never leak.

The committed phase uses tinydb_committed_crash_probe instead of a timing-only
sleep. This makes the crash point deterministic: pager_commit() has durably
synced the WAL and returned before the helper calls _exit(). The uncommitted
phase still kills the REPL while a transaction is active.

PRAGMA integrity_check runs after every recovery so B+ tree/page damage is
detected immediately instead of only checking logical row visibility.
"""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def find_executable(repo_root: Path) -> Path | None:
    candidates = [
        repo_root / "build" / "Debug" / "tinydb.exe",
        repo_root / "build" / "Release" / "tinydb.exe",
        repo_root / "build" / "tinydb.exe",
        repo_root / "build" / "tinydb",
    ]
    return next((path for path in candidates if path.exists()), None)


def find_committed_probe(repo_root: Path) -> Path | None:
    candidates = [
        repo_root / "build" / "Debug" / "tinydb_committed_crash_probe.exe",
        repo_root / "build" / "Release" / "tinydb_committed_crash_probe.exe",
        repo_root / "build" / "tinydb_committed_crash_probe.exe",
        repo_root / "build" / "tinydb_committed_crash_probe",
    ]
    return next((path for path in candidates if path.exists()), None)


def generated_paths(db_path: Path) -> list[Path]:
    """Return files TinyDB can create for the no-secondary-index workload."""
    return [
        db_path,
        Path(str(db_path) + ".wal"),
        Path(str(db_path) + ".free"),
        Path(str(db_path) + ".catalog"),
        Path(str(db_path) + ".catalog.wal"),
        Path(str(db_path) + ".username.idx"),
        Path(str(db_path) + ".username.idx.wal"),
    ]


def cleanup_database(db_path: Path) -> None:
    for path in generated_paths(db_path):
        try:
            path.unlink()
        except FileNotFoundError:
            pass


def run_commands(executable: Path, db_path: Path, commands: str) -> tuple[int, str, str]:
    process = subprocess.Popen(
        [str(executable), str(db_path)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    stdout, stderr = process.communicate(input=commands)
    return process.returncode, stdout, stderr


def run_committed_crash(
    probe: Path,
    db_path: Path,
    start_id: int,
    row_count: int,
) -> None:
    result = subprocess.run(
        [str(probe), str(db_path), str(start_id), str(row_count)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        timeout=30,
    )
    if result.returncode != 0:
        raise RuntimeError(
            "committed crash probe failed "
            f"(rc={result.returncode})\nstdout:\n{result.stdout}\nstderr:\n{result.stderr}"
        )


def crash_after_commands(
    executable: Path,
    db_path: Path,
    commands: str,
    settle_seconds: float,
) -> tuple[str, str]:
    process = subprocess.Popen(
        [str(executable), str(db_path)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    assert process.stdin is not None
    process.stdin.write(commands)
    process.stdin.flush()
    time.sleep(settle_seconds)

    if process.poll() is not None:
        stdout, stderr = process.communicate()
        raise RuntimeError(
            f"tinydb exited before crash injection (rc={process.returncode})\n"
            f"stdout:\n{stdout}\nstderr:\n{stderr}"
        )

    process.kill()
    stdout, stderr = process.communicate(timeout=10)
    return stdout, stderr


def sql_row(row_id: int, username: str, email: str) -> str:
    return f"({row_id}, {username}, {email})"


def insert_sql(row_id: int, username: str, email: str) -> str:
    return f"INSERT INTO users VALUES ({row_id}, '{username}', '{email}');\n"


def verify_state(
    executable: Path,
    db_path: Path,
    committed_rows: dict[int, tuple[str, str]],
    forbidden_rows: dict[int, tuple[str, str]],
) -> None:
    rc, stdout, stderr = run_commands(
        executable,
        db_path,
        "SELECT * FROM users;\nPRAGMA integrity_check;\n.exit\n",
    )
    if rc != 0:
        raise RuntimeError(
            f"verification process failed (rc={rc})\nstdout:\n{stdout}\nstderr:\n{stderr}"
        )
    if "ok" not in stdout:
        raise RuntimeError(f"PRAGMA integrity_check did not return ok:\n{stdout}")

    for row_id, (username, email) in committed_rows.items():
        expected = sql_row(row_id, username, email)
        if expected not in stdout:
            raise RuntimeError(f"committed row disappeared after recovery: {expected}")

    for row_id, (username, email) in forbidden_rows.items():
        forbidden = sql_row(row_id, username, email)
        if forbidden in stdout:
            raise RuntimeError(f"uncommitted row leaked after crash: {forbidden}")


def make_row(rng: random.Random, row_id: int, prefix: str) -> tuple[str, str]:
    token = rng.randrange(1_000_000_000)
    username = f"{prefix}_{row_id}_{token:x}"
    email = f"{prefix}{row_id}_{token:x}@stress.test"
    return username, email


def committed_probe_row(row_id: int) -> tuple[str, str]:
    return f"durable_{row_id}", f"durable{row_id}@crash.test"


def run_stress(
    executable: Path,
    committed_probe: Path,
    db_path: Path,
    iterations: int,
    rows_per_round: int,
    seed: int,
    settle_seconds: float,
) -> None:
    rng = random.Random(seed)
    committed_rows: dict[int, tuple[str, str]] = {}
    forbidden_rows: dict[int, tuple[str, str]] = {}
    next_committed_id = 1
    next_ghost_id = 1_000_000

    for iteration in range(1, iterations + 1):
        # Phase A: the helper returns from COMMIT, verifies a non-empty durable
        # WAL exists, then immediately _exit()s without tinydb_close(). Reopen
        # must recover every row from that committed WAL transaction.
        round_start_id = next_committed_id
        round_committed: dict[int, tuple[str, str]] = {}
        for _ in range(rows_per_round):
            row_id = next_committed_id
            next_committed_id += 1
            round_committed[row_id] = committed_probe_row(row_id)

        run_committed_crash(
            committed_probe,
            db_path,
            round_start_id,
            rows_per_round,
        )
        committed_rows.update(round_committed)
        verify_state(executable, db_path, committed_rows, forbidden_rows)

        # Phase B: write rows in an active transaction, then kill before COMMIT.
        # None of these rows may become visible after reopening the database.
        uncommitted_commands = "BEGIN;\n"
        round_forbidden: dict[int, tuple[str, str]] = {}
        for _ in range(rows_per_round):
            row_id = next_ghost_id
            next_ghost_id += 1
            username, email = make_row(rng, row_id, "ghost")
            round_forbidden[row_id] = (username, email)
            uncommitted_commands += insert_sql(row_id, username, email)

        crash_after_commands(executable, db_path, uncommitted_commands, settle_seconds)
        forbidden_rows.update(round_forbidden)
        verify_state(executable, db_path, committed_rows, forbidden_rows)

        print(
            f"round={iteration}/{iterations} committed={len(committed_rows)} "
            f"forbidden={len(forbidden_rows)} integrity=ok"
        )

    print(
        "RECOVERY_STRESS_OK "
        f"iterations={iterations} rows_per_round={rows_per_round} "
        f"committed={len(committed_rows)} rejected_uncommitted={len(forbidden_rows)} "
        f"seed={seed}"
    )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--iterations", type=int, default=20)
    parser.add_argument("--rows-per-round", type=int, default=2)
    parser.add_argument("--seed", type=int, default=1337)
    parser.add_argument(
        "--settle-seconds",
        type=float,
        default=0.35,
        help="time to let uncommitted commands reach the engine before injecting SIGKILL/TerminateProcess",
    )
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--committed-probe", type=Path)
    parser.add_argument("--db", type=Path)
    parser.add_argument("--keep-db", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.iterations <= 0 or args.rows_per_round <= 0:
        print("iterations and rows-per-round must be positive", file=sys.stderr)
        return 2
    if args.settle_seconds <= 0:
        print("settle-seconds must be positive", file=sys.stderr)
        return 2

    repo_root = Path(__file__).resolve().parents[1]
    executable = args.executable or find_executable(repo_root)
    committed_probe = args.committed_probe or find_committed_probe(repo_root)
    if executable is None or not executable.exists():
        print("Could not find the tinydb executable; build the project first.", file=sys.stderr)
        return 2
    if committed_probe is None or not committed_probe.exists():
        print(
            "Could not find tinydb_committed_crash_probe; build the project first.",
            file=sys.stderr,
        )
        return 2

    temp_dir: tempfile.TemporaryDirectory[str] | None = None
    if args.db is None:
        temp_dir = tempfile.TemporaryDirectory(prefix="tinydb-recovery-stress-")
        db_path = Path(temp_dir.name) / "stress.db"
    else:
        db_path = args.db.resolve()
        db_path.parent.mkdir(parents=True, exist_ok=True)
        if any(path.exists() for path in generated_paths(db_path)):
            print(
                f"Refusing to overwrite existing TinyDB files for {db_path}; choose a fresh --db path.",
                file=sys.stderr,
            )
            return 2

    try:
        cleanup_database(db_path)
        run_stress(
            executable=executable.resolve(),
            committed_probe=committed_probe.resolve(),
            db_path=db_path,
            iterations=args.iterations,
            rows_per_round=args.rows_per_round,
            seed=args.seed,
            settle_seconds=args.settle_seconds,
        )
        return 0
    except (RuntimeError, subprocess.SubprocessError) as exc:
        print(f"RECOVERY_STRESS_FAIL: {exc}", file=sys.stderr)
        return 1
    finally:
        if not args.keep_db:
            cleanup_database(db_path)
        if temp_dir is not None:
            temp_dir.cleanup()


if __name__ == "__main__":
    raise SystemExit(main())

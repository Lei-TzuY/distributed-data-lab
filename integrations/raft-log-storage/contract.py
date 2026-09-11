#!/usr/bin/env python3
"""Execute the bounded Raft committed-prefix -> durable log-engine contract."""

from __future__ import annotations

import json
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Any

from distlab.kv import Delete, Put, ReplicatedKV
from distlab.raft import LogEntry, RaftCluster, RaftRole
from distlab.replication import LeaderReplicator
from distlab.simulator import Simulator

ROOT = Path(__file__).resolve().parents[2]
DATABASE_PROJECT = ROOT / "projects" / "database-design-lab"
DEFAULT_DB_BINARY = DATABASE_PROJECT / "target" / "debug" / (
    "db-lab.exe" if os.name == "nt" else "db-lab"
)

CONTRACT_NAME = "raft-committed-prefix-to-append-log-v1"
PARTICIPANT_SOURCE_SHAS = {
    "database-design-lab": "fba592f25225621f83931662565120c43ede1885",
    "distributed-systems-lab": "f1638f9121ee550b688a3f6bf9ab03df369139dc",
}


def _hex(value: str) -> str:
    return value.encode("utf-8").hex()


def _build_committed_prefix() -> tuple[
    tuple[Put | Delete, ...], dict[str, str], dict[str, int]
]:
    commands: tuple[Put | Delete, ...] = (
        Put("account/alice", "100"),
        Put("account/bob", "50"),
        Put("account/alice", "125"),
        Delete("account/bob"),
        Put("region/臺北", "active"),
    )
    log = tuple(LogEntry(term=3, command=command) for command in commands)

    simulator = Simulator()
    simulator.persistent_state["n1"]["current_term"] = 2
    simulator.persistent_state["n1"]["log"] = log
    cluster = RaftCluster(simulator, ("n1", "n2", "n3"))
    leader = cluster.node("n1")
    leader.start_election()
    simulator.run()
    if leader.role is not RaftRole.LEADER or leader.current_term != 3:
        raise AssertionError("deterministic election did not produce n1 leader in term 3")

    replicator = LeaderReplicator(leader)
    if not replicator.replicate("n2"):
        raise AssertionError("majority replication did not commit the command prefix")

    state_machine = ReplicatedKV(cluster)
    leader_applied = state_machine.apply_committed("n1")

    # The first successful AppendEntries can create the majority commit on the
    # leader. A second probe carries that commit index to the follower.
    if not replicator.replicate("n2"):
        raise AssertionError("commit-index propagation to n2 failed")
    follower_applied = state_machine.apply_committed("n2")

    if tuple(item.entry.command for item in leader_applied) != commands:
        raise AssertionError("leader applied sequence diverges from committed Raft prefix")
    if tuple(item.entry.command for item in follower_applied) != commands:
        raise AssertionError("follower applied sequence diverges from committed Raft prefix")
    state_machine.assert_replica_consistency()
    if state_machine.snapshot("n1") != state_machine.snapshot("n2"):
        raise AssertionError("replicas did not converge on the committed prefix")

    trace_counts = {
        kind: sum(record.kind == kind for record in simulator.trace)
        for kind in ("raft-commit-advance", "raft-state-machine-apply", "kv-apply")
    }
    if trace_counts["raft-commit-advance"] < 1:
        raise AssertionError("no real Raft majority commit was observed")

    return commands, state_machine.snapshot("n1"), trace_counts


def _write_workload(path: Path, steps: list[dict[str, Any]]) -> None:
    path.write_text(
        json.dumps(
            {"format_version": 1, "seed": None, "steps": steps},
            ensure_ascii=False,
            indent=2,
            sort_keys=True,
        )
        + "\n",
        encoding="utf-8",
    )


def _command_steps(commands: tuple[Put | Delete, ...]) -> list[dict[str, str]]:
    steps: list[dict[str, str]] = []
    for command in commands:
        if isinstance(command, Put):
            steps.append({"op": "put", "key": _hex(command.key), "value": _hex(command.value)})
        elif isinstance(command, Delete):
            steps.append({"op": "delete", "key": _hex(command.key)})
        else:  # pragma: no cover - fail closed if the imported command model expands
            raise TypeError(f"unsupported committed command: {command!r}")
    return steps


def _run_database(binary: Path, state_path: Path, workload: Path) -> dict[str, Any]:
    completed = subprocess.run(
        [
            str(binary),
            "run",
            "--engine",
            "log",
            "--path",
            str(state_path),
            str(workload),
        ],
        cwd=DATABASE_PROJECT,
        check=False,
        capture_output=True,
        text=True,
    )
    if completed.returncode != 0:
        raise RuntimeError(
            "database-design-lab db-lab process failed: "
            f"exit={completed.returncode} stderr={completed.stderr.strip()!r}"
        )
    try:
        report = json.loads(completed.stdout)
    except json.JSONDecodeError as exc:
        raise RuntimeError("db-lab emitted non-JSON output") from exc
    if not isinstance(report, dict):
        raise TypeError("db-lab report must be a JSON object")
    return report


def execute_contract(binary: Path | None = None) -> dict[str, Any]:
    """Run both imported projects and return canonical, replayable evidence."""
    db_binary = (binary or Path(os.environ.get("DB_LAB_BIN", DEFAULT_DB_BINARY))).resolve()
    if not db_binary.is_file():
        raise FileNotFoundError(
            f"database CLI not found at {db_binary}; build db-cli or set DB_LAB_BIN"
        )

    commands, replicated_state, trace_counts = _build_committed_prefix()
    observed_keys = sorted({command.key for command in commands})

    with tempfile.TemporaryDirectory(prefix="raft-log-contract-") as raw_directory:
        directory = Path(raw_directory)
        state_path = directory / "state.log"
        apply_workload = directory / "apply.json"
        read_workload = directory / "read-after-process-reopen.json"
        _write_workload(apply_workload, _command_steps(commands))
        apply_report = _run_database(db_binary, state_path, apply_workload)

        # A separate db-lab process reopens the durable append log. This is a
        # real process and storage lifecycle boundary, not an in-memory oracle.
        _write_workload(
            read_workload,
            [{"op": "get", "key": _hex(key)} for key in observed_keys],
        )
        reopen_report = _run_database(db_binary, state_path, read_workload)

    outcomes = reopen_report.get("outcomes")
    if not isinstance(outcomes, list) or len(outcomes) != len(observed_keys):
        raise AssertionError("database reopen report has the wrong number of reads")

    database_state: dict[str, str | None] = {}
    for key, outcome in zip(observed_keys, outcomes, strict=True):
        if not isinstance(outcome, dict) or outcome.get("result") != "get":
            raise AssertionError("database reopen report contains a non-GET outcome")
        value = outcome.get("value")
        if value is not None and not isinstance(value, str):
            raise AssertionError("database GET value must be hexadecimal text or null")
        database_state[key] = value

    expected_state = {key: _hex(value) for key, value in replicated_state.items()}
    expected_reads = {key: expected_state.get(key) for key in observed_keys}
    if database_state != expected_reads:
        raise AssertionError(
            "durable database state diverges from the Raft-applied state: "
            f"expected={expected_reads!r} observed={database_state!r}"
        )
    if apply_report.get("steps_executed") != len(commands):
        raise AssertionError("database did not execute the complete committed prefix")

    return {
        "format_version": 1,
        "contract": CONTRACT_NAME,
        "participant_source_shas": PARTICIPANT_SOURCE_SHAS,
        "raft": {
            "leader": "n1",
            "term": 3,
            "committed_index": len(commands),
            "applied_indices": list(range(1, len(commands) + 1)),
            "replicas_checked": ["n1", "n2"],
            "trace_counts": trace_counts,
        },
        "database": {
            "engine": apply_report.get("engine"),
            "process_reopen_verified": True,
            "reads": database_state,
        },
        "result": "equivalent",
    }


def main() -> None:
    print(json.dumps(execute_contract(), ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()

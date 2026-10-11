#!/usr/bin/env python3
"""Execute the bounded Raft committed-prefix -> durable log-engine contract."""

from __future__ import annotations

import hashlib
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
from distlab.state_machine import AppliedEntry

ROOT = Path(__file__).resolve().parents[2]
DATABASE_PROJECT = ROOT / "projects" / "database-design-lab"
DEFAULT_DB_BINARY = (
    DATABASE_PROJECT
    / "target"
    / "debug"
    / ("db-lab.exe" if os.name == "nt" else "db-lab")
)

CONTRACT_NAME = "raft-committed-prefix-to-append-log-v1"
PARTICIPANT_SOURCE_SHAS = {
    "database-design-lab": "fba592f25225621f83931662565120c43ede1885",
    "distributed-systems-lab": "f1638f9121ee550b688a3f6bf9ab03df369139dc",
}


def _hex(value: str) -> str:
    return value.encode("utf-8").hex()


def _build_committed_prefix() -> tuple[
    tuple[AppliedEntry, ...], dict[str, str], dict[str, int]
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
        raise AssertionError(
            "deterministic election did not produce n1 leader in term 3"
        )

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
        raise AssertionError(
            "leader applied sequence diverges from committed Raft prefix"
        )
    if tuple(item.entry.command for item in follower_applied) != commands:
        raise AssertionError(
            "follower applied sequence diverges from committed Raft prefix"
        )
    state_machine.assert_replica_consistency()
    if state_machine.snapshot("n1") != state_machine.snapshot("n2"):
        raise AssertionError("replicas did not converge on the committed prefix")

    trace_counts = {
        kind: sum(record.kind == kind for record in simulator.trace)
        for kind in ("raft-commit-advance", "raft-state-machine-apply", "kv-apply")
    }
    if trace_counts["raft-commit-advance"] < 1:
        raise AssertionError("no real Raft majority commit was observed")

    return leader_applied, state_machine.snapshot("n1"), trace_counts


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
            steps.append(
                {"op": "put", "key": _hex(command.key), "value": _hex(command.value)}
            )
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


# UTF-8 application keys cannot collide with this reserved binary key.
PROJECTION_CHECKPOINT_KEY = b"\xffraft-log-projection-v1".hex()


def _prefix_digests(
    applied: tuple[AppliedEntry, ...],
) -> tuple[list[str], list[dict[str, str]]]:
    digest = hashlib.sha256(b"raft-log-projection-v1\n")
    digests = [digest.hexdigest()]
    steps: list[dict[str, str]] = []
    for index, record in enumerate(applied, start=1):
        if not isinstance(record, AppliedEntry):
            raise TypeError("projection requires state-machine AppliedEntry records")
        if record.index != index:
            raise ValueError(
                "projection requires the complete applied prefix from index one"
            )
        step = _command_steps((record.entry.command,))[0]
        payload = json.dumps(
            {"index": index, "term": record.entry.term, "step": step},
            sort_keys=True,
            separators=(",", ":"),
        ).encode("ascii")
        digest.update(len(payload).to_bytes(8, "big"))
        digest.update(payload)
        digests.append(digest.hexdigest())
        steps.append(step)
    return digests, steps


def project_applied_prefix(
    binary: Path, state_path: Path, applied: tuple[AppliedEntry, ...]
) -> dict[str, Any]:
    """Resume a single-writer, idempotent Put/Delete projection after interruption.

    The store must be fresh or exclusively owned by this projector. A durable
    intent binds partial data writes to their source history. Completion is
    acknowledged only after both the data and its checkpoint have been synced
    by the real database process. This is a post-consensus projection boundary,
    not an atomic Raft-acknowledgement/storage operation.
    """
    binary = binary.resolve()
    state_path = state_path.resolve()
    digests, steps = _prefix_digests(applied)
    with tempfile.TemporaryDirectory(prefix="raft-log-projection-") as raw_directory:
        directory = Path(raw_directory)
        workload = directory / "read-checkpoint.json"
        _write_workload(workload, [{"op": "get", "key": PROJECTION_CHECKPOINT_KEY}])
        report = _run_database(binary, state_path, workload)
        outcomes = report.get("outcomes")
        if (
            report.get("engine") != "append-log-v1"
            or not isinstance(outcomes, list)
            or len(outcomes) != 1
            or outcomes[0].get("result") != "get"
        ):
            raise AssertionError("invalid append-log checkpoint read")
        value = outcomes[0].get("value")
        checkpoint = (
            {
                "format_version": 1,
                "applied_index": 0,
                "applied_digest": digests[0],
                "pending": None,
            }
            if value is None
            else json.loads(bytes.fromhex(value).decode("utf-8"))
        )
        if (
            not isinstance(checkpoint, dict)
            or set(checkpoint)
            != {"format_version", "applied_index", "applied_digest", "pending"}
            or checkpoint["format_version"] != 1
        ):
            raise ValueError("invalid projection checkpoint")
        previous = checkpoint["applied_index"]
        if type(previous) is not int or not 0 <= previous <= len(applied):
            raise ValueError("source applied prefix is behind its durable checkpoint")
        if checkpoint["applied_digest"] != digests[previous]:
            raise ValueError("source history conflicts with its durable checkpoint")
        pending = checkpoint["pending"]
        if pending is not None:
            if (
                not isinstance(pending, dict)
                or set(pending) != {"index", "digest"}
                or type(pending["index"]) is not int
                or not previous < pending["index"] <= len(applied)
            ):
                raise ValueError("source applied prefix is behind a pending projection")
            if pending["digest"] != digests[pending["index"]]:
                raise ValueError("source history conflicts with a pending projection")

        result = {
            "engine": report["engine"],
            "previous_index": previous,
            "applied_index": len(applied),
            "commands_written": len(applied) - previous,
            "applied_digest": digests[-1],
            "recovered_intent": pending is not None,
        }
        if previous == len(applied):
            return result

        def save_checkpoint(name: str, marker: dict[str, Any]) -> None:
            path = directory / name
            encoded = json.dumps(marker, sort_keys=True, separators=(",", ":"))
            _write_workload(
                path,
                [
                    {
                        "op": "put",
                        "key": PROJECTION_CHECKPOINT_KEY,
                        "value": encoded.encode("utf-8").hex(),
                    }
                ],
            )
            saved = _run_database(binary, state_path, path)
            if saved.get("steps_executed") != 1:
                raise AssertionError(
                    "database did not persist the projection checkpoint"
                )

        save_checkpoint(
            "write-intent.json",
            {
                **checkpoint,
                "pending": {"index": len(applied), "digest": digests[-1]},
            },
        )
        workload = directory / "apply-suffix.json"
        _write_workload(workload, steps[previous:])
        applied_report = _run_database(binary, state_path, workload)
        if applied_report.get("steps_executed") != len(applied) - previous:
            raise AssertionError("database did not execute the complete applied suffix")
        save_checkpoint(
            "complete-checkpoint.json",
            {
                "format_version": 1,
                "applied_index": len(applied),
                "applied_digest": digests[-1],
                "pending": None,
            },
        )
        return result


def execute_contract(binary: Path | None = None) -> dict[str, Any]:
    """Run both imported projects and return canonical, replayable evidence."""
    db_binary = (
        binary or Path(os.environ.get("DB_LAB_BIN", DEFAULT_DB_BINARY))
    ).resolve()
    if not db_binary.is_file():
        raise FileNotFoundError(
            f"database CLI not found at {db_binary}; build db-cli or set DB_LAB_BIN"
        )

    applied, replicated_state, trace_counts = _build_committed_prefix()
    commands = tuple(record.entry.command for record in applied)
    observed_keys = sorted({command.key for command in commands})

    with tempfile.TemporaryDirectory(prefix="raft-log-contract-") as raw_directory:
        directory = Path(raw_directory)
        state_path = directory / "state.log"
        read_workload = directory / "read-after-process-reopen.json"
        projection = project_applied_prefix(db_binary, state_path, applied)
        projected_bytes = state_path.read_bytes()
        replay = project_applied_prefix(db_binary, state_path, applied)
        if (
            replay["commands_written"] != 0
            or state_path.read_bytes() != projected_bytes
        ):
            raise AssertionError(
                "unchanged applied-prefix replay mutated durable storage"
            )

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
    if projection["commands_written"] != len(commands):
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
            "engine": projection["engine"],
            "projection": projection,
            "unchanged_replay_verified": True,
            "process_reopen_verified": True,
            "reads": database_state,
        },
        "result": "equivalent",
    }


def main() -> None:
    print(json.dumps(execute_contract(), ensure_ascii=False, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()

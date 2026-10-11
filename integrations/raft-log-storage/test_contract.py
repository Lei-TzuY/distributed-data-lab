#!/usr/bin/env python3

from __future__ import annotations

import json
import os
import tempfile
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

import contract
from contract import CONTRACT_NAME, PARTICIPANT_SOURCE_SHAS, execute_contract
from distlab.kv import Put


class RaftLogStorageContractTest(unittest.TestCase):
    def test_committed_prefix_survives_database_process_reopen(self) -> None:
        binary = Path(os.environ["DB_LAB_BIN"])
        evidence = execute_contract(binary)

        self.assertEqual(evidence["contract"], CONTRACT_NAME)
        self.assertEqual(evidence["participant_source_shas"], PARTICIPANT_SOURCE_SHAS)
        self.assertEqual(evidence["result"], "equivalent")
        self.assertEqual(evidence["raft"]["committed_index"], 5)
        self.assertEqual(evidence["raft"]["applied_indices"], [1, 2, 3, 4, 5])
        self.assertEqual(evidence["raft"]["replicas_checked"], ["n1", "n2"])
        self.assertGreaterEqual(
            evidence["raft"]["trace_counts"]["raft-commit-advance"], 1
        )
        self.assertTrue(evidence["database"]["process_reopen_verified"])
        self.assertEqual(evidence["database"]["engine"], "append-log-v1")
        self.assertEqual(evidence["database"]["projection"]["applied_index"], 5)
        self.assertTrue(evidence["database"]["unchanged_replay_verified"])
        self.assertEqual(
            evidence["database"]["reads"],
            {
                "account/alice": "313235",
                "account/bob": None,
                "region/臺北": "616374697665",
            },
        )


class RaftLogStorageProjectionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.binary = Path(os.environ["DB_LAB_BIN"])

    def reads(self, directory: Path, state_path: Path) -> dict[str, str | None]:
        keys = ["account/alice", "account/bob", "region/臺北"]
        workload = directory / "observe.json"
        contract._write_workload(
            workload, [{"op": "get", "key": contract._hex(key)} for key in keys]
        )
        report = contract._run_database(self.binary, state_path, workload)
        return {
            key: outcome["value"]
            for key, outcome in zip(keys, report["outcomes"], strict=True)
        }

    def checkpoint(self, directory: Path, state_path: Path) -> dict[str, object]:
        workload = directory / "observe-checkpoint.json"
        contract._write_workload(
            workload, [{"op": "get", "key": contract.PROJECTION_CHECKPOINT_KEY}]
        )
        report = contract._run_database(self.binary, state_path, workload)
        return json.loads(bytes.fromhex(report["outcomes"][0]["value"]).decode("utf-8"))

    def test_incremental_projection_resumes_and_replay_is_a_noop(self) -> None:
        project = contract.project_applied_prefix
        applied, _, _ = contract._build_committed_prefix()
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            state_path = directory / "state.log"
            first = project(self.binary, state_path, applied[:3])
            self.assertEqual(first["applied_index"], 3)
            second = project(self.binary, state_path, tuple(applied))
            self.assertEqual(second["previous_index"], 3)
            self.assertEqual(second["commands_written"], 2)
            self.assertEqual(second["applied_index"], 5)
            durable_bytes = state_path.read_bytes()
            replay = project(self.binary, state_path, tuple(applied))
            self.assertEqual(replay["commands_written"], 0)
            self.assertEqual(state_path.read_bytes(), durable_bytes)
            self.assertEqual(self.reads(directory, state_path), EXPECTED_READS)

    def test_interruption_preserves_intent_and_rejects_an_older_prefix(self) -> None:
        project = contract.project_applied_prefix
        applied, _, _ = contract._build_committed_prefix()
        run_database = contract._run_database
        for phase in ("before", "after"):
            with self.subTest(phase=phase), tempfile.TemporaryDirectory() as raw:
                directory = Path(raw)
                state_path = directory / "state.log"
                project(self.binary, state_path, applied[:3])

                def interrupt_user_write(binary, path, workload, phase=phase):
                    steps = json.loads(workload.read_text(encoding="utf-8"))["steps"]
                    if any(
                        step["op"] in {"put", "delete"}
                        and step["key"] != contract.PROJECTION_CHECKPOINT_KEY
                        for step in steps
                    ):
                        if phase == "after":
                            run_database(binary, path, workload)
                        raise InterruptedError(
                            "projector interrupted at the data boundary"
                        )
                    return run_database(binary, path, workload)

                with (
                    patch.object(
                        contract, "_run_database", side_effect=interrupt_user_write
                    ),
                    self.assertRaises(InterruptedError),
                ):
                    project(self.binary, state_path, applied)
                marker = self.checkpoint(directory, state_path)
                self.assertEqual(marker["applied_index"], 3)
                self.assertEqual(marker["pending"]["index"], 5)
                if phase == "after":
                    self.assertEqual(self.reads(directory, state_path), EXPECTED_READS)
                else:
                    self.assertEqual(
                        self.reads(directory, state_path),
                        {
                            "account/alice": "313235",
                            "account/bob": "3530",
                            "region/臺北": None,
                        },
                    )
                interrupted_bytes = state_path.read_bytes()
                with self.assertRaises(ValueError):
                    project(self.binary, state_path, applied[:3])
                self.assertEqual(state_path.read_bytes(), interrupted_bytes)
                recovered = project(self.binary, state_path, tuple(applied))
                self.assertTrue(recovered["recovered_intent"])
                self.assertEqual(recovered["commands_written"], 2)
                self.assertEqual(self.reads(directory, state_path), EXPECTED_READS)
                marker = self.checkpoint(directory, state_path)
                self.assertEqual(marker["applied_index"], 5)
                self.assertIsNone(marker["pending"])

    def test_conflicting_or_older_applied_history_fails_before_writes(self) -> None:
        project = contract.project_applied_prefix
        applied, _, _ = contract._build_committed_prefix()
        # The overwritten first PUT changes history without changing final KV state.
        changed = replace(
            applied[0],
            entry=replace(applied[0].entry, command=Put("account/alice", "999")),
        )
        with tempfile.TemporaryDirectory() as raw:
            directory = Path(raw)
            state_path = directory / "state.log"
            project(self.binary, state_path, applied)
            original_bytes = state_path.read_bytes()
            for prefix in (applied[:3], (changed, *applied[1:])):
                with self.subTest(prefix=prefix), self.assertRaises(ValueError):
                    project(self.binary, state_path, prefix)
                self.assertEqual(state_path.read_bytes(), original_bytes)

    def test_unapplied_log_entries_never_reach_storage(self) -> None:
        project = contract.project_applied_prefix
        applied, _, _ = contract._build_committed_prefix()
        with tempfile.TemporaryDirectory() as raw:
            state_path = Path(raw) / "must-not-exist.log"
            with self.assertRaises(TypeError):
                project(
                    self.binary, state_path, tuple(record.entry for record in applied)
                )
            self.assertFalse(state_path.exists())


EXPECTED_READS = {
    "account/alice": "313235",
    "account/bob": None,
    "region/臺北": "616374697665",
}


if __name__ == "__main__":
    unittest.main()

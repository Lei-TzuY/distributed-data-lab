#!/usr/bin/env python3

from __future__ import annotations

import os
import unittest
from pathlib import Path

from contract import CONTRACT_NAME, PARTICIPANT_SOURCE_SHAS, execute_contract


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
        self.assertGreaterEqual(evidence["raft"]["trace_counts"]["raft-commit-advance"], 1)
        self.assertTrue(evidence["database"]["process_reopen_verified"])
        self.assertEqual(evidence["database"]["engine"], "append-log-v1")
        self.assertEqual(
            evidence["database"]["reads"],
            {
                "account/alice": "313235",
                "account/bob": None,
                "region/臺北": "616374697665",
            },
        )


if __name__ == "__main__":
    unittest.main()

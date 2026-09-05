#!/usr/bin/env python3
"""Validate distributed-data-lab migration manifest invariants."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "projects" / "manifest.json"
SHA40 = re.compile(r"^[0-9a-f]{40}$")
ALLOWED_STATUS = {
    "pre-flight",
    "ready-for-import",
    "hold",
    "imported-verified",
    "integration-verified",
}
REQUIRED_FIELDS = {
    "name",
    "source_repository",
    "target_path",
    "layer",
    "status",
    "observed_main_sha",
    "blocker",
}


def fail(message: str) -> None:
    print(f"manifest validation failed: {message}", file=sys.stderr)
    raise SystemExit(1)


def valid_sha(value: object) -> bool:
    return isinstance(value, str) and SHA40.fullmatch(value) is not None


def main() -> None:
    try:
        data = json.loads(MANIFEST.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read {MANIFEST.relative_to(ROOT)}: {exc}")

    if data.get("schema_version") != 1:
        fail("schema_version must be 1")
    if data.get("umbrella") != "Lei-TzuY/distributed-data-lab":
        fail("umbrella must be Lei-TzuY/distributed-data-lab")

    projects = data.get("projects")
    if not isinstance(projects, list) or not projects:
        fail("projects must be a non-empty list")

    names: set[str] = set()
    sources: set[str] = set()
    targets: set[str] = set()

    for index, project in enumerate(projects):
        if not isinstance(project, dict):
            fail(f"projects[{index}] must be an object")
        missing = REQUIRED_FIELDS - project.keys()
        if missing:
            fail(f"projects[{index}] missing fields: {', '.join(sorted(missing))}")

        name = project["name"]
        source = project["source_repository"]
        target = project["target_path"]
        status = project["status"]
        observed = project["observed_main_sha"]
        blocker = project["blocker"]

        if not isinstance(name, str) or not name:
            fail(f"projects[{index}].name must be non-empty")
        if source != f"Lei-TzuY/{name}":
            fail(f"{name}: source_repository must be Lei-TzuY/{name}")
        if target != f"projects/{name}":
            fail(f"{name}: target_path must be projects/{name}")
        if status not in ALLOWED_STATUS:
            fail(f"{name}: unsupported status {status!r}")
        if not valid_sha(observed):
            fail(f"{name}: observed_main_sha must be a 40-character lowercase hex SHA")

        if name in names or source in sources or target in targets:
            fail(f"{name}: duplicate project identity/source/target")
        names.add(name)
        sources.add(source)
        targets.add(target)

        if status == "hold":
            if not isinstance(blocker, str) or not blocker.strip():
                fail(f"{name}: HOLD entries require a blocker")
            if not isinstance(project.get("active_pr_number"), int):
                fail(f"{name}: current Phase 0 HOLD entry requires active_pr_number")
            if not valid_sha(project.get("active_pr_head_sha")):
                fail(f"{name}: current Phase 0 HOLD entry requires active_pr_head_sha")

        if status == "ready-for-import":
            if blocker is not None:
                fail(f"{name}: READY entry cannot have a blocker")
            if project.get("source_ci_conclusion") != "success":
                fail(f"{name}: READY entry requires successful source CI evidence")
            if not isinstance(project.get("source_ci_run_id"), int):
                fail(f"{name}: READY entry requires an integer source_ci_run_id")
            contract = project.get("source_equivalent_ci")
            if not isinstance(contract, list) or not contract or not all(
                isinstance(command, str) and command.strip() for command in contract
            ):
                fail(f"{name}: READY entry requires a non-empty source_equivalent_ci contract")

        if status in {"imported-verified", "integration-verified"}:
            if not (ROOT / target).is_dir():
                fail(f"{name}: verified import status requires an existing target subtree")
            if not valid_sha(project.get("imported_source_sha")):
                fail(f"{name}: verified import requires imported_source_sha")

    print(f"validated {len(projects)} distributed/data project entries")


if __name__ == "__main__":
    main()

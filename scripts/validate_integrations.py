#!/usr/bin/env python3
"""Validate distributed-data-lab cross-project integration metadata."""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
PROJECT_MANIFEST = ROOT / "projects" / "manifest.json"
INTEGRATION_MANIFEST = ROOT / "integrations" / "manifest.json"
SHA40 = re.compile(r"^[0-9a-f]{40}$")
REQUIRED_FIELDS = {
    "name",
    "status",
    "participants",
    "participant_source_shas",
    "target_path",
    "workflow_path",
    "scope",
    "verification_contract",
    "limitations",
}


def fail(message: str) -> None:
    print(f"integration manifest validation failed: {message}", file=sys.stderr)
    raise SystemExit(1)


def load_json(path: Path) -> dict[str, object]:
    try:
        value = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as exc:
        fail(f"cannot read {path.relative_to(ROOT)}: {exc}")
    if not isinstance(value, dict):
        fail(f"{path.relative_to(ROOT)} must contain a JSON object")
    return value


def string_list(value: object) -> bool:
    return (
        isinstance(value, list)
        and bool(value)
        and all(isinstance(item, str) and item.strip() for item in value)
    )


def main() -> None:
    project_data = load_json(PROJECT_MANIFEST)
    integration_data = load_json(INTEGRATION_MANIFEST)
    if integration_data.get("schema_version") != 1:
        fail("schema_version must be 1")
    if integration_data.get("umbrella") != "Lei-TzuY/distributed-data-lab":
        fail("umbrella identity is incorrect")

    projects = project_data.get("projects")
    if not isinstance(projects, list):
        fail("project manifest must contain a projects list")
    project_by_name = {
        project["name"]: project
        for project in projects
        if isinstance(project, dict) and isinstance(project.get("name"), str)
    }

    candidates = integration_data.get("candidate_integrations")
    verified = integration_data.get("verified_integrations")
    if not isinstance(candidates, list) or not isinstance(verified, list):
        fail("candidate_integrations and verified_integrations must be lists")
    if not candidates and not verified:
        fail("at least one integration entry is required")

    names: set[str] = set()
    targets: set[str] = set()
    for expected_status, integrations in (
        ("verification-candidate", candidates),
        ("integration-verified", verified),
    ):
        for index, integration in enumerate(integrations):
            if not isinstance(integration, dict):
                fail(f"{expected_status}[{index}] must be an object")
            missing = REQUIRED_FIELDS - integration.keys()
            if missing:
                fail(f"integration entry is missing: {', '.join(sorted(missing))}")
            name = integration["name"]
            target = integration["target_path"]
            participants = integration["participants"]
            source_shas = integration["participant_source_shas"]
            workflow = integration["workflow_path"]

            if integration["status"] != expected_status:
                fail(f"{name}: status must be {expected_status}")
            if not isinstance(name, str) or not name or name in names:
                fail(f"invalid or duplicate integration name: {name!r}")
            names.add(name)
            if (
                not isinstance(participants, list)
                or len(participants) < 2
                or len(set(participants)) != len(participants)
                or not all(isinstance(item, str) and item for item in participants)
            ):
                fail(f"{name}: participants must contain at least two unique names")
            if not isinstance(source_shas, dict) or set(source_shas) != set(participants):
                fail(f"{name}: participant_source_shas must match participants")
            for participant in participants:
                project = project_by_name.get(participant)
                if project is None or project.get("status") not in {
                    "imported-verified",
                    "integration-verified",
                }:
                    fail(f"{name}: {participant} is not a verified imported project")
                source_sha = source_shas[participant]
                if not isinstance(source_sha, str) or SHA40.fullmatch(source_sha) is None:
                    fail(f"{name}: {participant} has an invalid source SHA")
                if source_sha != project.get("imported_source_sha"):
                    fail(f"{name}: {participant} source SHA differs from import ledger")
            if (
                not isinstance(target, str)
                or not target.startswith("integrations/")
                or target in targets
                or not (ROOT / target).is_dir()
            ):
                fail(f"{name}: invalid or duplicate target_path")
            targets.add(target)
            if (
                not isinstance(workflow, str)
                or not workflow.startswith(".github/workflows/")
                or not (ROOT / workflow).is_file()
            ):
                fail(f"{name}: workflow_path is missing or invalid")
            if not isinstance(integration["scope"], str) or not integration["scope"].strip():
                fail(f"{name}: scope must be non-empty")
            if not string_list(integration["verification_contract"]):
                fail(f"{name}: verification_contract must be a non-empty string list")
            if not string_list(integration["limitations"]):
                fail(f"{name}: limitations must be a non-empty string list")

    print(
        f"validated {len(candidates)} candidate and {len(verified)} verified "
        "cross-project integration(s)"
    )


if __name__ == "__main__":
    main()

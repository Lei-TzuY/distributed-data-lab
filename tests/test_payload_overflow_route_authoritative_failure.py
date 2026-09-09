#!/usr/bin/env python3
"""Regression guard for payload overflow handler escalation semantics.

The non-full split route is allowed to escalate only when it has positively
matched a split-required leaf and stopped specifically because its internal
parent is full. Once the full-root route itself becomes applicable, any later
failure is authoritative and must not be masked by probing the non-root-parent
handler.
"""

from pathlib import Path


SOURCE = (
    Path(__file__).resolve().parents[1]
    / "src"
    / "record_payload_nonroot_overflow_route.c"
)

ESCALATION = (
    "payload-native INSERT reached a full internal parent; "
    "recursive parent overflow is not implemented yet"
)


def main() -> int:
    text = SOURCE.read_text(encoding="utf-8")

    assert ESCALATION in text, (
        "overflow routing must use the exact non-full-parent capacity failure "
        "as its explicit escalation contract"
    )
    assert "strcmp(message, TINYDB_PAYLOAD_FULL_PARENT_ESCALATION) == 0" in text, (
        "base-route escalation must be gated by the explicit capacity message"
    )

    base_call = text.index("tinydb_record_payload_try_nonroot_split_nonfull_base(")
    base_authoritative = text.index(
        "if (base_applicable &&\n        !base_failure_allows_parent_escalation"
    )
    root_call = text.index("tinydb_record_payload_try_full_root_parent_split(")
    root_authoritative = text.index("if (root_applicable) {")
    nonroot_call = text.index("tinydb_record_payload_try_full_nonroot_parent_split(")

    assert base_call < base_authoritative < root_call, (
        "an applicable base-path failure must be resolved before probing the "
        "full-root handler unless it is the explicit capacity escalation"
    )
    assert root_call < root_authoritative < nonroot_call, (
        "an applicable full-root failure must be authoritative before the "
        "non-root-parent handler is considered"
    )

    base_guard = text[base_authoritative:root_call]
    assert "return false;" in base_guard and "set_message(message," in base_guard, (
        "authoritative base failures must preserve their diagnostic and return"
    )

    root_guard = text[root_authoritative:nonroot_call]
    assert "return false;" in root_guard and "set_message(message," in root_guard, (
        "authoritative full-root failures must preserve their diagnostic and return"
    )

    print("payload overflow authoritative-routing regression passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Regression guard for payload-native non-root split sibling topology.

A split mutates the current leaf, its parent, and sometimes its next sibling.
Before that mutation begins, both neighboring sibling links must be reciprocal
and the key ranges must remain strictly ordered. This test locks the production
checks in place so a future refactor cannot silently fall back to one-way links.
"""

from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "src" / "record_payload_nonroot_split.c"


def main() -> int:
    text = SOURCE.read_text(encoding="utf-8")

    previous_guard = text.index("static bool previous_boundary_allows")
    next_guard = text.index("static bool next_boundary_allows")
    split = text.index("bool tinydb_record_payload_try_nonroot_split")

    previous_body = text[previous_guard:next_guard]
    assert "previous_next == current_page_num" in previous_body, (
        "payload split must reject a previous sibling whose next link does not point back to the current leaf"
    )
    assert "key > max_key" in previous_body, (
        "previous sibling validation must retain strict cross-leaf key ordering"
    )

    next_body = text[next_guard:split]
    assert "next_previous == current_page_num" in next_body, (
        "payload split must reject a next sibling whose prev link does not point back to the current leaf"
    )
    assert "min_key > current_max_key" in next_body, (
        "next sibling validation must retain strict cross-leaf key ordering"
    )

    call = text.index("previous_boundary_allows(table,")
    topology = text.index("!next_boundary_allows(next_before, left_page_num, old_left_max)")
    reservation = text.index("uint32_t claimed = get_unused_page_num(table->pager);")
    epoch = text.index("tinydb_generic_index_epoch_before_mutation(table, schema)")

    assert call < topology < reservation < epoch, (
        "reciprocal sibling topology must be validated before allocator reservation, durable epoch mutation, or page publication"
    )

    print("payload split reciprocal sibling guard regression passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

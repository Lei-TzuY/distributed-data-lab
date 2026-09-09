#!/usr/bin/env python3
"""Regression guard for payload-native no-split INSERT publication ordering.

All fallible slotted-page work must happen in a private image before the
durable generic-index epoch advances.  After that boundary, the live page is
updated with a single validated image copy and marked dirty.
"""

from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "src" / "record_payload_insert.c"


def main() -> int:
    text = SOURCE.read_text(encoding="utf-8")

    stage_decl = text.index("unsigned char after[PAGE_SIZE];")
    stage_copy = text.index("memcpy(after, page, sizeof(after));", stage_decl)
    staged_insert = text.index("tinydb_slotted_leaf_v2_insert(after,", stage_copy)
    staged_validate = text.index("tinydb_slotted_leaf_v2_validate(after, PAGE_SIZE)", staged_insert)
    epoch = text.index("tinydb_generic_index_epoch_before_mutation(table, schema)", staged_validate)
    publish = text.index("memcpy(page, after, PAGE_USABLE_SIZE);", epoch)
    dirty = text.index("mark_page_dirty(table->pager, target_page_num);", publish)

    assert stage_decl < stage_copy < staged_insert < staged_validate < epoch < publish < dirty, (
        "payload INSERT must stage and validate the private page image before "
        "advancing the durable index epoch, then publish exactly once"
    )

    pre_epoch = text[stage_decl:epoch]
    assert "tinydb_slotted_leaf_v2_insert(page," not in pre_epoch, (
        "the live pager frame must not be mutated before epoch publication"
    )

    post_epoch = text[epoch:dirty]
    assert "tinydb_slotted_leaf_v2_insert(" not in post_epoch, (
        "no fallible slotted insert may remain after the durable epoch advances"
    )
    assert "memcpy(page, after, PAGE_USABLE_SIZE);" in post_epoch, (
        "post-epoch publication must be a direct validated image copy"
    )

    print("payload no-split stage-before-epoch regression passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

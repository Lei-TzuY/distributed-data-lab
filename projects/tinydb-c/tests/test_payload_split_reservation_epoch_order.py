#!/usr/bin/env python3
"""Regression guard for payload-native V2 split mutation ordering.

The new leaf page reservation is reversible and must be claimed before the
persistent generic-index epoch advances. Once the epoch has advanced, the
validated page batch may be published. This keeps reservation races as true
pre-mutation failures instead of spuriously invalidating an otherwise current
secondary-index snapshot.
"""

from pathlib import Path


SOURCE = Path(__file__).resolve().parents[1] / "src" / "record_payload_nonroot_split.c"


def main() -> int:
    text = SOURCE.read_text(encoding="utf-8")

    claim = text.index("uint32_t claimed = get_unused_page_num(table->pager);")
    epoch = text.index("tinydb_generic_index_epoch_before_mutation(table, schema)")
    publish = text.index("tinydb_v2_publish_batch(entries,")

    assert claim < epoch < publish, (
        "payload split must claim its reversible page reservation before "
        "advancing the durable index epoch, and advance the epoch before page publication"
    )

    epoch_failure = text[epoch:publish]
    assert "restore_allocator_reservation(table->pager," in epoch_failure, (
        "epoch persistence failure must release the claimed page reservation"
    )

    pre_epoch = text[claim:epoch]
    assert "restore_allocator_reservation(table->pager," in pre_epoch, (
        "reservation/target acquisition failures must remain reversible before epoch publication"
    )

    print("payload split reservation/epoch ordering regression passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

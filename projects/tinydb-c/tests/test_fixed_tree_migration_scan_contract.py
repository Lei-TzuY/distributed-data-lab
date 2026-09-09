from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "leaf_cursor_read.h"


def _source() -> str:
    return HEADER.read_text(encoding="utf-8")


def _scan_body() -> str:
    source = _source()
    marker = "static inline bool tinydb_fixed_v1_tree_scan_compact_v2("
    assert marker in source, "fixed-tree compact migration scan seam is missing"
    return source[source.index(marker) :]


def _staging_body() -> str:
    source = _source()
    marker = "static inline bool tinydb_compact_v2_staging_leaf_chain_init("
    assert marker in source, "compact V2 private staging leaf-chain seam is missing"
    return source[source.index(marker) : source.index(
        "static inline bool tinydb_fixed_v1_tree_scan_compact_v2("
    )]


def test_fixed_tree_migration_scan_reuses_canonical_row_admission():
    body = _scan_body()
    assert "tinydb_leaf_page_is_fixed_v1" in body
    assert "value_length != ROW_SIZE" in body
    assert "tinydb_fixed_v1_row_encode_compact_v2" in body
    assert "envelope_length == 0u" in body


def test_fixed_tree_migration_scan_uses_checked_topology_traversal():
    body = _scan_body()
    assert "tinydb_leaf_read_start" in body
    assert "tinydb_leaf_read_advance_checked" in body
    assert "visitor != NULL" in body
    assert "!visitor(key, envelope, envelope_length, context)" in body


def test_fixed_tree_migration_scan_publishes_count_only_after_success():
    body = _scan_body()
    success = body.index("ok = true;")
    cleanup = body.index("done:")
    publish = body.index("if (ok && rows_scanned != NULL) *rows_scanned = count;")
    assert success < cleanup < publish
    assert "table->root_page_num = previous_root;" in body[cleanup:publish]


def test_private_staging_chain_never_touches_durable_state():
    body = _staging_body()
    forbidden = [
        "get_unused_page_num(",
        "get_page(",
        "mark_page_dirty(",
        "pager_commit(",
        "pager_checkpoint(",
        "tinydb_generic_index_epoch_before_mutation(",
        "multitable_catalog_save(",
    ]
    for token in forbidden:
        assert token not in body, f"private staging unexpectedly uses durable seam: {token}"
    assert "page_images" in body
    assert "page_numbers" in body


def test_private_staging_chain_rolls_pages_with_reciprocal_siblings():
    body = _staging_body()
    assert "required <= available" in body
    assert "chain->page_count >= chain->page_capacity" in body
    assert "TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET" in body
    assert "TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET" in body
    assert "next_page_num" in body
    assert "previous_page_num" in body
    assert "memcpy(current, previous, PAGE_USABLE_SIZE);" in body
    assert "memcpy(next_destination, next, PAGE_USABLE_SIZE);" in body


def test_private_staging_chain_rejects_non_monotonic_or_oversized_rows_before_publish():
    body = _staging_body()
    assert "envelope_length > UINT16_MAX" in body
    assert "key <= chain->last_key" in body
    assert body.index("chain->page_count >= chain->page_capacity") < body.index(
        "memcpy(current, previous, PAGE_USABLE_SIZE);"
    )


def test_private_staging_chain_validation_checks_global_order_and_links():
    body = _staging_body()
    assert "expected_prev" in body
    assert "expected_next" in body
    assert "key <= previous_key" in body
    assert "rows != chain->row_count" in body
    assert "previous_key == chain->last_key" in body


def test_fixed_tree_scan_connects_to_private_staging_chain_before_publication():
    source = _source()
    marker = "static inline bool tinydb_fixed_v1_tree_stage_compact_v2_leaf_chain("
    assert marker in source
    body = source[source.index(marker) :]
    assert "chain->page_count != 1u" in body
    assert "chain->row_count != 0u" in body
    assert "tinydb_fixed_v1_tree_scan_compact_v2(" in body
    assert "tinydb_compact_v2_staging_leaf_chain_visit" in body
    assert "scanned != chain->row_count" in body
    assert "tinydb_compact_v2_staging_leaf_chain_validate(chain)" in body

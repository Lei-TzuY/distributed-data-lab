from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_staging_tree.h"


def _source() -> str:
    return HEADER.read_text(encoding="utf-8")


def _build_body() -> str:
    source = _source()
    marker = "static inline bool tinydb_compact_v2_staging_single_root_build("
    assert marker in source, "single-level staging root builder is missing"
    return source[source.index(marker) :]


def _validate_body() -> str:
    source = _source()
    start = source.index(
        "static inline bool tinydb_compact_v2_staging_single_root_validate("
    )
    end = source.index(
        "static inline bool tinydb_compact_v2_staging_single_root_build("
    )
    return source[start:end]


def test_private_root_builder_is_bounded_to_one_internal_level():
    body = _build_body()
    assert "leaves->page_count < 2u" in body
    assert "leaves->page_count > INTERNAL_NODE_MAX_KEYS + 1u" in body
    assert "tinydb_compact_v2_staging_leaf_chain_validate(leaves)" in body


def test_private_root_builder_preflights_before_publishing_images():
    body = _build_body()
    preflight = body.index("uint32_t separator_keys[INTERNAL_NODE_MAX_KEYS];")
    separator_scan = body.index("tinydb_compact_v2_staging_leaf_max_key(")
    publish = body.index("memcpy(root_image, staged_root, PAGE_USABLE_SIZE);")
    assert preflight < separator_scan < publish
    assert body.index("right_max <= separator_keys[num_keys - 1u]") < publish


def test_private_root_uses_child_upper_bounds_as_separators():
    body = _build_body()
    assert "*internal_node_child(staged_root, i) = leaves->page_numbers[i];" in body
    assert "*internal_node_key(staged_root, i) = separator_keys[i];" in body
    assert "*internal_node_right_child(staged_root)" in body
    assert "leaves->page_numbers[leaves->page_count - 1u]" in body


def test_private_root_sets_reciprocal_parent_identity_only_after_preflight():
    body = _build_body()
    publish = body.index("memcpy(root_image, staged_root, PAGE_USABLE_SIZE);")
    parent_publish = body.index("*node_parent(leaf) = root_page_num;")
    assert publish < parent_publish
    assert "set_node_root(leaf, false);" in body
    assert "*node_parent(staged_root) = 0u;" in body
    assert "set_node_root(staged_root, true);" in body


def test_private_root_validator_checks_routing_and_parent_reciprocity():
    body = _validate_body()
    assert "get_node_type(root) != NODE_INTERNAL" in body
    assert "!is_node_root(root)" in body
    assert "*node_parent(root) != 0u" in body
    assert "*internal_node_num_keys(root) != expected_keys" in body
    assert "*internal_node_right_child(root)" in body
    assert "*node_parent((void*)leaf) != staging->root_page_num" in body
    assert "*internal_node_child(root, i)" in body
    assert "*internal_node_key(root, i) != max_key" in body


def test_private_root_page_number_cannot_alias_leaf_identity():
    source = _source()
    assert "tinydb_compact_v2_staging_root_page_number_is_private" in source
    helper = source[source.index(
        "static inline bool tinydb_compact_v2_staging_root_page_number_is_private("
    ) : source.index(
        "static inline bool tinydb_compact_v2_staging_leaf_max_key("
    )]
    assert "root_page_num == 0u" in helper
    assert "chain->page_numbers[i] == root_page_num" in helper


def test_private_topology_layer_never_touches_durable_state():
    source = _source()
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
        assert token not in source, f"private topology unexpectedly uses durable seam: {token}"

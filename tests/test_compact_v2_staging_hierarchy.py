import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_staging_hierarchy.h"


def source() -> str:
    return HEADER.read_text(encoding="utf-8")


def test_hierarchy_reduces_arbitrary_leaf_counts_to_one_root():
    text = source()
    assert "tinydb_compact_v2_staging_required_internal_pages" in text
    assert "while (nodes > 1u)" in text
    assert "while (current_count > 1u)" in text
    assert "parent_count" in text
    assert "base_children" in text
    assert "extra_children" in text
    assert "child_count < 2u || child_count > fanout" in text
    assert "built_levels != required_levels" in text


def test_hierarchy_balances_children_without_singleton_internal_nodes():
    text = source()
    build = text[text.index("static inline bool tinydb_compact_v2_staging_hierarchy_build(") :]
    assert "base_children = current_count / parent_count" in build
    assert "extra_children = current_count % parent_count" in build
    assert "base_children + (parent < extra_children ? 1u : 0u)" in build
    assert "child_count < 2u" in build


def test_hierarchy_preflights_page_identity_and_capacity_before_mutation():
    text = source()
    build = text[text.index("static inline bool tinydb_compact_v2_staging_hierarchy_build(") :]
    preflight_capacity = build.index("required_internal > internal_capacity")
    preflight_identity = build.index("tinydb_compact_v2_staging_hierarchy_page_numbers_valid(")
    allocation = build.index("malloc(")
    mutation = build.index("memset(internal_images")
    assert preflight_capacity < allocation < mutation
    assert preflight_identity < allocation < mutation
    assert "page_num == leaves->page_numbers[leaf]" in text
    assert "page_num == internal_page_numbers[j]" in text


def test_hierarchy_routes_by_child_upper_bounds_and_sets_parent_reciprocity():
    text = source()
    build = text[text.index("static inline bool tinydb_compact_v2_staging_hierarchy_build(") :]
    assert "*internal_node_child(node, child) = ref->page_num;" in build
    assert "*internal_node_key(node, child) = ref->max_key;" in build
    assert "*internal_node_right_child(node)" in build
    assert "*node_parent(ref->image) = node_page_num;" in build
    assert "set_node_root(current[0].image, true);" in build
    assert "*node_parent(current[0].image) = 0u;" in build


def test_hierarchy_validator_checks_reachability_uniqueness_and_subtree_bounds():
    text = source()
    validate = text[
        text.index("static inline bool tinydb_compact_v2_staging_hierarchy_validate(") :
        text.index("static inline bool tinydb_compact_v2_staging_hierarchy_build(")
    ]
    assert "tinydb_compact_v2_staging_hierarchy_inbound_count" in validate
    assert "tinydb_compact_v2_staging_hierarchy_reaches_root" in validate
    assert "tinydb_compact_v2_staging_hierarchy_subtree_max_key" in validate
    assert "*node_parent(child_image) != page_num" in validate
    assert "*internal_node_key(node, child) != child_max" in validate
    assert "right_max <= previous_separator" in validate
    assert "is_node_root(leaf)" in validate


def test_hierarchy_layer_never_touches_durable_state():
    text = source()
    forbidden = [
        "get_unused_page_num(",
        "get_page(",
        "mark_page_dirty(",
        "pager_commit(",
        "pager_checkpoint(",
        "multitable_catalog_save(",
        "tinydb_generic_index_epoch_before_mutation(",
    ]
    for token in forbidden:
        assert token not in text, f"private hierarchy unexpectedly uses durable seam: {token}"


def compile_header_on_active_toolchain():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for hierarchy compile regression")

    with tempfile.TemporaryDirectory(prefix="tinydb-hierarchy-") as tmp:
        tmp_path = Path(tmp)
        probe = tmp_path / "probe.c"
        probe.write_text(
            '#include "compact_v2_staging_hierarchy.h"\n'
            "bool tinydb_hierarchy_compile_probe(\n"
            "    TinyDBCompactV2StagingHierarchy* hierarchy,\n"
            "    TinyDBCompactV2StagingLeafChain* leaves,\n"
            "    unsigned char* images,\n"
            "    const uint32_t* pages,\n"
            "    uint32_t capacity) {\n"
            "  uint32_t required = 0u;\n"
            "  uint32_t levels = 0u;\n"
            "  return tinydb_compact_v2_staging_required_internal_pages(\n"
            "             leaves != NULL ? leaves->page_count : 0u, &required, &levels) &&\n"
            "         tinydb_compact_v2_staging_hierarchy_build(\n"
            "             hierarchy, leaves, images, pages, capacity);\n"
            "}\n",
            encoding="utf-8",
        )
        cmake_lists = tmp_path / "CMakeLists.txt"
        cmake_lists.write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBHierarchyCompile C)\n"
            "set(CMAKE_C_STANDARD 99)\n"
            "set(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n"
            "  add_compile_options(/W4 /WX /utf-8)\n"
            "else()\n"
            "  add_compile_options(-Wall -Wextra -Werror)\n"
            "endif()\n"
            "add_library(hierarchy_probe OBJECT probe.c)\n"
            f'target_include_directories(hierarchy_probe PRIVATE "{(ROOT / "src").as_posix()}")\n',
            encoding="utf-8",
        )
        build = tmp_path / "build"
        configure = subprocess.run(
            ["cmake", "-S", str(tmp_path), "-B", str(build)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=60,
        )
        if configure.returncode != 0:
            raise AssertionError(configure.stdout + configure.stderr)
        compile_result = subprocess.run(
            ["cmake", "--build", str(build), "--config", "Debug"],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=120,
        )
        if compile_result.returncode != 0:
            raise AssertionError(compile_result.stdout + compile_result.stderr)


def main():
    test_hierarchy_reduces_arbitrary_leaf_counts_to_one_root()
    test_hierarchy_balances_children_without_singleton_internal_nodes()
    test_hierarchy_preflights_page_identity_and_capacity_before_mutation()
    test_hierarchy_routes_by_child_upper_bounds_and_sets_parent_reciprocity()
    test_hierarchy_validator_checks_reachability_uniqueness_and_subtree_bounds()
    test_hierarchy_layer_never_touches_durable_state()
    compile_header_on_active_toolchain()
    print(
        "PASS: arbitrary-depth compact V2 staging hierarchy preflights capacity/identity, "
        "balances internal fanout, preserves child-upper-bound routing and parent reciprocity, "
        "validates reachability/inbound uniqueness, stays private from durable state, and "
        "compiles with the active CI C toolchain"
    )


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)

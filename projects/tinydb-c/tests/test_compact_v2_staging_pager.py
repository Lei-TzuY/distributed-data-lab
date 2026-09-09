import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_staging_pager.h"


def source() -> str:
    return HEADER.read_text(encoding="utf-8")


def test_claims_require_transaction_and_preflight_allocator_metadata():
    text = source()
    claim = text[
        text.index("static inline bool tinydb_compact_v2_staging_pager_claim_pages(") :
        text.index("static inline bool tinydb_compact_v2_staging_pager_claims_match_hierarchy(")
    ]
    assert "!pager->in_transaction" in claim
    assert "pager->free_page_count > pager->num_pages" in claim
    assert "page_num == 0u || page_num == INVALID_PAGE_NUM" in claim
    assert "page_num >= pager->num_pages" in claim
    assert "pager->free_pages[j]" in claim
    assert "append_needed" in claim


def test_claims_advance_allocator_identity_between_file_tail_pages():
    text = source()
    claim = text[
        text.index("static inline bool tinydb_compact_v2_staging_pager_claim_pages(") :
        text.index("static inline bool tinydb_compact_v2_staging_pager_claims_match_hierarchy(")
    ]
    get_unused = claim.index("get_unused_page_num(pager)")
    fetch = claim.index("get_page(pager, page_num)")
    publish = claim.index("claimed_pages[i] = page_num")
    assert get_unused < fetch < publish
    assert "get_unused_page_num() alone does not advance" in claim


def test_readback_verification_requires_exact_namespace_and_page_bytes():
    text = source()
    verify = text[
        text.index(
            "static inline bool tinydb_compact_v2_staging_pager_verify_materialized_hierarchy("
        ) :
        text.index("static inline bool tinydb_compact_v2_staging_pager_materialize_hierarchy(")
    ]
    assert "!pager->in_transaction" in verify
    assert "tinydb_compact_v2_staging_pager_claims_match_hierarchy" in verify
    assert "pager->free_pages[free_index] == page_num" in verify
    assert verify.count("get_page(pager, page_num)") == 2
    assert verify.count("memcmp(actual, expected, PAGE_SIZE) != 0") == 2
    assert "tinydb_compact_v2_staging_page_const" in verify
    assert "tinydb_compact_v2_staging_internal_page_const" in verify


def test_materialization_requires_readback_before_root_exposure():
    text = source()
    materialize = text[
        text.index("static inline bool tinydb_compact_v2_staging_pager_materialize_hierarchy(") :
    ]
    assert "tinydb_compact_v2_staging_pager_claims_match_hierarchy" in materialize
    preflight = materialize.index("tinydb_compact_v2_staging_pager_claims_match_hierarchy")
    dirty = materialize.index("mark_page_dirty(pager, page_num)")
    verify = materialize.index(
        "tinydb_compact_v2_staging_pager_verify_materialized_hierarchy("
    )
    publish = materialize.index("*staged_root_page_num = hierarchy->root_page_num")
    assert preflight < dirty < verify < publish
    assert "page_num >= pager->num_pages" in materialize
    assert "pager->free_pages[free_index] == page_num" in materialize
    assert "*staged_root_page_num = 0u" in materialize


def test_pager_staging_stops_before_durable_or_catalog_publication():
    text = source()
    forbidden = [
        "pager_commit(",
        "pager_checkpoint(",
        "multitable_catalog_save(",
        "tinydb_generic_index_epoch_before_mutation(",
    ]
    for token in forbidden:
        assert token not in text, f"pager staging crossed publication boundary: {token}"


def compile_header_on_active_toolchain():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for pager staging compile regression")

    with tempfile.TemporaryDirectory(prefix="tinydb-staging-pager-") as tmp:
        tmp_path = Path(tmp)
        probe = tmp_path / "probe.c"
        probe.write_text(
            '#include "compact_v2_staging_pager.h"\n'
            "bool tinydb_staging_pager_compile_probe(\n"
            "    Pager* pager,\n"
            "    const TinyDBCompactV2StagingHierarchy* hierarchy,\n"
            "    uint32_t* pages,\n"
            "    uint32_t count,\n"
            "    uint32_t* root) {\n"
            "  return tinydb_compact_v2_staging_pager_claim_pages(pager, count, pages) &&\n"
            "         tinydb_compact_v2_staging_pager_materialize_hierarchy(\n"
            "             pager, hierarchy, pages, count, root) &&\n"
            "         tinydb_compact_v2_staging_pager_verify_materialized_hierarchy(\n"
            "             pager, hierarchy, pages, count);\n"
            "}\n",
            encoding="utf-8",
        )
        cmake_lists = tmp_path / "CMakeLists.txt"
        cmake_lists.write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBStagingPagerCompile C)\n"
            "set(CMAKE_C_STANDARD 99)\n"
            "set(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n"
            "  add_compile_options(/W4 /WX /utf-8)\n"
            "else()\n"
            "  add_compile_options(-Wall -Wextra -Werror)\n"
            "endif()\n"
            "add_library(staging_pager_probe OBJECT probe.c)\n"
            f'target_include_directories(staging_pager_probe PRIVATE "{(ROOT / "src").as_posix()}")\n',
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
    test_claims_require_transaction_and_preflight_allocator_metadata()
    test_claims_advance_allocator_identity_between_file_tail_pages()
    test_readback_verification_requires_exact_namespace_and_page_bytes()
    test_materialization_requires_readback_before_root_exposure()
    test_pager_staging_stops_before_durable_or_catalog_publication()
    compile_header_on_active_toolchain()
    print(
        "PASS: compact V2 pager staging claims unique transaction-scoped page identities, "
        "requires exact hierarchy namespace ownership, verifies Pager readback byte-for-byte "
        "before exposing the staged root, stops before checkpoint/catalog publication, and "
        "compiles on the active CI toolchain"
    )


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)

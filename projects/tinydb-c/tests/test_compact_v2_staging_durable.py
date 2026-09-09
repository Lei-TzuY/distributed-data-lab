import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_staging_durable.h"


def source() -> str:
    return HEADER.read_text(encoding="utf-8")


def test_durable_boundary_requires_exact_dirty_staging_set():
    text = source()
    exact = text[
        text.index("static inline bool tinydb_compact_v2_staging_pager_dirty_set_is_exact(") :
        text.index("static inline bool tinydb_compact_v2_staging_pager_make_durable_unpublished(")
    ]
    assert "!pager->in_transaction" in exact
    assert "!pager->is_dirty[page_num]" in exact
    assert "tinydb_compact_v2_staging_page_is_claimed" in exact
    assert "if (!pager->is_dirty[page_num]) continue" in exact
    assert "page_num == claimed_pages[j]" in exact


def test_durable_boundary_verifies_before_commit_and_checkpoint():
    text = source()
    durable = text[
        text.index("static inline bool tinydb_compact_v2_staging_pager_make_durable_unpublished(") :
    ]
    verify = durable.index("tinydb_compact_v2_staging_pager_verify_materialized_hierarchy(")
    exact = durable.index("tinydb_compact_v2_staging_pager_dirty_set_is_exact(")
    commit = durable.index("pager_commit(pager)")
    checkpoint = durable.index("pager_checkpoint(pager)")
    expose = durable.index("*durable_staged_root_page_num = hierarchy->root_page_num")
    assert verify < exact < commit < checkpoint < expose
    assert "if (pager->in_transaction) return false" in durable
    assert "pager->is_dirty[page_num]" in durable
    assert durable.count("memcmp(actual, expected, PAGE_USABLE_SIZE) != 0") == 2


def test_durable_boundary_does_not_publish_catalog_or_schema():
    text = source()
    forbidden = [
        "multitable_catalog_save(",
        "catalog_save(",
        "schema_catalog",
        "tinydb_generic_index_epoch_before_mutation(",
    ]
    for token in forbidden:
        assert token not in text, f"durable staging crossed catalog publication boundary: {token}"


def compile_header_on_active_toolchain():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for durable staging compile regression")

    with tempfile.TemporaryDirectory(prefix="tinydb-staging-durable-") as tmp:
        tmp_path = Path(tmp)
        probe = tmp_path / "probe.c"
        probe.write_text(
            '#include "compact_v2_staging_durable.h"\n'
            "bool tinydb_staging_durable_compile_probe(\n"
            "    Pager* pager,\n"
            "    const TinyDBCompactV2StagingHierarchy* hierarchy,\n"
            "    uint32_t* pages,\n"
            "    uint32_t count,\n"
            "    uint32_t* root) {\n"
            "  return tinydb_compact_v2_staging_pager_dirty_set_is_exact(\n"
            "             pager, pages, count) &&\n"
            "         tinydb_compact_v2_staging_pager_make_durable_unpublished(\n"
            "             pager, hierarchy, pages, count, root);\n"
            "}\n",
            encoding="utf-8",
        )
        cmake_lists = tmp_path / "CMakeLists.txt"
        cmake_lists.write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBStagingDurableCompile C)\n"
            "set(CMAKE_C_STANDARD 99)\n"
            "set(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n"
            "  add_compile_options(/W4 /WX /utf-8)\n"
            "else()\n"
            "  add_compile_options(-Wall -Wextra -Werror)\n"
            "endif()\n"
            "add_library(staging_durable_probe OBJECT probe.c)\n"
            f'target_include_directories(staging_durable_probe PRIVATE "{(ROOT / "src").as_posix()}")\n',
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
    test_durable_boundary_requires_exact_dirty_staging_set()
    test_durable_boundary_verifies_before_commit_and_checkpoint()
    test_durable_boundary_does_not_publish_catalog_or_schema()
    compile_header_on_active_toolchain()
    print(
        "PASS: durable compact V2 staging rejects unrelated dirty pages, verifies the "
        "materialized hierarchy before WAL commit/checkpoint, remains unpublished to the "
        "catalog, and compiles on the active CI toolchain"
    )


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)

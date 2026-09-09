import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_manifest.h"


def test_manifest_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAGIC",
        "TINYDB_COMPACT_V2_MIGRATION_MANIFEST_VERSION",
        "tinydb_compact_v2_manifest_crc32(",
        "tinydb_compact_v2_migration_manifest_is_valid(",
        "old_root_page_num == manifest->staged_root_page_num",
        "old_schema_generation >= manifest->new_schema_generation",
        "page_num == manifest->old_root_page_num",
        "page_num == manifest->claimed_pages[j]",
        "staged_root_claimed",
        "tinydb_compact_v2_migration_manifest_classify_recovery(",
        "TINYDB_COMPACT_V2_MIGRATION_RECOVERY_RECLAIM_STAGING",
        "TINYDB_COMPACT_V2_MIGRATION_RECOVERY_KEEP_NEW_RECLAIM_OLD",
    ]:
        assert token in text, f"missing manifest recovery invariant: {token}"


def compile_and_run_manifest_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for migration manifest regression")

    c_source = '''#include "compact_v2_migration_manifest.h"
#include <stdio.h>
#include <string.h>

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\\n", message);
    return 0;
}

int main(void) {
    uint32_t pages[4] = {101u, 102u, 103u, 104u};
    TinyDBCompactV2MigrationManifest manifest;
    memset(&manifest, 0, sizeof(manifest));
    manifest.table_id = 7u;
    manifest.old_root_page_num = 9u;
    manifest.staged_root_page_num = 103u;
    manifest.old_schema_generation = UINT64_C(41);
    manifest.new_schema_generation = UINT64_C(42);
    manifest.phase = TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED;
    manifest.claimed_page_count = 4u;
    manifest.claimed_pages = pages;

    unsigned char encoded[256];
    memset(encoded, 0xa5, sizeof(encoded));
    size_t encoded_length = 999u;
    if (!expect(tinydb_compact_v2_migration_manifest_encode(
                    &manifest, encoded, sizeof(encoded), &encoded_length),
                "valid manifest failed to encode")) return 1;
    if (!expect(encoded_length == 64u, "unexpected encoded manifest size")) return 2;

    TinyDBCompactV2MigrationManifest decoded;
    uint32_t decoded_pages[4] = {0u, 0u, 0u, 0u};
    if (!expect(tinydb_compact_v2_migration_manifest_decode(
                    encoded, encoded_length, &decoded, decoded_pages, 4u),
                "valid manifest failed to decode")) return 3;
    if (!expect(decoded.table_id == manifest.table_id &&
                decoded.old_root_page_num == manifest.old_root_page_num &&
                decoded.staged_root_page_num == manifest.staged_root_page_num &&
                decoded.old_schema_generation == manifest.old_schema_generation &&
                decoded.new_schema_generation == manifest.new_schema_generation &&
                decoded.claimed_page_count == 4u &&
                memcmp(decoded_pages, pages, sizeof(pages)) == 0,
                "manifest round trip changed metadata")) return 4;

    if (!expect(tinydb_compact_v2_migration_manifest_classify_recovery(
                    &decoded, 9u, UINT64_C(41)) ==
                    TINYDB_COMPACT_V2_MIGRATION_RECOVERY_RECLAIM_STAGING,
                "old authoritative root did not classify staging as orphan")) return 5;
    if (!expect(tinydb_compact_v2_migration_manifest_classify_recovery(
                    &decoded, 103u, UINT64_C(42)) ==
                    TINYDB_COMPACT_V2_MIGRATION_RECOVERY_KEEP_NEW_RECLAIM_OLD,
                "published root did not classify old tree for reclaim")) return 6;
    if (!expect(tinydb_compact_v2_migration_manifest_classify_recovery(
                    &decoded, 103u, UINT64_C(41)) ==
                    TINYDB_COMPACT_V2_MIGRATION_RECOVERY_INVALID,
                "mixed root/generation state was accepted")) return 7;

    unsigned char corrupt[256];
    memcpy(corrupt, encoded, encoded_length);
    corrupt[24] ^= 0x80u;
    memset(&decoded, 0xcc, sizeof(decoded));
    if (!expect(!tinydb_compact_v2_migration_manifest_decode(
                    corrupt, encoded_length, &decoded, decoded_pages, 4u),
                "checksum corruption was accepted")) return 8;
    if (!expect(decoded.table_id == 0u && decoded.claimed_page_count == 0u,
                "failed decode published manifest metadata")) return 9;

    uint32_t duplicate_pages[4] = {101u, 102u, 103u, 103u};
    manifest.claimed_pages = duplicate_pages;
    encoded_length = 777u;
    if (!expect(!tinydb_compact_v2_migration_manifest_encode(
                    &manifest, encoded, sizeof(encoded), &encoded_length),
                "duplicate claimed page was accepted")) return 10;
    if (!expect(encoded_length == 0u, "failed encode published a length")) return 11;

    manifest.claimed_pages = pages;
    manifest.staged_root_page_num = 999u;
    if (!expect(!tinydb_compact_v2_migration_manifest_is_valid(&manifest),
                "unclaimed staged root was accepted")) return 12;

    puts("manifest_roundtrip=yes");
    puts("checksum_reject=yes");
    puts("recovery_classification=yes");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="tinydb-migration-manifest-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(c_source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\n"
            "project(TinyDBMigrationManifestProbe C)\n"
            "set(CMAKE_C_STANDARD 99)\n"
            "set(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n"
            "  add_compile_options(/W4 /WX /utf-8)\n"
            "else()\n"
            "  add_compile_options(-Wall -Wextra -Werror)\n"
            "endif()\n"
            "add_executable(migration_manifest_probe probe.c)\n"
            f'target_include_directories(migration_manifest_probe PRIVATE "{(ROOT / "src").as_posix()}")\n',
            encoding="utf-8",
        )
        build = tmp_path / "build"
        configure = subprocess.run(
            ["cmake", "-S", str(tmp_path), "-B", str(build)],
            capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=60,
        )
        if configure.returncode != 0:
            raise AssertionError(configure.stdout + configure.stderr)
        compile_result = subprocess.run(
            ["cmake", "--build", str(build), "--config", "Debug"],
            capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=120,
        )
        if compile_result.returncode != 0:
            raise AssertionError(compile_result.stdout + compile_result.stderr)

        exe = build / "migration_manifest_probe"
        if sys.platform.startswith("win"):
            exe = build / "Debug" / "migration_manifest_probe.exe"
        run = subprocess.run(
            [str(exe)], capture_output=True, text=True, encoding="utf-8", errors="ignore", timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in ["manifest_roundtrip=yes", "checksum_reject=yes", "recovery_classification=yes"]:
            if marker not in run.stdout:
                raise AssertionError(f"missing runtime marker {marker}: {run.stdout}")


def main():
    test_manifest_source_contract()
    compile_and_run_manifest_probe()
    print("PASS: compact V2 migration manifest round-trips, rejects corruption/ambiguous page sets, classifies reopen state, and compiles/runs on the active CI toolchain")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)

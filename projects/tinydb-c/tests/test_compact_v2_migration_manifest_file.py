import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "compact_v2_migration_manifest_file.h"


def test_source_contract():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_ABSENT",
        "TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_INVALID",
        "TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE",
        "errno == ENOENT",
        "migration manifest has invalid bounded length",
        "tinydb_compact_v2_migration_manifest_decode(",
        "memset(manifest_out, 0, sizeof(*manifest_out))",
    ]:
        assert token in text, f"missing manifest-file invariant: {token}"
    assert text.index("length > TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE") < text.index(
        "fread(encoded_scratch"
    )
    assert text.index("fread(encoded_scratch") < text.index(
        "tinydb_compact_v2_migration_manifest_decode("
    )


def configure_and_build(tmp_path: Path, source: str):
    (tmp_path / "probe.c").write_text(source, encoding="utf-8")
    cmake = (
        "cmake_minimum_required(VERSION 3.10)\n"
        "project(TinyDBManifestFileProbe C)\n"
        "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
        "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
        "add_executable(manifest_file_probe probe.c)\n"
        "if(MSVC)\n  target_compile_definitions(manifest_file_probe PRIVATE _CRT_SECURE_NO_WARNINGS)\nendif()\n"
        f'target_include_directories(manifest_file_probe PRIVATE "{(ROOT / "src").as_posix()}")\n'
    )
    (tmp_path / "CMakeLists.txt").write_text(cmake, encoding="utf-8")
    build = tmp_path / "build"
    configured = subprocess.run(
        ["cmake", "-S", str(tmp_path), "-B", str(build)],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=60,
    )
    if configured.returncode != 0:
        raise AssertionError(configured.stdout + configured.stderr)
    compiled = subprocess.run(
        ["cmake", "--build", str(build), "--config", "Debug"],
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="ignore",
        timeout=120,
    )
    if compiled.returncode != 0:
        raise AssertionError(compiled.stdout + compiled.stderr)
    exe = build / ("Debug/manifest_file_probe.exe" if sys.platform.startswith("win") else "manifest_file_probe")
    if not exe.exists() and sys.platform.startswith("win"):
        exe = build / "manifest_file_probe.exe"
    return exe


def run_probe():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required for manifest file regression")
    source = r'''#include "compact_v2_migration_manifest_file.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int expect(int condition, const char* message) {
    if (condition) return 1;
    fprintf(stderr, "%s\n", message);
    return 0;
}

static int write_bytes(const char* path, const unsigned char* data, size_t length) {
    FILE* f = fopen(path, "wb");
    if (!f) return 0;
    if (length != 0u && fwrite(data, 1u, length, f) != length) {
        fclose(f);
        return 0;
    }
    return fclose(f) == 0;
}

int main(int argc, char** argv) {
    if (argc != 2) return 100;
    const char* database = argv[1];
    char manifest_path[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_PATH_MAX];
    unsigned char scratch[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE];
    uint32_t claims_out[TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS];
    TinyDBCompactV2MigrationManifest loaded;
    char message[160];

    if (!expect(tinydb_compact_v2_migration_manifest_file_path(
                    database, manifest_path, sizeof(manifest_path)),
                "manifest path build failed")) return 1;

    TinyDBCompactV2MigrationManifestLoadResult result =
        tinydb_compact_v2_migration_manifest_load_file(
            database, &loaded, claims_out,
            TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS,
            scratch, sizeof(scratch), message, sizeof(message));
    if (!expect(result == TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_ABSENT,
                "missing sidecar was not treated as absent")) return 2;

    uint32_t claims[2] = {7u, 8u};
    TinyDBCompactV2MigrationManifest manifest = {
        3u, 0u, 7u, UINT64_C(12), UINT64_C(13),
        TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED, 2u, claims
    };
    size_t encoded_length = 0u;
    if (!expect(tinydb_compact_v2_migration_manifest_encode(
                    &manifest, scratch, sizeof(scratch), &encoded_length),
                "unable to encode valid manifest")) return 3;
    if (!expect(write_bytes(manifest_path, scratch, encoded_length),
                "unable to write valid manifest")) return 4;

    memset(&loaded, 0, sizeof(loaded));
    result = tinydb_compact_v2_migration_manifest_load_file(
        database, &loaded, claims_out,
        TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS,
        scratch, sizeof(scratch), message, sizeof(message));
    if (!expect(result == TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_OK,
                "valid manifest did not load")) return 5;
    if (!expect(loaded.table_id == 3u && loaded.old_root_page_num == 0u &&
                loaded.staged_root_page_num == 7u && loaded.claimed_page_count == 2u &&
                loaded.claimed_pages == claims_out && claims_out[0] == 7u && claims_out[1] == 8u,
                "decoded manifest identity drifted")) return 6;

    scratch[encoded_length - 1u] ^= 0x80u;
    if (!expect(write_bytes(manifest_path, scratch, encoded_length),
                "unable to write corrupt manifest")) return 7;
    memset(&loaded, 0x7f, sizeof(loaded));
    result = tinydb_compact_v2_migration_manifest_load_file(
        database, &loaded, claims_out,
        TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS,
        scratch, sizeof(scratch), message, sizeof(message));
    if (!expect(result == TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_INVALID,
                "checksum corruption was not rejected")) return 8;
    if (!expect(loaded.table_id == 0u && loaded.claimed_pages == NULL,
                "invalid manifest leaked partial decoded state")) return 9;

    memset(scratch, 0, TINYDB_COMPACT_V2_MIGRATION_MANIFEST_FIXED_SIZE);
    if (!expect(write_bytes(manifest_path, scratch, 8u),
                "unable to write truncated manifest")) return 10;
    result = tinydb_compact_v2_migration_manifest_load_file(
        database, &loaded, claims_out,
        TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS,
        scratch, sizeof(scratch), message, sizeof(message));
    if (!expect(result == TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_INVALID,
                "truncated manifest was not rejected")) return 11;

    {
        FILE* f = fopen(manifest_path, "wb");
        if (!f) return 12;
        for (size_t i = 0u; i <= TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_ENCODED_SIZE; i++) {
            if (fputc(0, f) == EOF) { fclose(f); return 13; }
        }
        if (fclose(f) != 0) return 14;
    }
    result = tinydb_compact_v2_migration_manifest_load_file(
        database, &loaded, claims_out,
        TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS,
        scratch, sizeof(scratch), message, sizeof(message));
    if (!expect(result == TINYDB_COMPACT_V2_MIGRATION_MANIFEST_LOAD_INVALID,
                "oversized manifest was not rejected before decode")) return 15;

    if (remove(manifest_path) != 0) return 16;
    puts("missing_absent=yes");
    puts("valid_loaded=yes");
    puts("corrupt_rejected=yes");
    puts("oversized_rejected=yes");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="tinydb-manifest-file-") as tmp:
        tmp_path = Path(tmp)
        exe = configure_and_build(tmp_path, source)
        database = tmp_path / "database.db"
        run = subprocess.run(
            [str(exe), str(database)],
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="ignore",
            timeout=30,
        )
        if run.returncode != 0:
            raise AssertionError(run.stdout + run.stderr)
        for marker in [
            "missing_absent=yes",
            "valid_loaded=yes",
            "corrupt_rejected=yes",
            "oversized_rejected=yes",
        ]:
            if marker not in run.stdout:
                raise AssertionError(f"missing runtime marker {marker}: {run.stdout}")


def main():
    test_source_contract()
    run_probe()
    print("PASS: active compact-V2 migration sidecars load with bounded fail-closed reopen semantics")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, subprocess.SubprocessError) as exc:
        print("FAIL:", exc)
        sys.exit(1)

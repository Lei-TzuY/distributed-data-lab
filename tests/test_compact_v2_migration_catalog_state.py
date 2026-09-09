import os
import shutil
import subprocess
import sys
import tempfile


PROBE = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "compact_v2_migration_catalog_state.h"

static void set_table(Catalog* catalog, uint32_t slot, uint32_t root) {
    TableSchema* schema = &catalog->schemas[slot];
    memset(schema, 0, sizeof(*schema));
    schema->root_page_num = root;
    schema->num_columns = 1u;
    schema->columns[0].type = COL_TYPE_INT;
    schema->columns[0].size = 4u;
    schema->columns[0].offset = 0u;
    schema->row_size = 4u;
}

int main(void) {
    Catalog catalog;
    TinyDBSchemaCatalogGenerationSnapshot snapshot;
    Pager pager;
    TinyDBCompactV2MigrationCatalogState state;
    uint32_t root = 99u;
    uint64_t generation = 99u;

    memset(&catalog, 0, sizeof(catalog));
    memset(&pager, 0, sizeof(pager));
    catalog.num_tables = 2u;
    set_table(&catalog, 0u, 4u);
    set_table(&catalog, 1u, 17u);
    pager.num_pages = 32u;

    if (!tinydb_schema_catalog_generation_bootstrap_legacy(&catalog, &snapshot)) return 1;
    snapshot.entries[0].schema_generation = 7u;
    snapshot.entries[1].schema_generation = 19u;

    state.catalog = &catalog;
    state.snapshot = &snapshot;
    state.pager = &pager;

    if (!tinydb_compact_v2_migration_catalog_state_is_valid(&state)) return 2;
    if (!tinydb_compact_v2_migration_catalog_state_read(&state, 2u, &root, &generation)) return 3;
    if (root != 17u || generation != 19u) return 4;

    root = 55u;
    generation = 66u;
    if (tinydb_compact_v2_migration_catalog_state_read(&state, 999u, &root, &generation)) return 5;
    if (root != 0u || generation != 0u) return 6;

    snapshot.entries[1].root_page_num = 18u;
    root = 55u;
    generation = 66u;
    if (tinydb_compact_v2_migration_catalog_state_read(&state, 2u, &root, &generation)) return 7;
    if (root != 0u || generation != 0u) return 8;
    snapshot.entries[1].root_page_num = 17u;

    snapshot.entries[1].table_id = snapshot.entries[0].table_id;
    if (tinydb_compact_v2_migration_catalog_state_is_valid(&state)) return 9;
    snapshot.entries[1].table_id = 2u;

    pager.num_pages = 17u;
    root = 55u;
    generation = 66u;
    if (tinydb_compact_v2_migration_catalog_state_read(&state, 2u, &root, &generation)) return 10;
    if (root != 0u || generation != 0u) return 11;

    pager.num_pages = 32u;
    if (!tinydb_compact_v2_migration_catalog_state_read(&state, 1u, &root, &generation)) return 12;
    if (root != 4u || generation != 7u) return 13;

    puts("COMPACT_V2_MIGRATION_CATALOG_STATE_OK");
    return 0;
}
'''


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    src = os.path.join(repo, "src")
    with tempfile.TemporaryDirectory() as tmp:
        probe = os.path.join(tmp, "probe.c")
        with open(probe, "w", newline="\n") as handle:
            handle.write(PROBE)

        if os.name == "nt":
            compiler = shutil.which("cl")
            if compiler is None:
                print("SKIP: MSVC compiler not available")
                return 0
            exe = os.path.join(tmp, "probe.exe")
            command = [compiler, "/nologo", "/std:c11", "/W4", "/WX", f"/I{src}", probe, f"/Fe:{exe}"]
        else:
            compiler = shutil.which("gcc") or shutil.which("cc")
            if compiler is None:
                print("SKIP: C compiler not available")
                return 0
            exe = os.path.join(tmp, "probe")
            command = [
                compiler,
                "-std=c11",
                "-D_XOPEN_SOURCE=700",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-pthread",
                f"-I{src}",
                probe,
                "-o",
                exe,
            ]

        build = subprocess.run(command, capture_output=True, text=True, timeout=120)
        if build.returncode != 0:
            sys.stdout.write(build.stdout)
            sys.stderr.write(build.stderr)
            return build.returncode

        run = subprocess.run([exe], capture_output=True, text=True, timeout=120)
        sys.stdout.write(run.stdout)
        sys.stderr.write(run.stderr)
        if run.returncode != 0:
            return run.returncode
        if "COMPACT_V2_MIGRATION_CATALOG_STATE_OK" not in run.stdout:
            raise AssertionError("probe did not report success")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

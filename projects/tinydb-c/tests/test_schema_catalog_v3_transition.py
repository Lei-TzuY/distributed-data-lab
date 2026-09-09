import shutil
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
HEADER = ROOT / "src" / "schema_catalog_v3_transition.h"


def test_transition_contract_present():
    text = HEADER.read_text(encoding="utf-8")
    for token in [
        "tinydb_schema_catalog_v3_same_physical_schema(",
        "tinydb_schema_catalog_v3_derive_next_snapshot(",
        "schema_generation++",
        "TINYDB_SCHEMA_GENERATION_INITIAL",
    ]:
        assert token in text


def test_transition_compiles_and_executes():
    if shutil.which("cmake") is None:
        raise AssertionError("cmake is required")
    source = r'''#include "schema_catalog_v3_transition.h"
#include <stdio.h>
#include <string.h>

static void init_table(TableSchema* s, const char* name, uint32_t root,
                       uint32_t varchar_size) {
    memset(s, 0, sizeof(*s));
    strcpy(s->name, name);
    s->root_page_num = root;
    s->num_columns = varchar_size ? 2u : 1u;
    strcpy(s->columns[0].name, "id");
    s->columns[0].type = COL_TYPE_INT;
    s->columns[0].size = 4u;
    s->columns[0].offset = 0u;
    s->row_size = 4u;
    if (varchar_size) {
        strcpy(s->columns[1].name, "payload");
        s->columns[1].type = COL_TYPE_VARCHAR;
        s->columns[1].size = varchar_size;
        s->columns[1].offset = 4u;
        s->row_size += varchar_size;
    }
}

int main(void) {
    Catalog oldc, cur;
    TinyDBSchemaCatalogGenerationSnapshot oldsnap, next;
    memset(&oldc, 0, sizeof(oldc));
    oldc.num_tables = 2u;
    init_table(&oldc.schemas[0], "narrow", 4u, 0u);
    init_table(&oldc.schemas[1], "wide", 17u, 512u);
    if (!tinydb_schema_catalog_generation_bootstrap_legacy(&oldc, &oldsnap)) return 1;

    cur = oldc;
    strcpy(cur.schemas[0].name, "renamed_narrow");
    cur.schemas[1].root_page_num = 23u;
    cur.schemas[1].columns[1].size = 1024u;
    cur.schemas[1].row_size = 1028u;
    cur.num_tables = 3u;
    init_table(&cur.schemas[2], "events", 31u, 64u);

    if (!tinydb_schema_catalog_v3_derive_next_snapshot(&oldc, &oldsnap, &cur, &next)) return 2;
    if (next.entries[0].table_id != 1u || next.entries[0].schema_generation != 1u) return 3;
    if (next.entries[1].table_id != 2u || next.entries[1].root_page_num != 23u ||
        next.entries[1].schema_generation != 2u) return 4;
    if (next.entries[2].table_id != 3u || next.entries[2].schema_generation != 1u) return 5;

    Catalog dropped = cur;
    dropped.num_tables = 1u;
    if (tinydb_schema_catalog_v3_derive_next_snapshot(&oldc, &oldsnap, &dropped, &next)) return 6;

    oldsnap.entries[1].schema_generation = UINT64_MAX;
    if (tinydb_schema_catalog_v3_derive_next_snapshot(&oldc, &oldsnap, &cur, &next)) return 7;

    puts("rename_preserves_generation=yes");
    puts("root_shape_change_advances_once=yes");
    puts("append_identity=yes");
    puts("drop_overflow_fail_closed=yes");
    return 0;
}
'''
    with tempfile.TemporaryDirectory(prefix="tinydb-v3-transition-") as tmp:
        tmp_path = Path(tmp)
        (tmp_path / "probe.c").write_text(source, encoding="utf-8")
        (tmp_path / "CMakeLists.txt").write_text(
            "cmake_minimum_required(VERSION 3.10)\nproject(TinyDBV3Transition C)\n"
            "set(CMAKE_C_STANDARD 99)\nset(CMAKE_C_STANDARD_REQUIRED TRUE)\n"
            "if(MSVC)\n  add_compile_options(/W4 /WX /utf-8)\nelse()\n  add_compile_options(-Wall -Wextra -Werror)\nendif()\n"
            "add_executable(v3_transition_probe probe.c)\n"
            f'target_include_directories(v3_transition_probe PRIVATE "{(ROOT / "src").as_posix()}")\n',
            encoding="utf-8",
        )
        build = tmp_path / "build"
        cfg = subprocess.run(["cmake", "-S", str(tmp_path), "-B", str(build)], capture_output=True, text=True, timeout=60)
        assert cfg.returncode == 0, cfg.stdout + cfg.stderr
        comp = subprocess.run(["cmake", "--build", str(build), "--config", "Debug"], capture_output=True, text=True, timeout=120)
        assert comp.returncode == 0, comp.stdout + comp.stderr
        exe = build / ("Debug/v3_transition_probe.exe" if shutil.which("cl") else "v3_transition_probe")
        run = subprocess.run([str(exe)], capture_output=True, text=True, timeout=30)
        assert run.returncode == 0, run.stdout + run.stderr
        for token in ["rename_preserves_generation=yes", "root_shape_change_advances_once=yes", "append_identity=yes", "drop_overflow_fail_closed=yes"]:
            assert token in run.stdout

import os
import shutil
import subprocess
import sys
import tempfile


PROBE = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "schema_repack.h"

static void set_column(TableColumn* column,
                       const char* name,
                       ColumnType type,
                       uint32_t offset,
                       uint32_t size) {
    memset(column, 0, sizeof(*column));
    snprintf(column->name, sizeof(column->name), "%s", name);
    column->type = type;
    column->offset = offset;
    column->size = size;
}

static void make_schema(TableSchema* schema, uint32_t title_size, uint32_t body_size) {
    memset(schema, 0, sizeof(*schema));
    snprintf(schema->name, sizeof(schema->name), "%s", "archive");
    schema->num_columns = 3u;
    set_column(&schema->columns[0], "id", COL_TYPE_INT, 0u, 4u);
    set_column(&schema->columns[1], "title", COL_TYPE_VARCHAR, 4u, title_size);
    set_column(&schema->columns[2], "body", COL_TYPE_VARCHAR, 4u + title_size, body_size);
    schema->row_size = 4u + title_size + body_size;
}

int main(void) {
    TableSchema source_schema;
    TableSchema destination_schema;
    TableSchema invalid_schema;
    TinyDBRecordPayload source;
    TinyDBRecordPayload destination;
    char message[160];
    uint32_t id = 42u;

    make_schema(&source_schema, 32u, 64u);
    make_schema(&destination_schema, 128u, 256u);

    memset(&source, 0, sizeof(source));
    source.length = source_schema.row_size;
    memcpy(source.bytes + source_schema.columns[0].offset, &id, sizeof(id));
    memcpy(source.bytes + source_schema.columns[1].offset, "hello", 6u);
    memcpy(source.bytes + source_schema.columns[2].offset, "world", 6u);

    if (!tinydb_schema_repack_widening_supported(&source_schema,
                                                 &destination_schema,
                                                 message,
                                                 sizeof(message))) return 1;
    if (!tinydb_schema_repack_widen_payload(&source_schema,
                                            &destination_schema,
                                            &source,
                                            &destination,
                                            message,
                                            sizeof(message))) return 2;
    if (destination.length != destination_schema.row_size) return 3;

    uint32_t decoded_id = 0u;
    memcpy(&decoded_id, destination.bytes, sizeof(decoded_id));
    if (decoded_id != 42u) return 4;
    if (strcmp((const char*)destination.bytes + destination_schema.columns[1].offset,
               "hello") != 0) return 5;
    if (strcmp((const char*)destination.bytes + destination_schema.columns[2].offset,
               "world") != 0) return 6;

    for (uint32_t i = source_schema.columns[1].size;
         i < destination_schema.columns[1].size;
         i++) {
        if (destination.bytes[destination_schema.columns[1].offset + i] != 0u) return 7;
    }
    for (uint32_t i = source_schema.columns[2].size;
         i < destination_schema.columns[2].size;
         i++) {
        if (destination.bytes[destination_schema.columns[2].offset + i] != 0u) return 8;
    }

    invalid_schema = destination_schema;
    invalid_schema.columns[1].size = 16u;
    invalid_schema.columns[2].offset = 20u;
    invalid_schema.row_size = 20u + invalid_schema.columns[2].size;
    if (tinydb_schema_repack_widening_supported(&source_schema,
                                                &invalid_schema,
                                                message,
                                                sizeof(message))) return 9;
    if (strstr(message, "narrowing") == NULL) return 10;

    invalid_schema = destination_schema;
    snprintf(invalid_schema.columns[1].name,
             sizeof(invalid_schema.columns[1].name),
             "%s",
             "renamed_title");
    if (tinydb_schema_repack_widening_supported(&source_schema,
                                                &invalid_schema,
                                                message,
                                                sizeof(message))) return 11;

    source.bytes[source_schema.columns[2].offset + source_schema.columns[2].size - 1u] = 'x';
    memset(source.bytes + source_schema.columns[2].offset, 'x', source_schema.columns[2].size);
    destination.length = 99u;
    destination.bytes[0] = 0xffu;
    if (tinydb_schema_repack_widen_payload(&source_schema,
                                           &destination_schema,
                                           &source,
                                           &destination,
                                           message,
                                           sizeof(message))) return 12;
    if (destination.length != 0u || destination.bytes[0] != 0u) return 13;
    if (strstr(message, "NUL-terminated") == NULL) return 14;

    source.length = source_schema.row_size - 1u;
    if (tinydb_schema_repack_widen_payload(&source_schema,
                                           &destination_schema,
                                           &source,
                                           &destination,
                                           message,
                                           sizeof(message))) return 15;
    if (destination.length != 0u) return 16;

    puts("SCHEMA_REPACK_WIDENING_OK");
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
            command = [
                compiler,
                "/nologo",
                "/std:c11",
                "/W4",
                "/WX",
                f"/I{src}",
                probe,
                f"/Fe:{exe}",
            ]
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
        if "SCHEMA_REPACK_WIDENING_OK" not in run.stdout:
            raise AssertionError("probe did not report success")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

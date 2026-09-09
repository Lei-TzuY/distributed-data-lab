import os
import shutil
import subprocess
import sys
import tempfile


PROBE = r'''
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "schema_repack_staging.h"

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

static void make_payload(const TableSchema* schema,
                         uint32_t id,
                         TinyDBRecordPayload* payload) {
    memset(payload, 0, sizeof(*payload));
    payload->length = schema->row_size;
    memcpy(payload->bytes, &id, sizeof(id));
    memset(payload->bytes + schema->columns[1].offset,
           (int)('a' + (id % 26u)),
           schema->columns[1].size - 1u);
    payload->bytes[schema->columns[1].offset + schema->columns[1].size - 1u] = 0u;
    memset(payload->bytes + schema->columns[2].offset,
           (int)('A' + (id % 26u)),
           schema->columns[2].size - 1u);
    payload->bytes[schema->columns[2].offset + schema->columns[2].size - 1u] = 0u;
}

static int verify_chain(const TinyDBCompactV2StagingLeafChain* chain,
                        const TableSchema* destination_schema,
                        uint32_t expected_rows) {
    uint32_t expected_key = 1u;
    for (uint32_t page_index = 0u; page_index < chain->page_count; page_index++) {
        const unsigned char* page = tinydb_compact_v2_staging_page_const(chain, page_index);
        uint32_t count = 0u;
        if (page == NULL || !tinydb_leaf_page_count(page, PAGE_SIZE, &count)) return 1;
        for (uint32_t cell = 0u; cell < count; cell++) {
            uint32_t key = 0u;
            const void* value = NULL;
            uint32_t value_length = 0u;
            TinyDBRecordPayload decoded;
            if (!tinydb_leaf_page_key_at(page, PAGE_SIZE, cell, &key) ||
                !tinydb_leaf_page_value_at(page, PAGE_SIZE, cell, &value, &value_length) ||
                key != expected_key ||
                !tinydb_row_envelope_decode_compact_v2(destination_schema,
                                                        (const unsigned char*)value,
                                                        value_length,
                                                        &decoded)) {
                return 2;
            }
            uint32_t decoded_key = 0u;
            memcpy(&decoded_key, decoded.bytes, sizeof(decoded_key));
            if (decoded_key != key || decoded.length != destination_schema->row_size) return 3;

            const TableColumn* title = &destination_schema->columns[1];
            const TableColumn* body = &destination_schema->columns[2];
            unsigned char expected_title = (unsigned char)('a' + (key % 26u));
            unsigned char expected_body = (unsigned char)('A' + (key % 26u));
            for (uint32_t i = 0u; i < 31u; i++) {
                if (decoded.bytes[title->offset + i] != expected_title) return 4;
            }
            if (decoded.bytes[title->offset + 31u] != 0u) return 5;
            for (uint32_t i = 32u; i < title->size; i++) {
                if (decoded.bytes[title->offset + i] != 0u) return 6;
            }
            for (uint32_t i = 0u; i < 63u; i++) {
                if (decoded.bytes[body->offset + i] != expected_body) return 7;
            }
            if (decoded.bytes[body->offset + 63u] != 0u) return 8;
            for (uint32_t i = 64u; i < body->size; i++) {
                if (decoded.bytes[body->offset + i] != 0u) return 9;
            }
            expected_key++;
        }
    }
    return expected_key == expected_rows + 1u ? 0 : 10;
}

int main(void) {
    TableSchema source_schema;
    TableSchema destination_schema;
    TableSchema drifted_schema;
    TableSchema narrowing_schema;
    TinyDBRecordPayload payload;
    TinyDBCompactV2StagingLeafChain chain;
    TinyDBSchemaRepackStaging staging;
    unsigned char pages[8u * PAGE_SIZE];
    const uint32_t page_numbers[8] = {101u, 102u, 103u, 104u, 105u, 106u, 107u, 108u};
    char message[192];

    make_schema(&source_schema, 32u, 64u);
    make_schema(&destination_schema, 128u, 256u);
    memset(pages, 0, sizeof(pages));
    if (!tinydb_compact_v2_staging_leaf_chain_init(&chain,
                                                    pages,
                                                    page_numbers,
                                                    8u)) return 1;
    if (!tinydb_schema_repack_staging_init(&staging,
                                           &source_schema,
                                           &destination_schema,
                                           &chain,
                                           message,
                                           sizeof(message))) return 2;

    for (uint32_t id = 1u; id <= 80u; id++) {
        make_payload(&source_schema, id, &payload);
        if (!tinydb_schema_repack_staging_visit(&source_schema, &payload, &staging)) {
            fprintf(stderr, "row %u staging failed: %s\n", id, staging.message);
            return 3;
        }
    }
    if (!tinydb_schema_repack_staging_finish(&staging, 80u, message, sizeof(message))) return 4;
    if (chain.row_count != 80u || staging.rows_staged != 80u || chain.page_count < 2u) return 5;
    if (!tinydb_compact_v2_staging_leaf_chain_validate(&chain)) return 6;
    int verified = verify_chain(&chain, &destination_schema, 80u);
    if (verified != 0) return 10 + verified;

    /* Source-schema drift must fail before appending another row. */
    drifted_schema = source_schema;
    snprintf(drifted_schema.columns[1].name,
             sizeof(drifted_schema.columns[1].name),
             "%s",
             "renamed_title");
    make_payload(&source_schema, 81u, &payload);
    uint64_t before_rows = chain.row_count;
    if (tinydb_schema_repack_staging_visit(&drifted_schema, &payload, &staging)) return 30;
    if (!staging.failed || chain.row_count != before_rows ||
        strstr(staging.message, "drifted") == NULL) return 31;
    if (tinydb_schema_repack_staging_finish(&staging, 80u, message, sizeof(message))) return 32;

    /* A destination narrowing must be rejected before staging starts. */
    narrowing_schema = source_schema;
    narrowing_schema.columns[1].size = 16u;
    narrowing_schema.columns[2].offset = 20u;
    narrowing_schema.row_size = 20u + narrowing_schema.columns[2].size;
    memset(pages, 0, sizeof(pages));
    if (!tinydb_compact_v2_staging_leaf_chain_init(&chain,
                                                    pages,
                                                    page_numbers,
                                                    8u)) return 33;
    if (tinydb_schema_repack_staging_init(&staging,
                                          &source_schema,
                                          &narrowing_schema,
                                          &chain,
                                          message,
                                          sizeof(message))) return 34;
    if (strstr(message, "narrowing") == NULL || chain.row_count != 0u) return 35;

    /* Ordering rejection leaves only the already accepted private prefix. */
    memset(pages, 0, sizeof(pages));
    if (!tinydb_compact_v2_staging_leaf_chain_init(&chain,
                                                    pages,
                                                    page_numbers,
                                                    8u)) return 36;
    if (!tinydb_schema_repack_staging_init(&staging,
                                           &source_schema,
                                           &destination_schema,
                                           &chain,
                                           message,
                                           sizeof(message))) return 37;
    make_payload(&source_schema, 2u, &payload);
    if (!tinydb_schema_repack_staging_visit(&source_schema, &payload, &staging)) return 38;
    make_payload(&source_schema, 1u, &payload);
    if (tinydb_schema_repack_staging_visit(&source_schema, &payload, &staging)) return 39;
    if (!staging.failed || chain.row_count != 1u || staging.rows_staged != 1u) return 40;

    puts("SCHEMA_REPACK_STAGING_OK");
    return 0;
}
'''


def main():
    repo = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    src = os.path.join(repo, "src")
    support_sources = [
        os.path.join(src, "slotted_leaf_v2.c"),
        os.path.join(src, "leaf_format.c"),
        os.path.join(src, "leaf_page_access.c"),
    ]
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
                *support_sources,
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
                *support_sources,
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
        if "SCHEMA_REPACK_STAGING_OK" not in run.stdout:
            raise AssertionError("probe did not report success")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

import os
import shutil
import subprocess
import sys
import tempfile


PROBE = r'''
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "schema_repack_table_scan.h"

NodeType get_node_type(void* node) {
    return (NodeType)*((uint8_t*)((char*)node + NODE_TYPE_OFFSET));
}
void set_node_type(void* node, NodeType type) {
    *((uint8_t*)((char*)node + NODE_TYPE_OFFSET)) = (uint8_t)type;
}
bool is_node_root(void* node) {
    return (bool)*((uint8_t*)((char*)node + IS_ROOT_OFFSET));
}
void set_node_root(void* node, bool is_root) {
    *((uint8_t*)((char*)node + IS_ROOT_OFFSET)) = (uint8_t)is_root;
}
uint32_t* node_parent(void* node) {
    return (uint32_t*)((char*)node + PARENT_POINTER_OFFSET);
}
uint32_t* internal_node_num_keys(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_NUM_KEYS_OFFSET);
}
uint32_t* internal_node_right_child(void* node) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_RIGHT_CHILD_OFFSET);
}
uint32_t* internal_node_cell(void* node, uint32_t cell_num) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_HEADER_SIZE +
                       cell_num * INTERNAL_NODE_CELL_SIZE);
}
uint32_t* internal_node_child(void* node, uint32_t child_num) {
    uint32_t count = *internal_node_num_keys(node);
    if (child_num > count) abort();
    return child_num == count ? internal_node_right_child(node)
                              : internal_node_cell(node, child_num);
}
uint32_t* internal_node_key(void* node, uint32_t key_num) {
    return (uint32_t*)((char*)internal_node_cell(node, key_num) +
                       INTERNAL_NODE_CHILD_SIZE);
}

static uint32_t g_rows = 0u;
static uint32_t g_fail_after = UINT32_MAX;

uint32_t tinydb_record_payload_try_scan(Table* table,
                                        const TableSchema* schema,
                                        TinyDBRecordPayloadVisitor visitor,
                                        void* context,
                                        bool* scan_complete,
                                        char* message,
                                        size_t message_size) {
    (void)table;
    if (scan_complete != NULL) *scan_complete = false;
    if (message != NULL && message_size > 0u) message[0] = '\0';
    for (uint32_t id = 1u; id <= g_rows; id++) {
        TinyDBRecordPayload payload;
        memset(&payload, 0, sizeof(payload));
        payload.length = schema->row_size;
        memcpy(payload.bytes, &id, sizeof(id));
        snprintf((char*)payload.bytes + schema->columns[1].offset,
                 schema->columns[1].size, "title-%u", id);
        snprintf((char*)payload.bytes + schema->columns[2].offset,
                 schema->columns[2].size, "body-%u", id);
        if (id > g_fail_after) {
            if (message != NULL && message_size > 0u) {
                snprintf(message, message_size, "%s", "synthetic late scan failure");
            }
            return 0u;
        }
        if (visitor != NULL && !visitor(schema, &payload, context)) {
            if (scan_complete != NULL) *scan_complete = true;
            return id;
        }
    }
    if (scan_complete != NULL) *scan_complete = true;
    return g_rows;
}

static void set_column(TableColumn* column, const char* name, ColumnType type,
                       uint32_t offset, uint32_t size) {
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
    set_column(&schema->columns[2], "body", COL_TYPE_VARCHAR,
               4u + title_size, body_size);
    schema->row_size = 4u + title_size + body_size;
}

static int run_complete_scan(void) {
    Table table;
    TableSchema source, destination;
    TinyDBCompactV2StagingLeafChain leaves;
    TinyDBSchemaRepackStaging staging;
    TinyDBCompactV2StagingHierarchy hierarchy;
    TinyDBSchemaRepackStagingTreeResult result;
    unsigned char leaf_images[16u * PAGE_SIZE];
    unsigned char internal_images[16u * PAGE_SIZE];
    const uint32_t leaf_pages[16] = {
        101u,102u,103u,104u,105u,106u,107u,108u,
        109u,110u,111u,112u,113u,114u,115u,116u
    };
    const uint32_t internal_pages[16] = {
        201u,202u,203u,204u,205u,206u,207u,208u,
        209u,210u,211u,212u,213u,214u,215u,216u
    };
    char message[192];

    memset(&table, 0, sizeof(table));
    table.pager = (Pager*)(uintptr_t)1u;
    make_schema(&source, 32u, 64u);
    make_schema(&destination, 128u, 256u);
    memset(leaf_images, 0, sizeof(leaf_images));
    memset(internal_images, 0, sizeof(internal_images));
    if (!tinydb_compact_v2_staging_leaf_chain_init(
            &leaves, leaf_images, leaf_pages, 16u)) return 1;

    g_rows = 120u;
    g_fail_after = UINT32_MAX;
    if (!tinydb_schema_repack_stage_table_scan(
            &table, &source, &destination, &leaves, &staging, &hierarchy,
            internal_images, internal_pages, 16u, &result,
            message, sizeof(message))) return 2;
    if (!result.ready || result.row_count != 120u ||
        result.leaf_page_count < 2u || result.internal_page_count == 0u ||
        result.root_page_num != hierarchy.root_page_num ||
        staging.rows_staged != 120u || leaves.row_count != 120u) return 3;
    return 0;
}

static int reject_late_scan_failure(void) {
    Table table;
    TableSchema source, destination;
    TinyDBCompactV2StagingLeafChain leaves;
    TinyDBSchemaRepackStaging staging;
    TinyDBCompactV2StagingHierarchy hierarchy;
    TinyDBSchemaRepackStagingTreeResult result;
    unsigned char leaf_images[8u * PAGE_SIZE];
    unsigned char internal_images[8u * PAGE_SIZE];
    const uint32_t leaf_pages[8] = {301u,302u,303u,304u,305u,306u,307u,308u};
    const uint32_t internal_pages[8] = {401u,402u,403u,404u,405u,406u,407u,408u};
    char message[192];

    memset(&table, 0, sizeof(table));
    table.pager = (Pager*)(uintptr_t)1u;
    make_schema(&source, 32u, 64u);
    make_schema(&destination, 128u, 256u);
    memset(leaf_images, 0, sizeof(leaf_images));
    memset(internal_images, 0, sizeof(internal_images));
    if (!tinydb_compact_v2_staging_leaf_chain_init(
            &leaves, leaf_images, leaf_pages, 8u)) return 1;

    memset(&result, 0xA5, sizeof(result));
    g_rows = 80u;
    g_fail_after = 20u;
    if (tinydb_schema_repack_stage_table_scan(
            &table, &source, &destination, &leaves, &staging, &hierarchy,
            internal_images, internal_pages, 8u, &result,
            message, sizeof(message))) return 2;
    if (result.ready || result.root_page_num != 0u || result.row_count != 0u ||
        staging.rows_staged != 20u || leaves.row_count != 20u ||
        hierarchy.built || strstr(message, "late scan failure") == NULL) return 3;
    return 0;
}

int main(void) {
    int rc = run_complete_scan();
    if (rc != 0) return 10 + rc;
    rc = reject_late_scan_failure();
    if (rc != 0) return 30 + rc;
    puts("SCHEMA_REPACK_TABLE_SCAN_OK");
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
            command = [compiler, "/nologo", "/std:c11", "/W4", "/WX",
                       f"/I{src}", probe, *support_sources, f"/Fe:{exe}"]
        else:
            compiler = shutil.which("gcc") or shutil.which("cc")
            if compiler is None:
                print("SKIP: C compiler not available")
                return 0
            exe = os.path.join(tmp, "probe")
            command = [compiler, "-std=c11", "-D_XOPEN_SOURCE=700",
                       "-Wall", "-Wextra", "-Werror", "-pthread",
                       f"-I{src}", probe, *support_sources, "-o", exe]
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
        if "SCHEMA_REPACK_TABLE_SCAN_OK" not in run.stdout:
            raise AssertionError("probe did not report success")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

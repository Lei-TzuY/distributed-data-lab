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
#include "schema_repack_table_durable.h"

static unsigned char pager_pages[64u * PAGE_SIZE];
static uint32_t commit_calls;
static uint32_t checkpoint_calls;
static uint32_t get_page_calls;
static uint32_t g_rows;
static uint32_t g_fail_after;

uint32_t get_unused_page_num(Pager* pager) { return pager->num_pages; }
void* get_page(Pager* pager, uint32_t page_num) {
    get_page_calls++;
    if (pager == NULL || page_num >= 64u) return NULL;
    if (page_num >= pager->num_pages) pager->num_pages = page_num + 1u;
    return pager_pages + (size_t)page_num * PAGE_SIZE;
}
void mark_page_dirty(Pager* pager, uint32_t page_num) {
    if (pager != NULL && pager->is_dirty != NULL && page_num < pager->page_capacity)
        pager->is_dirty[page_num] = true;
}
void pager_commit(Pager* pager) {
    if (pager == NULL || !pager->in_transaction) return;
    commit_calls++;
    for (uint32_t i = 0u; i < pager->page_capacity; i++) pager->is_dirty[i] = false;
    pager->in_transaction = false;
}
void pager_checkpoint(Pager* pager) { if (pager != NULL) checkpoint_calls++; }

NodeType get_node_type(void* node) { return (NodeType)*((uint8_t*)((char*)node + NODE_TYPE_OFFSET)); }
void set_node_type(void* node, NodeType type) { *((uint8_t*)((char*)node + NODE_TYPE_OFFSET)) = (uint8_t)type; }
bool is_node_root(void* node) { return (bool)*((uint8_t*)((char*)node + IS_ROOT_OFFSET)); }
void set_node_root(void* node, bool value) { *((uint8_t*)((char*)node + IS_ROOT_OFFSET)) = (uint8_t)value; }
uint32_t* node_parent(void* node) { return (uint32_t*)((char*)node + PARENT_POINTER_OFFSET); }
uint32_t* internal_node_num_keys(void* node) { return (uint32_t*)((char*)node + INTERNAL_NODE_NUM_KEYS_OFFSET); }
uint32_t* internal_node_right_child(void* node) { return (uint32_t*)((char*)node + INTERNAL_NODE_RIGHT_CHILD_OFFSET); }
uint32_t* internal_node_cell(void* node, uint32_t cell) {
    return (uint32_t*)((char*)node + INTERNAL_NODE_HEADER_SIZE + cell * INTERNAL_NODE_CELL_SIZE);
}
uint32_t* internal_node_child(void* node, uint32_t child) {
    uint32_t n = *internal_node_num_keys(node);
    if (child > n) abort();
    return child == n ? internal_node_right_child(node) : internal_node_cell(node, child);
}
uint32_t* internal_node_key(void* node, uint32_t key) {
    return (uint32_t*)((char*)internal_node_cell(node, key) + INTERNAL_NODE_CHILD_SIZE);
}

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
            if (message != NULL && message_size > 0u)
                snprintf(message, message_size, "%s", "synthetic late scan failure");
            return 0u;
        }
        if (visitor != NULL && !visitor(schema, &payload, context)) return 0u;
    }
    if (scan_complete != NULL) *scan_complete = true;
    return g_rows;
}

static void column(TableColumn* c, const char* name, ColumnType type, uint32_t off, uint32_t size) {
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->type = type; c->offset = off; c->size = size;
}
static void schema(TableSchema* s, uint32_t title, uint32_t body) {
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", "archive");
    s->num_columns = 3u;
    column(&s->columns[0], "id", COL_TYPE_INT, 0u, 4u);
    column(&s->columns[1], "title", COL_TYPE_VARCHAR, 4u, title);
    column(&s->columns[2], "body", COL_TYPE_VARCHAR, 4u + title, body);
    s->row_size = 4u + title + body;
}

static void reset_pager(Pager* pager, bool* dirty) {
    memset(pager, 0, sizeof(*pager));
    memset(dirty, 0, 64u * sizeof(*dirty));
    memset(pager_pages, 0, sizeof(pager_pages));
    pager->in_transaction = true;
    pager->num_pages = 1u;
    pager->page_capacity = 64u;
    pager->is_dirty = dirty;
    commit_calls = checkpoint_calls = get_page_calls = 0u;
}

int main(void) {
    Table table;
    Pager pager;
    bool dirty[64];
    TableSchema source, destination;
    TinyDBCompactV2StagingLeafChain leaves;
    TinyDBSchemaRepackStaging staging;
    TinyDBCompactV2StagingHierarchy hierarchy;
    TinyDBSchemaRepackDurableStageResult result;
    unsigned char leaf_images[16u * PAGE_SIZE];
    unsigned char internal_images[16u * PAGE_SIZE];
    const uint32_t private_pages[16] = {
        101u,102u,103u,104u,105u,106u,107u,108u,
        109u,110u,111u,112u,113u,114u,115u,116u
    };
    uint32_t claimed[32];
    char message[192];

    memset(&table, 0, sizeof(table));
    table.pager = &pager;
    schema(&source, 32u, 64u);
    schema(&destination, 128u, 256u);

    reset_pager(&pager, dirty);
    memset(leaf_images, 0, sizeof(leaf_images));
    memset(internal_images, 0, sizeof(internal_images));
    if (!tinydb_compact_v2_staging_leaf_chain_init(&leaves, leaf_images, private_pages, 16u)) return 1;
    g_rows = 120u; g_fail_after = UINT32_MAX;
    if (!tinydb_schema_repack_table_make_durable_unpublished(
            &table, &source, &destination, &leaves, &staging, &hierarchy,
            internal_images, 16u, claimed, 32u, &result, message, sizeof(message))) {
        fprintf(stderr, "%s\n", message); return 2;
    }
    if (!result.ready || result.row_count != 120u || result.leaf_page_count < 2u ||
        result.internal_page_count == 0u || result.root_page_num != hierarchy.root_page_num ||
        result.claimed_page_count != result.leaf_page_count + result.internal_page_count ||
        commit_calls != 1u || checkpoint_calls != 1u || pager.in_transaction ||
        staging.rows_staged != 120u || leaves.row_count != 120u) return 3;

    reset_pager(&pager, dirty);
    memset(leaf_images, 0, sizeof(leaf_images));
    memset(internal_images, 0, sizeof(internal_images));
    if (!tinydb_compact_v2_staging_leaf_chain_init(&leaves, leaf_images, private_pages, 16u)) return 4;
    memset(&result, 0xA5, sizeof(result));
    g_rows = 80u; g_fail_after = 20u;
    if (tinydb_schema_repack_table_make_durable_unpublished(
            &table, &source, &destination, &leaves, &staging, &hierarchy,
            internal_images, 16u, claimed, 32u, &result, message, sizeof(message))) return 5;
    if (result.ready || result.root_page_num != 0u || result.row_count != 0u ||
        staging.rows_staged != 20u || leaves.row_count != 20u ||
        get_page_calls != 0u || commit_calls != 0u || checkpoint_calls != 0u ||
        !pager.in_transaction || strstr(message, "late scan failure") == NULL) return 6;

    puts("SCHEMA_REPACK_TABLE_DURABLE_OK");
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
            sys.stdout.write(build.stdout); sys.stderr.write(build.stderr); return build.returncode
        run = subprocess.run([exe], capture_output=True, text=True, timeout=120)
        sys.stdout.write(run.stdout); sys.stderr.write(run.stderr)
        if run.returncode != 0: return run.returncode
        if "SCHEMA_REPACK_TABLE_DURABLE_OK" not in run.stdout:
            raise AssertionError("probe did not report success")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

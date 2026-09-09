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
#include "schema_repack_table_migration_prepare.h"

static unsigned char pager_pages[64u * PAGE_SIZE];
static unsigned char manifest_bytes[2048u];
static size_t manifest_length;
static uint32_t commit_calls, checkpoint_calls, get_page_calls;
static uint32_t write_temp_calls, sync_temp_calls, publish_temp_calls, sync_parent_calls;
static uint32_t g_rows, g_fail_after;
static bool g_fail_sync_parent;

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

static bool write_temp(void* context, const unsigned char* data, size_t length) {
    (void)context;
    write_temp_calls++;
    if (data == NULL || length > sizeof(manifest_bytes)) return false;
    memcpy(manifest_bytes, data, length);
    manifest_length = length;
    return true;
}
static bool sync_temp(void* context) { (void)context; sync_temp_calls++; return true; }
static bool publish_temp(void* context) { (void)context; publish_temp_calls++; return true; }
static bool sync_parent(void* context) {
    (void)context;
    sync_parent_calls++;
    return !g_fail_sync_parent;
}

static void set_column(TableColumn* c, const char* name, ColumnType type, uint32_t offset, uint32_t size) {
    memset(c, 0, sizeof(*c));
    snprintf(c->name, sizeof(c->name), "%s", name);
    c->type = type; c->offset = offset; c->size = size;
}
static void set_schema(TableSchema* s, uint32_t title_size, uint32_t body_size) {
    memset(s, 0, sizeof(*s));
    snprintf(s->name, sizeof(s->name), "%s", "archive");
    s->num_columns = 3u;
    set_column(&s->columns[0], "id", COL_TYPE_INT, 0u, 4u);
    set_column(&s->columns[1], "title", COL_TYPE_VARCHAR, 4u, title_size);
    set_column(&s->columns[2], "body", COL_TYPE_VARCHAR, 4u + title_size, body_size);
    s->row_size = 4u + title_size + body_size;
}
static void reset(Pager* pager, bool* dirty) {
    memset(pager, 0, sizeof(*pager));
    memset(dirty, 0, 64u * sizeof(*dirty));
    memset(pager_pages, 0, sizeof(pager_pages));
    memset(manifest_bytes, 0, sizeof(manifest_bytes));
    manifest_length = 0u;
    pager->in_transaction = true;
    pager->num_pages = 1u;
    pager->page_capacity = 64u;
    pager->is_dirty = dirty;
    commit_calls = checkpoint_calls = get_page_calls = 0u;
    write_temp_calls = sync_temp_calls = publish_temp_calls = sync_parent_calls = 0u;
    g_fail_sync_parent = false;
}

int main(void) {
    Table table;
    Pager pager;
    bool dirty[64];
    TableSchema source, destination;
    TinyDBCompactV2StagingLeafChain leaves;
    TinyDBSchemaRepackStaging staging;
    TinyDBCompactV2StagingHierarchy hierarchy;
    TinyDBSchemaRepackTableMigrationPrepareResult result;
    unsigned char leaf_images[16u * PAGE_SIZE];
    unsigned char internal_images[16u * PAGE_SIZE];
    unsigned char encode_scratch[2048u];
    const uint32_t private_pages[16] = {
        101u,102u,103u,104u,105u,106u,107u,108u,
        109u,110u,111u,112u,113u,114u,115u,116u
    };
    uint32_t claimed[32];
    char message[192];
    TinyDBCompactV2MigrationManifestStoreOps ops = {
        NULL, write_temp, sync_temp, publish_temp, sync_parent
    };

    memset(&table, 0, sizeof(table));
    table.pager = &pager;
    set_schema(&source, 32u, 64u);
    set_schema(&destination, 128u, 256u);

    reset(&pager, dirty);
    memset(leaf_images, 0, sizeof(leaf_images));
    memset(internal_images, 0, sizeof(internal_images));
    if (!tinydb_compact_v2_staging_leaf_chain_init(&leaves, leaf_images, private_pages, 16u)) return 1;
    g_rows = 120u; g_fail_after = UINT32_MAX;
    if (!tinydb_schema_repack_table_prepare_migration(
            &table, &source, &destination, &leaves, &staging, &hierarchy,
            internal_images, 16u, claimed, 32u, 7u, 77u, 12u, 13u,
            encode_scratch, sizeof(encode_scratch), &ops, &result, message, sizeof(message))) {
        fprintf(stderr, "%s\n", message); return 2;
    }
    if (!result.ready || !result.destination_durable || !result.recovery_intent_durable ||
        !result.durable_stage.ready || !result.intent.ready || result.durable_stage.row_count != 120u ||
        result.intent.manifest.table_id != 7u || result.intent.manifest.old_root_page_num != 77u ||
        result.intent.manifest.staged_root_page_num != result.durable_stage.root_page_num ||
        result.intent.manifest.old_schema_generation != 12u || result.intent.manifest.new_schema_generation != 13u ||
        result.intent.manifest.phase != TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED ||
        result.intent.manifest.claimed_page_count != result.durable_stage.claimed_page_count ||
        manifest_length == 0u || commit_calls != 1u || checkpoint_calls != 1u ||
        write_temp_calls != 1u || sync_temp_calls != 1u || publish_temp_calls != 1u || sync_parent_calls != 1u)
        return 3;

    /* Incomplete authoritative scan: no Pager or manifest side effects. */
    reset(&pager, dirty);
    memset(leaf_images, 0, sizeof(leaf_images));
    memset(internal_images, 0, sizeof(internal_images));
    if (!tinydb_compact_v2_staging_leaf_chain_init(&leaves, leaf_images, private_pages, 16u)) return 4;
    g_rows = 80u; g_fail_after = 20u;
    memset(&result, 0xA5, sizeof(result));
    if (tinydb_schema_repack_table_prepare_migration(
            &table, &source, &destination, &leaves, &staging, &hierarchy,
            internal_images, 16u, claimed, 32u, 7u, 77u, 12u, 13u,
            encode_scratch, sizeof(encode_scratch), &ops, &result, message, sizeof(message))) return 5;
    if (result.ready || result.destination_durable || result.recovery_intent_durable ||
        result.durable_stage.ready || result.intent.ready || get_page_calls != 0u ||
        commit_calls != 0u || checkpoint_calls != 0u || write_temp_calls != 0u ||
        sync_temp_calls != 0u || publish_temp_calls != 0u || sync_parent_calls != 0u ||
        strstr(message, "late scan failure") == NULL) return 6;

    /* Manifest durability failure must surface the already durable destination. */
    reset(&pager, dirty);
    memset(leaf_images, 0, sizeof(leaf_images));
    memset(internal_images, 0, sizeof(internal_images));
    if (!tinydb_compact_v2_staging_leaf_chain_init(&leaves, leaf_images, private_pages, 16u)) return 7;
    g_rows = 120u; g_fail_after = UINT32_MAX; g_fail_sync_parent = true;
    memset(&result, 0xA5, sizeof(result));
    if (tinydb_schema_repack_table_prepare_migration(
            &table, &source, &destination, &leaves, &staging, &hierarchy,
            internal_images, 16u, claimed, 32u, 7u, 77u, 12u, 13u,
            encode_scratch, sizeof(encode_scratch), &ops, &result, message, sizeof(message))) return 8;
    if (result.ready || !result.destination_durable || result.recovery_intent_durable ||
        !result.durable_stage.ready || result.durable_stage.root_page_num == 0u ||
        result.durable_stage.claimed_page_count == 0u || result.intent.ready ||
        commit_calls != 1u || checkpoint_calls != 1u || write_temp_calls != 1u ||
        sync_temp_calls != 1u || publish_temp_calls != 1u || sync_parent_calls != 1u ||
        strstr(message, "destination is durable") == NULL) return 9;

    puts("SCHEMA_REPACK_TABLE_MIGRATION_PREPARE_OK");
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
        if "SCHEMA_REPACK_TABLE_MIGRATION_PREPARE_OK" not in run.stdout:
            raise AssertionError("probe did not report success")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())

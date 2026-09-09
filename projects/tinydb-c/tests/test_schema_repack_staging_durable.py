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
#include "schema_repack_staging_durable.h"

static unsigned char pager_pages[64u * PAGE_SIZE];
static uint32_t commit_calls;
static uint32_t checkpoint_calls;

uint32_t get_unused_page_num(Pager* pager) {
    return pager->num_pages;
}

void* get_page(Pager* pager, uint32_t page_num) {
    if (pager == NULL || page_num >= 64u) return NULL;
    if (page_num >= pager->num_pages) pager->num_pages = page_num + 1u;
    return pager_pages + (size_t)page_num * PAGE_SIZE;
}

void mark_page_dirty(Pager* pager, uint32_t page_num) {
    if (pager != NULL && pager->is_dirty != NULL &&
        page_num < pager->page_capacity) pager->is_dirty[page_num] = true;
}

void pager_commit(Pager* pager) {
    if (pager == NULL || !pager->in_transaction) return;
    commit_calls++;
    for (uint32_t page_num = 0u; page_num < pager->page_capacity; page_num++) {
        pager->is_dirty[page_num] = false;
    }
    pager->in_transaction = false;
}

void pager_checkpoint(Pager* pager) {
    if (pager != NULL) checkpoint_calls++;
}

/* Production node accessors live in table.c. Keep this probe link-small while
 * preserving exactly the same on-page offset semantics. */
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
    uint32_t num_keys = *internal_node_num_keys(node);
    if (child_num > num_keys) abort();
    return child_num == num_keys
        ? internal_node_right_child(node)
        : internal_node_cell(node, child_num);
}
uint32_t* internal_node_key(void* node, uint32_t key_num) {
    return (uint32_t*)((char*)internal_node_cell(node, key_num) +
                       INTERNAL_NODE_CHILD_SIZE);
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

static void make_payload(const TableSchema* schema, uint32_t id,
                         TinyDBRecordPayload* payload) {
    memset(payload, 0, sizeof(*payload));
    payload->length = schema->row_size;
    memcpy(payload->bytes, &id, sizeof(id));
    snprintf((char*)payload->bytes + schema->columns[1].offset,
             schema->columns[1].size, "title-%u", id);
    snprintf((char*)payload->bytes + schema->columns[2].offset,
             schema->columns[2].size, "body-%u", id);
}

int main(void) {
    TableSchema source_schema, destination_schema;
    TinyDBRecordPayload payload;
    TinyDBCompactV2StagingLeafChain leaves;
    TinyDBSchemaRepackStaging staging;
    TinyDBCompactV2StagingHierarchy hierarchy;
    TinyDBSchemaRepackDurableStageResult result;
    Pager pager;
    bool dirty[64];
    unsigned char leaf_images[16u * PAGE_SIZE];
    unsigned char internal_images[16u * PAGE_SIZE];
    const uint32_t private_leaf_pages[16] = {
        101u,102u,103u,104u,105u,106u,107u,108u,
        109u,110u,111u,112u,113u,114u,115u,116u
    };
    uint32_t claimed_pages[32];
    char message[192];

    memset(&pager, 0, sizeof(pager));
    memset(dirty, 0, sizeof(dirty));
    memset(pager_pages, 0, sizeof(pager_pages));
    pager.in_transaction = true;
    pager.num_pages = 1u;
    pager.page_capacity = 64u;
    pager.is_dirty = dirty;

    make_schema(&source_schema, 32u, 64u);
    make_schema(&destination_schema, 128u, 256u);
    memset(leaf_images, 0, sizeof(leaf_images));
    memset(internal_images, 0, sizeof(internal_images));
    memset(claimed_pages, 0, sizeof(claimed_pages));

    if (!tinydb_compact_v2_staging_leaf_chain_init(
            &leaves, leaf_images, private_leaf_pages, 16u) ||
        !tinydb_schema_repack_staging_init(
            &staging, &source_schema, &destination_schema, &leaves,
            message, sizeof(message))) return 1;

    for (uint32_t id = 1u; id <= 120u; id++) {
        make_payload(&source_schema, id, &payload);
        if (!tinydb_schema_repack_staging_visit(
                &source_schema, &payload, &staging)) return 2;
    }
    if (leaves.page_count < 2u) return 3;

    if (!tinydb_schema_repack_staging_make_durable_unpublished(
            &pager, &staging, 120u, &hierarchy, internal_images, 16u,
            claimed_pages, 32u, &result, message, sizeof(message))) {
        fprintf(stderr, "%s\n", message);
        return 4;
    }

    if (!result.ready || result.row_count != 120u ||
        result.leaf_page_count < 2u || result.internal_page_count == 0u ||
        result.claimed_page_count != result.leaf_page_count + result.internal_page_count ||
        result.root_page_num == 0u || result.root_page_num != hierarchy.root_page_num ||
        commit_calls != 1u || checkpoint_calls != 1u || pager.in_transaction ||
        !tinydb_compact_v2_staging_hierarchy_validate(&hierarchy)) return 5;

    for (uint32_t i = 0u; i < result.claimed_page_count; i++) {
        uint32_t page_num = claimed_pages[i];
        if (page_num == 0u || page_num >= pager.num_pages || dirty[page_num]) return 6;
    }
    for (uint32_t i = 0u; i < leaves.page_count; i++) {
        const unsigned char* expected = tinydb_compact_v2_staging_page_const(&leaves, i);
        const unsigned char* actual = pager_pages + (size_t)claimed_pages[i] * PAGE_SIZE;
        if (expected == NULL || memcmp(actual, expected, PAGE_USABLE_SIZE) != 0) return 7;
    }

    puts("SCHEMA_REPACK_STAGING_DURABLE_OK");
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
        if "SCHEMA_REPACK_STAGING_DURABLE_OK" not in run.stdout:
            raise AssertionError("probe did not report durable staging success")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

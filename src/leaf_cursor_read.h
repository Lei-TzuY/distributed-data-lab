#ifndef LEAF_CURSOR_READ_H
#define LEAF_CURSOR_READ_H

#include "leaf_migration.h"
#include "leaf_page_access.h"
#include "slotted_leaf_v2.h"
#include "table.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Read-only cursor API that understands both fixed V1 and slotted V2 leaves.
 * The historical table/cursor API remains the production V1 mutation path. */
Cursor* tinydb_leaf_read_find(Table* table, uint32_t key);
Cursor* tinydb_leaf_read_start(Table* table);
Cursor* tinydb_leaf_read_end(Table* table);
void* tinydb_leaf_read_value(Cursor* cursor);

/* Checked traversal distinguishes a legitimate end-of-chain transition from
 * corrupt/out-of-range sibling metadata. This lets schema-aware scans fail
 * closed instead of silently returning a truncated prefix. */
bool tinydb_leaf_read_advance_checked(Cursor* cursor);
bool tinydb_leaf_read_retreat_checked(Cursor* cursor);

/* Compatibility wrappers keep the existing read-cursor surface available to
 * probes/callers that do not need an explicit corruption status. */
void tinydb_leaf_read_advance(Cursor* cursor);
void tinydb_leaf_read_retreat(Cursor* cursor);

/* Whole-tree migration input seam. The visitor receives rows in strict B+tree
 * key order, already converted from canonical fixed-V1 payloads into canonical
 * compact-V2 historical-generation envelopes. A NULL visitor performs a pure
 * validation/counting pass.
 *
 * This routine intentionally does not publish or mutate a destination tree.
 * A staging-tree builder may perform writes in the visitor, but must discard
 * that unpublished staging tree if this scan returns false. Source traversal
 * fails closed on mixed-format leaves, invalid fixed rows, broken sibling
 * reciprocity/order, callback rejection, or other cursor corruption.
 *
 * rows_scanned is published only after the complete source tree succeeds, so
 * callers can distinguish an atomic successful scan from a rejected prefix. */
typedef bool (*TinyDBFixedV1CompactVisitor)(uint32_t key,
                                            const void* envelope,
                                            uint32_t envelope_length,
                                            void* context);

/* Private compact-V2 leaf-chain staging surface for whole-tree migration.
 * page_images points at page_capacity consecutive PAGE_SIZE images owned by
 * the caller. page_numbers supplies the future physical identities reserved
 * for those images; zero is excluded because sibling metadata uses zero as the
 * end-of-chain sentinel. No Pager page, root, catalog, mutation epoch, or WAL
 * state is touched here.
 *
 * The builder consumes rows in strictly increasing primary-key order. A full
 * page rolls over into the next private image and reciprocal sibling links are
 * staged only after the new row has been proven to fit. Capacity exhaustion,
 * oversized values, duplicate/out-of-order keys, and malformed page images
 * therefore reject the current row without changing the accepted prefix.
 * A caller must discard the complete private chain if its source scan later
 * fails; publication is deliberately a separate recovery boundary. */
typedef struct {
    unsigned char* page_images;
    const uint32_t* page_numbers;
    uint32_t page_capacity;
    uint32_t page_count;
    uint64_t row_count;
    uint32_t last_key;
    bool has_last_key;
} TinyDBCompactV2StagingLeafChain;

static inline unsigned char* tinydb_compact_v2_staging_page(
    TinyDBCompactV2StagingLeafChain* chain,
    uint32_t index) {
    if (chain == NULL || chain->page_images == NULL ||
        index >= chain->page_capacity) {
        return NULL;
    }
    return chain->page_images + (size_t)index * PAGE_SIZE;
}

static inline const unsigned char* tinydb_compact_v2_staging_page_const(
    const TinyDBCompactV2StagingLeafChain* chain,
    uint32_t index) {
    if (chain == NULL || chain->page_images == NULL ||
        index >= chain->page_capacity) {
        return NULL;
    }
    return chain->page_images + (size_t)index * PAGE_SIZE;
}

static inline void tinydb_compact_v2_staging_write_u32_le(
    unsigned char* destination,
    uint32_t value) {
    destination[0] = (unsigned char)(value & 0xffu);
    destination[1] = (unsigned char)((value >> 8) & 0xffu);
    destination[2] = (unsigned char)((value >> 16) & 0xffu);
    destination[3] = (unsigned char)((value >> 24) & 0xffu);
}

static inline uint32_t tinydb_compact_v2_staging_read_u32_le(
    const unsigned char* source) {
    return (uint32_t)source[0] |
           ((uint32_t)source[1] << 8) |
           ((uint32_t)source[2] << 16) |
           ((uint32_t)source[3] << 24);
}

static inline bool tinydb_compact_v2_staging_leaf_chain_init(
    TinyDBCompactV2StagingLeafChain* chain,
    unsigned char* page_images,
    const uint32_t* page_numbers,
    uint32_t page_capacity) {
    if (chain == NULL || page_images == NULL || page_numbers == NULL ||
        page_capacity == 0u) {
        return false;
    }
    for (uint32_t i = 0u; i < page_capacity; i++) {
        if (page_numbers[i] == 0u) return false;
        for (uint32_t j = 0u; j < i; j++) {
            if (page_numbers[i] == page_numbers[j]) return false;
        }
    }

    unsigned char first[PAGE_SIZE];
    memset(first, 0, sizeof(first));
    if (!tinydb_slotted_leaf_v2_init(first, sizeof(first)) ||
        !tinydb_slotted_leaf_v2_validate(first, sizeof(first))) {
        return false;
    }

    memcpy(page_images, first, PAGE_USABLE_SIZE);
    chain->page_images = page_images;
    chain->page_numbers = page_numbers;
    chain->page_capacity = page_capacity;
    chain->page_count = 1u;
    chain->row_count = 0u;
    chain->last_key = 0u;
    chain->has_last_key = false;
    return true;
}

static inline bool tinydb_compact_v2_staging_leaf_chain_append(
    TinyDBCompactV2StagingLeafChain* chain,
    uint32_t key,
    const void* envelope,
    uint32_t envelope_length) {
    if (chain == NULL || chain->page_images == NULL ||
        chain->page_numbers == NULL || chain->page_count == 0u ||
        chain->page_count > chain->page_capacity || envelope == NULL ||
        envelope_length == 0u || envelope_length > UINT16_MAX ||
        (chain->has_last_key && key <= chain->last_key)) {
        return false;
    }

    uint32_t current_index = chain->page_count - 1u;
    unsigned char* current =
        tinydb_compact_v2_staging_page(chain, current_index);
    if (current == NULL ||
        !tinydb_slotted_leaf_v2_validate(current, PAGE_SIZE)) {
        return false;
    }

    uint32_t required = TINYDB_SLOTTED_V2_SLOT_SIZE + envelope_length;
    uint32_t available = tinydb_slotted_leaf_v2_free_bytes(current, PAGE_SIZE);
    if (required <= available) {
        unsigned char staged[PAGE_SIZE];
        memcpy(staged, current, sizeof(staged));
        if (!tinydb_slotted_leaf_v2_insert(staged,
                                           sizeof(staged),
                                           key,
                                           envelope,
                                           (uint16_t)envelope_length) ||
            !tinydb_slotted_leaf_v2_validate(staged, sizeof(staged))) {
            return false;
        }
        memcpy(current, staged, PAGE_USABLE_SIZE);
        chain->row_count++;
        chain->last_key = key;
        chain->has_last_key = true;
        return true;
    }

    if (chain->page_count >= chain->page_capacity) return false;

    unsigned char next[PAGE_SIZE];
    memset(next, 0, sizeof(next));
    if (!tinydb_slotted_leaf_v2_init(next, sizeof(next)) ||
        required > tinydb_slotted_leaf_v2_free_bytes(next, sizeof(next)) ||
        !tinydb_slotted_leaf_v2_insert(next,
                                       sizeof(next),
                                       key,
                                       envelope,
                                       (uint16_t)envelope_length)) {
        return false;
    }

    unsigned char previous[PAGE_SIZE];
    memcpy(previous, current, sizeof(previous));
    uint32_t next_index = chain->page_count;
    uint32_t previous_page_num = chain->page_numbers[current_index];
    uint32_t next_page_num = chain->page_numbers[next_index];
    tinydb_compact_v2_staging_write_u32_le(
        previous + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET,
        next_page_num);
    tinydb_compact_v2_staging_write_u32_le(
        next + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
        previous_page_num);
    if (!tinydb_slotted_leaf_v2_validate(previous, sizeof(previous)) ||
        !tinydb_slotted_leaf_v2_validate(next, sizeof(next))) {
        return false;
    }

    unsigned char* next_destination =
        tinydb_compact_v2_staging_page(chain, next_index);
    if (next_destination == NULL) return false;
    memcpy(current, previous, PAGE_USABLE_SIZE);
    memcpy(next_destination, next, PAGE_USABLE_SIZE);
    chain->page_count++;
    chain->row_count++;
    chain->last_key = key;
    chain->has_last_key = true;
    return true;
}

static inline bool tinydb_compact_v2_staging_leaf_chain_validate(
    const TinyDBCompactV2StagingLeafChain* chain) {
    if (chain == NULL || chain->page_images == NULL ||
        chain->page_numbers == NULL || chain->page_count == 0u ||
        chain->page_count > chain->page_capacity) {
        return false;
    }

    uint64_t rows = 0u;
    uint32_t previous_key = 0u;
    bool have_key = false;
    for (uint32_t i = 0u; i < chain->page_count; i++) {
        const unsigned char* page =
            tinydb_compact_v2_staging_page_const(chain, i);
        if (page == NULL ||
            !tinydb_slotted_leaf_v2_validate(page, PAGE_SIZE)) {
            return false;
        }

        uint32_t expected_prev = i == 0u ? 0u : chain->page_numbers[i - 1u];
        uint32_t expected_next = i + 1u == chain->page_count
            ? 0u
            : chain->page_numbers[i + 1u];
        if (tinydb_compact_v2_staging_read_u32_le(
                page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) != expected_prev ||
            tinydb_compact_v2_staging_read_u32_le(
                page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) != expected_next) {
            return false;
        }

        uint32_t count = 0u;
        if (!tinydb_leaf_page_count(page, PAGE_SIZE, &count) ||
            (i > 0u && count == 0u)) {
            return false;
        }
        for (uint32_t cell = 0u; cell < count; cell++) {
            uint32_t key = 0u;
            if (!tinydb_leaf_page_key_at(page, PAGE_SIZE, cell, &key) ||
                (have_key && key <= previous_key)) {
                return false;
            }
            previous_key = key;
            have_key = true;
            rows++;
        }
    }

    if (rows != chain->row_count || have_key != chain->has_last_key) {
        return false;
    }
    return !have_key || previous_key == chain->last_key;
}

static inline bool tinydb_compact_v2_staging_leaf_chain_visit(
    uint32_t key,
    const void* envelope,
    uint32_t envelope_length,
    void* context) {
    return tinydb_compact_v2_staging_leaf_chain_append(
        (TinyDBCompactV2StagingLeafChain*)context,
        key,
        envelope,
        envelope_length);
}

static inline bool tinydb_fixed_v1_tree_scan_compact_v2(
    Table* table,
    const TableSchema* schema,
    TinyDBFixedV1CompactVisitor visitor,
    void* context,
    uint64_t* rows_scanned) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        schema->root_page_num >= table->pager->num_pages) {
        return false;
    }

    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;

    Cursor* cursor = tinydb_leaf_read_start(table);
    uint64_t count = 0u;
    bool ok = false;
    unsigned char envelope[PAGE_SIZE];

    if (cursor == NULL) goto done;
    while (!cursor->end_of_table) {
        if (cursor->page_num >= table->pager->num_pages) goto done;

        void* page = get_page(table->pager, cursor->page_num);
        if (get_node_type(page) != NODE_LEAF ||
            !tinydb_leaf_page_is_fixed_v1(page, PAGE_SIZE)) {
            goto done;
        }

        uint32_t key = 0u;
        const void* value = NULL;
        uint32_t value_length = 0u;
        uint32_t envelope_length = 0u;
        if (!tinydb_leaf_page_key_at(page,
                                     PAGE_SIZE,
                                     cursor->cell_num,
                                     &key) ||
            !tinydb_leaf_page_value_at(page,
                                       PAGE_SIZE,
                                       cursor->cell_num,
                                       &value,
                                       &value_length) ||
            value_length != ROW_SIZE ||
            !tinydb_fixed_v1_row_encode_compact_v2(schema,
                                                    key,
                                                    value,
                                                    value_length,
                                                    envelope,
                                                    sizeof(envelope),
                                                    &envelope_length) ||
            envelope_length == 0u ||
            (visitor != NULL &&
             !visitor(key, envelope, envelope_length, context))) {
            goto done;
        }

        count++;
        if (!tinydb_leaf_read_advance_checked(cursor)) goto done;
    }

    ok = true;

done:
    free(cursor);
    table->root_page_num = previous_root;
    if (ok && rows_scanned != NULL) *rows_scanned = count;
    return ok;
}

/* Connect the fixed-tree ordered scan directly to the private V2 leaf-chain
 * builder. The chain must be freshly initialized. Success requires the scan
 * count, builder count, reciprocal sibling topology, and global key order to
 * agree. Failure may leave an accepted prefix in private memory only; callers
 * must discard that unpublished chain rather than publish a partial rebuild. */
static inline bool tinydb_fixed_v1_tree_stage_compact_v2_leaf_chain(
    Table* table,
    const TableSchema* schema,
    TinyDBCompactV2StagingLeafChain* chain,
    uint64_t* rows_staged) {
    if (chain == NULL || chain->page_count != 1u || chain->row_count != 0u ||
        chain->has_last_key ||
        !tinydb_compact_v2_staging_leaf_chain_validate(chain)) {
        return false;
    }

    uint64_t scanned = 0u;
    if (!tinydb_fixed_v1_tree_scan_compact_v2(
            table,
            schema,
            tinydb_compact_v2_staging_leaf_chain_visit,
            chain,
            &scanned) ||
        scanned != chain->row_count ||
        !tinydb_compact_v2_staging_leaf_chain_validate(chain)) {
        return false;
    }
    if (rows_staged != NULL) *rows_staged = scanned;
    return true;
}

#endif /* LEAF_CURSOR_READ_H */
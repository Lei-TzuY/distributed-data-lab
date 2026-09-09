#include "generic_index_epoch.h"
#include "internal_split_cascade_stage.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_split.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TINYDB_RECURSIVE_OVERFLOW_TRIGGER \
    "recursive internal overflow beyond the grandparent remains fail-closed"

typedef struct {
    uint32_t* page_nums;
    uint32_t* old_maxes;
    unsigned char* images;
    uint32_t count;
    uint32_t capacity;
} TinyDBInternalChain;

bool tinydb_record_insert_v2_nonroot_overflow_base(
    Table* table,
    const TableSchema* schema,
    const TinyDBValue* values,
    uint32_t value_count,
    char* message,
    size_t message_size);

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text);
    }
}

static uint32_t read_u32_native(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static void write_u32_native(unsigned char* bytes, uint32_t value) {
    memcpy(bytes, &value, sizeof(value));
}

static bool encode_compact_insert(const TableSchema* schema,
                                  const TinyDBValue* values,
                                  uint32_t value_count,
                                  uint32_t* key,
                                  unsigned char envelope[PAGE_SIZE],
                                  uint32_t* envelope_length,
                                  char* message,
                                  size_t message_size) {
    if (schema == NULL || values == NULL || key == NULL || envelope == NULL ||
        envelope_length == NULL ||
        !tinydb_schema_supports_records(schema, message, message_size)) {
        return false;
    }

    TinyDBRecord record;
    TinyDBRecordPayload payload;
    if (!tinydb_record_encode(schema,
                              values,
                              value_count,
                              &record,
                              message,
                              message_size) ||
        !tinydb_record_payload_from_record(schema,
                                           &record,
                                           &payload,
                                           message,
                                           message_size) ||
        payload.length < sizeof(uint32_t)) {
        return false;
    }
    memcpy(key, payload.bytes, sizeof(*key));

    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                               &payload,
                                               envelope,
                                               PAGE_SIZE,
                                               envelope_length) ||
        *envelope_length == 0u || *envelope_length > UINT16_MAX) {
        set_message(message,
                    message_size,
                    "unable to encode compact V2 row for recursive split");
        return false;
    }
    return true;
}

static bool previous_boundary_allows(Table* table,
                                     uint32_t previous_page_num,
                                     uint32_t key) {
    if (previous_page_num == 0u) return true;
    if (table == NULL || table->pager == NULL ||
        previous_page_num >= table->pager->num_pages) {
        return false;
    }

    unsigned char previous[PAGE_SIZE];
    memcpy(previous,
           get_page(table->pager, previous_page_num),
           PAGE_SIZE);
    uint32_t count = 0u;
    uint32_t max_key = 0u;
    return tinydb_leaf_page_count(previous, PAGE_SIZE, &count) &&
           count > 0u &&
           tinydb_leaf_page_key_at(previous,
                                   PAGE_SIZE,
                                   count - 1u,
                                   &max_key) &&
           key > max_key;
}

static void chain_free(TinyDBInternalChain* chain) {
    if (chain == NULL) return;
    free(chain->page_nums);
    free(chain->old_maxes);
    free(chain->images);
    memset(chain, 0, sizeof(*chain));
}

static bool chain_reserve(TinyDBInternalChain* chain, uint32_t needed) {
    if (chain == NULL) return false;
    if (needed <= chain->capacity) return true;

    uint32_t new_capacity = chain->capacity == 0u ? 4u : chain->capacity;
    while (new_capacity < needed) {
        if (new_capacity > UINT32_MAX / 2u) {
            new_capacity = needed;
            break;
        }
        new_capacity *= 2u;
    }

    uint32_t* new_page_nums =
        (uint32_t*)realloc(chain->page_nums,
                           (size_t)new_capacity * sizeof(uint32_t));
    if (new_page_nums == NULL) return false;
    chain->page_nums = new_page_nums;

    uint32_t* new_old_maxes =
        (uint32_t*)realloc(chain->old_maxes,
                           (size_t)new_capacity * sizeof(uint32_t));
    if (new_old_maxes == NULL) return false;
    chain->old_maxes = new_old_maxes;

    unsigned char* new_images =
        (unsigned char*)realloc(chain->images,
                                (size_t)new_capacity * PAGE_SIZE);
    if (new_images == NULL) return false;
    chain->images = new_images;
    chain->capacity = new_capacity;
    return true;
}

static bool subtree_max_key(Table* table,
                            uint32_t page_num,
                            uint32_t* max_key_out) {
    if (table == NULL || table->pager == NULL || max_key_out == NULL ||
        page_num >= table->pager->num_pages) {
        return false;
    }

    uint32_t current_page_num = page_num;
    for (uint32_t depth = 0u; depth <= table->pager->num_pages; depth++) {
        if (current_page_num >= table->pager->num_pages) return false;
        unsigned char page[PAGE_SIZE];
        memcpy(page,
               get_page(table->pager, current_page_num),
               PAGE_SIZE);

        if (get_node_type(page) == NODE_LEAF) {
            uint32_t count = 0u;
            return tinydb_leaf_page_count(page, PAGE_SIZE, &count) &&
                   count > 0u &&
                   tinydb_leaf_page_key_at(page,
                                           PAGE_SIZE,
                                           count - 1u,
                                           max_key_out);
        }

        if (get_node_type(page) != NODE_INTERNAL ||
            !tinydb_parent_stage_validate(page, PAGE_SIZE)) {
            return false;
        }
        uint32_t keys = tinydb_parent_stage_read_u32(
            page + INTERNAL_NODE_NUM_KEYS_OFFSET);
        uint32_t next_page_num = tinydb_parent_stage_child_at(page, keys);
        if (next_page_num == 0u || next_page_num == current_page_num) {
            return false;
        }
        current_page_num = next_page_num;
    }
    return false;
}

static bool collect_internal_chain(Table* table,
                                   const TableSchema* schema,
                                   uint32_t first_parent_page_num,
                                   TinyDBInternalChain* chain) {
    if (table == NULL || table->pager == NULL || schema == NULL ||
        chain == NULL || first_parent_page_num == 0u ||
        first_parent_page_num >= table->pager->num_pages) {
        return false;
    }

    uint32_t current_page_num = first_parent_page_num;
    for (uint32_t depth = 0u; depth <= table->pager->num_pages; depth++) {
        if (current_page_num >= table->pager->num_pages ||
            !chain_reserve(chain, chain->count + 1u)) {
            return false;
        }
        for (uint32_t i = 0u; i < chain->count; i++) {
            if (chain->page_nums[i] == current_page_num) return false;
        }

        unsigned char* image = chain->images +
                               (size_t)chain->count * PAGE_SIZE;
        memcpy(image,
               get_page(table->pager, current_page_num),
               PAGE_SIZE);
        if (get_node_type(image) != NODE_INTERNAL ||
            !tinydb_parent_stage_validate(image, PAGE_SIZE) ||
            !subtree_max_key(table,
                             current_page_num,
                             &chain->old_maxes[chain->count])) {
            return false;
        }
        chain->page_nums[chain->count] = current_page_num;
        chain->count++;

        if (image[IS_ROOT_OFFSET] != 0u) {
            return current_page_num == schema->root_page_num;
        }

        uint32_t parent_page_num = read_u32_native(
            image + PARENT_POINTER_OFFSET);
        if (parent_page_num == 0u || parent_page_num == current_page_num) {
            return false;
        }
        current_page_num = parent_page_num;
    }
    return false;
}

static bool planned_internal_page_count(const TinyDBInternalChain* chain,
                                        uint32_t* new_page_count_out,
                                        bool* root_grows_out) {
    if (chain == NULL || chain->count == 0u || new_page_count_out == NULL ||
        root_grows_out == NULL) {
        return false;
    }

    uint32_t new_page_count = 0u;
    bool stopped = false;
    bool root_grows = false;
    for (uint32_t i = 0u; i < chain->count; i++) {
        const unsigned char* page = chain->images + (size_t)i * PAGE_SIZE;
        uint32_t keys = tinydb_parent_stage_read_u32(
            page + INTERNAL_NODE_NUM_KEYS_OFFSET);
        if (keys == 0u || keys > INTERNAL_NODE_MAX_KEYS) return false;
        if (keys < INTERNAL_NODE_MAX_KEYS) {
            stopped = true;
            break;
        }
        if (page[IS_ROOT_OFFSET] != 0u) {
            if (new_page_count > UINT32_MAX - 2u) return false;
            new_page_count += 2u;
            root_grows = true;
            stopped = true;
            break;
        }
        if (new_page_count == UINT32_MAX) return false;
        new_page_count++;
    }
    if (!stopped) return false;

    *new_page_count_out = new_page_count;
    *root_grows_out = root_grows;
    return true;
}

static bool peek_unused_page_nums(const Pager* pager,
                                  uint32_t* page_nums,
                                  uint32_t count) {
    if (pager == NULL || page_nums == NULL || count == 0u) return false;
    uint32_t free_count = pager->free_page_count;
    uint32_t appended = count > free_count ? count - free_count : 0u;
    if (appended > 0u && pager->num_pages > INVALID_PAGE_NUM - appended) {
        return false;
    }

    for (uint32_t i = 0u; i < count; i++) {
        page_nums[i] = i < free_count
            ? pager->free_pages[free_count - 1u - i]
            : pager->num_pages + (i - free_count);
        if (page_nums[i] == 0u || page_nums[i] == INVALID_PAGE_NUM) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (page_nums[j] == page_nums[i]) return false;
        }
    }
    return true;
}

static bool reserved_pages_disjoint(const uint32_t* reserved_pages,
                                    uint32_t reserved_count,
                                    uint32_t left_page_num,
                                    uint32_t previous_page_num,
                                    uint32_t next_page_num,
                                    const TinyDBInternalChain* chain) {
    if (reserved_pages == NULL || chain == NULL) return false;
    for (uint32_t i = 0u; i < reserved_count; i++) {
        uint32_t page_num = reserved_pages[i];
        if (page_num == left_page_num || page_num == previous_page_num ||
            page_num == next_page_num) {
            return false;
        }
        for (uint32_t j = 0u; j < chain->count; j++) {
            if (page_num == chain->page_nums[j]) return false;
        }
    }
    return true;
}

static bool claim_reserved_page_nums(Pager* pager,
                                     const uint32_t* page_nums,
                                     uint32_t count) {
    if (pager == NULL || page_nums == NULL) return false;
    for (uint32_t i = 0u; i < count; i++) {
        uint32_t claimed = get_unused_page_num(pager);
        if (claimed != page_nums[i]) return false;
        (void)get_page(pager, claimed);
    }
    return true;
}

static bool page_num_in_list(const uint32_t* page_nums,
                             uint32_t count,
                             uint32_t page_num) {
    for (uint32_t i = 0u; i < count; i++) {
        if (page_nums[i] == page_num) return true;
    }
    return false;
}

static bool validate_changed_internal_topology(
    Table* table,
    const TinyDBInternalChain* chain,
    uint32_t stop_level,
    const unsigned char* new_internal_pages,
    const uint32_t* new_internal_page_nums,
    uint32_t new_internal_page_count,
    uint32_t right_leaf_page_num) {
    if (table == NULL || table->pager == NULL || chain == NULL ||
        stop_level >= chain->count ||
        (new_internal_page_count > 0u &&
         (new_internal_pages == NULL || new_internal_page_nums == NULL))) {
        return false;
    }

    uint32_t internal_page_count = stop_level + 1u + new_internal_page_count;
    uint32_t max_refs = internal_page_count * (INTERNAL_NODE_MAX_KEYS + 1u);
    uint32_t* child_refs =
        (uint32_t*)malloc((size_t)max_refs * sizeof(uint32_t));
    if (child_refs == NULL) return false;
    uint32_t child_ref_count = 0u;
    bool right_leaf_seen = false;

    for (uint32_t group = 0u; group < 2u; group++) {
        uint32_t count = group == 0u ? stop_level + 1u
                                     : new_internal_page_count;
        for (uint32_t i = 0u; i < count; i++) {
            const unsigned char* image = group == 0u
                ? chain->images + (size_t)i * PAGE_SIZE
                : new_internal_pages + (size_t)i * PAGE_SIZE;
            uint32_t page_num = group == 0u
                ? chain->page_nums[i]
                : new_internal_page_nums[i];
            if (!tinydb_parent_stage_validate(image, PAGE_SIZE)) {
                free(child_refs);
                return false;
            }
            uint32_t keys = tinydb_parent_stage_read_u32(
                image + INTERNAL_NODE_NUM_KEYS_OFFSET);
            for (uint32_t c = 0u; c <= keys; c++) {
                uint32_t child = tinydb_parent_stage_child_at(image, c);
                bool child_is_new_internal = page_num_in_list(
                    new_internal_page_nums,
                    new_internal_page_count,
                    child);
                bool child_exists = child < table->pager->num_pages ||
                                    child == right_leaf_page_num ||
                                    child_is_new_internal;
                if (child == 0u || child == page_num || !child_exists) {
                    free(child_refs);
                    return false;
                }
                for (uint32_t seen = 0u; seen < child_ref_count; seen++) {
                    if (child_refs[seen] == child) {
                        free(child_refs);
                        return false;
                    }
                }
                child_refs[child_ref_count++] = child;
                if (child == right_leaf_page_num) right_leaf_seen = true;
            }
        }
    }

    bool all_new_seen = true;
    for (uint32_t i = 0u; i < new_internal_page_count; i++) {
        if (!page_num_in_list(child_refs,
                              child_ref_count,
                              new_internal_page_nums[i])) {
            all_new_seen = false;
            break;
        }
    }
    free(child_refs);
    return right_leaf_seen && all_new_seen;
}

static void publish_page(Pager* pager,
                         uint32_t page_num,
                         const unsigned char image[PAGE_SIZE]) {
    unsigned char* page = (unsigned char*)get_page(pager, page_num);
    memcpy(page, image, PAGE_USABLE_SIZE);
    mark_page_dirty(pager, page_num);
}

static void assign_children_to_parent(Pager* pager,
                                      uint32_t parent_page_num,
                                      const unsigned char parent[PAGE_SIZE]) {
    uint32_t keys = tinydb_parent_stage_read_u32(
        parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    for (uint32_t i = 0u; i <= keys; i++) {
        uint32_t child_page_num = tinydb_parent_stage_child_at(parent, i);
        unsigned char* child =
            (unsigned char*)get_page(pager, child_page_num);
        if (read_u32_native(child + PARENT_POINTER_OFFSET) != parent_page_num) {
            write_u32_native(child + PARENT_POINTER_OFFSET, parent_page_num);
            mark_page_dirty(pager, child_page_num);
        }
    }
}

static bool try_recursive_internal_overflow(
    Table* table,
    const TableSchema* schema,
    const TinyDBValue* values,
    uint32_t value_count,
    bool* applicable,
    char* message,
    size_t message_size) {
    if (applicable != NULL) *applicable = false;
    if (table == NULL || table->pager == NULL || schema == NULL) return false;

    uint32_t key = 0u;
    uint32_t envelope_length = 0u;
    unsigned char envelope[PAGE_SIZE];
    memset(envelope, 0, sizeof(envelope));
    if (!encode_compact_insert(schema,
                               values,
                               value_count,
                               &key,
                               envelope,
                               &envelope_length,
                               message,
                               message_size)) {
        return false;
    }

    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    if (cursor == NULL || cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= table->pager->num_pages) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    uint32_t left_page_num = cursor->page_num;
    unsigned char left_before[PAGE_SIZE];
    memcpy(left_before,
           get_page(table->pager, left_page_num),
           PAGE_SIZE);
    uint32_t count = 0u;
    uint32_t found_key = 0u;
    if (tinydb_leaf_format_detect_page(left_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(left_before, PAGE_SIZE) ||
        !tinydb_leaf_page_count(left_before, PAGE_SIZE, &count) || count < 2u ||
        (cursor->cell_num < count &&
         tinydb_leaf_page_key_at(left_before,
                                 PAGE_SIZE,
                                 cursor->cell_num,
                                 &found_key) &&
         found_key == key)) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    uint32_t old_left_max = 0u;
    uint32_t previous_page_num = 0u;
    uint32_t next_page_num = 0u;
    uint32_t required = TINYDB_SLOTTED_V2_SLOT_SIZE + envelope_length;
    if (!tinydb_leaf_page_key_at(left_before,
                                 PAGE_SIZE,
                                 count - 1u,
                                 &old_left_max) ||
        !tinydb_leaf_page_prev(left_before,
                               PAGE_SIZE,
                               &previous_page_num) ||
        !tinydb_leaf_page_next(left_before, PAGE_SIZE, &next_page_num) ||
        required <= tinydb_slotted_leaf_v2_free_bytes(left_before, PAGE_SIZE) ||
        !previous_boundary_allows(table, previous_page_num, key) ||
        (next_page_num != 0u &&
         (next_page_num >= table->pager->num_pages ||
          next_page_num == left_page_num))) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    bool is_tail = next_page_num == 0u;
    if (!is_tail && key >= old_left_max) {
        free(cursor);
        table->root_page_num = previous_root;
        return false;
    }

    uint32_t parent_page_num = read_u32_native(
        left_before + PARENT_POINTER_OFFSET);
    free(cursor);
    table->root_page_num = previous_root;
    if (parent_page_num == 0u || parent_page_num >= table->pager->num_pages ||
        parent_page_num == schema->root_page_num) {
        return false;
    }

    TinyDBInternalChain chain;
    memset(&chain, 0, sizeof(chain));
    if (!collect_internal_chain(table, schema, parent_page_num, &chain)) {
        chain_free(&chain);
        return false;
    }
    if (chain.count < 2u ||
        tinydb_parent_stage_read_u32(
            chain.images + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            INTERNAL_NODE_MAX_KEYS ||
        tinydb_parent_stage_read_u32(
            chain.images + PAGE_SIZE + INTERNAL_NODE_NUM_KEYS_OFFSET) !=
            INTERNAL_NODE_MAX_KEYS) {
        chain_free(&chain);
        return false;
    }
    if (applicable != NULL) *applicable = true;

    unsigned char next_before[PAGE_SIZE];
    memset(next_before, 0, sizeof(next_before));
    if (!is_tail) {
        memcpy(next_before,
               get_page(table->pager, next_page_num),
               PAGE_SIZE);
        uint32_t next_prev = 0u;
        if (tinydb_leaf_format_detect_page(next_before, PAGE_SIZE) !=
                TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
            !tinydb_slotted_leaf_v2_validate(next_before, PAGE_SIZE) ||
            !tinydb_leaf_page_prev(next_before,
                                    PAGE_SIZE,
                                    &next_prev) ||
            next_prev != left_page_num) {
            set_message(message,
                        message_size,
                        "invalid V2 next sibling blocks recursive split");
            chain_free(&chain);
            return false;
        }
    }

    uint32_t internal_new_count = 0u;
    bool root_grows = false;
    if (!planned_internal_page_count(&chain,
                                     &internal_new_count,
                                     &root_grows) ||
        internal_new_count == 0u || internal_new_count == UINT32_MAX) {
        set_message(message,
                    message_size,
                    "unable to plan recursive internal page allocation");
        chain_free(&chain);
        return false;
    }

    uint32_t reserved_count = internal_new_count + 1u;
    uint32_t* reserved_pages =
        (uint32_t*)malloc((size_t)reserved_count * sizeof(uint32_t));
    unsigned char* new_internal_pages =
        (unsigned char*)calloc((size_t)internal_new_count, PAGE_SIZE);
    if (reserved_pages == NULL || new_internal_pages == NULL) {
        free(reserved_pages);
        free(new_internal_pages);
        chain_free(&chain);
        set_message(message,
                    message_size,
                    "unable to allocate recursive split scratch state");
        return false;
    }
    if (!peek_unused_page_nums(table->pager,
                               reserved_pages,
                               reserved_count) ||
        !reserved_pages_disjoint(reserved_pages,
                                 reserved_count,
                                 left_page_num,
                                 previous_page_num,
                                 next_page_num,
                                 &chain)) {
        free(reserved_pages);
        free(new_internal_pages);
        chain_free(&chain);
        set_message(message,
                    message_size,
                    "recursive split page reservation collided with topology");
        return false;
    }

    uint32_t right_leaf_page_num = reserved_pages[0];
    uint32_t* new_internal_page_nums = reserved_pages + 1u;
    unsigned char left_after[PAGE_SIZE];
    unsigned char right_leaf_after[PAGE_SIZE];
    unsigned char next_after[PAGE_SIZE];
    memcpy(left_after, left_before, PAGE_SIZE);
    memset(right_leaf_after, 0, PAGE_SIZE);
    memset(next_after, 0, PAGE_SIZE);
    if (!is_tail) memcpy(next_after, next_before, PAGE_SIZE);

    if (!tinydb_slotted_leaf_v2_split_nonroot(left_after,
                                               PAGE_SIZE,
                                               left_page_num,
                                               right_leaf_after,
                                               PAGE_SIZE,
                                               right_leaf_page_num,
                                               NULL)) {
        free(reserved_pages);
        free(new_internal_pages);
        chain_free(&chain);
        set_message(message,
                    message_size,
                    "unable to stage V2 leaf split for recursive overflow");
        return false;
    }
    if (!is_tail) {
        tinydb_slotted_split_write_u32(
            next_after + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            right_leaf_page_num);
        if (!tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE)) {
            free(reserved_pages);
            free(new_internal_pages);
            chain_free(&chain);
            set_message(message,
                        message_size,
                        "recursive split next-sibling backlink staging failed");
            return false;
        }
    }

    uint16_t left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    uint16_t right_count =
        tinydb_slotted_leaf_v2_count(right_leaf_after, PAGE_SIZE);
    uint32_t staged_left_max = 0u;
    uint32_t staged_right_max = 0u;
    if (left_count == 0u || right_count == 0u ||
        !tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &staged_left_max) ||
        !tinydb_leaf_page_key_at(right_leaf_after,
                                 PAGE_SIZE,
                                 (uint32_t)right_count - 1u,
                                 &staged_right_max) ||
        staged_right_max != old_left_max) {
        free(reserved_pages);
        free(new_internal_pages);
        chain_free(&chain);
        set_message(message,
                    message_size,
                    "recursive leaf split changed its existing upper boundary");
        return false;
    }

    void* destination = key <= staged_left_max
        ? (void*)left_after
        : (void*)right_leaf_after;
    if (!tinydb_slotted_leaf_v2_insert(destination,
                                       PAGE_SIZE,
                                       key,
                                       envelope,
                                       (uint16_t)envelope_length) ||
        !tinydb_slotted_leaf_v2_validate(left_after, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(right_leaf_after, PAGE_SIZE) ||
        (!is_tail && !tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE))) {
        free(reserved_pages);
        free(new_internal_pages);
        chain_free(&chain);
        set_message(message,
                    message_size,
                    "recursive split leaves could not accept the pending row");
        return false;
    }

    left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    right_count = tinydb_slotted_leaf_v2_count(right_leaf_after, PAGE_SIZE);
    uint32_t checked_left_max = 0u;
    uint32_t checked_right_max = 0u;
    if (!tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &checked_left_max) ||
        !tinydb_leaf_page_key_at(right_leaf_after,
                                 PAGE_SIZE,
                                 (uint32_t)right_count - 1u,
                                 &checked_right_max) ||
        checked_left_max != staged_left_max ||
        (!is_tail && checked_right_max != old_left_max) ||
        (is_tail && checked_right_max <= checked_left_max)) {
        free(reserved_pages);
        free(new_internal_pages);
        chain_free(&chain);
        set_message(message,
                    message_size,
                    "pending row invalidated recursive split boundaries");
        return false;
    }

    uint32_t new_pages_used = 0u;
    uint32_t stop_level = 0u;
    bool staged_root_grew = false;
    if (!tinydb_stage_internal_split_cascade(
            chain.images,
            PAGE_SIZE,
            chain.page_nums,
            chain.old_maxes,
            chain.count,
            new_internal_pages,
            PAGE_SIZE,
            new_internal_page_nums,
            internal_new_count,
            left_page_num,
            right_leaf_page_num,
            old_left_max,
            checked_left_max,
            checked_right_max,
            &new_pages_used,
            &stop_level,
            &staged_root_grew) ||
        new_pages_used != internal_new_count ||
        staged_root_grew != root_grows ||
        !validate_changed_internal_topology(table,
                                            &chain,
                                            stop_level,
                                            new_internal_pages,
                                            new_internal_page_nums,
                                            new_pages_used,
                                            right_leaf_page_num)) {
        free(reserved_pages);
        free(new_internal_pages);
        chain_free(&chain);
        set_message(message,
                    message_size,
                    "recursive internal split staging rejected the ancestor chain");
        return false;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        free(reserved_pages);
        free(new_internal_pages);
        chain_free(&chain);
        set_message(message,
                    message_size,
                    "unable to persist generic-index mutation epoch");
        return false;
    }
    if (!claim_reserved_page_nums(table->pager,
                                  reserved_pages,
                                  reserved_count)) {
        free(reserved_pages);
        free(new_internal_pages);
        chain_free(&chain);
        set_message(message,
                    message_size,
                    "recursive split page reservation changed before publication");
        return false;
    }

    publish_page(table->pager, left_page_num, left_after);
    publish_page(table->pager, right_leaf_page_num, right_leaf_after);
    if (!is_tail) publish_page(table->pager, next_page_num, next_after);
    for (uint32_t i = 0u; i <= stop_level; i++) {
        publish_page(table->pager,
                     chain.page_nums[i],
                     chain.images + (size_t)i * PAGE_SIZE);
    }
    for (uint32_t i = 0u; i < new_pages_used; i++) {
        publish_page(table->pager,
                     new_internal_page_nums[i],
                     new_internal_pages + (size_t)i * PAGE_SIZE);
    }

    for (uint32_t i = 0u; i <= stop_level; i++) {
        assign_children_to_parent(
            table->pager,
            chain.page_nums[i],
            chain.images + (size_t)i * PAGE_SIZE);
    }
    for (uint32_t i = 0u; i < new_pages_used; i++) {
        assign_children_to_parent(
            table->pager,
            new_internal_page_nums[i],
            new_internal_pages + (size_t)i * PAGE_SIZE);
    }

    if (!table->in_transaction) pager_commit(table->pager);
    free(reserved_pages);
    free(new_internal_pages);
    chain_free(&chain);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

bool tinydb_record_insert(Table* table,
                          const TableSchema* schema,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size) {
    char base_message[TINYDB_RECORD_MESSAGE_MAX];
    base_message[0] = '\0';
    if (tinydb_record_insert_v2_nonroot_overflow_base(table,
                                                      schema,
                                                      values,
                                                      value_count,
                                                      base_message,
                                                      sizeof(base_message))) {
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    if (strcmp(base_message, TINYDB_RECURSIVE_OVERFLOW_TRIGGER) != 0) {
        set_message(message,
                    message_size,
                    base_message[0] != '\0'
                        ? base_message
                        : "generic insert is not supported for this tree topology");
        return false;
    }

    bool applicable = false;
    char recursive_message[TINYDB_RECORD_MESSAGE_MAX];
    recursive_message[0] = '\0';
    if (try_recursive_internal_overflow(table,
                                        schema,
                                        values,
                                        value_count,
                                        &applicable,
                                        recursive_message,
                                        sizeof(recursive_message))) {
        if (message != NULL && message_size > 0u) message[0] = '\0';
        return true;
    }

    if (applicable && recursive_message[0] != '\0') {
        set_message(message, message_size, recursive_message);
    } else if (recursive_message[0] != '\0') {
        set_message(message, message_size, recursive_message);
    } else {
        set_message(message, message_size, base_message);
    }
    return false;
}

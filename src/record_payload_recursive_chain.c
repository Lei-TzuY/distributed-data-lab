#include "record_payload_recursive_chain.h"

#include "generic_index_epoch.h"
#include "internal_split_cascade_stage.h"
#include "leaf_cursor_read.h"
#include "leaf_format.h"
#include "leaf_page_access.h"
#include "record_payload_ancestor_chain.h"
#include "record_payload_overflow_reservation.h"
#include "record_payload_page_claim_plan.h"
#include "record_payload_page_claim_transaction.h"
#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_split.h"
#include "slotted_v2_pager_publish_batch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message != NULL && message_size > 0u) {
        snprintf(message, message_size, "%s", text != NULL ? text : "");
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

static unsigned char* image_at(unsigned char* images, uint32_t index) {
    return images + (size_t)index * PAGE_SIZE;
}

static const unsigned char* image_at_const(const unsigned char* images,
                                            uint32_t index) {
    return images + (size_t)index * PAGE_SIZE;
}

static bool subtree_max(Table* table,
                        uint32_t page_num,
                        uint32_t* max_key_out) {
    if (table == NULL || table->pager == NULL || max_key_out == NULL ||
        page_num >= table->pager->num_pages) {
        return false;
    }

    uint32_t current = page_num;
    for (uint32_t depth = 0u; depth <= table->pager->num_pages; depth++) {
        if (current >= table->pager->num_pages) return false;
        unsigned char page[PAGE_SIZE];
        memcpy(page, get_page(table->pager, current), PAGE_SIZE);
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
        uint32_t child = tinydb_parent_stage_child_at(page, keys);
        if (child == current || child >= table->pager->num_pages) return false;
        current = child;
    }
    return false;
}

static bool previous_boundary_allows(Table* table,
                                     uint32_t previous_page_num,
                                     uint32_t current_page_num,
                                     uint32_t key) {
    if (previous_page_num == 0u) return true;
    if (table == NULL || table->pager == NULL ||
        previous_page_num >= table->pager->num_pages) {
        return false;
    }

    unsigned char previous[PAGE_SIZE];
    memcpy(previous, get_page(table->pager, previous_page_num), PAGE_SIZE);
    uint32_t previous_next = 0u;
    uint32_t count = 0u;
    uint32_t max_key = 0u;
    return tinydb_leaf_format_detect_page(previous, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(previous, PAGE_SIZE) &&
           tinydb_leaf_page_next(previous, PAGE_SIZE, &previous_next) &&
           previous_next == current_page_num &&
           tinydb_leaf_page_count(previous, PAGE_SIZE, &count) && count > 0u &&
           tinydb_leaf_page_key_at(previous,
                                   PAGE_SIZE,
                                   count - 1u,
                                   &max_key) &&
           key > max_key;
}

static bool next_boundary_allows(const unsigned char next_page[PAGE_SIZE],
                                 uint32_t current_page_num,
                                 uint32_t current_max_key) {
    uint32_t previous = 0u;
    uint32_t count = 0u;
    uint32_t min_key = 0u;
    return next_page != NULL &&
           tinydb_leaf_format_detect_page(next_page, PAGE_SIZE) ==
               TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 &&
           tinydb_slotted_leaf_v2_validate(next_page, PAGE_SIZE) &&
           tinydb_leaf_page_prev(next_page, PAGE_SIZE, &previous) &&
           previous == current_page_num &&
           tinydb_leaf_page_count(next_page, PAGE_SIZE, &count) && count > 0u &&
           tinydb_leaf_page_key_at(next_page, PAGE_SIZE, 0u, &min_key) &&
           min_key > current_max_key;
}

static bool page_claims_avoid_siblings(
    const TinyDBPayloadPageClaimPlan* claim_plan,
    uint32_t previous_page_num,
    uint32_t next_page_num) {
    if (claim_plan == NULL || claim_plan->page_nums == NULL) return false;
    for (uint32_t i = 0u; i < claim_plan->count; i++) {
        if ((previous_page_num != 0u &&
             claim_plan->page_nums[i] == previous_page_num) ||
            (next_page_num != 0u &&
             claim_plan->page_nums[i] == next_page_num)) {
            return false;
        }
    }
    return true;
}

static bool child_assignment_preflight(
    Pager* pager,
    uint32_t parent_page_num,
    const unsigned char parent[PAGE_SIZE]) {
    if (pager == NULL || parent == NULL ||
        !tinydb_parent_stage_validate(parent, PAGE_SIZE)) {
        return false;
    }
    uint32_t keys = tinydb_parent_stage_read_u32(
        parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    for (uint32_t i = 0u; i <= keys; i++) {
        uint32_t child_page_num = tinydb_parent_stage_child_at(parent, i);
        /*
         * A valid in-range page number is sufficient here. Avoid fetching a
         * child merely to prove existence: staged recursive publication is
         * deliberately page-number based so validation must not churn the
         * small LRU pool or create artificial frame-retention requirements.
         * Descendants are fetched later by assign_children(), after staged
         * publication pages are dirty and safe to evict through no-steal spill.
         */
        if (child_page_num >= pager->num_pages ||
            child_page_num == parent_page_num) {
            return false;
        }
    }
    return true;
}

static bool assign_children(Pager* pager,
                            uint32_t parent_page_num,
                            const unsigned char parent[PAGE_SIZE]) {
    if (!child_assignment_preflight(pager, parent_page_num, parent)) {
        return false;
    }
    uint32_t keys = tinydb_parent_stage_read_u32(
        parent + INTERNAL_NODE_NUM_KEYS_OFFSET);
    for (uint32_t i = 0u; i <= keys; i++) {
        uint32_t child_page_num = tinydb_parent_stage_child_at(parent, i);
        unsigned char* child = (unsigned char*)get_page(pager, child_page_num);
        if (read_u32_native(child + PARENT_POINTER_OFFSET) != parent_page_num) {
            write_u32_native(child + PARENT_POINTER_OFFSET, parent_page_num);
            mark_page_dirty(pager, child_page_num);
        }
    }
    return true;
}

static void release_work(TinyDBPayloadAncestorChain* chain,
                         TinyDBPayloadPageClaimPlan* claim_plan,
                         unsigned char* ancestor_images,
                         uint32_t* old_maxes,
                         unsigned char* new_internal_images,
                         unsigned char** ancestor_targets,
                         unsigned char** new_internal_targets) {
    tinydb_record_payload_ancestor_chain_release(chain);
    tinydb_record_payload_page_claim_plan_release(claim_plan);
    free(ancestor_images);
    free(old_maxes);
    free(new_internal_images);
    free(ancestor_targets);
    free(new_internal_targets);
}

bool tinydb_record_payload_try_bounded_recursive_overflow(
    Table* table,
    const TableSchema* schema,
    uint32_t key,
    const unsigned char* envelope,
    uint32_t envelope_length,
    bool* applicable,
    char* message,
    size_t message_size) {
    if (applicable != NULL) *applicable = false;
    if (table == NULL || table->pager == NULL || schema == NULL ||
        envelope == NULL || envelope_length == 0u ||
        envelope_length > UINT16_MAX ||
        schema->root_page_num >= table->pager->num_pages) {
        return false;
    }

    TinyDBPayloadAncestorChain chain;
    TinyDBPayloadOverflowPlan plan;
    TinyDBPayloadOverflowReservation reservation = {0};
    TinyDBPayloadPageClaimPlan claim_plan = {0};
    char local_message[TINYDB_RECORD_MESSAGE_MAX];
    local_message[0] = '\0';

    if (!tinydb_record_payload_collect_ancestor_chain(table,
                                                       schema,
                                                       key,
                                                       &chain,
                                                       local_message,
                                                       sizeof(local_message))) {
        return false;
    }
    if (!tinydb_record_payload_plan_overflow_chain(table,
                                                    schema,
                                                    &chain,
                                                    &plan,
                                                    local_message,
                                                    sizeof(local_message)) ||
        plan.full_internal_levels < 2u) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        return false;
    }
    if (applicable != NULL) *applicable = true;

    uint32_t participating_ancestors = plan.requires_root_growth
        ? chain.count
        : plan.stopping_ancestor_index + 1u;
    if (participating_ancestors == 0u ||
        participating_ancestors > chain.count ||
        (!plan.requires_root_growth &&
         plan.stopping_ancestor_index != plan.full_internal_levels)) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        set_message(message,
                    message_size,
                    "payload-native bounded recursive plan has an invalid stopping ancestor");
        return false;
    }

    if (!tinydb_record_payload_size_overflow_reservation(&chain,
                                                         &plan,
                                                         &reservation,
                                                         local_message,
                                                         sizeof(local_message)) ||
        reservation.new_leaf_pages != 1u ||
        reservation.new_internal_pages < 2u ||
        !tinydb_record_payload_prepare_page_claim_plan(table->pager,
                                                       &chain,
                                                       &reservation,
                                                       &claim_plan,
                                                       local_message,
                                                       sizeof(local_message))) {
        tinydb_record_payload_ancestor_chain_release(&chain);
        tinydb_record_payload_page_claim_plan_release(&claim_plan);
        set_message(message,
                    message_size,
                    local_message[0] != '\0'
                        ? local_message
                        : "unable to prepare bounded recursive payload overflow");
        return false;
    }

    uint32_t previous_root = table->root_page_num;
    table->root_page_num = schema->root_page_num;
    Cursor* cursor = tinydb_leaf_read_find(table, key);
    table->root_page_num = previous_root;
    if (cursor == NULL || cursor->page_num != chain.leaf_page_num ||
        cursor->page_num == INVALID_PAGE_NUM ||
        cursor->page_num >= table->pager->num_pages) {
        free(cursor);
        release_work(&chain, &claim_plan, NULL, NULL, NULL, NULL, NULL);
        set_message(message,
                    message_size,
                    "bounded recursive payload target changed after ancestry preflight");
        return false;
    }

    uint32_t left_page_num = cursor->page_num;
    unsigned char left_before[PAGE_SIZE];
    memcpy(left_before, get_page(table->pager, left_page_num), PAGE_SIZE);
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
        release_work(&chain, &claim_plan, NULL, NULL, NULL, NULL, NULL);
        set_message(message, message_size, "invalid bounded recursive payload leaf");
        return false;
    }
    free(cursor);

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
        !previous_boundary_allows(table,
                                  previous_page_num,
                                  left_page_num,
                                  key) ||
        next_page_num == left_page_num ||
        (next_page_num != 0u && next_page_num >= table->pager->num_pages) ||
        !page_claims_avoid_siblings(&claim_plan,
                                    previous_page_num,
                                    next_page_num)) {
        release_work(&chain, &claim_plan, NULL, NULL, NULL, NULL, NULL);
        set_message(message,
                    message_size,
                    "bounded recursive payload leaf/sibling preflight failed");
        return false;
    }

    bool is_tail = next_page_num == 0u;
    if (!is_tail && key >= old_left_max) {
        release_work(&chain, &claim_plan, NULL, NULL, NULL, NULL, NULL);
        set_message(message,
                    message_size,
                    "bounded recursive non-tail split would change an encoded upper boundary");
        return false;
    }

    uint32_t base_publish_count = is_tail ? 2u : 3u;
    if (participating_ancestors >
            UINT32_MAX - reservation.new_internal_pages) {
        release_work(&chain, &claim_plan, NULL, NULL, NULL, NULL, NULL);
        set_message(message,
                    message_size,
                    "payload-native recursive publication count overflowed");
        return false;
    }
    uint32_t publish_count =
        participating_ancestors + reservation.new_internal_pages;
    if (publish_count > UINT32_MAX - base_publish_count) {
        release_work(&chain, &claim_plan, NULL, NULL, NULL, NULL, NULL);
        set_message(message,
                    message_size,
                    "payload-native recursive publication count overflowed");
        return false;
    }
    publish_count += base_publish_count;
    if (!tinydb_v2_publish_batch_snapshot_size(publish_count, NULL)) {
        release_work(&chain, &claim_plan, NULL, NULL, NULL, NULL, NULL);
        set_message(message,
                    message_size,
                    "payload-native recursive overflow exceeds the atomic rollback-byte budget");
        return false;
    }

    unsigned char next_before[PAGE_SIZE];
    memset(next_before, 0, PAGE_SIZE);
    if (!is_tail) {
        memcpy(next_before, get_page(table->pager, next_page_num), PAGE_SIZE);
        if (!next_boundary_allows(next_before,
                                  left_page_num,
                                  old_left_max)) {
            release_work(&chain, &claim_plan, NULL, NULL, NULL, NULL, NULL);
            set_message(message,
                        message_size,
                        "bounded recursive payload next sibling is not reciprocal");
            return false;
        }
    }

    unsigned char* ancestor_images = (unsigned char*)calloc(
        (size_t)participating_ancestors, PAGE_SIZE);
    uint32_t* old_maxes = (uint32_t*)calloc(
        (size_t)participating_ancestors, sizeof(uint32_t));
    unsigned char* new_internal_images = (unsigned char*)calloc(
        (size_t)reservation.new_internal_pages, PAGE_SIZE);
    unsigned char** ancestor_targets = (unsigned char**)calloc(
        (size_t)participating_ancestors, sizeof(unsigned char*));
    unsigned char** new_internal_targets = (unsigned char**)calloc(
        (size_t)reservation.new_internal_pages, sizeof(unsigned char*));
    if (ancestor_images == NULL || old_maxes == NULL ||
        new_internal_images == NULL || ancestor_targets == NULL ||
        new_internal_targets == NULL) {
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "unable to allocate bounded recursive payload staging images");
        return false;
    }

    bool ancestors_valid = true;
    for (uint32_t i = 0u; i < participating_ancestors; i++) {
        uint32_t page_num = chain.internal_pages[i];
        unsigned char* image = image_at(ancestor_images, i);
        memcpy(image, get_page(table->pager, page_num), PAGE_SIZE);
        uint32_t key_count = tinydb_parent_stage_read_u32(
            image + INTERNAL_NODE_NUM_KEYS_OFFSET);
        bool should_be_full = i < plan.full_internal_levels;
        if (!tinydb_parent_stage_validate(image, PAGE_SIZE) ||
            (should_be_full && key_count != INTERNAL_NODE_MAX_KEYS) ||
            (!should_be_full && key_count >= INTERNAL_NODE_MAX_KEYS)) {
            ancestors_valid = false;
            break;
        }
        if (should_be_full && !subtree_max(table, page_num, &old_maxes[i])) {
            ancestors_valid = false;
            break;
        }
    }
    if (!ancestors_valid) {
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "bounded recursive payload ancestry changed after planning");
        return false;
    }

    const uint32_t right_leaf_page_num = claim_plan.page_nums[0];
    const uint32_t* new_internal_page_nums = claim_plan.page_nums + 1u;
    unsigned char left_after[PAGE_SIZE];
    unsigned char right_after[PAGE_SIZE];
    unsigned char next_after[PAGE_SIZE];
    memcpy(left_after, left_before, PAGE_SIZE);
    memset(right_after, 0, PAGE_SIZE);
    memset(next_after, 0, PAGE_SIZE);
    if (!is_tail) memcpy(next_after, next_before, PAGE_SIZE);

    if (!tinydb_slotted_leaf_v2_split_nonroot(left_after,
                                               PAGE_SIZE,
                                               left_page_num,
                                               right_after,
                                               PAGE_SIZE,
                                               right_leaf_page_num,
                                               NULL)) {
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "unable to stage bounded recursive payload leaf split");
        return false;
    }
    if (!is_tail) {
        tinydb_slotted_split_write_u32(
            next_after + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET,
            right_leaf_page_num);
        if (!tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE)) {
            release_work(&chain,
                         &claim_plan,
                         ancestor_images,
                         old_maxes,
                         new_internal_images,
                         ancestor_targets,
                         new_internal_targets);
            set_message(message,
                        message_size,
                        "bounded recursive payload sibling relink staging failed");
            return false;
        }
    }

    uint16_t left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    uint16_t right_count = tinydb_slotted_leaf_v2_count(right_after, PAGE_SIZE);
    uint32_t staged_left_max = 0u;
    uint32_t staged_right_max = 0u;
    if (left_count == 0u || right_count == 0u ||
        !tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &staged_left_max) ||
        !tinydb_leaf_page_key_at(right_after,
                                 PAGE_SIZE,
                                 (uint32_t)right_count - 1u,
                                 &staged_right_max) ||
        staged_right_max != old_left_max) {
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "bounded recursive payload split changed its pre-insert boundary");
        return false;
    }

    void* destination = key <= staged_left_max
        ? (void*)left_after
        : (void*)right_after;
    if (!tinydb_slotted_leaf_v2_insert(destination,
                                       PAGE_SIZE,
                                       key,
                                       envelope,
                                       (uint16_t)envelope_length) ||
        !tinydb_slotted_leaf_v2_validate(left_after, PAGE_SIZE) ||
        !tinydb_slotted_leaf_v2_validate(right_after, PAGE_SIZE) ||
        (!is_tail && !tinydb_slotted_leaf_v2_validate(next_after, PAGE_SIZE))) {
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "bounded recursive payload split leaves could not accept the pending row");
        return false;
    }

    left_count = tinydb_slotted_leaf_v2_count(left_after, PAGE_SIZE);
    right_count = tinydb_slotted_leaf_v2_count(right_after, PAGE_SIZE);
    uint32_t checked_left_max = 0u;
    uint32_t checked_right_max = 0u;
    if (!tinydb_leaf_page_key_at(left_after,
                                 PAGE_SIZE,
                                 (uint32_t)left_count - 1u,
                                 &checked_left_max) ||
        !tinydb_leaf_page_key_at(right_after,
                                 PAGE_SIZE,
                                 (uint32_t)right_count - 1u,
                                 &checked_right_max) ||
        checked_left_max != staged_left_max ||
        (!is_tail && checked_right_max != old_left_max) ||
        (is_tail && checked_right_max <= checked_left_max)) {
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "bounded recursive pending row invalidated split boundaries");
        return false;
    }

    uint32_t new_pages_used = 0u;
    uint32_t stop_level = 0u;
    bool root_grew = false;
    if (!tinydb_stage_internal_split_cascade(
            ancestor_images,
            PAGE_SIZE,
            chain.internal_pages,
            old_maxes,
            participating_ancestors,
            new_internal_images,
            PAGE_SIZE,
            new_internal_page_nums,
            reservation.new_internal_pages,
            left_page_num,
            right_leaf_page_num,
            old_left_max,
            checked_left_max,
            checked_right_max,
            &new_pages_used,
            &stop_level,
            &root_grew) ||
        new_pages_used != reservation.new_internal_pages ||
        stop_level + 1u != participating_ancestors ||
        root_grew != plan.requires_root_growth) {
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "bounded recursive internal cascade staging failed");
        return false;
    }

    TinyDBV2PagerPublishEntry* entries = (TinyDBV2PagerPublishEntry*)calloc(
        (size_t)publish_count, sizeof(TinyDBV2PagerPublishEntry));
    if (entries == NULL) {
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "unable to allocate recursive publication metadata");
        return false;
    }

    uint32_t claimed_count = 0u;
    if (!tinydb_record_payload_claim_prepared_pages(table->pager,
                                                    &claim_plan,
                                                    &claimed_count,
                                                    local_message,
                                                    sizeof(local_message))) {
        free(entries);
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    local_message[0] != '\0'
                        ? local_message
                        : "unable to claim bounded recursive payload pages");
        return false;
    }

    bool targets_valid = true;
    for (uint32_t i = 0u; i < participating_ancestors && targets_valid; i++) {
        targets_valid = child_assignment_preflight(
            table->pager,
            chain.internal_pages[i],
            image_at_const(ancestor_images, i));
    }
    for (uint32_t i = 0u; i < reservation.new_internal_pages && targets_valid; i++) {
        targets_valid = child_assignment_preflight(
            table->pager,
            new_internal_page_nums[i],
            image_at_const(new_internal_images, i));
    }
    if (!targets_valid) {
        (void)tinydb_record_payload_rollback_claimed_pages(table->pager,
                                                           &claim_plan,
                                                           claimed_count,
                                                           local_message,
                                                           sizeof(local_message));
        free(entries);
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "bounded recursive publication targets failed preflight");
        return false;
    }

    uint32_t entry_count = 0u;
    entries[entry_count++] = (TinyDBV2PagerPublishEntry){
        left_page_num, left_after};
    entries[entry_count++] = (TinyDBV2PagerPublishEntry){
        right_leaf_page_num, right_after};
    if (!is_tail) {
        entries[entry_count++] = (TinyDBV2PagerPublishEntry){
            next_page_num, next_after};
    }
    for (uint32_t i = 0u; i < participating_ancestors; i++) {
        entries[entry_count++] = (TinyDBV2PagerPublishEntry){
            chain.internal_pages[i], image_at(ancestor_images, i)};
    }
    for (uint32_t i = 0u; i < reservation.new_internal_pages; i++) {
        entries[entry_count++] = (TinyDBV2PagerPublishEntry){
            new_internal_page_nums[i], image_at(new_internal_images, i)};
    }
    if (entry_count != publish_count) {
        (void)tinydb_record_payload_rollback_claimed_pages(table->pager,
                                                           &claim_plan,
                                                           claimed_count,
                                                           local_message,
                                                           sizeof(local_message));
        free(entries);
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "bounded recursive publication count changed after staging");
        return false;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        (void)tinydb_record_payload_rollback_claimed_pages(table->pager,
                                                           &claim_plan,
                                                           claimed_count,
                                                           local_message,
                                                           sizeof(local_message));
        free(entries);
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "unable to persist generic-index mutation epoch");
        return false;
    }

    if (!tinydb_v2_pager_publish_batch(table->pager,
                                       entries,
                                       entry_count,
                                       TINYDB_V2_PUBLISH_NO_FAIL)) {
        (void)tinydb_record_payload_rollback_claimed_pages(table->pager,
                                                           &claim_plan,
                                                           claimed_count,
                                                           local_message,
                                                           sizeof(local_message));
        free(entries);
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "bounded recursive Pager-aware atomic page publication failed");
        return false;
    }
    free(entries);

    bool parents_ok = true;
    for (uint32_t i = 0u; i < participating_ancestors; i++) {
        parents_ok = assign_children(table->pager,
                                     chain.internal_pages[i],
                                     image_at_const(ancestor_images, i)) &&
                     parents_ok;
    }
    for (uint32_t i = 0u; i < reservation.new_internal_pages; i++) {
        parents_ok = assign_children(table->pager,
                                     new_internal_page_nums[i],
                                     image_at_const(new_internal_images, i)) &&
                     parents_ok;
    }
    if (!parents_ok) {
        release_work(&chain,
                     &claim_plan,
                     ancestor_images,
                     old_maxes,
                     new_internal_images,
                     ancestor_targets,
                     new_internal_targets);
        set_message(message,
                    message_size,
                    "bounded recursive descendant reparenting failed after validated publication");
        return false;
    }

    if (!table->in_transaction) pager_commit(table->pager);
    release_work(&chain,
                 &claim_plan,
                 ancestor_images,
                 old_maxes,
                 new_internal_images,
                 ancestor_targets,
                 new_internal_targets);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}
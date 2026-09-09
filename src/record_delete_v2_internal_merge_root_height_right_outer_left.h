#ifndef TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_ROOT_HEIGHT_RIGHT_OUTER_LEFT_H
#define TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_ROOT_HEIGHT_RIGHT_OUTER_LEFT_H

#include "generic_index_epoch.h"
#include "internal_root_height_right_outer_left_lower_stage.h"
#include "internal_root_collapse_stage.h"
#include "record_delete_v2_internal_merge_root.h"
#include "slotted_v2_publish_batch.h"

/*
 * Final bounded height-4 -> height-3 contraction for the eight-leaf minimum
 * fixture: remove the left child of the rightmost bottom parent.
 *
 *   p2 [A, B] + p3 [removed, D] -> p3 [A, B, D]
 *
 * The dedicated lower-stage repairs B<->D and reparents A/B/D to p3. The two
 * bottom parents under the left grandparent are then staged under the right
 * grandparent, which becomes [p0,p1,p3] and is promoted into the stable catalog
 * root. Publication happens only after every scratch image validates.
 */
static inline TinyDBInternalMergeRootResult
 tinydb_try_delete_v2_internal_merge_root_height_right_outer_left(
    Table* table,
    const TableSchema* schema,
    uint32_t removed_leaf_page_num,
    const unsigned char removed_leaf_before[PAGE_SIZE],
    uint32_t removed_key,
    char* message,
    size_t message_size) {
    Pager* pager = table == NULL ? NULL : table->pager;
    if (pager == NULL || schema == NULL || removed_leaf_before == NULL ||
        removed_leaf_page_num == 0u ||
        removed_leaf_page_num == INVALID_PAGE_NUM ||
        removed_leaf_page_num >= pager->num_pages ||
        removed_leaf_page_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(pager, removed_leaf_page_num) ||
        tinydb_leaf_format_detect_page(removed_leaf_before, PAGE_SIZE) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(removed_leaf_before, PAGE_SIZE)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t removed_count = 0u;
    uint32_t removed_only_key = 0u;
    if (!tinydb_leaf_page_count(removed_leaf_before, PAGE_SIZE, &removed_count) ||
        removed_count != 1u ||
        !tinydb_leaf_page_key_at(removed_leaf_before,
                                 PAGE_SIZE,
                                 0u,
                                 &removed_only_key) ||
        removed_only_key != removed_key) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t kept_num = tinydb_internal_borrow_read_u32(
        removed_leaf_before + PARENT_POINTER_OFFSET);
    if (kept_num == 0u || kept_num == INVALID_PAGE_NUM ||
        kept_num >= pager->num_pages ||
        tinydb_internal_borrow_page_free(pager, kept_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char kept[PAGE_SIZE];
    memcpy(kept, get_page(pager, kept_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(kept, PAGE_SIZE) ||
        kept[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            kept + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(kept, 0u) != removed_leaf_page_num ||
        tinydb_parent_stage_key_at(kept, 0u) != removed_key) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t right_grand_num = tinydb_internal_borrow_read_u32(
        kept + PARENT_POINTER_OFFSET);
    if (right_grand_num == 0u || right_grand_num == INVALID_PAGE_NUM ||
        right_grand_num >= pager->num_pages ||
        right_grand_num == schema->root_page_num ||
        tinydb_internal_borrow_page_free(pager, right_grand_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char right_grand[PAGE_SIZE];
    memcpy(right_grand, get_page(pager, right_grand_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(right_grand, PAGE_SIZE) ||
        right_grand[IS_ROOT_OFFSET] != 0u ||
        tinydb_parent_stage_read_u32(
            right_grand + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(right_grand, 1u) != kept_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t obsolete_num = tinydb_parent_stage_child_at(right_grand, 0u);
    if (obsolete_num == 0u || obsolete_num == INVALID_PAGE_NUM ||
        obsolete_num >= pager->num_pages || obsolete_num == kept_num ||
        tinydb_internal_borrow_page_free(pager, obsolete_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char obsolete[PAGE_SIZE];
    memcpy(obsolete, get_page(pager, obsolete_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(obsolete, PAGE_SIZE) ||
        obsolete[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(obsolete + PARENT_POINTER_OFFSET) !=
            right_grand_num ||
        tinydb_parent_stage_read_u32(
            obsolete + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t root_num = tinydb_internal_borrow_read_u32(
        right_grand + PARENT_POINTER_OFFSET);
    if (root_num != schema->root_page_num || root_num >= pager->num_pages ||
        tinydb_internal_borrow_page_free(pager, root_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char root_after[PAGE_SIZE];
    memcpy(root_after, get_page(pager, root_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(root_after, PAGE_SIZE) ||
        root_after[IS_ROOT_OFFSET] == 0u ||
        tinydb_parent_stage_read_u32(root_after + PARENT_POINTER_OFFSET) != 0u ||
        tinydb_parent_stage_read_u32(
            root_after + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u ||
        tinydb_parent_stage_child_at(root_after, 1u) != right_grand_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t left_grand_num = tinydb_parent_stage_child_at(root_after, 0u);
    if (left_grand_num == 0u || left_grand_num == INVALID_PAGE_NUM ||
        left_grand_num >= pager->num_pages || left_grand_num == right_grand_num ||
        tinydb_internal_borrow_page_free(pager, left_grand_num)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }
    unsigned char left_grand[PAGE_SIZE];
    memcpy(left_grand, get_page(pager, left_grand_num), PAGE_SIZE);
    if (!tinydb_parent_stage_validate(left_grand, PAGE_SIZE) ||
        left_grand[IS_ROOT_OFFSET] != 0u ||
        tinydb_internal_borrow_read_u32(left_grand + PARENT_POINTER_OFFSET) !=
            root_num ||
        tinydb_parent_stage_read_u32(
            left_grand + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t p_num[2] = {
        tinydb_parent_stage_child_at(left_grand, 0u),
        tinydb_parent_stage_child_at(left_grand, 1u)
    };
    unsigned char p_after[2][PAGE_SIZE];
    for (uint32_t i = 0u; i < 2u; i++) {
        if (p_num[i] == 0u || p_num[i] == INVALID_PAGE_NUM ||
            p_num[i] >= pager->num_pages ||
            tinydb_internal_borrow_page_free(pager, p_num[i])) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        memcpy(p_after[i], get_page(pager, p_num[i]), PAGE_SIZE);
        if (!tinydb_parent_stage_validate(p_after[i], PAGE_SIZE) ||
            p_after[i][IS_ROOT_OFFSET] != 0u ||
            tinydb_internal_borrow_read_u32(
                p_after[i] + PARENT_POINTER_OFFSET) != left_grand_num ||
            tinydb_parent_stage_read_u32(
                p_after[i] + INTERNAL_NODE_NUM_KEYS_OFFSET) != 1u) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
    }

    uint32_t survivor_nums[3] = {
        tinydb_parent_stage_child_at(obsolete, 0u),
        tinydb_parent_stage_child_at(obsolete, 1u),
        tinydb_parent_stage_child_at(kept, 1u)
    };
    unsigned char survivor_after[3][PAGE_SIZE];
    void* survivor_pages[3] = {
        survivor_after[0], survivor_after[1], survivor_after[2]
    };
    for (uint32_t i = 0u; i < 3u; i++) {
        if (survivor_nums[i] == 0u || survivor_nums[i] == INVALID_PAGE_NUM ||
            survivor_nums[i] >= pager->num_pages ||
            tinydb_internal_borrow_page_free(pager, survivor_nums[i])) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
        memcpy(survivor_after[i], get_page(pager, survivor_nums[i]), PAGE_SIZE);
    }

    uint32_t a_count = 0u;
    uint32_t a_max = 0u;
    uint32_t b_count = 0u;
    uint32_t b_max = 0u;
    uint32_t d_count = 0u;
    uint32_t d_max = 0u;
    if (!tinydb_leaf_page_count(survivor_after[0], PAGE_SIZE, &a_count) ||
        a_count == 0u ||
        !tinydb_leaf_page_key_at(survivor_after[0], PAGE_SIZE,
                                 a_count - 1u, &a_max) ||
        !tinydb_leaf_page_count(survivor_after[1], PAGE_SIZE, &b_count) ||
        b_count == 0u ||
        !tinydb_leaf_page_key_at(survivor_after[1], PAGE_SIZE,
                                 b_count - 1u, &b_max) ||
        !tinydb_leaf_page_count(survivor_after[2], PAGE_SIZE, &d_count) ||
        d_count == 0u ||
        !tinydb_leaf_page_key_at(survivor_after[2], PAGE_SIZE,
                                 d_count - 1u, &d_max) ||
        a_max != tinydb_parent_stage_key_at(obsolete, 0u) ||
        b_max != tinydb_parent_stage_key_at(right_grand, 0u) ||
        b_max >= removed_key || removed_key >= d_max) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    if (!tinydb_stage_root_height_right_outer_left_lower_merge(
            obsolete, PAGE_SIZE, obsolete_num,
            kept, PAGE_SIZE, kept_num,
            right_grand_num, right_grand_num,
            removed_leaf_before, PAGE_SIZE, removed_leaf_page_num, removed_key,
            survivor_pages, survivor_nums)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    uint32_t left0_max = tinydb_parent_stage_key_at(left_grand, 0u);
    uint32_t left_subtree_max = tinydb_parent_stage_key_at(root_after, 0u);
    if (left0_max >= left_subtree_max || left_subtree_max >= a_max ||
        a_max >= b_max || b_max >= d_max) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    for (uint32_t i = 0u; i < 2u; i++) {
        tinydb_parent_stage_write_u32(
            p_after[i] + PARENT_POINTER_OFFSET, right_grand_num);
        if (!tinydb_root_collapse_direct_child_valid(
                p_after[i], PAGE_SIZE, right_grand_num)) {
            return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
        }
    }

    const uint32_t merged_children[3] = {p_num[0], p_num[1], kept_num};
    const uint32_t merged_maxes[3] = {left0_max, left_subtree_max, d_max};
    if (!tinydb_nonroot_merge_build_three_child_internal(
            right_grand, root_num, merged_children, merged_maxes)) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    void* direct_children[3] = {p_after[0], p_after[1], kept};
    uint32_t promoted = INVALID_PAGE_NUM;
    if (!tinydb_stage_internal_root_collapse_to_internal(
            root_after, PAGE_SIZE, root_num,
            right_grand, PAGE_SIZE, right_grand_num, d_max,
            left_grand_num, left_subtree_max,
            direct_children, merged_children, 3u, &promoted) ||
        promoted != right_grand_num) {
        return TINYDB_INTERNAL_MERGE_ROOT_NOT_APPLICABLE;
    }

    if (!tinydb_generic_index_epoch_before_mutation(table, schema)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "unable to persist generic-index mutation epoch");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }

    TinyDBV2PublishEntry entries[7];
    entries[0] = (TinyDBV2PublishEntry){
        root_num, (unsigned char*)get_page(pager, root_num), root_after};
    entries[1] = (TinyDBV2PublishEntry){
        p_num[0], (unsigned char*)get_page(pager, p_num[0]), p_after[0]};
    entries[2] = (TinyDBV2PublishEntry){
        p_num[1], (unsigned char*)get_page(pager, p_num[1]), p_after[1]};
    entries[3] = (TinyDBV2PublishEntry){
        kept_num, (unsigned char*)get_page(pager, kept_num), kept};
    for (uint32_t i = 0u; i < 3u; i++) {
        entries[4u + i] = (TinyDBV2PublishEntry){
            survivor_nums[i],
            (unsigned char*)get_page(pager, survivor_nums[i]),
            survivor_after[i]};
    }

    if (!tinydb_v2_publish_batch(entries, 7u, TINYDB_V2_PUBLISH_NO_FAIL)) {
        tinydb_internal_merge_root_message(
            message, message_size,
            "right-outer-left recursive V2 root-height publication failed");
        return TINYDB_INTERNAL_MERGE_ROOT_FAILURE;
    }
    for (uint32_t i = 0u; i < 7u; i++) {
        mark_page_dirty(pager, entries[i].page_num);
    }

    pager_free_page(pager, removed_leaf_page_num);
    pager_free_page(pager, obsolete_num);
    pager_free_page(pager, left_grand_num);
    pager_free_page(pager, right_grand_num);
    if (!table->in_transaction) pager_commit(pager);
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return TINYDB_INTERNAL_MERGE_ROOT_SUCCESS;
}

#endif /* TINYDB_RECORD_DELETE_V2_INTERNAL_MERGE_ROOT_HEIGHT_RIGHT_OUTER_LEFT_H */

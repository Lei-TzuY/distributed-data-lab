#ifndef TINYDB_RECORD_PAYLOAD_PAGE_CLAIM_PLAN_H
#define TINYDB_RECORD_PAYLOAD_PAGE_CLAIM_PLAN_H

#include "record_payload_overflow_reservation.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t* page_nums;
    uint32_t count;
    uint32_t original_num_pages;
    uint32_t original_free_page_count;
} TinyDBPayloadPageClaimPlan;

static void tinydb_record_payload_page_claim_plan_release(
    TinyDBPayloadPageClaimPlan* claim_plan) {
    if (claim_plan == NULL) return;
    free(claim_plan->page_nums);
    memset(claim_plan, 0, sizeof(*claim_plan));
}

static bool tinydb_record_payload_claim_page_is_live_ancestor(
    const TinyDBPayloadAncestorChain* chain,
    uint32_t page_num) {
    if (chain == NULL) return true;
    if (page_num == chain->leaf_page_num) return true;
    for (uint32_t i = 0u; i < chain->count; i++) {
        if (chain->internal_pages[i] == page_num) return true;
    }
    return false;
}

static bool tinydb_record_payload_prepare_page_claim_plan(
    const Pager* pager,
    const TinyDBPayloadAncestorChain* chain,
    const TinyDBPayloadOverflowReservation* reservation,
    TinyDBPayloadPageClaimPlan* claim_plan,
    char* message,
    size_t message_size) {
    if (claim_plan != NULL) memset(claim_plan, 0, sizeof(*claim_plan));
    if (pager == NULL || chain == NULL || reservation == NULL ||
        claim_plan == NULL || chain->internal_pages == NULL ||
        chain->count == 0u || chain->leaf_page_num == INVALID_PAGE_NUM ||
        reservation->total_pages == 0u ||
        reservation->new_leaf_pages > UINT32_MAX - reservation->new_internal_pages ||
        reservation->total_pages !=
            reservation->new_leaf_pages + reservation->new_internal_pages) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "invalid recursive payload page-claim plan input");
        return false;
    }
    if (pager->free_page_count > pager->num_pages ||
        (pager->free_page_count > 0u && pager->free_pages == NULL)) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "recursive payload page-claim plan found an invalid allocator snapshot");
        return false;
    }

    uint32_t free_count = pager->free_page_count;
    uint32_t appended = reservation->total_pages > free_count
        ? reservation->total_pages - free_count
        : 0u;
    if (appended > 0u && pager->num_pages > INVALID_PAGE_NUM - appended) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "recursive payload page-claim plan overflows the pager address space");
        return false;
    }
#if SIZE_MAX < UINT32_MAX
    if ((size_t)reservation->total_pages > SIZE_MAX / sizeof(uint32_t)) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "recursive payload page-claim allocation overflows");
        return false;
    }
#endif

    uint32_t* page_nums = (uint32_t*)calloc(
        (size_t)reservation->total_pages, sizeof(uint32_t));
    if (page_nums == NULL) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "unable to allocate recursive payload page-claim plan");
        return false;
    }

    for (uint32_t i = 0u; i < reservation->total_pages; i++) {
        bool reuses_free_page = i < free_count;
        uint32_t page_num = reuses_free_page
            ? pager->free_pages[free_count - 1u - i]
            : pager->num_pages + (i - free_count);
        if (page_num == 0u || page_num == INVALID_PAGE_NUM ||
            (reuses_free_page && page_num >= pager->num_pages) ||
            tinydb_record_payload_claim_page_is_live_ancestor(chain, page_num)) {
            free(page_nums);
            tinydb_payload_ancestor_set_message(
                message, message_size, "recursive payload page reservation aliases or escapes live topology");
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (page_nums[j] == page_num) {
                free(page_nums);
                tinydb_payload_ancestor_set_message(
                    message, message_size, "recursive payload page reservation contains duplicates");
                return false;
            }
        }
        page_nums[i] = page_num;
    }

    claim_plan->page_nums = page_nums;
    claim_plan->count = reservation->total_pages;
    claim_plan->original_num_pages = pager->num_pages;
    claim_plan->original_free_page_count = pager->free_page_count;
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

#endif

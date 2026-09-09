#ifndef TINYDB_RECORD_PAYLOAD_PAGE_CLAIM_TRANSACTION_H
#define TINYDB_RECORD_PAYLOAD_PAGE_CLAIM_TRANSACTION_H

#include "record_payload_page_claim_plan.h"

#include <stdbool.h>
#include <stdint.h>

static bool tinydb_record_payload_rollback_claimed_pages(
    Pager* pager,
    const TinyDBPayloadPageClaimPlan* claim_plan,
    uint32_t claimed_count,
    char* message,
    size_t message_size) {
    if (pager == NULL || claim_plan == NULL || claim_plan->page_nums == NULL ||
        claimed_count > claim_plan->count ||
        claim_plan->original_free_page_count > claim_plan->original_num_pages) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "invalid recursive payload page-claim rollback input");
        return false;
    }

    uint32_t reused_count = claimed_count < claim_plan->original_free_page_count
        ? claimed_count
        : claim_plan->original_free_page_count;

    if (pager->num_pages < claim_plan->original_num_pages ||
        pager->free_page_count + reused_count != claim_plan->original_free_page_count) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "recursive payload page-claim rollback found unexpected allocator state");
        return false;
    }

    if (pager->num_pages > claim_plan->original_num_pages) {
        pager_shrink(pager, claim_plan->original_num_pages);
    }

    for (uint32_t i = reused_count; i > 0u; i--) {
        pager->free_pages[pager->free_page_count++] = claim_plan->page_nums[i - 1u];
    }

    if (pager->num_pages != claim_plan->original_num_pages ||
        pager->free_page_count != claim_plan->original_free_page_count) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "recursive payload page-claim rollback did not restore allocator counts");
        return false;
    }
    for (uint32_t i = 0u; i < reused_count; i++) {
        uint32_t restored = pager->free_pages[
            claim_plan->original_free_page_count - 1u - i];
        if (restored != claim_plan->page_nums[i]) {
            tinydb_payload_ancestor_set_message(
                message, message_size, "recursive payload page-claim rollback did not restore free-stack order");
            return false;
        }
    }

    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

static bool tinydb_record_payload_next_prepared_claim_matches(
    const Pager* pager,
    const TinyDBPayloadPageClaimPlan* claim_plan,
    uint32_t claim_index) {
    if (pager == NULL || claim_plan == NULL || claim_plan->page_nums == NULL ||
        claim_index >= claim_plan->count ||
        (pager->free_page_count > 0u && pager->free_pages == NULL)) {
        return false;
    }

    uint32_t next_page_num = pager->free_page_count > 0u
        ? pager->free_pages[pager->free_page_count - 1u]
        : pager->num_pages;
    return next_page_num == claim_plan->page_nums[claim_index];
}

static bool tinydb_record_payload_claim_prepared_pages(
    Pager* pager,
    const TinyDBPayloadPageClaimPlan* claim_plan,
    uint32_t* claimed_count_out,
    char* message,
    size_t message_size) {
    if (claimed_count_out != NULL) *claimed_count_out = 0u;
    if (pager == NULL || claim_plan == NULL || claim_plan->page_nums == NULL ||
        claim_plan->count == 0u || claimed_count_out == NULL ||
        pager->num_pages != claim_plan->original_num_pages ||
        pager->free_page_count != claim_plan->original_free_page_count) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "recursive payload page-claim plan is stale");
        return false;
    }

    uint32_t claimed_count = 0u;
    for (uint32_t i = 0u; i < claim_plan->count; i++) {
        if (!tinydb_record_payload_next_prepared_claim_matches(
                pager, claim_plan, i)) {
            if (!tinydb_record_payload_rollback_claimed_pages(
                    pager,
                    claim_plan,
                    claimed_count,
                    message,
                    message_size)) {
                return false;
            }
            *claimed_count_out = 0u;
            tinydb_payload_ancestor_set_message(
                message, message_size, "recursive payload allocator no longer matches the prepared page claims");
            return false;
        }

        uint32_t claimed = get_unused_page_num(pager);
        if (claimed != claim_plan->page_nums[i]) {
            if (!tinydb_record_payload_rollback_claimed_pages(
                    pager,
                    claim_plan,
                    claimed_count,
                    message,
                    message_size)) {
                return false;
            }
            *claimed_count_out = 0u;
            tinydb_payload_ancestor_set_message(
                message, message_size, "recursive payload allocator changed during a prepared page claim");
            return false;
        }

        (void)get_page(pager, claimed);
        claimed_count++;
        *claimed_count_out = claimed_count;
    }

    uint32_t appended = claim_plan->count > claim_plan->original_free_page_count
        ? claim_plan->count - claim_plan->original_free_page_count
        : 0u;
    if (pager->free_page_count !=
            claim_plan->original_free_page_count -
                (claim_plan->count < claim_plan->original_free_page_count
                    ? claim_plan->count
                    : claim_plan->original_free_page_count) ||
        pager->num_pages != claim_plan->original_num_pages + appended) {
        if (!tinydb_record_payload_rollback_claimed_pages(
                pager,
                claim_plan,
                claimed_count,
                message,
                message_size)) {
            return false;
        }
        tinydb_payload_ancestor_set_message(
            message, message_size, "recursive payload page claims produced unexpected allocator state");
        *claimed_count_out = 0u;
        return false;
    }

    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

#endif

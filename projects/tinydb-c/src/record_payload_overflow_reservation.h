#ifndef TINYDB_RECORD_PAYLOAD_OVERFLOW_RESERVATION_H
#define TINYDB_RECORD_PAYLOAD_OVERFLOW_RESERVATION_H

#include "record_payload_ancestor_chain.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t new_leaf_pages;
    uint32_t new_internal_pages;
    uint32_t total_pages;
} TinyDBPayloadOverflowReservation;

/*
 * Convert a validated recursive-overflow preflight into the complete allocator
 * budget required before mutation publication starts.
 *
 * Every leaf overflow needs one new right leaf. Each full non-root internal
 * ancestor needs one new right internal page. If the full chain reaches the
 * catalog-stable root, root growth needs two replacement internal children
 * instead of the single right page used by an ordinary non-root split. That
 * makes the all-full case `full_internal_levels + 1` new internal pages.
 *
 * The function is deliberately allocation-free and overflow-checked so the
 * eventual recursive mutation path can reserve all pages atomically before it
 * publishes the generic-index epoch or any staged topology image.
 */
static bool tinydb_record_payload_size_overflow_reservation(
    const TinyDBPayloadAncestorChain* chain,
    const TinyDBPayloadOverflowPlan* plan,
    TinyDBPayloadOverflowReservation* reservation,
    char* message,
    size_t message_size) {
    if (reservation != NULL) {
        reservation->new_leaf_pages = 0u;
        reservation->new_internal_pages = 0u;
        reservation->total_pages = 0u;
    }
    if (chain == NULL || plan == NULL || reservation == NULL ||
        chain->internal_pages == NULL || chain->count == 0u ||
        chain->leaf_page_num == INVALID_PAGE_NUM) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "invalid payload overflow reservation input");
        return false;
    }

    uint32_t new_internal_pages = 0u;
    if (plan->requires_root_growth) {
        if (plan->stopping_ancestor_index != INVALID_PAGE_NUM ||
            plan->full_internal_levels != chain->count ||
            plan->full_internal_levels == UINT32_MAX) {
            tinydb_payload_ancestor_set_message(
                message, message_size, "payload overflow root-growth reservation disagrees with the ancestry plan");
            return false;
        }
        new_internal_pages = plan->full_internal_levels + 1u;
    } else {
        if (plan->stopping_ancestor_index == INVALID_PAGE_NUM ||
            plan->stopping_ancestor_index >= chain->count ||
            plan->stopping_ancestor_index != plan->full_internal_levels) {
            tinydb_payload_ancestor_set_message(
                message, message_size, "payload overflow ancestor reservation disagrees with the stopping level");
            return false;
        }
        new_internal_pages = plan->full_internal_levels;
    }

    if (new_internal_pages == UINT32_MAX) {
        tinydb_payload_ancestor_set_message(
            message, message_size, "payload overflow reservation page count overflows");
        return false;
    }

    reservation->new_leaf_pages = 1u;
    reservation->new_internal_pages = new_internal_pages;
    reservation->total_pages = new_internal_pages + 1u;
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

#endif

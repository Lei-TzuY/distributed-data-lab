#include "pager_try_pin.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_COUNT (MAX_BUFFER_POOL_SIZE + 1u)
#define MARKER_BASE 0x7A110000u
#define DIRTY_MARKER 0xD17A5EEDu
#define EXTRA_PAGE MAX_BUFFER_POOL_SIZE

static uint32_t expected_marker(uint32_t page_num) {
    return MARKER_BASE ^ page_num;
}

static bool handle_marker_equals(PagerPageHandle* handle, uint32_t marker) {
    uint32_t actual = 0u;
    if (handle == NULL || !handle->pinned || handle->data == NULL) return false;
    memcpy(&actual, handle->data, sizeof(actual));
    return actual == marker;
}

static bool handle_marker_matches(PagerPageHandle* handle, uint32_t page_num) {
    return handle_marker_equals(handle, expected_marker(page_num));
}

static bool spill_marker_equals(Pager* pager, uint32_t page_num, uint32_t marker) {
    uint32_t actual = 0u;
    if (pager == NULL || page_num >= pager->page_capacity ||
        pager->dirty_page_spills[page_num] == NULL) {
        return false;
    }
    memcpy(&actual, pager->dirty_page_spills[page_num], sizeof(actual));
    return actual == marker;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: pager_try_pin_exhaustion_probe DATABASE\n");
        return 2;
    }

    Pager* pager = pager_open(argv[1]);
    if (pager->num_pages != 0u) {
        fprintf(stderr, "probe database must be empty\n");
        pager_close(pager);
        return 2;
    }

    for (uint32_t page_num = 0u; page_num < PAGE_COUNT; page_num++) {
        uint32_t marker = expected_marker(page_num);
        void* page = get_page(pager, page_num);
        memset(page, 0, PAGE_SIZE);
        memcpy(page, &marker, sizeof(marker));
        mark_page_dirty(pager, page_num);
        pager_unpin_page(pager, page_num);
    }
    pager_checkpoint(pager);

    PagerPageHandle owners[MAX_BUFFER_POOL_SIZE];
    for (uint32_t page_num = 0u; page_num < MAX_BUFFER_POOL_SIZE; page_num++) {
        if (!pager_pin_page_handle(pager, page_num, &owners[page_num]) ||
            !handle_marker_matches(&owners[page_num], page_num)) {
            fprintf(stderr, "unable to establish full-pool ownership at page %u\n",
                    page_num);
            return 1;
        }
    }

    PagerPageHandle extra_resident;
    if (pager_try_pin_existing_page_handle(pager, 1u, &extra_resident) !=
            PAGER_TRY_PIN_OK ||
        !handle_marker_matches(&extra_resident, 1u)) {
        fprintf(stderr, "resident try-pin failed while pool was fully pinned\n");
        return 1;
    }
    if (!pager_release_page_handle(&extra_resident)) {
        fprintf(stderr, "unable to release resident try-pin handle\n");
        return 1;
    }

    PagerPageHandle blocked;
    uint32_t evictions_before = pager->evictions;
    PagerTryPinStatus blocked_status =
        pager_try_pin_existing_page_handle(pager, EXTRA_PAGE, &blocked);
    if (blocked_status != PAGER_TRY_PIN_BUSY || blocked.pinned ||
        pager->evictions != evictions_before) {
        fprintf(stderr,
                "nonresident try-pin did not fail closed on all-pinned pool\n");
        return 1;
    }

    PagerPageHandle invalid;
    if (pager_try_pin_existing_page_handle(pager, PAGE_COUNT, &invalid) !=
            PAGER_TRY_PIN_INVALID_PAGE ||
        invalid.pinned) {
        fprintf(stderr, "out-of-range try-pin did not return INVALID_PAGE\n");
        return 1;
    }

    if (!pager_release_page_handle(&owners[0])) {
        fprintf(stderr, "unable to release one owner for retry\n");
        return 1;
    }

    PagerPageHandle retry;
    PagerTryPinStatus retry_status =
        pager_try_pin_existing_page_handle(pager, EXTRA_PAGE, &retry);
    if (retry_status != PAGER_TRY_PIN_OK ||
        !handle_marker_matches(&retry, EXTRA_PAGE) ||
        pager->evictions <= evictions_before) {
        fprintf(stderr, "try-pin did not recover after one frame became available\n");
        return 1;
    }
    if (!pager_release_page_handle(&retry)) {
        fprintf(stderr, "unable to release retry handle\n");
        return 1;
    }

    for (uint32_t page_num = 1u; page_num < MAX_BUFFER_POOL_SIZE; page_num++) {
        if (!pager_release_page_handle(&owners[page_num])) {
            fprintf(stderr, "unable to release owner %u\n", page_num);
            return 1;
        }
    }

    if (!pager_try_close(pager)) {
        fprintf(stderr, "try-pin exhaustion phase leaked ownership\n");
        return 1;
    }

    /*
     * Reopen with an empty cache, make page zero the LRU victim, dirty it
     * without touching its LRU position, then require the try-pin replacement
     * path to preserve the uncommitted image in no-steal spill storage.
     */
    pager = pager_open(argv[1]);
    pager_begin_transaction(pager);

    void* dirty_page = get_page(pager, 0u);
    for (uint32_t page_num = 1u; page_num < MAX_BUFFER_POOL_SIZE; page_num++) {
        void* page = get_page(pager, page_num);
        uint32_t marker = 0u;
        memcpy(&marker, page, sizeof(marker));
        if (marker != expected_marker(page_num)) {
            fprintf(stderr, "reopen marker mismatch at page %u\n", page_num);
            return 1;
        }
        pager_unpin_page(pager, page_num);
    }
    uint32_t dirty_marker = DIRTY_MARKER;
    memcpy(dirty_page, &dirty_marker, sizeof(dirty_marker));
    mark_page_dirty(pager, 0u);
    pager_unpin_page(pager, 0u);

    evictions_before = pager->evictions;
    PagerPageHandle spill_trigger;
    if (pager_try_pin_existing_page_handle(pager, EXTRA_PAGE, &spill_trigger) !=
            PAGER_TRY_PIN_OK ||
        !handle_marker_matches(&spill_trigger, EXTRA_PAGE) ||
        pager->evictions <= evictions_before ||
        !spill_marker_equals(pager, 0u, DIRTY_MARKER)) {
        fprintf(stderr, "try-pin did not preserve dirty LRU victim in spill storage\n");
        return 1;
    }
    if (!pager_release_page_handle(&spill_trigger)) {
        fprintf(stderr, "unable to release dirty-spill trigger handle\n");
        return 1;
    }

    PagerPageHandle dirty_reload;
    if (pager_try_pin_existing_page_handle(pager, 0u, &dirty_reload) !=
            PAGER_TRY_PIN_OK ||
        !handle_marker_equals(&dirty_reload, DIRTY_MARKER)) {
        fprintf(stderr, "try-pin reloaded disk image instead of dirty spill\n");
        return 1;
    }
    if (!pager_release_page_handle(&dirty_reload)) {
        fprintf(stderr, "unable to release dirty reload handle\n");
        return 1;
    }

    pager_rollback(pager);
    if (pager->in_transaction) {
        fprintf(stderr, "rollback did not finish after dirty spill phase\n");
        return 1;
    }

    PagerPageHandle rolled_back;
    if (pager_try_pin_existing_page_handle(pager, 0u, &rolled_back) !=
            PAGER_TRY_PIN_OK ||
        !handle_marker_matches(&rolled_back, 0u)) {
        fprintf(stderr, "rollback did not restore persisted page-zero marker\n");
        return 1;
    }
    if (!pager_release_page_handle(&rolled_back)) {
        fprintf(stderr, "unable to release rollback verification handle\n");
        return 1;
    }

    if (!pager_try_close(pager)) {
        fprintf(stderr, "try-pin dirty-spill phase leaked ownership\n");
        return 1;
    }

    printf("PAGER_TRY_PIN_EXHAUSTION_OK all_pinned_busy=yes "
           "resident_hit=yes release_retry=yes invalid_page=yes "
           "no_process_exit=yes checksum_marker=yes dirty_spill=yes "
           "spill_reload=yes rollback_restore=yes\n");
    return 0;
}

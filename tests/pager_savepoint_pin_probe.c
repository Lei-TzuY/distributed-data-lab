#include "pager.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define ORIGINAL_MARKER 0x51A70001u
#define MODIFIED_MARKER 0x51A7BEEFu
#define TARGET_PAGE 1u

static uint32_t read_marker(Pager* pager, uint32_t page_num) {
    uint32_t marker = 0u;
    void* page = get_page(pager, page_num);
    memcpy(&marker, page, sizeof(marker));
    pager_unpin_page(pager, page_num);
    return marker;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: pager_savepoint_pin_probe DATABASE\n");
        return 2;
    }

    Pager* pager = pager_open(argv[1]);
    if (pager->num_pages != 0u) {
        fprintf(stderr, "probe database must be empty\n");
        pager_close(pager);
        return 2;
    }

    for (uint32_t page_num = 0u; page_num < 4u; page_num++) {
        uint32_t marker = ORIGINAL_MARKER ^ page_num;
        void* page = get_page(pager, page_num);
        memset(page, 0, PAGE_SIZE);
        memcpy(page, &marker, sizeof(marker));
        mark_page_dirty(pager, page_num);
        pager_unpin_page(pager, page_num);
    }
    pager_checkpoint(pager);

    pager_begin_transaction(pager);
    if (!pager_savepoint(pager, "pin_guard")) {
        fprintf(stderr, "unable to create pin_guard savepoint\n");
        pager_close(pager);
        return 1;
    }

    {
        void* page = get_page(pager, TARGET_PAGE);
        uint32_t marker = MODIFIED_MARKER;
        memcpy(page, &marker, sizeof(marker));
        mark_page_dirty(pager, TARGET_PAGE);
        pager_unpin_page(pager, TARGET_PAGE);
    }

    PagerPageHandle handle;
    if (!pager_pin_page_handle(pager, TARGET_PAGE, &handle)) {
        fprintf(stderr, "unable to pin savepoint rollback target\n");
        pager_close(pager);
        return 1;
    }
    if (pager->pin_admissions != 0u || pager->pin_barrier_active) {
        fprintf(stderr, "pin admission did not drain after handle publication\n");
        pager_close(pager);
        return 1;
    }

    uint32_t savepoints_before = pager->savepoint_count;
    if (pager_rollback_to_savepoint(pager, "pin_guard")) {
        fprintf(stderr, "savepoint rollback unexpectedly crossed a live pin\n");
        pager_close(pager);
        return 1;
    }
    if (!pager->in_transaction ||
        pager->savepoint_count != savepoints_before ||
        pager->pin_barrier_active ||
        pager->pin_admissions != 0u ||
        read_marker(pager, TARGET_PAGE) != MODIFIED_MARKER ||
        !handle.pinned ||
        pager->frames[handle.frame_idx].pin_count == 0u) {
        fprintf(stderr, "refused savepoint rollback mutated Pager state\n");
        pager_close(pager);
        return 1;
    }

    if (!pager_release_page_handle(&handle)) {
        fprintf(stderr, "unable to release savepoint rollback handle\n");
        pager_close(pager);
        return 1;
    }

    if (!pager_rollback_to_savepoint(pager, "pin_guard")) {
        fprintf(stderr, "savepoint rollback did not resume after pin release\n");
        pager_close(pager);
        return 1;
    }
    if (pager->pin_barrier_active || pager->pin_admissions != 0u ||
        read_marker(pager, TARGET_PAGE) != (ORIGINAL_MARKER ^ TARGET_PAGE)) {
        fprintf(stderr, "successful savepoint rollback did not restore snapshot\n");
        pager_close(pager);
        return 1;
    }

    pager_rollback(pager);
    if (pager->in_transaction) {
        fprintf(stderr, "transaction rollback did not finish after savepoint probe\n");
        pager_close(pager);
        return 1;
    }

    PagerPageHandle close_handle;
    if (!pager_pin_page_handle(pager, TARGET_PAGE, &close_handle)) {
        fprintf(stderr, "unable to pin close guard target\n");
        pager_close(pager);
        return 1;
    }
    int close_frame = close_handle.frame_idx;
    if (pager_try_close(pager)) {
        fprintf(stderr, "pager_try_close unexpectedly destroyed a pinned Pager\n");
        return 1;
    }
    if (!close_handle.pinned || pager->pin_barrier_active ||
        pager->frames[close_frame].page_num != TARGET_PAGE ||
        pager->frames[close_frame].pin_count == 0u ||
        read_marker(pager, TARGET_PAGE) != (ORIGINAL_MARKER ^ TARGET_PAGE)) {
        fprintf(stderr, "try-close guard invalidated a live pinned handle\n");
        return 1;
    }
    if (!pager_release_page_handle(&close_handle)) {
        fprintf(stderr, "unable to release close guard handle\n");
        return 1;
    }

    if (!pager_try_close(pager)) {
        fprintf(stderr, "pager_try_close did not succeed after pin release\n");
        return 1;
    }
    printf("SAVEPOINT_PIN_BARRIER_OK existing_pin_rejected=yes "
           "release_retry=yes admission_drained=yes barrier_cleared=yes "
           "close_pin_guard=yes try_close_busy=yes try_close_success=yes\n");
    return 0;
}

#include "pager.h"
#include "slotted_v2_pager_publish_batch.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TARGET_PAGE (TABLE_MAX_PAGES + 32u)
#define MARKER_BASE 0xC0DE0000u
#define PUBLISH_MARKER_BASE 0xA7100000u
#define PREEXISTING_DIRTY_MARKER_BASE 0xD17A0000u
#define PREEXISTING_DIRTY_PAGE 7u
#define PINNED_TEST_PAGE 0u
#define LEGACY_READ_LOCK_PAGE 1u
#define LEGACY_WRITE_LOCK_PAGE 2u
#define FREE_GUARD_PAGE 3u
#define ROLLBACK_GUARD_PAGE 4u
#define PUBLISH_PAGE_COUNT 64u
#define PUBLISH_FAIL_AFTER 33u
#define EVICTION_CHURN_START 128u
#define EVICTION_CHURN_PAGES (MAX_BUFFER_POOL_SIZE * 3u)

static int verify_page(Pager* pager, uint32_t page_num) {
    uint32_t marker = 0;
    void* page = get_page(pager, page_num);
    memcpy(&marker, page, sizeof(marker));
    return marker == (MARKER_BASE ^ page_num) ? 0 : 1;
}

static int verify_publish_marker(Pager* pager,
                                 uint32_t page_num,
                                 uint32_t marker_base) {
    uint32_t marker = 0;
    void* page = get_page(pager, page_num);
    memcpy(&marker, page, sizeof(marker));
    return marker == (marker_base ^ page_num) ? 0 : 1;
}

static void force_eviction_churn(Pager* pager) {
    for (uint32_t i = 0u; i < EVICTION_CHURN_PAGES; i++) {
        uint32_t page_num = EVICTION_CHURN_START + i;
        (void)get_page(pager, page_num);
        pager_unpin_page(pager, page_num);
    }
}

static int exercise_pinned_page_handle(Pager* pager) {
    PagerPageHandle handle;
    if (!pager_pin_page_handle(pager, PINNED_TEST_PAGE, &handle) ||
        !handle.pinned || handle.data == NULL || handle.frame_idx < 0) {
        fprintf(stderr, "unable to pin page handle\n");
        return 1;
    }

    uint32_t explicit_pin_count = pager->frames[handle.frame_idx].pin_count;
    pager_unpin_page(pager, PINNED_TEST_PAGE);
    if (explicit_pin_count == 0u ||
        pager->frames[handle.frame_idx].pin_count != explicit_pin_count) {
        fprintf(stderr, "generic unpin stole an explicit page-handle pin\n");
        return 1;
    }

    uint32_t marker = 0u;
    if (!pager_page_handle_acquire_read(&handle)) {
        fprintf(stderr, "unable to acquire pinned read lock\n");
        return 1;
    }
    memcpy(&marker, handle.data, sizeof(marker));
    if (!pager_page_handle_release_read(&handle) ||
        marker != (MARKER_BASE ^ PINNED_TEST_PAGE)) {
        fprintf(stderr, "pinned read handle returned the wrong page image\n");
        return 1;
    }

    if (!pager_page_handle_acquire_write(&handle) ||
        !pager_page_handle_release_write(&handle)) {
        fprintf(stderr, "pinned write lock lifecycle failed\n");
        return 1;
    }

    int pinned_frame = handle.frame_idx;
    void* pinned_data = handle.data;
    uint32_t evictions_before = pager->evictions;
    force_eviction_churn(pager);

    marker = 0u;
    memcpy(&marker, handle.data, sizeof(marker));
    if (pager->evictions <= evictions_before ||
        pager->page_table[PINNED_TEST_PAGE] != pinned_frame ||
        pager->frames[pinned_frame].page_num != PINNED_TEST_PAGE ||
        pager->frames[pinned_frame].data != pinned_data ||
        pager->frames[pinned_frame].pin_count == 0u ||
        marker != (MARKER_BASE ^ PINNED_TEST_PAGE)) {
        fprintf(stderr, "LRU evicted or corrupted a pinned page handle\n");
        return 1;
    }

    if (!pager_release_page_handle(&handle) || handle.pinned) {
        fprintf(stderr, "unable to release pinned page handle\n");
        return 1;
    }

    evictions_before = pager->evictions;
    force_eviction_churn(pager);
    if (pager->evictions <= evictions_before ||
        pager->page_table[PINNED_TEST_PAGE] != -1 ||
        verify_page(pager, PINNED_TEST_PAGE) != 0) {
        fprintf(stderr, "released page did not return to the eviction set\n");
        return 1;
    }

    return 0;
}

static int exercise_legacy_page_number_locks(Pager* pager) {
    uint32_t marker = 0u;
    void* read_data = get_page(pager, LEGACY_READ_LOCK_PAGE);
    int read_frame = pager->page_table[LEGACY_READ_LOCK_PAGE];
    if (read_frame < 0) {
        fprintf(stderr, "legacy read-lock page was not resident\n");
        return 1;
    }

    pager_acquire_read_lock(pager, LEGACY_READ_LOCK_PAGE);
    uint32_t read_pin_count = pager->frames[read_frame].pin_count;
    pager_unpin_page(pager, LEGACY_READ_LOCK_PAGE);
    if (read_pin_count == 0u ||
        pager->frames[read_frame].pin_count != read_pin_count) {
        fprintf(stderr, "generic unpin stole a legacy read-lock pin\n");
        return 1;
    }

    uint32_t evictions_before = pager->evictions;
    force_eviction_churn(pager);
    memcpy(&marker, read_data, sizeof(marker));
    if (pager->evictions <= evictions_before ||
        pager->page_table[LEGACY_READ_LOCK_PAGE] != read_frame ||
        pager->frames[read_frame].page_num != LEGACY_READ_LOCK_PAGE ||
        pager->frames[read_frame].data != read_data ||
        marker != (MARKER_BASE ^ LEGACY_READ_LOCK_PAGE)) {
        fprintf(stderr, "legacy read lock did not protect frame identity\n");
        return 1;
    }
    pager_release_read_lock(pager, LEGACY_READ_LOCK_PAGE);
    if (pager->frames[read_frame].pin_count != 0u) {
        fprintf(stderr, "legacy read lock leaked its frame pin\n");
        return 1;
    }

    force_eviction_churn(pager);
    if (pager->page_table[LEGACY_READ_LOCK_PAGE] != -1 ||
        verify_page(pager, LEGACY_READ_LOCK_PAGE) != 0) {
        fprintf(stderr, "legacy read-lock page did not become evictable\n");
        return 1;
    }

    void* write_data = get_page(pager, LEGACY_WRITE_LOCK_PAGE);
    int write_frame = pager->page_table[LEGACY_WRITE_LOCK_PAGE];
    if (write_frame < 0) {
        fprintf(stderr, "legacy write-lock page was not resident\n");
        return 1;
    }

    pager_acquire_write_lock(pager, LEGACY_WRITE_LOCK_PAGE);
    uint32_t write_pin_count = pager->frames[write_frame].pin_count;
    pager_unpin_page(pager, LEGACY_WRITE_LOCK_PAGE);
    if (write_pin_count == 0u ||
        pager->frames[write_frame].pin_count != write_pin_count) {
        fprintf(stderr, "generic unpin stole a legacy write-lock pin\n");
        return 1;
    }

    evictions_before = pager->evictions;
    force_eviction_churn(pager);
    marker = 0u;
    memcpy(&marker, write_data, sizeof(marker));
    if (pager->evictions <= evictions_before ||
        pager->page_table[LEGACY_WRITE_LOCK_PAGE] != write_frame ||
        pager->frames[write_frame].page_num != LEGACY_WRITE_LOCK_PAGE ||
        pager->frames[write_frame].data != write_data ||
        marker != (MARKER_BASE ^ LEGACY_WRITE_LOCK_PAGE)) {
        fprintf(stderr, "legacy write lock did not protect frame identity\n");
        return 1;
    }
    pager_release_write_lock(pager, LEGACY_WRITE_LOCK_PAGE);
    if (pager->frames[write_frame].pin_count != 0u) {
        fprintf(stderr, "legacy write lock leaked its frame pin\n");
        return 1;
    }

    force_eviction_churn(pager);
    if (pager->page_table[LEGACY_WRITE_LOCK_PAGE] != -1 ||
        verify_page(pager, LEGACY_WRITE_LOCK_PAGE) != 0) {
        fprintf(stderr, "legacy write-lock page did not become evictable\n");
        return 1;
    }

    return 0;
}

static int exercise_destructive_pin_guards(Pager* pager) {
    PagerPageHandle handle;

    if (!pager_pin_page_handle(pager, FREE_GUARD_PAGE, &handle)) {
        fprintf(stderr, "unable to pin free-page guard target\n");
        return 1;
    }
    uint32_t free_count_before = pager->free_page_count;
    int free_frame = handle.frame_idx;
    pager_free_page(pager, FREE_GUARD_PAGE);
    if (pager->free_page_count != free_count_before ||
        pager->page_table[FREE_GUARD_PAGE] != free_frame ||
        pager->frames[free_frame].page_num != FREE_GUARD_PAGE ||
        pager->frames[free_frame].pin_count == 0u ||
        verify_page(pager, FREE_GUARD_PAGE) != 0) {
        fprintf(stderr, "free-page guard allowed a pinned page to be reclaimed\n");
        return 1;
    }
    if (!pager_release_page_handle(&handle)) {
        fprintf(stderr, "unable to release free-page guard handle\n");
        return 1;
    }

    if (!pager_pin_page_handle(pager, TARGET_PAGE, &handle)) {
        fprintf(stderr, "unable to pin shrink guard target\n");
        return 1;
    }
    uint32_t pages_before = pager->num_pages;
    int shrink_frame = handle.frame_idx;
    pager_shrink(pager, TARGET_PAGE);
    if (pager->num_pages != pages_before ||
        pager->page_table[TARGET_PAGE] != shrink_frame ||
        pager->frames[shrink_frame].page_num != TARGET_PAGE ||
        pager->frames[shrink_frame].pin_count == 0u ||
        verify_page(pager, TARGET_PAGE) != 0) {
        fprintf(stderr, "shrink guard invalidated a pinned page\n");
        return 1;
    }
    if (!pager_release_page_handle(&handle)) {
        fprintf(stderr, "unable to release shrink guard handle\n");
        return 1;
    }

    pager_begin_transaction(pager);
    if (!pager_pin_page_handle(pager, ROLLBACK_GUARD_PAGE, &handle)) {
        fprintf(stderr, "unable to pin rollback guard target\n");
        return 1;
    }
    int rollback_frame = handle.frame_idx;
    pager_rollback(pager);
    if (!pager->in_transaction ||
        pager->page_table[ROLLBACK_GUARD_PAGE] != rollback_frame ||
        pager->frames[rollback_frame].page_num != ROLLBACK_GUARD_PAGE ||
        pager->frames[rollback_frame].pin_count == 0u ||
        verify_page(pager, ROLLBACK_GUARD_PAGE) != 0) {
        fprintf(stderr, "transaction rollback invalidated a pinned page\n");
        return 1;
    }
    if (!pager_release_page_handle(&handle)) {
        fprintf(stderr, "unable to release rollback guard handle\n");
        return 1;
    }
    pager_rollback(pager);
    if (pager->in_transaction || verify_page(pager, ROLLBACK_GUARD_PAGE) != 0) {
        fprintf(stderr, "rollback did not resume after pin release\n");
        return 1;
    }

    return 0;
}

static int exercise_pager_aware_publication(Pager* pager) {
    unsigned char* staged = (unsigned char*)calloc(
        PUBLISH_PAGE_COUNT, PAGE_SIZE);
    TinyDBV2PagerPublishEntry* entries =
        (TinyDBV2PagerPublishEntry*)calloc(
            PUBLISH_PAGE_COUNT, sizeof(TinyDBV2PagerPublishEntry));
    if (staged == NULL || entries == NULL) {
        free(entries);
        free(staged);
        fprintf(stderr, "unable to allocate pager-aware publication probe\n");
        return 1;
    }

    /*
     * Start with one page already dirty before staged publication begins, then
     * force it out of the 16-frame cache. This proves rollback restores both
     * the pre-existing transactional image and its dirty state even when the
     * before-image originally came back through no-steal spill storage.
     */
    {
        unsigned char* page =
            (unsigned char*)get_page(pager, PREEXISTING_DIRTY_PAGE);
        uint32_t dirty_marker =
            PREEXISTING_DIRTY_MARKER_BASE ^ PREEXISTING_DIRTY_PAGE;
        memcpy(page, &dirty_marker, sizeof(dirty_marker));
        mark_page_dirty(pager, PREEXISTING_DIRTY_PAGE);
        pager_unpin_page(pager, PREEXISTING_DIRTY_PAGE);
    }

    uint32_t preexisting_evictions = pager->evictions;
    force_eviction_churn(pager);
    if (pager->evictions <= preexisting_evictions ||
        pager->page_table[PREEXISTING_DIRTY_PAGE] != -1 ||
        !pager_page_is_dirty(pager, PREEXISTING_DIRTY_PAGE) ||
        verify_publish_marker(pager,
                              PREEXISTING_DIRTY_PAGE,
                              PREEXISTING_DIRTY_MARKER_BASE) != 0) {
        fprintf(stderr,
                "pre-existing dirty page did not survive no-steal eviction\n");
        free(entries);
        free(staged);
        return 1;
    }

    for (uint32_t i = 0u; i < PUBLISH_PAGE_COUNT; i++) {
        unsigned char* staged_page = staged + (size_t)i * PAGE_SIZE;
        memcpy(staged_page, get_page(pager, i), PAGE_SIZE);
        uint32_t publish_marker = PUBLISH_MARKER_BASE ^ i;
        memcpy(staged_page, &publish_marker, sizeof(publish_marker));
        entries[i].page_num = i;
        entries[i].staged = staged_page;
    }

    uint32_t evictions_before = pager->evictions;
    if (tinydb_v2_pager_publish_batch(pager,
                                      entries,
                                      PUBLISH_PAGE_COUNT,
                                      PUBLISH_FAIL_AFTER)) {
        fprintf(stderr, "injected pager-aware publication unexpectedly succeeded\n");
        free(entries);
        free(staged);
        return 1;
    }
    for (uint32_t i = 0u; i < PUBLISH_PAGE_COUNT; i++) {
        uint32_t expected_marker_base = i == PREEXISTING_DIRTY_PAGE
            ? PREEXISTING_DIRTY_MARKER_BASE
            : MARKER_BASE;
        bool expected_dirty = i == PREEXISTING_DIRTY_PAGE;
        if (verify_publish_marker(pager, i, expected_marker_base) != 0 ||
            pager_page_is_dirty(pager, i) != expected_dirty) {
            fprintf(stderr,
                    "pager-aware rollback failed on page %u\n",
                    i);
            free(entries);
            free(staged);
            return 1;
        }
    }
    if (pager->evictions <= evictions_before) {
        fprintf(stderr, "pager-aware publication did not exercise LRU eviction\n");
        free(entries);
        free(staged);
        return 1;
    }

    /* Re-evict the restored dirty page and prove its rolled-back bytes are now
     * the spill image, rather than the rejected staged publication image. */
    force_eviction_churn(pager);
    if (pager->page_table[PREEXISTING_DIRTY_PAGE] != -1 ||
        verify_publish_marker(pager,
                              PREEXISTING_DIRTY_PAGE,
                              PREEXISTING_DIRTY_MARKER_BASE) != 0 ||
        !pager_page_is_dirty(pager, PREEXISTING_DIRTY_PAGE)) {
        fprintf(stderr,
                "pre-existing dirty rollback did not survive re-eviction\n");
        free(entries);
        free(staged);
        return 1;
    }

    if (!tinydb_v2_pager_publish_batch(pager,
                                       entries,
                                       PUBLISH_PAGE_COUNT,
                                       TINYDB_V2_PUBLISH_NO_FAIL)) {
        fprintf(stderr, "pager-aware publication failed\n");
        free(entries);
        free(staged);
        return 1;
    }
    for (uint32_t i = 0u; i < PUBLISH_PAGE_COUNT; i++) {
        if (verify_publish_marker(pager, i, PUBLISH_MARKER_BASE) != 0 ||
            !pager_page_is_dirty(pager, i)) {
            fprintf(stderr,
                    "pager-aware publication mismatch on page %u\n",
                    i);
            free(entries);
            free(staged);
            return 1;
        }
    }

    free(entries);
    free(staged);
    return 0;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: tinydb_pager_growth_probe DATABASE\n");
        return 2;
    }

    Pager* pager = pager_open(argv[1]);
    if (pager->num_pages != 0) {
        fprintf(stderr, "probe database must be empty\n");
        pager_close(pager);
        return 2;
    }

    for (uint32_t page_num = 0; page_num <= TARGET_PAGE; page_num++) {
        uint32_t marker = MARKER_BASE ^ page_num;
        void* page = get_page(pager, page_num);
        memset(page, 0, PAGE_SIZE);
        memcpy(page, &marker, sizeof(marker));
        mark_page_dirty(pager, page_num);
    }

    if (pager_metadata_capacity(pager) <= TABLE_MAX_PAGES) {
        fprintf(stderr,
                "pager metadata did not grow beyond legacy ceiling: %u\n",
                pager_metadata_capacity(pager));
        pager_close(pager);
        return 1;
    }

    pager_commit(pager);
    pager_checkpoint(pager);
    uint32_t capacity_after_growth = pager_metadata_capacity(pager);
    uint32_t pages_after_growth = pager->num_pages;
    pager_close(pager);

    pager = pager_open(argv[1]);
    if (pager->num_pages != TARGET_PAGE + 1u) {
        fprintf(stderr,
                "unexpected page count after reopen: got=%u expected=%u\n",
                pager->num_pages,
                TARGET_PAGE + 1u);
        pager_close(pager);
        return 1;
    }

    const uint32_t samples[] = {
        0u,
        1u,
        TABLE_MAX_PAGES - 1u,
        TABLE_MAX_PAGES,
        TARGET_PAGE
    };
    for (size_t i = 0; i < sizeof(samples) / sizeof(samples[0]); i++) {
        if (verify_page(pager, samples[i]) != 0) {
            fprintf(stderr, "marker mismatch on page %u\n", samples[i]);
            pager_close(pager);
            return 1;
        }
    }

    if (exercise_pinned_page_handle(pager) != 0 ||
        exercise_legacy_page_number_locks(pager) != 0 ||
        exercise_destructive_pin_guards(pager) != 0 ||
        exercise_pager_aware_publication(pager) != 0) {
        pager_close(pager);
        return 1;
    }

    printf("PAGER_GROWTH_OK pages=%u capacity=%u legacy_ceiling=%u "
           "pager_publish_pages=%u buffer_pool=%u eviction_safe=yes "
           "publish_rollback=yes preexisting_dirty_rollback=yes "
           "pin_eviction_guard=yes pinned_rwlock=yes "
           "legacy_lock_pin=yes unowned_unpin_safe=yes "
           "destructive_pin_guard=yes\n",
           pages_after_growth,
           capacity_after_growth,
           (unsigned)TABLE_MAX_PAGES,
           (unsigned)PUBLISH_PAGE_COUNT,
           (unsigned)MAX_BUFFER_POOL_SIZE);
    pager_close(pager);
    return 0;
}

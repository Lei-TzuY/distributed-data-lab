#ifndef TINYDB_SLOTTED_V2_PAGER_PUBLISH_BATCH_H
#define TINYDB_SLOTTED_V2_PAGER_PUBLISH_BATCH_H

#include "slotted_v2_publish_budget.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t page_num;
    const unsigned char* staged;
} TinyDBV2PagerPublishEntry;

/*
 * Pager-aware variant of staged V2 publication. Unlike the pointer-based
 * helper, this routine never requires every target page to remain resident in
 * the buffer pool at once. It snapshots and publishes by page number, marking
 * each modified frame dirty before the next get_page() may evict it. Dirty
 * evictions are therefore preserved by the Pager's no-steal spill storage.
 *
 * This is the appropriate publication boundary for recursive tree mutations
 * whose staged page set can exceed MAX_BUFFER_POOL_SIZE. The shared rollback
 * byte budget bounds before-image memory independently of buffer-pool size.
 */
static bool tinydb_v2_pager_publish_batch(
    Pager* pager,
    const TinyDBV2PagerPublishEntry* entries,
    uint32_t count,
    uint32_t fail_after) {
    if (pager == NULL || entries == NULL || fail_after == 0u) return false;

    size_t snapshot_bytes = 0u;
    if (!tinydb_v2_publish_batch_snapshot_size(count, &snapshot_bytes)) {
        return false;
    }

    for (uint32_t i = 0u; i < count; i++) {
        if (entries[i].page_num == INVALID_PAGE_NUM ||
            entries[i].page_num >= pager->num_pages ||
            entries[i].staged == NULL) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (entries[j].page_num == entries[i].page_num) return false;
        }
    }

    unsigned char* before = (unsigned char*)malloc(snapshot_bytes);
    bool* before_dirty = (bool*)calloc((size_t)count, sizeof(bool));
    if (before == NULL || before_dirty == NULL) {
        free(before_dirty);
        free(before);
        return false;
    }

    for (uint32_t i = 0u; i < count; i++) {
        before_dirty[i] = pager_page_is_dirty(pager, entries[i].page_num);
        unsigned char* target =
            (unsigned char*)get_page(pager, entries[i].page_num);
        memcpy(before + (size_t)i * (size_t)PAGE_USABLE_SIZE,
               target,
               PAGE_USABLE_SIZE);
        pager_unpin_page(pager, entries[i].page_num);
    }

    for (uint32_t i = 0u; i < count; i++) {
        unsigned char* target =
            (unsigned char*)get_page(pager, entries[i].page_num);
        memcpy(target, entries[i].staged, PAGE_USABLE_SIZE);
        mark_page_dirty(pager, entries[i].page_num);
        pager_unpin_page(pager, entries[i].page_num);

        if (fail_after != TINYDB_V2_PUBLISH_NO_FAIL && i + 1u == fail_after) {
            for (uint32_t j = 0u; j < count; j++) {
                unsigned char* rollback_target =
                    (unsigned char*)get_page(pager, entries[j].page_num);
                memcpy(rollback_target,
                       before + (size_t)j * (size_t)PAGE_USABLE_SIZE,
                       PAGE_USABLE_SIZE);
                pager_restore_page_dirty_state(pager,
                                               entries[j].page_num,
                                               before_dirty[j]);
                pager_unpin_page(pager, entries[j].page_num);
            }
            free(before_dirty);
            free(before);
            return false;
        }
    }

    free(before_dirty);
    free(before);
    return true;
}

#endif

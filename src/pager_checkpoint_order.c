#include "pager_try_checkpoint.h"
#include "pager_try_pin.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/types.h>
#endif

void pager_checkpoint_legacy_base(Pager* pager);

static void allocation_failure(void) {
    fprintf(stderr, "Unable to snapshot overlapping dirty pages before checkpoint.\n");
    exit(EXIT_FAILURE);
}

void pager_checkpoint(Pager* pager) {
    if (pager == NULL) return;

    /* Only pages that carry both an older committed shadow and a newer dirty
     * image need special ordering. Plain dirty pages can remain on the legacy
     * checkpoint fast path without extra copies or a second write. */
    uint32_t overlap_count = 0u;
    for (uint32_t i = 0u; i < pager->num_pages; i++) {
        if (pager->is_dirty[i] && pager->committed_pages[i] != NULL) {
            overlap_count++;
        }
    }
    if (overlap_count == 0u) {
        pager_checkpoint_legacy_base(pager);
        return;
    }

    uint32_t* overlap_pages =
        (uint32_t*)malloc((size_t)overlap_count * sizeof(uint32_t));
    unsigned char* overlap_images =
        (unsigned char*)malloc((size_t)overlap_count * PAGE_SIZE);
    if (overlap_pages == NULL || overlap_images == NULL) {
        free(overlap_pages);
        free(overlap_images);
        allocation_failure();
    }

    uint32_t saved = 0u;
    for (uint32_t i = 0u; i < pager->num_pages; i++) {
        if (!pager->is_dirty[i] || pager->committed_pages[i] == NULL) continue;

        overlap_pages[saved] = i;
        memcpy(overlap_images + (size_t)saved * PAGE_SIZE,
               get_page(pager, i),
               PAGE_SIZE);
        pager_unpin_page(pager, i);

        /* Temporarily hide only the newer overlapping image so the first pass
         * can publish its older WAL-committed shadow. Other dirty pages remain
         * visible and are flushed normally by that same pass. */
        pager->is_dirty[i] = false;
        int frame_index = pager->page_table[i];
        if (frame_index != -1) pager->frames[frame_index].is_dirty = false;
        saved++;
    }

    pager_checkpoint_legacy_base(pager);

    /* Restore the newer overlapping images after committed shadows have been
     * published, then flush only those restored pages in the second pass. */
    for (uint32_t i = 0u; i < saved; i++) {
        uint32_t page_num = overlap_pages[i];
        void* page = get_page(pager, page_num);
        memcpy(page,
               overlap_images + (size_t)i * PAGE_SIZE,
               PAGE_SIZE);
        mark_page_dirty(pager, page_num);
        pager_unpin_page(pager, page_num);
    }

    free(overlap_images);
    free(overlap_pages);
    pager_checkpoint_legacy_base(pager);
}

bool pager_try_checkpoint(Pager* pager,
                          char* message,
                          size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (pager == NULL) {
        if (message != NULL && message_size > 0u) {
            snprintf(message, message_size, "%s", "invalid Pager handle");
        }
        return false;
    }

    /*
     * Do not begin the historical checkpoint until every dirty logical page
     * has proved that it can be materialized through the bounded buffer pool.
     * Releasing each handle immediately keeps the preflight compatible with a
     * single replaceable frame and preserves the no-steal dirty-spill path.
     */
    uint32_t num_pages = pager->num_pages;
    for (uint32_t page_num = 0u; page_num < num_pages; page_num++) {
        if (!pager_page_is_dirty(pager, page_num)) continue;

        PagerPageHandle handle;
        PagerTryPinStatus status = pager_try_pin_existing_page_handle(
            pager,
            page_num,
            &handle);
        if (status != PAGER_TRY_PIN_OK) {
            if (message != NULL && message_size > 0u) {
                snprintf(message,
                         message_size,
                         "checkpoint preflight could not acquire dirty page %u: %s",
                         page_num,
                         pager_try_pin_status_string(status));
            }
            return false;
        }

        if (!pager_release_page_handle(&handle)) {
            if (message != NULL && message_size > 0u) {
                snprintf(message,
                         message_size,
                         "checkpoint preflight could not release dirty page %u",
                         page_num);
            }
            return false;
        }
    }

    pager_checkpoint(pager);
    return true;
}

/* ── Non-fatal existing-page acquisition ───────────────────────────────
 *
 * Keep this implementation in one Pager-owned translation unit instead of
 * emitting a private copy into every caller through pager_try_pin.h. The
 * entire acquisition remains atomic with respect to Pager metadata: lookup,
 * victim choice, no-steal spill, page load/checksum validation, LRU mutation,
 * and publication of pin_count=1 occur while pager_lock is held.
 */

static uint32_t try_pin_fnv1a32(const void* data, size_t length) {
    const uint8_t* bytes = (const uint8_t*)data;
    uint32_t hash = 2166136261u;
    for (size_t i = 0u; i < length; i++) {
        hash ^= bytes[i];
        hash *= 16777619u;
    }
    return hash;
}

static bool try_pin_checksum_valid(const void* page) {
    uint32_t stored = 0u;
    memcpy(&stored,
           (const uint8_t*)page + PAGE_USABLE_SIZE,
           PAGE_CHECKSUM_SIZE);
    return stored == try_pin_fnv1a32(page, PAGE_USABLE_SIZE);
}

static bool try_pin_seek_page(FILE* file, uint32_t page_num) {
    uint64_t offset = (uint64_t)page_num * (uint64_t)PAGE_SIZE;
    if (offset > (uint64_t)INT64_MAX) return false;
#ifdef _WIN32
    return _fseeki64(file, (__int64)offset, SEEK_SET) == 0;
#else
    return fseeko(file, (off_t)offset, SEEK_SET) == 0;
#endif
}

static void try_pin_lru_remove_locked(Pager* pager, int frame_idx) {
    if (frame_idx < 0 || frame_idx >= MAX_BUFFER_POOL_SIZE) return;
    Frame* frame = &pager->frames[frame_idx];

    if (frame->lru_prev != -1) {
        pager->frames[frame->lru_prev].lru_next = frame->lru_next;
    } else if (pager->lru_head == frame_idx) {
        pager->lru_head = frame->lru_next;
    }

    if (frame->lru_next != -1) {
        pager->frames[frame->lru_next].lru_prev = frame->lru_prev;
    } else if (pager->lru_tail == frame_idx) {
        pager->lru_tail = frame->lru_prev;
    }

    frame->lru_prev = -1;
    frame->lru_next = -1;
}

static void try_pin_lru_touch_locked(Pager* pager, int frame_idx) {
    if (pager->lru_head == frame_idx) return;
    try_pin_lru_remove_locked(pager, frame_idx);
    pager->frames[frame_idx].lru_next = pager->lru_head;
    pager->frames[frame_idx].lru_prev = -1;
    if (pager->lru_head != -1) {
        pager->frames[pager->lru_head].lru_prev = frame_idx;
    }
    pager->lru_head = frame_idx;
    if (pager->lru_tail == -1) pager->lru_tail = frame_idx;
}

static int try_pin_choose_frame_locked(Pager* pager) {
    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        if (pager->frames[i].page_num == INVALID_PAGE_NUM) return i;
    }

    int victim = pager->lru_tail;
    while (victim != -1 && pager->frames[victim].pin_count != 0u) {
        victim = pager->frames[victim].lru_prev;
    }
    return victim;
}

static void try_pin_fill_handle(PagerPageHandle* handle,
                                Pager* pager,
                                uint32_t page_num,
                                int frame_idx) {
    handle->pager = pager;
    handle->page_num = page_num;
    handle->frame_idx = frame_idx;
    handle->data = pager->frames[frame_idx].data;
    handle->pinned = true;
    handle->lock_mode = PAGER_PAGE_LOCK_NONE;
}

PagerTryPinStatus pager_try_pin_existing_page_handle(
    Pager* pager,
    uint32_t page_num,
    PagerPageHandle* handle) {
    if (handle == NULL) return PAGER_TRY_PIN_INVALID_PAGE;
    memset(handle, 0, sizeof(*handle));
    handle->page_num = INVALID_PAGE_NUM;
    handle->frame_idx = -1;

    if (pager == NULL || page_num == INVALID_PAGE_NUM) {
        return PAGER_TRY_PIN_INVALID_PAGE;
    }

    db_rwlock_wrlock(&pager->pager_lock);

    if (pager->pin_barrier_active) {
        db_rwlock_wrunlock(&pager->pager_lock);
        return PAGER_TRY_PIN_BUSY;
    }
    if (page_num >= pager->num_pages || page_num >= pager->page_capacity) {
        db_rwlock_wrunlock(&pager->pager_lock);
        return PAGER_TRY_PIN_INVALID_PAGE;
    }

    int frame_idx = pager->page_table[page_num];
    if (frame_idx >= 0 && frame_idx < MAX_BUFFER_POOL_SIZE &&
        pager->frames[frame_idx].page_num == page_num) {
        if (pager->frames[frame_idx].pin_count == UINT32_MAX) {
            db_rwlock_wrunlock(&pager->pager_lock);
            return PAGER_TRY_PIN_BUSY;
        }
        pager->cache_hits++;
        pager->frames[frame_idx].pin_count++;
        try_pin_lru_touch_locked(pager, frame_idx);
        try_pin_fill_handle(handle, pager, page_num, frame_idx);
        db_rwlock_wrunlock(&pager->pager_lock);
        return PAGER_TRY_PIN_OK;
    }

    pager->cache_misses++;
    frame_idx = try_pin_choose_frame_locked(pager);
    if (frame_idx == -1) {
        db_rwlock_wrunlock(&pager->pager_lock);
        return PAGER_TRY_PIN_BUSY;
    }

    uint8_t incoming[PAGE_SIZE];
    memset(incoming, 0, sizeof(incoming));
    if (pager->is_dirty[page_num] &&
        pager->dirty_page_spills[page_num] != NULL) {
        memcpy(incoming, pager->dirty_page_spills[page_num], PAGE_SIZE);
    } else if (pager->committed_pages[page_num] != NULL) {
        memcpy(incoming, pager->committed_pages[page_num], PAGE_SIZE);
    } else {
        uint64_t page_end = ((uint64_t)page_num + 1u) * (uint64_t)PAGE_SIZE;
        if (page_end <= pager->file_length) {
            if (!try_pin_seek_page(pager->file, page_num)) {
                clearerr(pager->file);
                db_rwlock_wrunlock(&pager->pager_lock);
                return PAGER_TRY_PIN_IO_ERROR;
            }
            if (fread(incoming, 1u, PAGE_SIZE, pager->file) != PAGE_SIZE) {
                clearerr(pager->file);
                db_rwlock_wrunlock(&pager->pager_lock);
                return PAGER_TRY_PIN_IO_ERROR;
            }
            if (!try_pin_checksum_valid(incoming)) {
                db_rwlock_wrunlock(&pager->pager_lock);
                return PAGER_TRY_PIN_CORRUPT_PAGE;
            }
        }
    }

    Frame* victim = &pager->frames[frame_idx];
    uint32_t victim_page = victim->page_num;
    void* new_spill = NULL;
    if (victim_page != INVALID_PAGE_NUM && victim->is_dirty &&
        pager->dirty_page_spills[victim_page] == NULL) {
        new_spill = malloc(PAGE_SIZE);
        if (new_spill == NULL) {
            db_rwlock_wrunlock(&pager->pager_lock);
            return PAGER_TRY_PIN_NO_MEMORY;
        }
    }

    if (victim_page != INVALID_PAGE_NUM) {
        if (victim->is_dirty) {
            if (new_spill != NULL) {
                pager->dirty_page_spills[victim_page] = new_spill;
                new_spill = NULL;
            }
            memcpy(pager->dirty_page_spills[victim_page],
                   victim->data,
                   PAGE_SIZE);
        }
        if (victim_page < pager->page_capacity &&
            pager->page_table[victim_page] == frame_idx) {
            pager->page_table[victim_page] = -1;
        }
        pager->evictions++;
    }
    free(new_spill);

    try_pin_lru_remove_locked(pager, frame_idx);
    memcpy(victim->data, incoming, PAGE_SIZE);
    victim->page_num = page_num;
    victim->is_dirty = pager->is_dirty[page_num];
    victim->pin_count = 1u;
    pager->page_table[page_num] = frame_idx;
    try_pin_lru_touch_locked(pager, frame_idx);
    try_pin_fill_handle(handle, pager, page_num, frame_idx);

    db_rwlock_wrunlock(&pager->pager_lock);
    return PAGER_TRY_PIN_OK;
}

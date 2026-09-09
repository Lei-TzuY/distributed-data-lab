#include "pager.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_COUNT 96u
#define FIRST_CHURN_PAGE 2u
#define WORKER_COUNT 4u
#define WORKER_ROUNDS 3u
#define MARKER_BASE 0xC0A50000u
#define HANDLE_TARGET_PAGE 0u
#define LEGACY_TARGET_PAGE 1u

typedef struct {
    Pager* pager;
    uint32_t worker_id;
    int failed;
} ChurnWorker;

static uint32_t expected_marker(uint32_t page_num) {
    return MARKER_BASE ^ page_num;
}

static int churn_worker_body(ChurnWorker* worker) {
    const uint32_t churn_count = PAGE_COUNT - FIRST_CHURN_PAGE;
    for (uint32_t round = 0u; round < WORKER_ROUNDS; round++) {
        for (uint32_t i = 0u; i < churn_count; i++) {
            uint32_t offset = (i + worker->worker_id * 17u + round * 11u) % churn_count;
            uint32_t page_num = FIRST_CHURN_PAGE + offset;
            PagerPageHandle handle;
            if (!pager_pin_page_handle(worker->pager, page_num, &handle)) {
                worker->failed = 1;
                return 1;
            }
            if (!pager_page_handle_acquire_read(&handle)) {
                worker->failed = 2;
                (void)pager_release_page_handle(&handle);
                return 2;
            }
            uint32_t marker = 0u;
            memcpy(&marker, handle.data, sizeof(marker));
            if (!pager_page_handle_release_read(&handle)) {
                worker->failed = 3;
                return 3;
            }
            if (marker != expected_marker(page_num)) {
                worker->failed = 4;
                (void)pager_release_page_handle(&handle);
                return 4;
            }
            if (!pager_release_page_handle(&handle)) {
                worker->failed = 5;
                return 5;
            }
        }
    }
    return 0;
}

#ifdef _WIN32
typedef HANDLE TestThread;

static DWORD WINAPI churn_worker_entry(LPVOID arg) {
    (void)churn_worker_body((ChurnWorker*)arg);
    return 0u;
}

static bool start_test_thread(TestThread* thread, ChurnWorker* worker) {
    *thread = CreateThread(NULL, 0u, churn_worker_entry, worker, 0u, NULL);
    return *thread != NULL;
}

static bool join_test_thread(TestThread thread) {
    DWORD wait_result = WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    return wait_result == WAIT_OBJECT_0;
}
#else
typedef pthread_t TestThread;

static void* churn_worker_entry(void* arg) {
    (void)churn_worker_body((ChurnWorker*)arg);
    return NULL;
}

static bool start_test_thread(TestThread* thread, ChurnWorker* worker) {
    return pthread_create(thread, NULL, churn_worker_entry, worker) == 0;
}

static bool join_test_thread(TestThread thread) {
    return pthread_join(thread, NULL) == 0;
}
#endif

static bool run_concurrent_churn(Pager* pager) {
    TestThread threads[WORKER_COUNT];
    ChurnWorker workers[WORKER_COUNT];
    uint32_t started = 0u;

    for (uint32_t i = 0u; i < WORKER_COUNT; i++) {
        workers[i].pager = pager;
        workers[i].worker_id = i;
        workers[i].failed = 0;
        if (!start_test_thread(&threads[i], &workers[i])) {
            for (uint32_t j = 0u; j < started; j++) {
                (void)join_test_thread(threads[j]);
            }
            return false;
        }
        started++;
    }

    bool ok = true;
    for (uint32_t i = 0u; i < started; i++) {
        if (!join_test_thread(threads[i])) ok = false;
    }
    for (uint32_t i = 0u; i < WORKER_COUNT; i++) {
        if (workers[i].failed != 0) ok = false;
    }
    return ok;
}

static bool churn_after_release(Pager* pager, uint32_t target_page) {
    for (uint32_t page_num = FIRST_CHURN_PAGE; page_num < PAGE_COUNT; page_num++) {
        PagerPageHandle handle;
        if (!pager_pin_page_handle(pager, page_num, &handle)) return false;
        if (!pager_release_page_handle(&handle)) return false;
    }

    db_rwlock_rdlock(&pager->pager_lock);
    bool evicted = target_page < pager->page_capacity &&
        pager->page_table[target_page] == -1;
    db_rwlock_rdunlock(&pager->pager_lock);
    return evicted;
}

static bool frame_identity_is_stable(Pager* pager,
                                     uint32_t page_num,
                                     int frame_idx,
                                     void* data) {
    bool stable = false;
    db_rwlock_rdlock(&pager->pager_lock);
    if (page_num < pager->page_capacity &&
        frame_idx >= 0 && frame_idx < MAX_BUFFER_POOL_SIZE &&
        pager->page_table[page_num] == frame_idx &&
        pager->frames[frame_idx].page_num == page_num &&
        pager->frames[frame_idx].data == data &&
        pager->frames[frame_idx].pin_count > 0u) {
        uint32_t marker = 0u;
        memcpy(&marker, data, sizeof(marker));
        stable = marker == expected_marker(page_num);
    }
    db_rwlock_rdunlock(&pager->pager_lock);
    return stable;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: pager_concurrent_pin_probe DATABASE\n");
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

    PagerPageHandle target;
    if (!pager_pin_page_handle(pager, HANDLE_TARGET_PAGE, &target)) {
        fprintf(stderr, "unable to pin shared-Pager handle target\n");
        pager_close(pager);
        return 1;
    }
    int target_frame = target.frame_idx;
    void* target_data = target.data;
    uint32_t evictions_before = pager->evictions;

    if (!run_concurrent_churn(pager)) {
        fprintf(stderr, "concurrent handle churn failed\n");
        return 1;
    }
    if (pager->evictions <= evictions_before ||
        !frame_identity_is_stable(pager, HANDLE_TARGET_PAGE,
                                  target_frame, target_data)) {
        fprintf(stderr, "concurrent LRU churn recycled a pinned handle frame\n");
        return 1;
    }
    if (!pager_release_page_handle(&target)) {
        fprintf(stderr, "unable to release shared-Pager handle target\n");
        return 1;
    }
    if (!churn_after_release(pager, HANDLE_TARGET_PAGE)) {
        fprintf(stderr, "released handle target did not become evictable\n");
        return 1;
    }

    void* legacy_data = get_page(pager, LEGACY_TARGET_PAGE);
    uint32_t legacy_marker = 0u;
    memcpy(&legacy_marker, legacy_data, sizeof(legacy_marker));
    if (legacy_marker != expected_marker(LEGACY_TARGET_PAGE)) {
        fprintf(stderr, "legacy lock target marker mismatch\n");
        return 1;
    }
    pager_acquire_write_lock(pager, LEGACY_TARGET_PAGE);

    int legacy_frame = -1;
    db_rwlock_rdlock(&pager->pager_lock);
    if (LEGACY_TARGET_PAGE < pager->page_capacity) {
        legacy_frame = pager->page_table[LEGACY_TARGET_PAGE];
    }
    db_rwlock_rdunlock(&pager->pager_lock);
    if (legacy_frame < 0 || legacy_frame >= MAX_BUFFER_POOL_SIZE ||
        !frame_identity_is_stable(pager, LEGACY_TARGET_PAGE,
                                  legacy_frame, legacy_data)) {
        fprintf(stderr, "legacy write lock did not establish pin ownership\n");
        return 1;
    }

    evictions_before = pager->evictions;
    if (!run_concurrent_churn(pager)) {
        fprintf(stderr, "concurrent legacy-lock churn failed\n");
        return 1;
    }
    if (pager->evictions <= evictions_before ||
        !frame_identity_is_stable(pager, LEGACY_TARGET_PAGE,
                                  legacy_frame, legacy_data)) {
        fprintf(stderr, "concurrent LRU churn recycled a legacy-locked frame\n");
        return 1;
    }
    pager_release_write_lock(pager, LEGACY_TARGET_PAGE);
    if (!churn_after_release(pager, LEGACY_TARGET_PAGE)) {
        fprintf(stderr, "released legacy-lock target did not become evictable\n");
        return 1;
    }

    if (!pager_try_close(pager)) {
        fprintf(stderr, "concurrent probe leaked Pager pin ownership\n");
        return 1;
    }

    printf("CONCURRENT_PIN_LRU_OK handle_guard=yes legacy_lock_guard=yes "
           "worker_pins=yes post_release_evict=yes try_close=yes\n");
    return 0;
}

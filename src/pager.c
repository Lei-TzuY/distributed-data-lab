#include "pager.h"

#include <limits.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#else
#include <sys/types.h>
#include <unistd.h>
#endif

#define WAL_TXN_MAGIC       0x57414c54u /* "WALT" - structured WAL v1 */
#define WAL_TXN_V2_MAGIC    0x57414c32u /* "WAL2" - includes free-page state */
#define WAL_COMMIT_MAGIC    0x57414c43u /* "WALC" */
#define FREE_LIST_MAGIC     0x46524545u /* "FREE" */
#define FREE_LIST_VERSION   1u

static void fail_io(const char* message) {
    printf("%s: %d\n", message, errno);
    exit(EXIT_FAILURE);
}

static void fail_allocation(const char* message) {
    printf("%s\n", message);
    exit(EXIT_FAILURE);
}

static void* checked_realloc_array(void* ptr,
                                   uint32_t count,
                                   size_t element_size,
                                   const char* message) {
    if (element_size != 0 && (size_t)count > SIZE_MAX / element_size) {
        fail_allocation(message);
    }
    void* result = realloc(ptr, (size_t)count * element_size);
    if (result == NULL && count != 0) {
        fail_allocation(message);
    }
    return result;
}

static void* checked_calloc_array(uint32_t count,
                                  size_t element_size,
                                  const char* message) {
    if (element_size != 0 && (size_t)count > SIZE_MAX / element_size) {
        fail_allocation(message);
    }
    void* result = calloc((size_t)count, element_size);
    if (result == NULL && count != 0) {
        fail_allocation(message);
    }
    return result;
}

/* ── Page checksum (FNV-1a 32-bit) ────────────────────────────────────── */
static uint32_t fnv1a_extend(uint32_t hash, const void* data, size_t len) {
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        hash ^= p[i];
        hash *= 16777619u;
    }
    return hash;
}

static uint32_t fnv1a_32(const void* data, size_t len) {
    return fnv1a_extend(2166136261u, data, len);
}

static void page_write_checksum(void* page) {
    uint32_t csum = fnv1a_32(page, PAGE_USABLE_SIZE);
    memcpy((char*)page + PAGE_USABLE_SIZE, &csum, PAGE_CHECKSUM_SIZE);
}

static void page_verify_checksum(const void* page, uint32_t page_num) {
    uint32_t stored;
    memcpy(&stored, (const char*)page + PAGE_USABLE_SIZE, PAGE_CHECKSUM_SIZE);
    uint32_t computed = fnv1a_32(page, PAGE_USABLE_SIZE);
    if (stored != computed) {
        printf("Corruption detected: page %u checksum mismatch "
               "(stored 0x%08x, computed 0x%08x).\n",
               page_num, stored, computed);
        exit(EXIT_FAILURE);
    }
}

/* ── 64-bit file positioning ──────────────────────────────────────────── */
static uint64_t tell_file(FILE* file) {
#ifdef _WIN32
    __int64 position = _ftelli64(file);
    if (position < 0) fail_io("Error reading file position");
    return (uint64_t)position;
#else
    off_t position = ftello(file);
    if (position < 0) fail_io("Error reading file position");
    return (uint64_t)position;
#endif
}

static void seek_offset(FILE* file, uint64_t offset, int origin) {
    if (offset > (uint64_t)INT64_MAX) {
        printf("File offset is too large.\n");
        exit(EXIT_FAILURE);
    }
#ifdef _WIN32
    if (_fseeki64(file, (__int64)offset, origin) != 0) {
        fail_io("Error seeking");
    }
#else
    if (fseeko(file, (off_t)offset, origin) != 0) {
        fail_io("Error seeking");
    }
#endif
}

static uint64_t get_file_length(FILE* file) {
#ifdef _WIN32
    if (_fseeki64(file, 0, SEEK_END) != 0) fail_io("Error seeking");
#else
    if (fseeko(file, 0, SEEK_END) != 0) fail_io("Error seeking");
#endif
    return tell_file(file);
}

static void seek_file(FILE* file, uint32_t page_num) {
    uint64_t offset = (uint64_t)page_num * (uint64_t)PAGE_SIZE;
    seek_offset(file, offset, SEEK_SET);
}

static void write_bytes(FILE* file, const void* buffer, size_t size) {
    if (fwrite(buffer, 1, size, file) != size) {
        fail_io("Error writing");
    }
}

static bool read_bytes(FILE* file, void* buffer, size_t size) {
    return fread(buffer, 1, size, file) == size;
}

static void sync_file(FILE* file) {
    if (fflush(file) != 0) {
        fail_io("Error flushing");
    }
#ifdef _WIN32
    if (_commit(_fileno(file)) != 0) {
        fail_io("Error syncing");
    }
#else
    if (fsync(fileno(file)) != 0) {
        fail_io("Error syncing");
    }
#endif
}

static void truncate_file(FILE* file, uint64_t length) {
    if (fflush(file) != 0) {
        fail_io("Error flushing before truncate");
    }
    if (length > (uint64_t)INT64_MAX) {
        printf("Database is too large to truncate on this platform.\n");
        exit(EXIT_FAILURE);
    }
#ifdef _WIN32
    if (_chsize_s(_fileno(file), (__int64)length) != 0) {
        fail_io("Error truncating database");
    }
#else
    if (ftruncate(fileno(file), (off_t)length) != 0) {
        fail_io("Error truncating database");
    }
#endif
}

/* ── Durable free-page metadata ───────────────────────────────────────── */
static void make_sidecar_path(const char* filename,
                              const char* suffix,
                              char* output,
                              size_t output_size) {
    int written = snprintf(output, output_size, "%s%s", filename, suffix);
    if (written < 0 || (size_t)written >= output_size) {
        printf("Database sidecar path is too long.\n");
        exit(EXIT_FAILURE);
    }
}

static bool validate_free_page_list(uint32_t num_pages,
                                    const uint32_t* free_pages,
                                    uint32_t free_page_count) {
    if (free_page_count > num_pages) return false;
    if (free_page_count == 0) return true;
    if (num_pages == 0 || free_pages == NULL) return false;

    bool* seen = (bool*)checked_calloc_array(
        num_pages, sizeof(bool),
        "Unable to validate free-page metadata.");
    bool valid = true;
    for (uint32_t i = 0; i < free_page_count; i++) {
        uint32_t page_num = free_pages[i];
        if (page_num == 0 || page_num >= num_pages || seen[page_num]) {
            valid = false;
            break;
        }
        seen[page_num] = true;
    }
    free(seen);
    return valid;
}

static uint32_t free_list_checksum(uint32_t num_pages,
                                   uint32_t free_page_count,
                                   const uint32_t* free_pages) {
    uint32_t hash = 2166136261u;
    uint32_t magic = FREE_LIST_MAGIC;
    uint32_t version = FREE_LIST_VERSION;
    hash = fnv1a_extend(hash, &magic, sizeof(magic));
    hash = fnv1a_extend(hash, &version, sizeof(version));
    hash = fnv1a_extend(hash, &num_pages, sizeof(num_pages));
    hash = fnv1a_extend(hash, &free_page_count, sizeof(free_page_count));
    if (free_page_count > 0) {
        hash = fnv1a_extend(hash,
                            free_pages,
                            (size_t)free_page_count * sizeof(uint32_t));
    }
    return hash;
}

static void persist_free_list_snapshot(const char* filename,
                                       uint32_t num_pages,
                                       uint32_t free_page_count,
                                       const uint32_t* free_pages) {
    if (!validate_free_page_list(num_pages, free_pages, free_page_count)) {
        printf("BUG: refusing to persist invalid free-page metadata.\n");
        exit(EXIT_FAILURE);
    }

    char free_path[1024];
    char free_wal_path[1024];
    make_sidecar_path(filename, ".free", free_path, sizeof(free_path));
    make_sidecar_path(filename, ".free.wal", free_wal_path, sizeof(free_wal_path));

    FILE* file = fopen(free_wal_path, "wb");
    if (file == NULL) fail_io("Unable to open free-page metadata WAL");

    uint32_t magic = FREE_LIST_MAGIC;
    uint32_t version = FREE_LIST_VERSION;
    uint32_t checksum = free_list_checksum(num_pages,
                                           free_page_count,
                                           free_pages);
    write_bytes(file, &magic, sizeof(magic));
    write_bytes(file, &version, sizeof(version));
    write_bytes(file, &num_pages, sizeof(num_pages));
    write_bytes(file, &free_page_count, sizeof(free_page_count));
    if (free_page_count > 0) {
        write_bytes(file,
                    free_pages,
                    (size_t)free_page_count * sizeof(uint32_t));
    }
    write_bytes(file, &checksum, sizeof(checksum));
    sync_file(file);
    fclose(file);

    if (remove(free_path) != 0 && errno != ENOENT) {
        fail_io("Unable to replace free-page metadata");
    }
    if (rename(free_wal_path, free_path) != 0) {
        fail_io("Unable to publish free-page metadata");
    }
}

static bool read_free_list_snapshot(const char* path,
                                    uint32_t expected_num_pages,
                                    uint32_t** free_pages_out,
                                    uint32_t* free_page_count_out) {
    *free_pages_out = NULL;
    *free_page_count_out = 0;

    FILE* file = fopen(path, "rb");
    if (file == NULL) return false;

    uint64_t length = get_file_length(file);
    seek_offset(file, 0, SEEK_SET);

    uint32_t magic = 0;
    uint32_t version = 0;
    uint32_t num_pages = 0;
    uint32_t free_page_count = 0;
    if (!read_bytes(file, &magic, sizeof(magic)) ||
        !read_bytes(file, &version, sizeof(version)) ||
        !read_bytes(file, &num_pages, sizeof(num_pages)) ||
        !read_bytes(file, &free_page_count, sizeof(free_page_count))) {
        fclose(file);
        return false;
    }

    uint64_t expected_length = sizeof(uint32_t) * 5u +
        (uint64_t)free_page_count * sizeof(uint32_t);
    if (magic != FREE_LIST_MAGIC ||
        version != FREE_LIST_VERSION ||
        num_pages != expected_num_pages ||
        free_page_count > num_pages ||
        expected_length != length) {
        fclose(file);
        return false;
    }

    uint32_t* free_pages = NULL;
    if (free_page_count > 0) {
        free_pages = (uint32_t*)checked_calloc_array(
            free_page_count, sizeof(uint32_t),
            "Unable to load free-page metadata.");
        if (!read_bytes(file,
                        free_pages,
                        (size_t)free_page_count * sizeof(uint32_t))) {
            free(free_pages);
            fclose(file);
            return false;
        }
    }

    uint32_t stored_checksum = 0;
    bool valid = read_bytes(file, &stored_checksum, sizeof(stored_checksum)) &&
        stored_checksum == free_list_checksum(num_pages,
                                              free_page_count,
                                              free_pages) &&
        validate_free_page_list(num_pages, free_pages, free_page_count);
    fclose(file);
    if (!valid) {
        free(free_pages);
        return false;
    }

    *free_pages_out = free_pages;
    *free_page_count_out = free_page_count;
    return true;
}

static void load_free_list_snapshot(Pager* pager) {
    char free_path[1024];
    char free_wal_path[1024];
    make_sidecar_path(pager->filename, ".free", free_path, sizeof(free_path));
    make_sidecar_path(pager->filename, ".free.wal", free_wal_path, sizeof(free_wal_path));

    uint32_t* loaded_pages = NULL;
    uint32_t loaded_count = 0;
    if (read_free_list_snapshot(free_wal_path,
                                pager->num_pages,
                                &loaded_pages,
                                &loaded_count)) {
        if (remove(free_path) != 0 && errno != ENOENT) {
            free(loaded_pages);
            fail_io("Unable to recover free-page metadata");
        }
        if (rename(free_wal_path, free_path) != 0) {
            free(loaded_pages);
            fail_io("Unable to recover free-page metadata");
        }
    } else if (!read_free_list_snapshot(free_path,
                                        pager->num_pages,
                                        &loaded_pages,
                                        &loaded_count)) {
        if (remove(free_wal_path) != 0 && errno != ENOENT) {
            fail_io("Unable to discard invalid free-page metadata WAL");
        }
        return;
    }

    if (loaded_count > 0) {
        memcpy(pager->free_pages,
               loaded_pages,
               (size_t)loaded_count * sizeof(uint32_t));
    }
    pager->free_page_count = loaded_count;
    free(loaded_pages);
}

/* ── Growable pager metadata ──────────────────────────────────────────── */
static void pager_reserve_capacity(Pager* pager, uint32_t min_capacity) {
    if (min_capacity <= pager->page_capacity) return;

    uint32_t old_capacity = pager->page_capacity;
    uint32_t new_capacity = old_capacity == 0 ? PAGER_INITIAL_CAPACITY : old_capacity;
    if (new_capacity == 0) new_capacity = 1;

    while (new_capacity < min_capacity) {
        uint64_t doubled = (uint64_t)new_capacity * 2u;
        if (doubled >= (uint64_t)min_capacity && doubled <= (uint64_t)UINT32_MAX) {
            new_capacity = (uint32_t)doubled;
        } else {
            new_capacity = min_capacity;
        }
    }

    pager->page_table = (int*)checked_realloc_array(
        pager->page_table, new_capacity, sizeof(int),
        "Unable to grow pager page table.");
    pager->is_dirty = (bool*)checked_realloc_array(
        pager->is_dirty, new_capacity, sizeof(bool),
        "Unable to grow pager dirty map.");
    pager->free_pages = (uint32_t*)checked_realloc_array(
        pager->free_pages, new_capacity, sizeof(uint32_t),
        "Unable to grow pager free-page list.");
    pager->dirty_page_spills = (void**)checked_realloc_array(
        pager->dirty_page_spills, new_capacity, sizeof(void*),
        "Unable to grow pager dirty spill map.");
    pager->committed_pages = (void**)checked_realloc_array(
        pager->committed_pages, new_capacity, sizeof(void*),
        "Unable to grow pager committed-page map.");

    for (uint32_t i = old_capacity; i < new_capacity; i++) {
        pager->page_table[i] = -1;
        pager->is_dirty[i] = false;
        pager->dirty_page_spills[i] = NULL;
        pager->committed_pages[i] = NULL;
    }
    pager->page_capacity = new_capacity;
}

static void store_page_copy(void** slots, uint32_t page_num, const void* data) {
    if (slots[page_num] == NULL) {
        slots[page_num] = malloc(PAGE_SIZE);
        if (slots[page_num] == NULL) {
            fail_allocation("Unable to allocate pager page shadow.");
        }
    }
    memcpy(slots[page_num], data, PAGE_SIZE);
}

static void free_page_slots(void** slots, uint32_t capacity) {
    if (slots == NULL) return;
    for (uint32_t i = 0; i < capacity; i++) {
        free(slots[i]);
        slots[i] = NULL;
    }
}

static void clear_transaction_free_snapshot(Pager* pager) {
    free(pager->transaction_free_pages);
    pager->transaction_free_pages = NULL;
    pager->transaction_free_page_count = 0;
}

uint32_t pager_metadata_capacity(Pager* pager) {
    return pager == NULL ? 0u : pager->page_capacity;
}

/* ── LRU buffer pool ──────────────────────────────────────────────────── */
static void lru_remove(Pager* pager, int frame_idx) {
    if (frame_idx == -1) return;
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

static void lru_touch(Pager* pager, int frame_idx) {
    if (pager->lru_head == frame_idx) return;

    lru_remove(pager, frame_idx);
    pager->frames[frame_idx].lru_next = pager->lru_head;
    pager->frames[frame_idx].lru_prev = -1;

    if (pager->lru_head != -1) {
        pager->frames[pager->lru_head].lru_prev = frame_idx;
    }
    pager->lru_head = frame_idx;
    if (pager->lru_tail == -1) pager->lru_tail = frame_idx;
}

static int lru_evict(Pager* pager) {
    int victim = pager->lru_tail;
    while (victim != -1 && pager->frames[victim].pin_count != 0) {
        victim = pager->frames[victim].lru_prev;
    }
    if (victim == -1) {
        printf("Buffer pool is exhausted: all frames are pinned.\n");
        exit(EXIT_FAILURE);
    }

    Frame* frame = &pager->frames[victim];
    uint32_t victim_page = frame->page_num;
    if (victim_page != INVALID_PAGE_NUM) {
        if (frame->is_dirty) {
            /* No-steal: never write uncommitted data to the main DB. */
            store_page_copy(pager->dirty_page_spills, victim_page, frame->data);
            frame->is_dirty = false;
        }
        pager->page_table[victim_page] = -1;
        pager->evictions++;
    }

    lru_remove(pager, victim);
    frame->page_num = INVALID_PAGE_NUM;
    frame->is_dirty = false;
    frame->pin_count = 0;
    return victim;
}

static void clear_page_cache(Pager* pager) {
    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        pager->frames[i].page_num = INVALID_PAGE_NUM;
        pager->frames[i].is_dirty = false;
        pager->frames[i].pin_count = 0;
        pager->frames[i].lru_prev = -1;
        pager->frames[i].lru_next = -1;
    }
    for (uint32_t i = 0; i < pager->page_capacity; i++) {
        pager->page_table[i] = -1;
    }
    pager->lru_head = -1;
    pager->lru_tail = -1;
}

/* ── WAL recovery ─────────────────────────────────────────────────────── */
static void recover_wal(const char* filename, const char* wal_filename) {
    FILE* wal_file = fopen(wal_filename, "rb");
    if (wal_file == NULL) return;

    printf("WAL file found. Recovering...\n");

    FILE* db_file = fopen(filename, "r+b");
    if (db_file == NULL) db_file = fopen(filename, "w+b");
    if (db_file == NULL) fail_io("Unable to open database during recovery");

    uint64_t wal_length = get_file_length(wal_file);
    seek_offset(wal_file, 0, SEEK_SET);

    uint32_t* recovered_free_pages = NULL;
    uint32_t recovered_free_page_count = 0;
    uint32_t recovered_num_pages = 0;
    bool recovered_free_state = false;

    while (tell_file(wal_file) < wal_length) {
        uint32_t first_word = 0;
        if (!read_bytes(wal_file, &first_word, sizeof(first_word))) break;

        bool format_v2 = first_word == WAL_TXN_V2_MAGIC;
        bool format_v1 = first_word == WAL_TXN_MAGIC;
        bool structured = format_v1 || format_v2;
        uint32_t page_count = 0;
        uint32_t final_num_pages = 0;
        uint32_t free_page_count = 0;

        if (format_v2) {
            if (!read_bytes(wal_file, &page_count, sizeof(page_count)) ||
                !read_bytes(wal_file, &final_num_pages, sizeof(final_num_pages)) ||
                !read_bytes(wal_file, &free_page_count, sizeof(free_page_count))) {
                printf("Ignoring incomplete WAL transaction.\n");
                break;
            }
        } else if (format_v1) {
            if (!read_bytes(wal_file, &page_count, sizeof(page_count)) ||
                !read_bytes(wal_file, &final_num_pages, sizeof(final_num_pages))) {
                printf("Ignoring incomplete WAL transaction.\n");
                break;
            }
        } else {
            page_count = first_word; /* legacy WAL format */
        }

        uint64_t current = tell_file(wal_file);
        uint64_t remaining = wal_length - current;
        uint64_t bytes_per_page = sizeof(uint32_t) + (uint64_t)PAGE_SIZE;
        uint64_t needed = (uint64_t)page_count * bytes_per_page + sizeof(uint32_t);
        if (format_v2) {
            needed += (uint64_t)free_page_count * sizeof(uint32_t);
        }
        if ((!structured && page_count == 0) ||
            (format_v2 && free_page_count > final_num_pages) ||
            needed > remaining) {
            printf("Ignoring incomplete WAL transaction.\n");
            break;
        }

        uint32_t* txn_free_pages = NULL;
        if (format_v2 && free_page_count > 0) {
            txn_free_pages = (uint32_t*)checked_calloc_array(
                free_page_count, sizeof(uint32_t),
                "Unable to allocate WAL free-page buffer.");
            if (!read_bytes(wal_file,
                            txn_free_pages,
                            (size_t)free_page_count * sizeof(uint32_t)) ||
                !validate_free_page_list(final_num_pages,
                                         txn_free_pages,
                                         free_page_count)) {
                free(txn_free_pages);
                printf("Ignoring incomplete WAL transaction.\n");
                break;
            }
        }

        uint32_t* page_nums = NULL;
        void** page_buffers = NULL;
        if (page_count > 0) {
            page_nums = (uint32_t*)checked_calloc_array(
                page_count, sizeof(uint32_t),
                "Unable to allocate WAL page-number buffer.");
            page_buffers = (void**)checked_calloc_array(
                page_count, sizeof(void*),
                "Unable to allocate WAL transaction buffer.");
        }

        bool complete = true;
        for (uint32_t i = 0; i < page_count; i++) {
            page_buffers[i] = malloc(PAGE_SIZE);
            if (page_buffers[i] == NULL) {
                fail_allocation("Unable to allocate WAL transaction page.");
            }
            if (!read_bytes(wal_file, &page_nums[i], sizeof(page_nums[i])) ||
                page_nums[i] == INVALID_PAGE_NUM ||
                (structured && page_nums[i] >= final_num_pages) ||
                !read_bytes(wal_file, page_buffers[i], PAGE_SIZE)) {
                complete = false;
                break;
            }
        }

        uint32_t commit_magic = 0;
        if (complete && !read_bytes(wal_file, &commit_magic, sizeof(commit_magic))) {
            complete = false;
        }

        if (!complete || commit_magic != WAL_COMMIT_MAGIC) {
            printf("Ignoring incomplete WAL transaction.\n");
            for (uint32_t i = 0; i < page_count; i++) free(page_buffers[i]);
            free(page_buffers);
            free(page_nums);
            free(txn_free_pages);
            break;
        }

        for (uint32_t i = 0; i < page_count; i++) {
            seek_file(db_file, page_nums[i]);
            write_bytes(db_file, page_buffers[i], PAGE_SIZE);
            free(page_buffers[i]);
        }
        free(page_buffers);
        free(page_nums);

        if (structured) {
            truncate_file(db_file, (uint64_t)final_num_pages * (uint64_t)PAGE_SIZE);
        }

        if (format_v2) {
            free(recovered_free_pages);
            recovered_free_pages = txn_free_pages;
            txn_free_pages = NULL;
            recovered_free_page_count = free_page_count;
            recovered_num_pages = final_num_pages;
            recovered_free_state = true;
        }
        free(txn_free_pages);
    }

    sync_file(db_file);
    if (recovered_free_state) {
        persist_free_list_snapshot(filename,
                                   recovered_num_pages,
                                   recovered_free_page_count,
                                   recovered_free_pages);
    }
    free(recovered_free_pages);
    fclose(db_file);
    fclose(wal_file);

    if (remove(wal_filename) != 0) {
        fail_io("Unable to remove recovered WAL");
    }
    printf("Recovery complete.\n");
}

Pager* pager_open(const char* filename) {
    Pager* pager = (Pager*)calloc(1, sizeof(Pager));
    if (pager == NULL) fail_allocation("Unable to allocate pager.");

    snprintf(pager->filename, sizeof(pager->filename), "%s", filename);
    snprintf(pager->wal_filename, sizeof(pager->wal_filename), "%s.wal", filename);
    recover_wal(pager->filename, pager->wal_filename);

    FILE* file = fopen(filename, "r+b");
    if (file == NULL) file = fopen(filename, "w+b");
    if (file == NULL) fail_io("Unable to open database");

    uint64_t file_length = get_file_length(file);
    if (file_length % PAGE_SIZE != 0) {
        printf("Db file is not a whole number of pages. Corrupt file.\n");
        exit(EXIT_FAILURE);
    }

    uint64_t page_count64 = file_length / PAGE_SIZE;
    if (page_count64 > (uint64_t)INVALID_PAGE_NUM) {
        printf("Database has too many pages for 32-bit page numbers.\n");
        exit(EXIT_FAILURE);
    }

    pager->file = file;
    pager->file_length = file_length;
    pager->num_pages = (uint32_t)page_count64;
    pager->page_capacity = 0;
    pager->page_table = NULL;
    pager->is_dirty = NULL;
    pager->free_pages = NULL;
    pager->dirty_page_spills = NULL;
    pager->committed_pages = NULL;
    pager->transaction_free_pages = NULL;
    pager->in_transaction = false;
    pager->transaction_file_length = 0;
    pager->transaction_num_pages = 0;
    pager->free_page_count = 0;
    pager->transaction_free_page_count = 0;
    pager->savepoint_count = 0;
    memset(pager->savepoints, 0, sizeof(pager->savepoints));

    pager->lru_head = -1;
    pager->lru_tail = -1;
    pager->cache_hits = 0;
    pager->cache_misses = 0;
    pager->evictions = 0;

    uint32_t initial_capacity = pager->num_pages > PAGER_INITIAL_CAPACITY
        ? pager->num_pages
        : PAGER_INITIAL_CAPACITY;
    pager_reserve_capacity(pager, initial_capacity);
    load_free_list_snapshot(pager);

    db_rwlock_init(&pager->pager_lock);
    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        pager->frames[i].page_num = INVALID_PAGE_NUM;
        pager->frames[i].data = calloc(1, PAGE_SIZE);
        if (pager->frames[i].data == NULL) {
            fail_allocation("Unable to allocate buffer-pool frame.");
        }
        pager->frames[i].is_dirty = false;
        pager->frames[i].pin_count = 0;
        pager->frames[i].lru_prev = -1;
        pager->frames[i].lru_next = -1;
        db_rwlock_init(&pager->frames[i].rwlock);
    }

    return pager;
}

void* get_page(Pager* pager, uint32_t page_num) {
    if (page_num == INVALID_PAGE_NUM) {
        printf("Tried to fetch the reserved invalid page number.\n");
        exit(EXIT_FAILURE);
    }

    db_rwlock_wrlock(&pager->pager_lock);
    pager_reserve_capacity(pager, page_num + 1u);

    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) {
        pager->cache_hits++;
        lru_touch(pager, frame_idx);
        void* data = pager->frames[frame_idx].data;
        db_rwlock_wrunlock(&pager->pager_lock);
        return data;
    }

    pager->cache_misses++;
    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        if (pager->frames[i].page_num == INVALID_PAGE_NUM) {
            frame_idx = i;
            break;
        }
    }
    if (frame_idx == -1) frame_idx = lru_evict(pager);

    void* page_data = pager->frames[frame_idx].data;
    memset(page_data, 0, PAGE_SIZE);

    if (pager->is_dirty[page_num] && pager->dirty_page_spills[page_num] != NULL) {
        memcpy(page_data, pager->dirty_page_spills[page_num], PAGE_SIZE);
    } else if (pager->committed_pages[page_num] != NULL) {
        memcpy(page_data, pager->committed_pages[page_num], PAGE_SIZE);
    } else {
        uint64_t file_pages = pager->file_length / PAGE_SIZE;
        if ((uint64_t)page_num < file_pages) {
            seek_file(pager->file, page_num);
            if (fread(page_data, 1, PAGE_SIZE, pager->file) != PAGE_SIZE) {
                fail_io("Error reading page");
            }
            page_verify_checksum(page_data, page_num);
        }
    }

    if (page_num >= pager->num_pages) pager->num_pages = page_num + 1u;

    pager->frames[frame_idx].page_num = page_num;
    pager->frames[frame_idx].is_dirty = pager->is_dirty[page_num];
    pager->frames[frame_idx].pin_count = 0;
    pager->page_table[page_num] = frame_idx;
    lru_touch(pager, frame_idx);

    db_rwlock_wrunlock(&pager->pager_lock);
    return page_data;
}

void pager_unpin_page(Pager* pager, uint32_t page_num) {
    if (page_num >= pager->page_capacity) return;
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1 && pager->frames[frame_idx].pin_count > 0) {
        pager->frames[frame_idx].pin_count--;
    }
}

void pager_print_buffer_pool_stats(Pager* pager) {
    uint32_t total = pager->cache_hits + pager->cache_misses;
    double hit_ratio = total > 0
        ? ((double)pager->cache_hits / (double)total * 100.0)
        : 0.0;

    printf("=== Buffer Pool Manager Statistics ===\n");
    printf("Capacity    : %d pages\n", MAX_BUFFER_POOL_SIZE);
    printf("Metadata Cap: %u pages\n", pager->page_capacity);
    printf("Hits        : %u\n", pager->cache_hits);
    printf("Misses      : %u\n", pager->cache_misses);
    printf("Hit Ratio   : %.2f%%\n", hit_ratio);
    printf("Evictions   : %u\n", pager->evictions);
    printf("LRU Queue   (MRU -> LRU):\n");

    int curr = pager->lru_head;
    int pos = 0;
    while (curr != -1) {
        Frame* f = &pager->frames[curr];
        printf("  [%2d] Frame %2d -> Page %u (dirty=%d, pins=%u)\n",
               pos++, curr, f->page_num, f->is_dirty ? 1 : 0, f->pin_count);
        curr = f->lru_next;
    }
}

void pager_flush(Pager* pager, uint32_t page_num, uint32_t size) {
    if (page_num >= pager->page_capacity) return;
    int frame_idx = pager->page_table[page_num];
    if (frame_idx == -1) return;

    void* page_data = pager->frames[frame_idx].data;
    page_write_checksum(page_data);
    seek_file(pager->file, page_num);
    write_bytes(pager->file, page_data, size);

    uint64_t end_offset = (uint64_t)page_num * PAGE_SIZE + size;
    if (end_offset > pager->file_length) pager->file_length = end_offset;
}

uint32_t get_unused_page_num(Pager* pager) {
    if (pager->free_page_count > 0) {
        return pager->free_pages[--pager->free_page_count];
    }
    if (pager->num_pages == INVALID_PAGE_NUM) {
        printf("Database exhausted the 32-bit page-number space.\n");
        exit(EXIT_FAILURE);
    }
    return pager->num_pages;
}

void pager_free_page(Pager* pager, uint32_t page_num) {
    if (page_num == 0) {
        printf("BUG: attempted to free page 0 (always root).\n");
        exit(EXIT_FAILURE);
    }
    if (page_num == INVALID_PAGE_NUM || page_num >= pager->num_pages) {
        printf("BUG: attempted to free invalid page %u.\n", page_num);
        exit(EXIT_FAILURE);
    }

    pager_reserve_capacity(pager, page_num + 1u);
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) {
        pager->frames[frame_idx].page_num = INVALID_PAGE_NUM;
        pager->frames[frame_idx].is_dirty = false;
        pager->frames[frame_idx].pin_count = 0;
        lru_remove(pager, frame_idx);
        pager->page_table[page_num] = -1;
    }

    free(pager->dirty_page_spills[page_num]);
    pager->dirty_page_spills[page_num] = NULL;
    pager->is_dirty[page_num] = false;
    pager->free_pages[pager->free_page_count++] = page_num;
}

void pager_shrink(Pager* pager, uint32_t new_num_pages) {
    if (new_num_pages >= pager->num_pages) return;

    uint32_t old_num_pages = pager->num_pages;
    for (uint32_t i = new_num_pages; i < old_num_pages; i++) {
        if (i < pager->page_capacity) {
            int frame_idx = pager->page_table[i];
            if (frame_idx != -1) {
                pager->frames[frame_idx].page_num = INVALID_PAGE_NUM;
                pager->frames[frame_idx].is_dirty = false;
                pager->frames[frame_idx].pin_count = 0;
                lru_remove(pager, frame_idx);
                pager->page_table[i] = -1;
            }
            pager->is_dirty[i] = false;
            free(pager->dirty_page_spills[i]);
            pager->dirty_page_spills[i] = NULL;
            free(pager->committed_pages[i]);
            pager->committed_pages[i] = NULL;
        }
    }

    uint32_t kept = 0;
    for (uint32_t i = 0; i < pager->free_page_count; i++) {
        if (pager->free_pages[i] < new_num_pages) {
            pager->free_pages[kept++] = pager->free_pages[i];
        }
    }
    pager->free_page_count = kept;
    pager->num_pages = new_num_pages;
    /* Physical truncation is deferred to checkpoint/WAL recovery. */
}

void mark_page_dirty(Pager* pager, uint32_t page_num) {
    if (page_num == INVALID_PAGE_NUM) {
        printf("Tried to dirty the reserved invalid page number.\n");
        exit(EXIT_FAILURE);
    }

    db_rwlock_wrlock(&pager->pager_lock);
    pager_reserve_capacity(pager, page_num + 1u);
    pager->is_dirty[page_num] = true;
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) pager->frames[frame_idx].is_dirty = true;
    db_rwlock_wrunlock(&pager->pager_lock);
}

/* ── Savepoints ──────────────────────────────────────────────────────── */
static void free_savepoint_snapshots(Savepoint* sp) {
    if (sp == NULL) return;
    if (sp->page_snapshots != NULL) {
        for (uint32_t i = 0; i < sp->capacity; i++) free(sp->page_snapshots[i]);
    }
    free(sp->page_snapshots);
    free(sp->free_pages);
    free(sp->is_dirty);
    memset(sp, 0, sizeof(*sp));
}

static void clear_all_savepoints(Pager* pager) {
    for (uint32_t i = 0; i < pager->savepoint_count; i++) {
        free_savepoint_snapshots(&pager->savepoints[i]);
    }
    pager->savepoint_count = 0;
}

/* ── Transactions ────────────────────────────────────────────────────── */
void pager_begin_transaction(Pager* pager) {
    if (pager->in_transaction) {
        printf("Pager transaction already active.\n");
        exit(EXIT_FAILURE);
    }

    pager_checkpoint(pager);
    clear_all_savepoints(pager);
    clear_transaction_free_snapshot(pager);

    pager->transaction_file_length = pager->file_length;
    pager->transaction_num_pages = pager->num_pages;
    pager->transaction_free_page_count = pager->free_page_count;
    if (pager->free_page_count > 0) {
        pager->transaction_free_pages = (uint32_t*)checked_calloc_array(
            pager->free_page_count, sizeof(uint32_t),
            "Unable to snapshot transaction free-page list.");
        memcpy(pager->transaction_free_pages,
               pager->free_pages,
               (size_t)pager->free_page_count * sizeof(uint32_t));
    }
    pager->in_transaction = true;
}

void pager_commit(Pager* pager) {
    uint32_t dirty_count = 0;
    for (uint32_t i = 0; i < pager->num_pages; i++) {
        if (pager->is_dirty[i]) dirty_count++;
    }

    if (!validate_free_page_list(pager->num_pages,
                                 pager->free_pages,
                                 pager->free_page_count)) {
        printf("BUG: pager free-page list is invalid at commit.\n");
        exit(EXIT_FAILURE);
    }

    FILE* wal_file = fopen(pager->wal_filename, "ab");
    if (wal_file == NULL) fail_io("Failed to open WAL for commit");

    uint32_t txn_magic = WAL_TXN_V2_MAGIC;
    write_bytes(wal_file, &txn_magic, sizeof(txn_magic));
    write_bytes(wal_file, &dirty_count, sizeof(dirty_count));
    write_bytes(wal_file, &pager->num_pages, sizeof(pager->num_pages));
    write_bytes(wal_file, &pager->free_page_count, sizeof(pager->free_page_count));
    if (pager->free_page_count > 0) {
        write_bytes(wal_file,
                    pager->free_pages,
                    (size_t)pager->free_page_count * sizeof(uint32_t));
    }

    for (uint32_t i = 0; i < pager->num_pages; i++) {
        if (!pager->is_dirty[i]) continue;
        void* page_data = get_page(pager, i);
        page_write_checksum(page_data);
        write_bytes(wal_file, &i, sizeof(i));
        write_bytes(wal_file, page_data, PAGE_SIZE);
        store_page_copy(pager->committed_pages, i, page_data);
        pager_unpin_page(pager, i);
    }

    uint32_t commit_magic = WAL_COMMIT_MAGIC;
    write_bytes(wal_file, &commit_magic, sizeof(commit_magic));
    sync_file(wal_file);
    fclose(wal_file);

    /* The main WAL is durable first. If a crash lands before this sidecar is
     * published, WAL recovery replays the same free-page snapshot. */
    persist_free_list_snapshot(pager->filename,
                               pager->num_pages,
                               pager->free_page_count,
                               pager->free_pages);

    for (uint32_t i = 0; i < pager->num_pages; i++) {
        if (!pager->is_dirty[i]) continue;
        pager->is_dirty[i] = false;
        free(pager->dirty_page_spills[i]);
        pager->dirty_page_spills[i] = NULL;
        int frame_idx = pager->page_table[i];
        if (frame_idx != -1) pager->frames[frame_idx].is_dirty = false;
    }

    clear_all_savepoints(pager);
    clear_transaction_free_snapshot(pager);
    pager->in_transaction = false;
    pager->transaction_file_length = 0;
    pager->transaction_num_pages = 0;
}

void pager_rollback(Pager* pager) {
    if (!pager->in_transaction) return;

    clear_all_savepoints(pager);
    free_page_slots(pager->dirty_page_spills, pager->page_capacity);
    memset(pager->is_dirty, 0, (size_t)pager->page_capacity * sizeof(bool));
    clear_page_cache(pager);

    pager->num_pages = pager->transaction_num_pages;
    pager->file_length = pager->transaction_file_length;
    pager->free_page_count = pager->transaction_free_page_count;
    if (pager->free_page_count > 0 && pager->transaction_free_pages != NULL) {
        memcpy(pager->free_pages,
               pager->transaction_free_pages,
               (size_t)pager->free_page_count * sizeof(uint32_t));
    }

    truncate_file(pager->file, pager->transaction_file_length);
    sync_file(pager->file);
    clear_transaction_free_snapshot(pager);
    pager->transaction_file_length = 0;
    pager->transaction_num_pages = 0;
    pager->in_transaction = false;
}

void pager_checkpoint(Pager* pager) {
    /* Flush any non-transactional dirty state (for example a brand-new root). */
    for (uint32_t i = 0; i < pager->num_pages; i++) {
        if (!pager->is_dirty[i]) continue;
        void* page_data = get_page(pager, i);
        page_write_checksum(page_data);
        seek_file(pager->file, i);
        write_bytes(pager->file, page_data, PAGE_SIZE);
        pager->is_dirty[i] = false;
        int frame_idx = pager->page_table[i];
        if (frame_idx != -1) pager->frames[frame_idx].is_dirty = false;
        free(pager->dirty_page_spills[i]);
        pager->dirty_page_spills[i] = NULL;
    }

    /* Apply the newest committed image of every page modified since checkpoint. */
    for (uint32_t i = 0; i < pager->page_capacity; i++) {
        if (pager->committed_pages[i] == NULL) continue;
        if (i < pager->num_pages) {
            page_write_checksum(pager->committed_pages[i]);
            seek_file(pager->file, i);
            write_bytes(pager->file, pager->committed_pages[i], PAGE_SIZE);
        }
        free(pager->committed_pages[i]);
        pager->committed_pages[i] = NULL;
    }

    uint64_t target_length = (uint64_t)pager->num_pages * (uint64_t)PAGE_SIZE;
    truncate_file(pager->file, target_length);
    pager->file_length = target_length;
    sync_file(pager->file);

    /* Publish free-space state before deleting the database WAL. A crash before
     * WAL removal can therefore recover both page images and allocator state. */
    persist_free_list_snapshot(pager->filename,
                               pager->num_pages,
                               pager->free_page_count,
                               pager->free_pages);

    if (remove(pager->wal_filename) != 0 && errno != ENOENT) {
        fail_io("Unable to remove checkpointed WAL");
    }
}

void pager_close(Pager* pager) {
    if (pager == NULL) return;

    clear_all_savepoints(pager);
    clear_transaction_free_snapshot(pager);
    free_page_slots(pager->dirty_page_spills, pager->page_capacity);
    free_page_slots(pager->committed_pages, pager->page_capacity);

    for (int i = 0; i < MAX_BUFFER_POOL_SIZE; i++) {
        free(pager->frames[i].data);
        pager->frames[i].data = NULL;
        db_rwlock_destroy(&pager->frames[i].rwlock);
    }
    db_rwlock_destroy(&pager->pager_lock);
    fclose(pager->file);

    free(pager->page_table);
    free(pager->is_dirty);
    free(pager->free_pages);
    free(pager->dirty_page_spills);
    free(pager->committed_pages);
    free(pager);
}

bool pager_savepoint(Pager* pager, const char* name) {
    if (pager->savepoint_count >= MAX_SAVEPOINTS) return false;

    Savepoint* sp = &pager->savepoints[pager->savepoint_count];
    memset(sp, 0, sizeof(*sp));
    strncpy(sp->name, name, sizeof(sp->name) - 1);
    sp->file_length = pager->file_length;
    sp->num_pages = pager->num_pages;
    sp->free_page_count = pager->free_page_count;
    sp->capacity = pager->num_pages;

    if (sp->free_page_count > 0) {
        sp->free_pages = (uint32_t*)checked_calloc_array(
            sp->free_page_count, sizeof(uint32_t),
            "Unable to allocate savepoint free-page snapshot.");
        memcpy(sp->free_pages,
               pager->free_pages,
               (size_t)sp->free_page_count * sizeof(uint32_t));
    }

    if (sp->capacity > 0) {
        sp->is_dirty = (bool*)checked_calloc_array(
            sp->capacity, sizeof(bool),
            "Unable to allocate savepoint dirty map.");
        sp->page_snapshots = (void**)checked_calloc_array(
            sp->capacity, sizeof(void*),
            "Unable to allocate savepoint page map.");
        memcpy(sp->is_dirty,
               pager->is_dirty,
               (size_t)sp->capacity * sizeof(bool));
    }

    for (uint32_t i = 0; i < sp->num_pages; i++) {
        void* page_data = get_page(pager, i);
        sp->page_snapshots[i] = malloc(PAGE_SIZE);
        if (sp->page_snapshots[i] == NULL) {
            fail_allocation("Unable to allocate savepoint page snapshot.");
        }
        memcpy(sp->page_snapshots[i], page_data, PAGE_SIZE);
        pager_unpin_page(pager, i);
    }

    pager->savepoint_count++;
    return true;
}

bool pager_rollback_to_savepoint(Pager* pager, const char* name) {
    int target_idx = -1;
    for (int i = (int)pager->savepoint_count - 1; i >= 0; i--) {
        if (strcmp(pager->savepoints[i].name, name) == 0) {
            target_idx = i;
            break;
        }
    }
    if (target_idx == -1) return false;

    Savepoint* sp = &pager->savepoints[target_idx];
    pager_reserve_capacity(pager, sp->num_pages);

    free_page_slots(pager->dirty_page_spills, pager->page_capacity);
    memset(pager->is_dirty, 0, (size_t)pager->page_capacity * sizeof(bool));
    clear_page_cache(pager);

    pager->num_pages = sp->num_pages;
    pager->free_page_count = sp->free_page_count;
    if (sp->free_page_count > 0) {
        memcpy(pager->free_pages,
               sp->free_pages,
               (size_t)sp->free_page_count * sizeof(uint32_t));
    }

    for (uint32_t i = 0; i < sp->num_pages; i++) {
        void* page_data = get_page(pager, i);
        memcpy(page_data, sp->page_snapshots[i], PAGE_SIZE);
        if (sp->is_dirty[i]) mark_page_dirty(pager, i);
        pager_unpin_page(pager, i);
    }

    for (uint32_t i = (uint32_t)target_idx + 1u; i < pager->savepoint_count; i++) {
        free_savepoint_snapshots(&pager->savepoints[i]);
    }
    pager->savepoint_count = (uint32_t)target_idx + 1u;
    return true;
}

bool pager_release_savepoint(Pager* pager, const char* name) {
    int target_idx = -1;
    for (int i = (int)pager->savepoint_count - 1; i >= 0; i--) {
        if (strcmp(pager->savepoints[i].name, name) == 0) {
            target_idx = i;
            break;
        }
    }
    if (target_idx == -1) return false;

    for (uint32_t i = (uint32_t)target_idx; i < pager->savepoint_count; i++) {
        free_savepoint_snapshots(&pager->savepoints[i]);
    }
    pager->savepoint_count = (uint32_t)target_idx;
    return true;
}

void pager_acquire_read_lock(Pager* pager, uint32_t page_num) {
    if (page_num >= pager->page_capacity) return;
    db_rwlock_rdlock(&pager->pager_lock);
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) db_rwlock_rdlock(&pager->frames[frame_idx].rwlock);
    db_rwlock_rdunlock(&pager->pager_lock);
}

void pager_release_read_lock(Pager* pager, uint32_t page_num) {
    if (page_num >= pager->page_capacity) return;
    db_rwlock_rdlock(&pager->pager_lock);
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) db_rwlock_rdunlock(&pager->frames[frame_idx].rwlock);
    db_rwlock_rdunlock(&pager->pager_lock);
}

void pager_acquire_write_lock(Pager* pager, uint32_t page_num) {
    if (page_num >= pager->page_capacity) return;
    db_rwlock_rdlock(&pager->pager_lock);
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) db_rwlock_wrlock(&pager->frames[frame_idx].rwlock);
    db_rwlock_rdunlock(&pager->pager_lock);
}

void pager_release_write_lock(Pager* pager, uint32_t page_num) {
    if (page_num >= pager->page_capacity) return;
    db_rwlock_rdlock(&pager->pager_lock);
    int frame_idx = pager->page_table[page_num];
    if (frame_idx != -1) db_rwlock_wrunlock(&pager->frames[frame_idx].rwlock);
    db_rwlock_rdunlock(&pager->pager_lock);
}

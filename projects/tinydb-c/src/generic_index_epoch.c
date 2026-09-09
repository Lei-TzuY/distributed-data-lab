#include "generic_index_epoch.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <io.h>
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

#define GENERIC_INDEX_EPOCH_MAGIC 0x47494550u
#define GENERIC_INDEX_EPOCH_VERSION 1u
#define FNV64_OFFSET 1469598103934665603ULL
#define FNV64_PRIME 1099511628211ULL

static uint64_t fnv64_update(uint64_t hash, const void* data, size_t size) {
    const unsigned char* bytes = (const unsigned char*)data;
    for (size_t i = 0; i < size; i++) {
        hash ^= (uint64_t)bytes[i];
        hash *= FNV64_PRIME;
    }
    return hash;
}

static void encode_u32(uint32_t value, unsigned char out[4]) {
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8) & 0xffu);
    out[2] = (unsigned char)((value >> 16) & 0xffu);
    out[3] = (unsigned char)((value >> 24) & 0xffu);
}

static void encode_u64(uint64_t value, unsigned char out[8]) {
    for (uint32_t i = 0; i < 8; i++) {
        out[i] = (unsigned char)((value >> (i * 8u)) & 0xffu);
    }
}

static uint32_t decode_u32(const unsigned char in[4]) {
    return ((uint32_t)in[0]) |
           ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static uint64_t decode_u64(const unsigned char in[8]) {
    uint64_t value = 0;
    for (uint32_t i = 0; i < 8; i++) {
        value |= ((uint64_t)in[i]) << (i * 8u);
    }
    return value;
}

static bool write_bytes(FILE* file, const void* data, size_t size) {
    return fwrite(data, 1, size, file) == size;
}

static bool read_bytes(FILE* file, void* data, size_t size) {
    return fread(data, 1, size, file) == size;
}

static bool sync_file(FILE* file) {
    if (fflush(file) != 0) return false;
#ifdef _WIN32
    return _commit(_fileno(file)) == 0;
#else
    return fsync(fileno(file)) == 0;
#endif
}

#ifndef _WIN32
static bool sync_parent_directory(const char* filename) {
    if (filename == NULL || filename[0] == '\0') return false;

    char directory[600];
    int written = snprintf(directory, sizeof(directory), "%s", filename);
    if (written < 0 || (size_t)written >= sizeof(directory)) return false;

    char* slash = strrchr(directory, '/');
    if (slash == NULL) {
        snprintf(directory, sizeof(directory), ".");
    } else if (slash == directory) {
        slash[1] = '\0';
    } else {
        *slash = '\0';
    }

    int fd = open(directory, O_RDONLY);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}
#endif

static bool replace_file_atomically(const char* temporary,
                                    const char* destination) {
#ifdef _WIN32
    return MoveFileExA(temporary,
                       destination,
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
    if (rename(temporary, destination) != 0) return false;
    return sync_parent_directory(destination);
#endif
}

static bool epoch_filename(const Table* table, char* output, size_t output_size) {
    if (table == NULL || table->pager == NULL) return false;
    int written = snprintf(output,
                           output_size,
                           "%s.gidx.epoch",
                           table->pager->filename);
    return written >= 0 && (size_t)written < output_size;
}

static bool remove_file_durably_if_present(const char* filename) {
    if (filename == NULL || filename[0] == '\0') return false;

    errno = 0;
    if (remove(filename) != 0) {
        return errno == ENOENT;
    }

#ifndef _WIN32
    if (!sync_parent_directory(filename)) return false;
#endif
    return true;
}

static bool remove_snapshot_durably(const char* filename) {
    if (filename == NULL || filename[0] == '\0') return false;

    errno = 0;
    if (remove(filename) != 0) {
        return errno == ENOENT;
    }

#ifndef _WIN32
    /*
     * remove(2) only updates the directory entry in memory. The epoch file is
     * the authority for reusing persistent range snapshots, so publishing a
     * replacement epoch before the snapshot deletion is durable leaves a
     * crash window where an old sidecar can reappear after reboot. Persist the
     * namespace mutation before the new authority is written.
     */
    if (!sync_parent_directory(filename)) return false;
#endif
    return true;
}

static bool cleanup_interrupted_snapshot_publications(Table* table) {
    if (table == NULL) return false;

    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (!index->enabled || index->num_columns != 1 ||
            index->index_filename[0] == '\0') {
            continue;
        }

        char temporary[640];
        int written = snprintf(temporary,
                               sizeof(temporary),
                               "%s.range.tmp",
                               index->index_filename);
        if (written < 0 || (size_t)written >= sizeof(temporary)) return false;

        /*
         * A range-snapshot temporary is never authoritative. If publication
         * was interrupted before replacement, the existing final snapshot is
         * still the only reusable cache image; if replacement already won,
         * this is merely leftover namespace garbage. Remove it durably before
         * epoch validation allows any persistent snapshot to be trusted.
         */
        if (!remove_file_durably_if_present(temporary)) return false;
    }
    return true;
}

static bool purge_generic_index_snapshots(Table* table) {
    if (table == NULL) return false;

    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (!index->enabled || index->num_columns != 1 ||
            index->index_filename[0] == '\0') {
            continue;
        }

        char filename[640];
        int written = snprintf(filename,
                               sizeof(filename),
                               "%s.range",
                               index->index_filename);
        if (written < 0 || (size_t)written >= sizeof(filename)) return false;

        if (!remove_snapshot_durably(filename)) return false;

        char temporary[640];
        written = snprintf(temporary,
                           sizeof(temporary),
                           "%s.range.tmp",
                           index->index_filename);
        if (written < 0 || (size_t)written >= sizeof(temporary)) return false;
        if (!remove_file_durably_if_present(temporary)) return false;
    }
    return true;
}

static uint64_t epoch_checksum(uint64_t epoch) {
    unsigned char u32[4];
    unsigned char u64[8];
    uint64_t hash = FNV64_OFFSET;

    encode_u32(GENERIC_INDEX_EPOCH_MAGIC, u32);
    hash = fnv64_update(hash, u32, sizeof(u32));
    encode_u32(GENERIC_INDEX_EPOCH_VERSION, u32);
    hash = fnv64_update(hash, u32, sizeof(u32));
    encode_u64(epoch, u64);
    hash = fnv64_update(hash, u64, sizeof(u64));
    return hash;
}

static bool test_fail_before_epoch_replace(void) {
    const char* value = getenv("TINYDB_TEST_FAIL_GENERIC_INDEX_EPOCH_BEFORE_REPLACE");
    return value != NULL && strcmp(value, "1") == 0;
}

static bool write_epoch_file(const char* filename, uint64_t epoch) {
    if (filename == NULL || filename[0] == '\0') return false;

    char temporary[640];
    int written = snprintf(temporary, sizeof(temporary), "%s.tmp", filename);
    if (written < 0 || (size_t)written >= sizeof(temporary)) return false;

    FILE* file = fopen(temporary, "wb");
    if (file == NULL) return false;

    unsigned char u32[4];
    unsigned char u64[8];
    bool ok = true;

    encode_u32(GENERIC_INDEX_EPOCH_MAGIC, u32);
    ok = ok && write_bytes(file, u32, sizeof(u32));
    encode_u32(GENERIC_INDEX_EPOCH_VERSION, u32);
    ok = ok && write_bytes(file, u32, sizeof(u32));
    encode_u64(epoch, u64);
    ok = ok && write_bytes(file, u64, sizeof(u64));
    encode_u64(epoch_checksum(epoch), u64);
    ok = ok && write_bytes(file, u64, sizeof(u64));

    if (ok) ok = sync_file(file);
    if (fclose(file) != 0) ok = false;
    if (!ok) {
        (void)remove_file_durably_if_present(temporary);
        return false;
    }

    /*
     * Deterministic crash-window regression seam. The temporary epoch is fully
     * durable here, while the previous final epoch is still authoritative.
     * Returning failure models interruption before atomic publication and lets
     * recovery tests prove that indexed mutations fail closed and that the
     * orphan is discarded on the next open/read barrier.
     */
    if (test_fail_before_epoch_replace()) return false;

    if (!replace_file_atomically(temporary, filename)) {
        (void)remove_file_durably_if_present(temporary);
        return false;
    }
    return true;
}

static bool cleanup_interrupted_epoch_publication(const char* filename) {
    if (filename == NULL || filename[0] == '\0') return false;

    char temporary[640];
    int written = snprintf(temporary, sizeof(temporary), "%s.tmp", filename);
    if (written < 0 || (size_t)written >= sizeof(temporary)) return false;

    /*
     * The temporary epoch is never authoritative. A crash before atomic
     * replacement means mutation could not have proceeded past the epoch
     * barrier; a crash after replacement leaves the final file authoritative.
     * Either way, an orphan temporary file is recovery garbage and must not be
     * carried indefinitely or mistaken for a second source of truth.
     */
    return remove_file_durably_if_present(temporary);
}

static bool read_epoch_file(const char* filename, uint64_t* epoch) {
    FILE* file = fopen(filename, "rb");
    if (file == NULL) return false;

    unsigned char u32[4];
    unsigned char u64[8];
    uint32_t magic = 0;
    uint32_t version = 0;
    uint64_t value = 0;
    uint64_t checksum = 0;

    bool ok = read_bytes(file, u32, sizeof(u32));
    if (ok) magic = decode_u32(u32);
    ok = ok && read_bytes(file, u32, sizeof(u32));
    if (ok) version = decode_u32(u32);
    ok = ok && read_bytes(file, u64, sizeof(u64));
    if (ok) value = decode_u64(u64);
    ok = ok && read_bytes(file, u64, sizeof(u64));
    if (ok) checksum = decode_u64(u64);
    if (ok) ok = fgetc(file) == EOF && !ferror(file);
    fclose(file);

    if (!ok || magic != GENERIC_INDEX_EPOCH_MAGIC ||
        version != GENERIC_INDEX_EPOCH_VERSION ||
        checksum != epoch_checksum(value)) {
        return false;
    }
    *epoch = value;
    return true;
}

static uint64_t fresh_epoch_seed(void) {
    uint64_t now = (uint64_t)time(NULL);
    uint64_t seed = (now << 32) ^ 0x9e3779b97f4a7c15ULL;
    if (seed == 0 || seed == UINT64_MAX) seed = 1;
    return seed;
}

static bool has_generic_index_for_schema(Table* table,
                                         const TableSchema* schema) {
    if (table == NULL || schema == NULL) return false;
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        GenericSecondaryIndex* index = &table->sec_indexes[i];
        if (index->enabled && index->num_columns == 1 &&
            strcmp(index->table_name, schema->name) == 0 &&
            strcmp(index->column_name, "id") != 0) {
            return true;
        }
    }
    return false;
}

bool tinydb_generic_index_epoch_current(Table* table, uint64_t* epoch) {
    if (table == NULL || epoch == NULL) return false;

    char filename[600];
    if (!epoch_filename(table, filename, sizeof(filename))) return false;
    if (!cleanup_interrupted_epoch_publication(filename)) return false;
    if (!cleanup_interrupted_snapshot_publications(table)) return false;

    uint64_t value = 0;
    if (read_epoch_file(filename, &value)) {
        *epoch = value;
        return true;
    }

    /*
     * The epoch sidecar is the authority that lets persistent range snapshots
     * prove they describe the current database state. If that authority is
     * missing or corrupt, a freshly generated epoch must not accidentally
     * make an old snapshot current again (fresh_epoch_seed is deliberately
     * lightweight and can repeat within the same second). Remove every
     * persistent generic snapshot first, then publish the replacement epoch.
     */
    if (!purge_generic_index_snapshots(table)) return false;

    value = fresh_epoch_seed();
    if (!write_epoch_file(filename, value)) return false;
    *epoch = value;
    return true;
}

bool tinydb_generic_index_epoch_before_mutation(Table* table,
                                                const TableSchema* schema) {
    if (!has_generic_index_for_schema(table, schema)) return true;

    char filename[600];
    if (!epoch_filename(table, filename, sizeof(filename))) return false;
    if (!cleanup_interrupted_epoch_publication(filename)) return false;
    if (!cleanup_interrupted_snapshot_publications(table)) return false;

    uint64_t epoch = 0;
    if (!read_epoch_file(filename, &epoch)) {
        /* Do not advance from unauthenticated epoch metadata while retaining
         * snapshots that may have been produced by an older incarnation. */
        if (!purge_generic_index_snapshots(table)) return false;
        epoch = fresh_epoch_seed();
    }
    if (epoch == UINT64_MAX) {
        epoch = fresh_epoch_seed();
    } else {
        epoch++;
    }
    return write_epoch_file(filename, epoch);
}

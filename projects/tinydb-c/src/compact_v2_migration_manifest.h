#ifndef COMPACT_V2_MIGRATION_MANIFEST_H
#define COMPACT_V2_MIGRATION_MANIFEST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/*
 * Persistent recovery metadata for fixed-V1 -> compact-V2 tree replacement.
 *
 * This header deliberately owns only the binary manifest contract and recovery
 * classification.  File creation/rename/fsync and catalog publication remain
 * separate durability boundaries.  The format is fixed-endian, versioned,
 * checksummed, length-delimited, and bounded so reopen can reject malformed or
 * adversarial metadata before trusting page identities.
 */

#define TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAGIC UINT32_C(0x32474d54) /* TMG2 */
#define TINYDB_COMPACT_V2_MIGRATION_MANIFEST_VERSION UINT16_C(1)
#define TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS UINT32_C(4096)
#define TINYDB_COMPACT_V2_MIGRATION_MANIFEST_FIXED_SIZE ((size_t)44u)
#define TINYDB_COMPACT_V2_MIGRATION_MANIFEST_CHECKSUM_SIZE ((size_t)4u)

typedef enum TinyDBCompactV2MigrationPhase {
    TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED = 1,
    TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED = 2
} TinyDBCompactV2MigrationPhase;

typedef struct TinyDBCompactV2MigrationManifest {
    uint32_t table_id;
    uint32_t old_root_page_num;
    uint32_t staged_root_page_num;
    uint64_t old_schema_generation;
    uint64_t new_schema_generation;
    TinyDBCompactV2MigrationPhase phase;
    uint32_t claimed_page_count;
    const uint32_t* claimed_pages;
} TinyDBCompactV2MigrationManifest;

typedef enum TinyDBCompactV2MigrationRecoveryAction {
    TINYDB_COMPACT_V2_MIGRATION_RECOVERY_INVALID = 0,
    TINYDB_COMPACT_V2_MIGRATION_RECOVERY_RECLAIM_STAGING = 1,
    TINYDB_COMPACT_V2_MIGRATION_RECOVERY_KEEP_NEW_RECLAIM_OLD = 2
} TinyDBCompactV2MigrationRecoveryAction;

static inline void tinydb_compact_v2_manifest_write_u16(unsigned char* out, uint16_t v) {
    out[0] = (unsigned char)(v & 0xffu);
    out[1] = (unsigned char)((v >> 8u) & 0xffu);
}

static inline void tinydb_compact_v2_manifest_write_u32(unsigned char* out, uint32_t v) {
    out[0] = (unsigned char)(v & 0xffu);
    out[1] = (unsigned char)((v >> 8u) & 0xffu);
    out[2] = (unsigned char)((v >> 16u) & 0xffu);
    out[3] = (unsigned char)((v >> 24u) & 0xffu);
}

static inline void tinydb_compact_v2_manifest_write_u64(unsigned char* out, uint64_t v) {
    for (uint32_t i = 0u; i < 8u; i++) {
        out[i] = (unsigned char)((v >> (8u * i)) & UINT64_C(0xff));
    }
}

static inline uint16_t tinydb_compact_v2_manifest_read_u16(const unsigned char* in) {
    return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8u));
}

static inline uint32_t tinydb_compact_v2_manifest_read_u32(const unsigned char* in) {
    return ((uint32_t)in[0]) |
           ((uint32_t)in[1] << 8u) |
           ((uint32_t)in[2] << 16u) |
           ((uint32_t)in[3] << 24u);
}

static inline uint64_t tinydb_compact_v2_manifest_read_u64(const unsigned char* in) {
    uint64_t value = UINT64_C(0);
    for (uint32_t i = 0u; i < 8u; i++) {
        value |= ((uint64_t)in[i]) << (8u * i);
    }
    return value;
}

static inline uint32_t tinydb_compact_v2_manifest_crc32(
    const unsigned char* data,
    size_t length) {
    uint32_t crc = UINT32_C(0xffffffff);
    if (data == NULL && length != 0u) return 0u;
    for (size_t i = 0u; i < length; i++) {
        crc ^= (uint32_t)data[i];
        for (uint32_t bit = 0u; bit < 8u; bit++) {
            uint32_t mask = (uint32_t)(0u - (crc & 1u));
            crc = (crc >> 1u) ^ (UINT32_C(0xedb88320) & mask);
        }
    }
    return ~crc;
}

static inline bool tinydb_compact_v2_migration_manifest_is_valid(
    const TinyDBCompactV2MigrationManifest* manifest) {
    if (manifest == NULL || manifest->table_id == 0u ||
        manifest->staged_root_page_num == 0u ||
        manifest->old_root_page_num == manifest->staged_root_page_num ||
        manifest->old_schema_generation >= manifest->new_schema_generation ||
        manifest->claimed_page_count == 0u ||
        manifest->claimed_page_count > TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS ||
        manifest->claimed_pages == NULL ||
        (manifest->phase != TINYDB_COMPACT_V2_MIGRATION_PHASE_DURABLE_UNPUBLISHED &&
         manifest->phase != TINYDB_COMPACT_V2_MIGRATION_PHASE_CATALOG_PUBLISHED)) {
        return false;
    }

    bool staged_root_claimed = false;
    for (uint32_t i = 0u; i < manifest->claimed_page_count; i++) {
        uint32_t page_num = manifest->claimed_pages[i];
        if (page_num == 0u || page_num == manifest->old_root_page_num) return false;
        if (page_num == manifest->staged_root_page_num) staged_root_claimed = true;
        for (uint32_t j = 0u; j < i; j++) {
            if (page_num == manifest->claimed_pages[j]) return false;
        }
    }
    return staged_root_claimed;
}

static inline bool tinydb_compact_v2_migration_manifest_encoded_size(
    uint32_t claimed_page_count,
    size_t* encoded_size) {
    if (encoded_size != NULL) *encoded_size = 0u;
    if (encoded_size == NULL || claimed_page_count == 0u ||
        claimed_page_count > TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAX_CLAIMS) {
        return false;
    }
    /* MAX_CLAIMS bounds the variable vector to 16 KiB. Together with the
     * fixed header/checksum this fits even the minimum 16-bit C size_t range,
     * so no wider uint32_t-vs-size_t overflow comparison is required here. */
    *encoded_size = TINYDB_COMPACT_V2_MIGRATION_MANIFEST_FIXED_SIZE +
                    ((size_t)claimed_page_count * sizeof(uint32_t)) +
                    TINYDB_COMPACT_V2_MIGRATION_MANIFEST_CHECKSUM_SIZE;
    return true;
}

static inline bool tinydb_compact_v2_migration_manifest_encode(
    const TinyDBCompactV2MigrationManifest* manifest,
    unsigned char* destination,
    size_t destination_capacity,
    size_t* encoded_length) {
    size_t needed = 0u;
    if (encoded_length != NULL) *encoded_length = 0u;
    if (manifest == NULL || destination == NULL || encoded_length == NULL ||
        !tinydb_compact_v2_migration_manifest_is_valid(manifest) ||
        !tinydb_compact_v2_migration_manifest_encoded_size(
            manifest->claimed_page_count, &needed) ||
        destination_capacity < needed) {
        return false;
    }

    unsigned char* scratch = destination;
    memset(scratch, 0, needed);
    tinydb_compact_v2_manifest_write_u32(scratch + 0u, TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAGIC);
    tinydb_compact_v2_manifest_write_u16(scratch + 4u, TINYDB_COMPACT_V2_MIGRATION_MANIFEST_VERSION);
    tinydb_compact_v2_manifest_write_u16(scratch + 6u, (uint16_t)manifest->phase);
    tinydb_compact_v2_manifest_write_u32(scratch + 8u, (uint32_t)needed);
    tinydb_compact_v2_manifest_write_u32(scratch + 12u, manifest->table_id);
    tinydb_compact_v2_manifest_write_u32(scratch + 16u, manifest->old_root_page_num);
    tinydb_compact_v2_manifest_write_u32(scratch + 20u, manifest->staged_root_page_num);
    tinydb_compact_v2_manifest_write_u64(scratch + 24u, manifest->old_schema_generation);
    tinydb_compact_v2_manifest_write_u64(scratch + 32u, manifest->new_schema_generation);
    tinydb_compact_v2_manifest_write_u32(scratch + 40u, manifest->claimed_page_count);
    for (uint32_t i = 0u; i < manifest->claimed_page_count; i++) {
        tinydb_compact_v2_manifest_write_u32(
            scratch + TINYDB_COMPACT_V2_MIGRATION_MANIFEST_FIXED_SIZE +
                ((size_t)i * sizeof(uint32_t)),
            manifest->claimed_pages[i]);
    }
    tinydb_compact_v2_manifest_write_u32(
        scratch + needed - TINYDB_COMPACT_V2_MIGRATION_MANIFEST_CHECKSUM_SIZE,
        tinydb_compact_v2_manifest_crc32(
            scratch, needed - TINYDB_COMPACT_V2_MIGRATION_MANIFEST_CHECKSUM_SIZE));
    *encoded_length = needed;
    return true;
}

static inline bool tinydb_compact_v2_migration_manifest_decode(
    const unsigned char* source,
    size_t source_length,
    TinyDBCompactV2MigrationManifest* manifest_out,
    uint32_t* claimed_pages_out,
    uint32_t claimed_pages_capacity) {
    TinyDBCompactV2MigrationManifest candidate;
    size_t expected_size = 0u;
    if (manifest_out != NULL) memset(manifest_out, 0, sizeof(*manifest_out));
    if (source == NULL || manifest_out == NULL || claimed_pages_out == NULL ||
        source_length < TINYDB_COMPACT_V2_MIGRATION_MANIFEST_FIXED_SIZE +
                            TINYDB_COMPACT_V2_MIGRATION_MANIFEST_CHECKSUM_SIZE ||
        tinydb_compact_v2_manifest_read_u32(source + 0u) !=
            TINYDB_COMPACT_V2_MIGRATION_MANIFEST_MAGIC ||
        tinydb_compact_v2_manifest_read_u16(source + 4u) !=
            TINYDB_COMPACT_V2_MIGRATION_MANIFEST_VERSION ||
        tinydb_compact_v2_manifest_read_u32(source + 8u) != source_length) {
        return false;
    }

    uint32_t claim_count = tinydb_compact_v2_manifest_read_u32(source + 40u);
    if (!tinydb_compact_v2_migration_manifest_encoded_size(claim_count, &expected_size) ||
        expected_size != source_length || claim_count > claimed_pages_capacity) {
        return false;
    }
    uint32_t expected_crc = tinydb_compact_v2_manifest_read_u32(
        source + source_length - TINYDB_COMPACT_V2_MIGRATION_MANIFEST_CHECKSUM_SIZE);
    uint32_t actual_crc = tinydb_compact_v2_manifest_crc32(
        source, source_length - TINYDB_COMPACT_V2_MIGRATION_MANIFEST_CHECKSUM_SIZE);
    if (expected_crc != actual_crc) return false;

    memset(&candidate, 0, sizeof(candidate));
    candidate.phase = (TinyDBCompactV2MigrationPhase)tinydb_compact_v2_manifest_read_u16(source + 6u);
    candidate.table_id = tinydb_compact_v2_manifest_read_u32(source + 12u);
    candidate.old_root_page_num = tinydb_compact_v2_manifest_read_u32(source + 16u);
    candidate.staged_root_page_num = tinydb_compact_v2_manifest_read_u32(source + 20u);
    candidate.old_schema_generation = tinydb_compact_v2_manifest_read_u64(source + 24u);
    candidate.new_schema_generation = tinydb_compact_v2_manifest_read_u64(source + 32u);
    candidate.claimed_page_count = claim_count;
    for (uint32_t i = 0u; i < claim_count; i++) {
        claimed_pages_out[i] = tinydb_compact_v2_manifest_read_u32(
            source + TINYDB_COMPACT_V2_MIGRATION_MANIFEST_FIXED_SIZE +
                ((size_t)i * sizeof(uint32_t)));
    }
    candidate.claimed_pages = claimed_pages_out;
    if (!tinydb_compact_v2_migration_manifest_is_valid(&candidate)) return false;

    *manifest_out = candidate;
    return true;
}

static inline TinyDBCompactV2MigrationRecoveryAction
 tinydb_compact_v2_migration_manifest_classify_recovery(
    const TinyDBCompactV2MigrationManifest* manifest,
    uint32_t authoritative_root_page_num,
    uint64_t authoritative_schema_generation) {
    if (!tinydb_compact_v2_migration_manifest_is_valid(manifest)) {
        return TINYDB_COMPACT_V2_MIGRATION_RECOVERY_INVALID;
    }
    if (authoritative_root_page_num == manifest->old_root_page_num &&
        authoritative_schema_generation == manifest->old_schema_generation) {
        return TINYDB_COMPACT_V2_MIGRATION_RECOVERY_RECLAIM_STAGING;
    }
    if (authoritative_root_page_num == manifest->staged_root_page_num &&
        authoritative_schema_generation == manifest->new_schema_generation) {
        return TINYDB_COMPACT_V2_MIGRATION_RECOVERY_KEEP_NEW_RECLAIM_OLD;
    }
    return TINYDB_COMPACT_V2_MIGRATION_RECOVERY_INVALID;
}

#endif

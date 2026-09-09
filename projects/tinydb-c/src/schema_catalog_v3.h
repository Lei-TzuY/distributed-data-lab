#ifndef TINYDB_SCHEMA_CATALOG_V3_H
#define TINYDB_SCHEMA_CATALOG_V3_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "schema_catalog_generation.h"

/*
 * Durable V3 identity/generation block.
 *
 * The existing schema-catalog V2 codec owns table shape, roots and views.  This
 * block is the bounded, endian-stable portion that V3 adds so migration reopen
 * can resolve a stable table id and monotonic schema generation without using
 * mutable names.  The root is repeated deliberately and must match the shape
 * catalog before the metadata can become authoritative.
 *
 * Wire format (little endian):
 *   magic:u32, version:u32, total_size:u32, num_tables:u32,
 *   entries[num_tables] { table_id:u32, root_page_num:u32, generation:u64 },
 *   checksum:u64
 * The checksum is FNV-1a over every byte before the checksum field.
 */

#define TINYDB_SCHEMA_CATALOG_V3_MAGIC UINT32_C(0x33435354) /* TSC3 */
#define TINYDB_SCHEMA_CATALOG_V3_VERSION UINT32_C(3)
#define TINYDB_SCHEMA_CATALOG_V3_HEADER_SIZE 16u
#define TINYDB_SCHEMA_CATALOG_V3_ENTRY_SIZE 16u
#define TINYDB_SCHEMA_CATALOG_V3_CHECKSUM_SIZE 8u
#define TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE \
    (TINYDB_SCHEMA_CATALOG_V3_HEADER_SIZE + \
     (MAX_TABLES * TINYDB_SCHEMA_CATALOG_V3_ENTRY_SIZE) + \
     TINYDB_SCHEMA_CATALOG_V3_CHECKSUM_SIZE)
#define TINYDB_SCHEMA_CATALOG_V3_FNV64_OFFSET UINT64_C(1469598103934665603)
#define TINYDB_SCHEMA_CATALOG_V3_FNV64_PRIME UINT64_C(1099511628211)

typedef enum TinyDBSchemaCatalogV3DecodeResult {
    TINYDB_SCHEMA_CATALOG_V3_DECODE_OK = 0,
    TINYDB_SCHEMA_CATALOG_V3_DECODE_TRUNCATED,
    TINYDB_SCHEMA_CATALOG_V3_DECODE_INVALID
} TinyDBSchemaCatalogV3DecodeResult;

static inline void tinydb_schema_catalog_v3_put_u32(unsigned char* out,
                                                     uint32_t value) {
    out[0] = (unsigned char)(value & 0xffu);
    out[1] = (unsigned char)((value >> 8) & 0xffu);
    out[2] = (unsigned char)((value >> 16) & 0xffu);
    out[3] = (unsigned char)((value >> 24) & 0xffu);
}

static inline void tinydb_schema_catalog_v3_put_u64(unsigned char* out,
                                                     uint64_t value) {
    for (uint32_t i = 0u; i < 8u; i++) {
        out[i] = (unsigned char)((value >> (8u * i)) & UINT64_C(0xff));
    }
}

static inline uint32_t tinydb_schema_catalog_v3_get_u32(const unsigned char* in) {
    return ((uint32_t)in[0]) |
           ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) |
           ((uint32_t)in[3] << 24);
}

static inline uint64_t tinydb_schema_catalog_v3_get_u64(const unsigned char* in) {
    uint64_t value = 0u;
    for (uint32_t i = 0u; i < 8u; i++) {
        value |= ((uint64_t)in[i]) << (8u * i);
    }
    return value;
}

static inline uint64_t tinydb_schema_catalog_v3_checksum(const unsigned char* data,
                                                          size_t size) {
    uint64_t hash = TINYDB_SCHEMA_CATALOG_V3_FNV64_OFFSET;
    for (size_t i = 0u; i < size; i++) {
        hash ^= (uint64_t)data[i];
        hash *= TINYDB_SCHEMA_CATALOG_V3_FNV64_PRIME;
    }
    return hash;
}

static inline bool tinydb_schema_catalog_v3_encoded_size(uint32_t num_tables,
                                                          size_t* size_out) {
    if (size_out != NULL) *size_out = 0u;
    if (size_out == NULL || num_tables == 0u || num_tables > MAX_TABLES) {
        return false;
    }
    *size_out = TINYDB_SCHEMA_CATALOG_V3_HEADER_SIZE +
                ((size_t)num_tables * TINYDB_SCHEMA_CATALOG_V3_ENTRY_SIZE) +
                TINYDB_SCHEMA_CATALOG_V3_CHECKSUM_SIZE;
    return true;
}

static inline bool tinydb_schema_catalog_v3_encode(
    const Catalog* catalog,
    const TinyDBSchemaCatalogGenerationSnapshot* snapshot,
    unsigned char* output,
    size_t output_capacity,
    size_t* output_size) {
    if (output_size != NULL) *output_size = 0u;
    if (catalog == NULL || snapshot == NULL || output == NULL || output_size == NULL ||
        !tinydb_schema_catalog_generation_is_valid(catalog, snapshot)) {
        return false;
    }

    size_t encoded_size = 0u;
    if (!tinydb_schema_catalog_v3_encoded_size(snapshot->num_tables, &encoded_size) ||
        output_capacity < encoded_size) {
        return false;
    }

    memset(output, 0, encoded_size);
    tinydb_schema_catalog_v3_put_u32(output, TINYDB_SCHEMA_CATALOG_V3_MAGIC);
    tinydb_schema_catalog_v3_put_u32(output + 4u, TINYDB_SCHEMA_CATALOG_V3_VERSION);
    tinydb_schema_catalog_v3_put_u32(output + 8u, (uint32_t)encoded_size);
    tinydb_schema_catalog_v3_put_u32(output + 12u, snapshot->num_tables);

    size_t offset = TINYDB_SCHEMA_CATALOG_V3_HEADER_SIZE;
    for (uint32_t i = 0u; i < snapshot->num_tables; i++) {
        const TinyDBSchemaCatalogGenerationEntry* entry = &snapshot->entries[i];
        tinydb_schema_catalog_v3_put_u32(output + offset, entry->table_id);
        tinydb_schema_catalog_v3_put_u32(output + offset + 4u, entry->root_page_num);
        tinydb_schema_catalog_v3_put_u64(output + offset + 8u, entry->schema_generation);
        offset += TINYDB_SCHEMA_CATALOG_V3_ENTRY_SIZE;
    }

    tinydb_schema_catalog_v3_put_u64(
        output + offset,
        tinydb_schema_catalog_v3_checksum(output, offset));
    *output_size = encoded_size;
    return true;
}

static inline TinyDBSchemaCatalogV3DecodeResult tinydb_schema_catalog_v3_decode(
    const Catalog* catalog,
    const unsigned char* input,
    size_t input_size,
    TinyDBSchemaCatalogGenerationSnapshot* snapshot_out) {
    tinydb_schema_catalog_generation_zero(snapshot_out);
    if (catalog == NULL || input == NULL || snapshot_out == NULL) {
        return TINYDB_SCHEMA_CATALOG_V3_DECODE_INVALID;
    }
    if (input_size < TINYDB_SCHEMA_CATALOG_V3_HEADER_SIZE +
                     TINYDB_SCHEMA_CATALOG_V3_CHECKSUM_SIZE) {
        return TINYDB_SCHEMA_CATALOG_V3_DECODE_TRUNCATED;
    }

    const uint32_t magic = tinydb_schema_catalog_v3_get_u32(input);
    const uint32_t version = tinydb_schema_catalog_v3_get_u32(input + 4u);
    const uint32_t declared_size = tinydb_schema_catalog_v3_get_u32(input + 8u);
    const uint32_t num_tables = tinydb_schema_catalog_v3_get_u32(input + 12u);
    size_t expected_size = 0u;
    if (magic != TINYDB_SCHEMA_CATALOG_V3_MAGIC ||
        version != TINYDB_SCHEMA_CATALOG_V3_VERSION ||
        !tinydb_schema_catalog_v3_encoded_size(num_tables, &expected_size) ||
        declared_size != expected_size || expected_size > TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE) {
        return TINYDB_SCHEMA_CATALOG_V3_DECODE_INVALID;
    }
    if (input_size < expected_size) return TINYDB_SCHEMA_CATALOG_V3_DECODE_TRUNCATED;
    if (input_size != expected_size) return TINYDB_SCHEMA_CATALOG_V3_DECODE_INVALID;

    const size_t checksum_offset = expected_size - TINYDB_SCHEMA_CATALOG_V3_CHECKSUM_SIZE;
    if (tinydb_schema_catalog_v3_get_u64(input + checksum_offset) !=
        tinydb_schema_catalog_v3_checksum(input, checksum_offset)) {
        return TINYDB_SCHEMA_CATALOG_V3_DECODE_INVALID;
    }

    TinyDBSchemaCatalogGenerationSnapshot decoded;
    tinydb_schema_catalog_generation_zero(&decoded);
    decoded.num_tables = num_tables;
    size_t offset = TINYDB_SCHEMA_CATALOG_V3_HEADER_SIZE;
    for (uint32_t i = 0u; i < num_tables; i++) {
        decoded.entries[i].table_id = tinydb_schema_catalog_v3_get_u32(input + offset);
        decoded.entries[i].root_page_num = tinydb_schema_catalog_v3_get_u32(input + offset + 4u);
        decoded.entries[i].schema_generation = tinydb_schema_catalog_v3_get_u64(input + offset + 8u);
        offset += TINYDB_SCHEMA_CATALOG_V3_ENTRY_SIZE;
    }

    if (!tinydb_schema_catalog_generation_is_valid(catalog, &decoded)) {
        return TINYDB_SCHEMA_CATALOG_V3_DECODE_INVALID;
    }
    *snapshot_out = decoded;
    return TINYDB_SCHEMA_CATALOG_V3_DECODE_OK;
}

#endif /* TINYDB_SCHEMA_CATALOG_V3_H */

#ifndef ROW_ENVELOPE_H
#define ROW_ENVELOPE_H

#include "record_payload.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TINYDB_ROW_ENVELOPE_MAGIC 0x31575254u /* TRW1 */
#define TINYDB_ROW_ENVELOPE_VERSION 1u
#define TINYDB_ROW_ENVELOPE_VERSION_COMPACT_V2 2u
#define TINYDB_ROW_ENVELOPE_HEADER_SIZE 20u
#define TINYDB_ROW_ENVELOPE_V2_HEADER_SIZE 24u
#define TINYDB_ROW_ENVELOPE_V2_DIRECTORY_ENTRY_SIZE 8u
#define TINYDB_ROW_ENVELOPE_MAGIC_OFFSET 0u
#define TINYDB_ROW_ENVELOPE_VERSION_OFFSET 4u
#define TINYDB_ROW_ENVELOPE_HEADER_SIZE_OFFSET 6u
#define TINYDB_ROW_ENVELOPE_LOGICAL_LENGTH_OFFSET 8u
#define TINYDB_ROW_ENVELOPE_SCHEMA_FINGERPRINT_OFFSET 12u
#define TINYDB_ROW_ENVELOPE_V2_FIELD_COUNT_OFFSET 20u
#define TINYDB_ROW_ENVELOPE_V2_DIRECTORY_ENTRY_SIZE_OFFSET 22u

static inline uint16_t tinydb_row_envelope_read_u16_le(
    const unsigned char* bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8));
}

static inline uint32_t tinydb_row_envelope_read_u32_le(
    const unsigned char* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static inline uint64_t tinydb_row_envelope_read_u64_le(
    const unsigned char* bytes) {
    uint64_t value = 0u;
    for (uint32_t i = 0u; i < 8u; i++) {
        value |= (uint64_t)bytes[i] << (8u * i);
    }
    return value;
}

static inline void tinydb_row_envelope_write_u16_le(unsigned char* bytes,
                                                     uint16_t value) {
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
}

static inline void tinydb_row_envelope_write_u32_le(unsigned char* bytes,
                                                     uint32_t value) {
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    bytes[2] = (unsigned char)((value >> 16) & 0xffu);
    bytes[3] = (unsigned char)((value >> 24) & 0xffu);
}

static inline void tinydb_row_envelope_write_u64_le(unsigned char* bytes,
                                                     uint64_t value) {
    for (uint32_t i = 0u; i < 8u; i++) {
        bytes[i] = (unsigned char)((value >> (8u * i)) & 0xffu);
    }
}

static inline uint64_t tinydb_row_envelope_hash_byte(uint64_t hash,
                                                      unsigned char byte) {
    hash ^= (uint64_t)byte;
    return hash * UINT64_C(1099511628211);
}

static inline uint64_t tinydb_row_envelope_hash_u32(uint64_t hash,
                                                     uint32_t value) {
    for (uint32_t i = 0u; i < 4u; i++) {
        hash = tinydb_row_envelope_hash_byte(
            hash, (unsigned char)((value >> (8u * i)) & 0xffu));
    }
    return hash;
}

static inline uint64_t tinydb_row_envelope_hash_text(uint64_t hash,
                                                      const char* text) {
    if (text == NULL) return tinydb_row_envelope_hash_byte(hash, 0u);
    while (*text != '\0') {
        hash = tinydb_row_envelope_hash_byte(hash, (unsigned char)*text++);
    }
    return tinydb_row_envelope_hash_byte(hash, 0u);
}

static inline uint64_t tinydb_row_envelope_schema_fingerprint(
    const TableSchema* schema) {
    if (schema == NULL) return 0u;
    uint64_t hash = UINT64_C(1469598103934665603);
    hash = tinydb_row_envelope_hash_text(hash, schema->name);
    hash = tinydb_row_envelope_hash_u32(hash, schema->num_columns);
    hash = tinydb_row_envelope_hash_u32(hash, schema->row_size);
    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        hash = tinydb_row_envelope_hash_text(hash, column->name);
        hash = tinydb_row_envelope_hash_u32(hash, (uint32_t)column->type);
        hash = tinydb_row_envelope_hash_u32(hash, column->offset);
        hash = tinydb_row_envelope_hash_u32(hash, column->size);
    }
    return hash;
}

static inline bool tinydb_row_envelope_encode(
    const TableSchema* schema,
    const TinyDBRecordPayload* payload,
    void* output,
    size_t output_capacity,
    uint32_t* stored_length) {
    if (schema == NULL || payload == NULL || output == NULL ||
        payload->length == 0u || payload->length != schema->row_size) {
        return false;
    }
    uint32_t total = TINYDB_ROW_ENVELOPE_HEADER_SIZE + payload->length;
    if (total > output_capacity || total > UINT16_MAX) return false;

    unsigned char* bytes = (unsigned char*)output;
    memset(bytes, 0, total);
    tinydb_row_envelope_write_u32_le(
        bytes + TINYDB_ROW_ENVELOPE_MAGIC_OFFSET,
        TINYDB_ROW_ENVELOPE_MAGIC);
    tinydb_row_envelope_write_u16_le(
        bytes + TINYDB_ROW_ENVELOPE_VERSION_OFFSET,
        TINYDB_ROW_ENVELOPE_VERSION);
    tinydb_row_envelope_write_u16_le(
        bytes + TINYDB_ROW_ENVELOPE_HEADER_SIZE_OFFSET,
        TINYDB_ROW_ENVELOPE_HEADER_SIZE);
    tinydb_row_envelope_write_u32_le(
        bytes + TINYDB_ROW_ENVELOPE_LOGICAL_LENGTH_OFFSET,
        payload->length);
    tinydb_row_envelope_write_u64_le(
        bytes + TINYDB_ROW_ENVELOPE_SCHEMA_FINGERPRINT_OFFSET,
        tinydb_row_envelope_schema_fingerprint(schema));
    memcpy(bytes + TINYDB_ROW_ENVELOPE_HEADER_SIZE,
           payload->bytes,
           payload->length);
    if (stored_length != NULL) *stored_length = total;
    return true;
}

static inline bool tinydb_row_envelope_v2_column_length(
    const TableColumn* column,
    const TinyDBRecordPayload* payload,
    uint32_t* stored_length) {
    if (column == NULL || payload == NULL || stored_length == NULL ||
        column->offset > payload->length ||
        column->size > payload->length - column->offset) {
        return false;
    }
    if (column->type == COL_TYPE_INT) {
        if (column->size != sizeof(uint32_t)) return false;
        *stored_length = (uint32_t)sizeof(uint32_t);
        return true;
    }
    if (column->type == COL_TYPE_VARCHAR) {
        const unsigned char* field = payload->bytes + column->offset;
        for (uint32_t i = 0u; i < column->size; i++) {
            if (field[i] == '\0') {
                *stored_length = i + 1u;
                return true;
            }
        }
    }
    return false;
}

static inline bool tinydb_row_envelope_encode_compact_v2(
    const TableSchema* schema,
    const TinyDBRecordPayload* payload,
    void* output,
    size_t output_capacity,
    uint32_t* stored_length) {
    if (schema == NULL || payload == NULL || output == NULL ||
        schema->num_columns == 0u || schema->num_columns > MAX_COLUMNS_PER_TABLE ||
        payload->length == 0u || payload->length != schema->row_size) {
        return false;
    }

    uint32_t directory_size =
        schema->num_columns * TINYDB_ROW_ENVELOPE_V2_DIRECTORY_ENTRY_SIZE;
    uint32_t body_offset = TINYDB_ROW_ENVELOPE_V2_HEADER_SIZE + directory_size;
    uint32_t total = body_offset;
    uint32_t lengths[MAX_COLUMNS_PER_TABLE];
    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        if (!tinydb_row_envelope_v2_column_length(&schema->columns[i],
                                                  payload,
                                                  &lengths[i]) ||
            lengths[i] > UINT16_MAX ||
            total > UINT16_MAX - lengths[i]) {
            return false;
        }
        total += lengths[i];
    }
    if (total > output_capacity || total > UINT16_MAX) return false;

    unsigned char* bytes = (unsigned char*)output;
    memset(bytes, 0, total);
    tinydb_row_envelope_write_u32_le(
        bytes + TINYDB_ROW_ENVELOPE_MAGIC_OFFSET,
        TINYDB_ROW_ENVELOPE_MAGIC);
    tinydb_row_envelope_write_u16_le(
        bytes + TINYDB_ROW_ENVELOPE_VERSION_OFFSET,
        TINYDB_ROW_ENVELOPE_VERSION_COMPACT_V2);
    tinydb_row_envelope_write_u16_le(
        bytes + TINYDB_ROW_ENVELOPE_HEADER_SIZE_OFFSET,
        TINYDB_ROW_ENVELOPE_V2_HEADER_SIZE);
    tinydb_row_envelope_write_u32_le(
        bytes + TINYDB_ROW_ENVELOPE_LOGICAL_LENGTH_OFFSET,
        payload->length);
    tinydb_row_envelope_write_u64_le(
        bytes + TINYDB_ROW_ENVELOPE_SCHEMA_FINGERPRINT_OFFSET,
        tinydb_row_envelope_schema_fingerprint(schema));
    tinydb_row_envelope_write_u16_le(
        bytes + TINYDB_ROW_ENVELOPE_V2_FIELD_COUNT_OFFSET,
        (uint16_t)schema->num_columns);
    tinydb_row_envelope_write_u16_le(
        bytes + TINYDB_ROW_ENVELOPE_V2_DIRECTORY_ENTRY_SIZE_OFFSET,
        TINYDB_ROW_ENVELOPE_V2_DIRECTORY_ENTRY_SIZE);

    uint32_t cursor = body_offset;
    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        unsigned char* entry = bytes + TINYDB_ROW_ENVELOPE_V2_HEADER_SIZE +
                               i * TINYDB_ROW_ENVELOPE_V2_DIRECTORY_ENTRY_SIZE;
        tinydb_row_envelope_write_u32_le(entry, cursor);
        tinydb_row_envelope_write_u32_le(entry + 4u, lengths[i]);
        memcpy(bytes + cursor,
               payload->bytes + schema->columns[i].offset,
               lengths[i]);
        cursor += lengths[i];
    }
    if (cursor != total) return false;
    if (stored_length != NULL) *stored_length = total;
    return true;
}

static inline bool tinydb_row_envelope_decode_v1(
    const TableSchema* schema,
    const unsigned char* bytes,
    uint32_t stored_length,
    TinyDBRecordPayload* payload) {
    if (tinydb_row_envelope_read_u16_le(
            bytes + TINYDB_ROW_ENVELOPE_HEADER_SIZE_OFFSET) !=
        TINYDB_ROW_ENVELOPE_HEADER_SIZE) {
        return false;
    }
    uint32_t logical_length = tinydb_row_envelope_read_u32_le(
        bytes + TINYDB_ROW_ENVELOPE_LOGICAL_LENGTH_OFFSET);
    if (logical_length == 0u || logical_length != schema->row_size ||
        logical_length > ROW_SIZE ||
        stored_length != TINYDB_ROW_ENVELOPE_HEADER_SIZE + logical_length ||
        tinydb_row_envelope_read_u64_le(
            bytes + TINYDB_ROW_ENVELOPE_SCHEMA_FINGERPRINT_OFFSET) !=
            tinydb_row_envelope_schema_fingerprint(schema)) {
        return false;
    }

    memset(payload, 0, sizeof(*payload));
    payload->length = logical_length;
    memcpy(payload->bytes,
           bytes + TINYDB_ROW_ENVELOPE_HEADER_SIZE,
           logical_length);
    return true;
}

static inline bool tinydb_row_envelope_prefix_schema(
    const TableSchema* schema,
    uint32_t field_count,
    uint32_t logical_length,
    TableSchema* prefix) {
    if (schema == NULL || prefix == NULL || field_count == 0u ||
        field_count > schema->num_columns) {
        return false;
    }

    uint32_t expected_length = 0u;
    for (uint32_t i = 0u; i < field_count; i++) {
        const TableColumn* column = &schema->columns[i];
        if (column->offset != expected_length || column->size == 0u ||
            expected_length > UINT32_MAX - column->size) {
            return false;
        }
        expected_length += column->size;
    }
    if (expected_length != logical_length) return false;

    *prefix = *schema;
    prefix->num_columns = field_count;
    prefix->row_size = logical_length;
    return true;
}

static inline bool tinydb_row_envelope_decode_compact_v2(
    const TableSchema* schema,
    const unsigned char* bytes,
    uint32_t stored_length,
    TinyDBRecordPayload* payload) {
    if (stored_length < TINYDB_ROW_ENVELOPE_V2_HEADER_SIZE ||
        tinydb_row_envelope_read_u16_le(
            bytes + TINYDB_ROW_ENVELOPE_HEADER_SIZE_OFFSET) !=
            TINYDB_ROW_ENVELOPE_V2_HEADER_SIZE ||
        tinydb_row_envelope_read_u16_le(
            bytes + TINYDB_ROW_ENVELOPE_V2_DIRECTORY_ENTRY_SIZE_OFFSET) !=
            TINYDB_ROW_ENVELOPE_V2_DIRECTORY_ENTRY_SIZE) {
        return false;
    }

    uint32_t logical_length = tinydb_row_envelope_read_u32_le(
        bytes + TINYDB_ROW_ENVELOPE_LOGICAL_LENGTH_OFFSET);
    uint32_t field_count = tinydb_row_envelope_read_u16_le(
        bytes + TINYDB_ROW_ENVELOPE_V2_FIELD_COUNT_OFFSET);
    if (field_count == 0u || field_count > schema->num_columns ||
        logical_length == 0u || logical_length > schema->row_size) {
        return false;
    }

    TableSchema stored_schema;
    if (!tinydb_row_envelope_prefix_schema(schema,
                                           field_count,
                                           logical_length,
                                           &stored_schema) ||
        tinydb_row_envelope_read_u64_le(
            bytes + TINYDB_ROW_ENVELOPE_SCHEMA_FINGERPRINT_OFFSET) !=
            tinydb_row_envelope_schema_fingerprint(&stored_schema)) {
        return false;
    }

    uint32_t directory_size =
        field_count * TINYDB_ROW_ENVELOPE_V2_DIRECTORY_ENTRY_SIZE;
    uint32_t expected_offset = TINYDB_ROW_ENVELOPE_V2_HEADER_SIZE + directory_size;
    if (expected_offset > stored_length) return false;

    memset(payload, 0, sizeof(*payload));
    payload->length = schema->row_size;
    for (uint32_t i = 0u; i < field_count; i++) {
        const TableColumn* column = &schema->columns[i];
        const unsigned char* entry = bytes + TINYDB_ROW_ENVELOPE_V2_HEADER_SIZE +
                                     i * TINYDB_ROW_ENVELOPE_V2_DIRECTORY_ENTRY_SIZE;
        uint32_t offset = tinydb_row_envelope_read_u32_le(entry);
        uint32_t length = tinydb_row_envelope_read_u32_le(entry + 4u);
        if (offset != expected_offset || length == 0u ||
            offset > stored_length || length > stored_length - offset ||
            column->offset > payload->length ||
            column->size > payload->length - column->offset) {
            return false;
        }

        if (column->type == COL_TYPE_INT) {
            if (column->size != sizeof(uint32_t) ||
                length != sizeof(uint32_t)) {
                return false;
            }
        } else if (column->type == COL_TYPE_VARCHAR) {
            if (length > column->size || bytes[offset + length - 1u] != '\0' ||
                (length > 1u && memchr(bytes + offset, '\0', length - 1u) != NULL)) {
                return false;
            }
        } else {
            return false;
        }

        memcpy(payload->bytes + column->offset, bytes + offset, length);
        expected_offset += length;
    }
    return expected_offset == stored_length;
}

static inline bool tinydb_row_envelope_decode(
    const TableSchema* schema,
    const void* stored_value,
    uint32_t stored_length,
    TinyDBRecordPayload* payload) {
    if (schema == NULL || stored_value == NULL || payload == NULL ||
        stored_length < TINYDB_ROW_ENVELOPE_HEADER_SIZE) {
        return false;
    }
    const unsigned char* bytes = (const unsigned char*)stored_value;
    if (tinydb_row_envelope_read_u32_le(
            bytes + TINYDB_ROW_ENVELOPE_MAGIC_OFFSET) !=
        TINYDB_ROW_ENVELOPE_MAGIC) {
        return false;
    }

    uint16_t version = tinydb_row_envelope_read_u16_le(
        bytes + TINYDB_ROW_ENVELOPE_VERSION_OFFSET);
    if (version == TINYDB_ROW_ENVELOPE_VERSION) {
        return tinydb_row_envelope_decode_v1(schema,
                                             bytes,
                                             stored_length,
                                             payload);
    }
    if (version == TINYDB_ROW_ENVELOPE_VERSION_COMPACT_V2) {
        return tinydb_row_envelope_decode_compact_v2(schema,
                                                     bytes,
                                                     stored_length,
                                                     payload);
    }
    return false;
}

#endif /* ROW_ENVELOPE_H */
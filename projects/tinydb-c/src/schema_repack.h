#ifndef SCHEMA_REPACK_H
#define SCHEMA_REPACK_H

#include "record_payload.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Pure logical-row repacking seam used by physical schema migrations.
 *
 * The first supported transformation is deliberately narrow: schemas must
 * describe the same ordered columns with the same types, while VARCHAR storage
 * widths may stay unchanged or grow. INT representation is fixed at 4 bytes.
 * This is enough to move a populated compact-V2 table from (for example)
 * VARCHAR(31) to VARCHAR(127) without interpreting the row through the legacy
 * TinyDBRecord ABI. No page/catalog mutation happens here; callers can stage
 * an entirely new tree before publishing a new schema generation.
 */

static inline void tinydb_schema_repack_set_message(char* message,
                                                    size_t message_size,
                                                    const char* text) {
    if (message == NULL || message_size == 0u) return;
    snprintf(message, message_size, "%s", text != NULL ? text : "");
}

static inline bool tinydb_schema_repack_name_valid(const char* name) {
    return name != NULL && name[0] != '\0' && memchr(name, '\0', MAX_NAME_SIZE) != NULL;
}

static inline bool tinydb_schema_repack_layout_valid(const TableSchema* schema) {
    if (schema == NULL || schema->num_columns == 0u ||
        schema->num_columns > MAX_COLUMNS_PER_TABLE ||
        schema->row_size == 0u || schema->row_size > TINYDB_RECORD_PAYLOAD_MAX) {
        return false;
    }

    uint32_t expected_offset = 0u;
    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        if (!tinydb_schema_repack_name_valid(column->name) ||
            column->offset != expected_offset || column->size == 0u ||
            column->offset > schema->row_size ||
            column->size > schema->row_size - column->offset) {
            return false;
        }
        if (column->type == COL_TYPE_INT) {
            if (column->size != sizeof(uint32_t)) return false;
        } else if (column->type == COL_TYPE_VARCHAR) {
            if (column->size < 2u) return false;
        } else {
            return false;
        }
        if (expected_offset > UINT32_MAX - column->size) return false;
        expected_offset += column->size;
    }
    return expected_offset == schema->row_size;
}

static inline bool tinydb_schema_repack_widening_supported(
    const TableSchema* source_schema,
    const TableSchema* destination_schema,
    char* message,
    size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (!tinydb_schema_repack_layout_valid(source_schema) ||
        !tinydb_schema_repack_layout_valid(destination_schema)) {
        tinydb_schema_repack_set_message(message,
                                         message_size,
                                         "source and destination schemas must have valid contiguous payload layouts");
        return false;
    }
    if (source_schema->num_columns != destination_schema->num_columns) {
        tinydb_schema_repack_set_message(message,
                                         message_size,
                                         "VARCHAR widening cannot add or drop columns");
        return false;
    }
    if (source_schema->columns[0].type != COL_TYPE_INT ||
        source_schema->columns[0].offset != 0u ||
        source_schema->columns[0].size != sizeof(uint32_t)) {
        tinydb_schema_repack_set_message(message,
                                         message_size,
                                         "schema repacking requires an INT primary key as the first column");
        return false;
    }

    for (uint32_t i = 0u; i < source_schema->num_columns; i++) {
        const TableColumn* source = &source_schema->columns[i];
        const TableColumn* destination = &destination_schema->columns[i];
        if (strcmp(source->name, destination->name) != 0) {
            tinydb_schema_repack_set_message(message,
                                             message_size,
                                             "VARCHAR widening cannot reorder or rename columns");
            return false;
        }
        if (source->type != destination->type) {
            tinydb_schema_repack_set_message(message,
                                             message_size,
                                             "VARCHAR widening cannot change column types");
            return false;
        }
        if (source->type == COL_TYPE_INT) {
            if (source->size != destination->size) {
                tinydb_schema_repack_set_message(message,
                                                 message_size,
                                                 "INT representation cannot change during VARCHAR widening");
                return false;
            }
        } else if (destination->size < source->size) {
            tinydb_schema_repack_set_message(message,
                                             message_size,
                                             "VARCHAR narrowing is not supported by the widening repacker");
            return false;
        }
    }
    return true;
}

static inline bool tinydb_schema_repack_widen_payload(
    const TableSchema* source_schema,
    const TableSchema* destination_schema,
    const TinyDBRecordPayload* source_payload,
    TinyDBRecordPayload* destination_payload,
    char* message,
    size_t message_size) {
    if (destination_payload != NULL) memset(destination_payload, 0, sizeof(*destination_payload));
    if (!tinydb_schema_repack_widening_supported(source_schema,
                                                 destination_schema,
                                                 message,
                                                 message_size)) {
        return false;
    }
    if (source_payload == NULL || destination_payload == NULL) {
        tinydb_schema_repack_set_message(message,
                                         message_size,
                                         "source and destination payloads are required");
        return false;
    }
    if (source_payload->length != source_schema->row_size) {
        tinydb_schema_repack_set_message(message,
                                         message_size,
                                         "source payload length does not match its schema");
        return false;
    }

    for (uint32_t i = 0u; i < source_schema->num_columns; i++) {
        const TableColumn* source = &source_schema->columns[i];
        const TableColumn* destination = &destination_schema->columns[i];
        const unsigned char* source_bytes = source_payload->bytes + source->offset;
        unsigned char* destination_bytes = destination_payload->bytes + destination->offset;

        if (source->type == COL_TYPE_VARCHAR) {
            bool terminated = false;
            for (uint32_t j = 0u; j < source->size; j++) {
                if (source_bytes[j] == '\0') {
                    terminated = true;
                    break;
                }
            }
            if (!terminated) {
                memset(destination_payload, 0, sizeof(*destination_payload));
                tinydb_schema_repack_set_message(message,
                                                 message_size,
                                                 "source VARCHAR field is not NUL-terminated within its storage width");
                return false;
            }
        }
        memcpy(destination_bytes, source_bytes, source->size);
    }

    destination_payload->length = destination_schema->row_size;
    if (message != NULL && message_size > 0u) message[0] = '\0';
    return true;
}

#endif /* SCHEMA_REPACK_H */

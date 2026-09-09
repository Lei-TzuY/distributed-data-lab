#ifndef SCHEMA_REPACK_STAGING_H
#define SCHEMA_REPACK_STAGING_H

#include "leaf_cursor_read.h"
#include "row_envelope.h"
#include "schema_repack.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/*
 * Unpublished schema-repack staging seam.
 *
 * This adapter deliberately stops before Pager allocation, WAL, migration
 * manifests, or catalog publication. A generic payload scan may feed rows into
 * the visitor below; each source row is validated against the configured old
 * schema, repacked into the widened destination layout, encoded as a compact-V2
 * row envelope using the destination schema fingerprint, and appended to a
 * private leaf chain in strict primary-key order.
 *
 * Failure can leave an accepted prefix only in caller-owned private memory.
 * Callers must discard the entire chain unless finish() succeeds. This mirrors
 * the existing fixed-V1 staging contract and gives future ALTER/recovery code a
 * clean all-or-discard boundary before any durable state is touched.
 */
typedef struct {
    const TableSchema* source_schema;
    const TableSchema* destination_schema;
    TinyDBCompactV2StagingLeafChain* chain;
    uint64_t source_schema_fingerprint;
    uint64_t rows_staged;
    bool failed;
    char message[192];
} TinyDBSchemaRepackStaging;

static inline void tinydb_schema_repack_staging_set_message(
    TinyDBSchemaRepackStaging* staging,
    const char* message) {
    if (staging == NULL) return;
    if (message == NULL) message = "schema repack staging failed";
    (void)snprintf(staging->message, sizeof(staging->message), "%s", message);
}

static inline bool tinydb_schema_repack_staging_init(
    TinyDBSchemaRepackStaging* staging,
    const TableSchema* source_schema,
    const TableSchema* destination_schema,
    TinyDBCompactV2StagingLeafChain* chain,
    char* message,
    size_t message_size) {
    if (staging != NULL) memset(staging, 0, sizeof(*staging));
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (staging == NULL || source_schema == NULL || destination_schema == NULL ||
        chain == NULL || chain->page_count != 1u || chain->row_count != 0u ||
        chain->has_last_key ||
        !tinydb_compact_v2_staging_leaf_chain_validate(chain)) {
        if (message != NULL && message_size > 0u) {
            (void)snprintf(message, message_size, "%s",
                           "repack staging requires a fresh validated private leaf chain");
        }
        return false;
    }

    char validation[160];
    if (!tinydb_schema_repack_widening_supported(source_schema,
                                                 destination_schema,
                                                 validation,
                                                 sizeof(validation))) {
        if (message != NULL && message_size > 0u) {
            (void)snprintf(message, message_size, "%s", validation);
        }
        return false;
    }

    staging->source_schema = source_schema;
    staging->destination_schema = destination_schema;
    staging->chain = chain;
    staging->source_schema_fingerprint =
        tinydb_row_envelope_schema_fingerprint(source_schema);
    if (staging->source_schema_fingerprint == 0u) {
        if (message != NULL && message_size > 0u) {
            (void)snprintf(message, message_size, "%s",
                           "source schema fingerprint is invalid");
        }
        memset(staging, 0, sizeof(*staging));
        return false;
    }
    return true;
}

static inline bool tinydb_schema_repack_staging_visit(
    const TableSchema* schema,
    const TinyDBRecordPayload* source_payload,
    void* context) {
    TinyDBSchemaRepackStaging* staging = (TinyDBSchemaRepackStaging*)context;
    if (staging == NULL || staging->failed || staging->source_schema == NULL ||
        staging->destination_schema == NULL || staging->chain == NULL ||
        schema == NULL || source_payload == NULL) {
        return false;
    }

    if (tinydb_row_envelope_schema_fingerprint(schema) !=
        staging->source_schema_fingerprint) {
        staging->failed = true;
        tinydb_schema_repack_staging_set_message(
            staging, "source scan schema drifted during repack staging");
        return false;
    }

    TinyDBRecordPayload destination_payload;
    char validation[160];
    if (!tinydb_schema_repack_widen_payload(staging->source_schema,
                                            staging->destination_schema,
                                            source_payload,
                                            &destination_payload,
                                            validation,
                                            sizeof(validation))) {
        staging->failed = true;
        tinydb_schema_repack_staging_set_message(staging, validation);
        return false;
    }

    /* widening_supported() has already proven the first field is the stable
     * four-byte INT primary key at offset zero, so extracting it directly keeps
     * this pure-memory staging header independent of record_payload.c linkage. */
    uint32_t key = 0u;
    memcpy(&key, destination_payload.bytes, sizeof(key));

    unsigned char envelope[PAGE_SIZE];
    uint32_t envelope_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(staging->destination_schema,
                                                &destination_payload,
                                                envelope,
                                                sizeof(envelope),
                                                &envelope_length) ||
        envelope_length == 0u) {
        staging->failed = true;
        tinydb_schema_repack_staging_set_message(
            staging, "repacked row cannot be encoded as a compact-V2 envelope");
        return false;
    }

    if (!tinydb_compact_v2_staging_leaf_chain_append(staging->chain,
                                                      key,
                                                      envelope,
                                                      envelope_length)) {
        staging->failed = true;
        tinydb_schema_repack_staging_set_message(
            staging, "repacked row cannot be appended to the private staging chain");
        return false;
    }

    staging->rows_staged++;
    staging->message[0] = '\0';
    return true;
}

static inline bool tinydb_schema_repack_staging_finish(
    TinyDBSchemaRepackStaging* staging,
    uint64_t expected_rows,
    char* message,
    size_t message_size) {
    if (message != NULL && message_size > 0u) message[0] = '\0';
    if (staging == NULL || staging->failed || staging->chain == NULL ||
        staging->rows_staged != expected_rows ||
        staging->chain->row_count != expected_rows ||
        !tinydb_compact_v2_staging_leaf_chain_validate(staging->chain)) {
        const char* detail = staging != NULL && staging->message[0] != '\0'
            ? staging->message
            : "schema repack staging did not finish with a complete validated row set";
        if (message != NULL && message_size > 0u) {
            (void)snprintf(message, message_size, "%s", detail);
        }
        return false;
    }
    return true;
}

#endif /* SCHEMA_REPACK_STAGING_H */

#ifndef TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_H
#define TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "schema_catalog_v3.h"

/*
 * Production V3 schema-catalog envelope.
 *
 * V2 already has a bounded schema-shape payload.  V3 must not persist the
 * stable table-id / schema-generation block in a separate durability domain:
 * root + schema shape + generation are one publication decision.  This helper
 * therefore packs the opaque V2-compatible shape payload and the checksummed
 * V3 identity block into one outer checksummed byte stream suitable for the
 * existing .schema WAL -> main-file publication sequence.
 *
 * Wire format (little endian):
 *   magic:u32, version:u32, total_size:u32,
 *   shape_size:u32, identity_size:u32,
 *   shape[shape_size], identity[identity_size], checksum:u64
 *
 * The outer checksum covers the header and both sections.  The identity
 * section retains its own checksum and must subsequently be decoded against
 * the decoded shape Catalog with tinydb_schema_catalog_v3_decode().
 */

#define TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAGIC UINT32_C(0x56435354) /* TSCV */
#define TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_VERSION UINT32_C(3)
#define TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE 20u
#define TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_CHECKSUM_SIZE 8u
#define TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SHAPE_SIZE 32768u
#define TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE \
    (TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE + \
     TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SHAPE_SIZE + \
     TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE + \
     TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_CHECKSUM_SIZE)

typedef enum TinyDBSchemaCatalogV3EnvelopeDecodeResult {
    TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK = 0,
    TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_TRUNCATED,
    TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID
} TinyDBSchemaCatalogV3EnvelopeDecodeResult;

typedef struct TinyDBSchemaCatalogV3EnvelopeView {
    const unsigned char* shape;
    size_t shape_size;
    const unsigned char* identity;
    size_t identity_size;
} TinyDBSchemaCatalogV3EnvelopeView;

static inline void tinydb_schema_catalog_v3_envelope_zero_view(
    TinyDBSchemaCatalogV3EnvelopeView* view) {
    if (view != NULL) memset(view, 0, sizeof(*view));
}

static inline bool tinydb_schema_catalog_v3_envelope_size(size_t shape_size,
                                                           size_t identity_size,
                                                           size_t* encoded_size_out) {
    if (encoded_size_out != NULL) *encoded_size_out = 0u;
    if (encoded_size_out == NULL || shape_size == 0u ||
        shape_size > TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SHAPE_SIZE ||
        identity_size < TINYDB_SCHEMA_CATALOG_V3_HEADER_SIZE +
                        TINYDB_SCHEMA_CATALOG_V3_CHECKSUM_SIZE ||
        identity_size > TINYDB_SCHEMA_CATALOG_V3_MAX_SIZE) {
        return false;
    }
    *encoded_size_out = TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE +
                        shape_size + identity_size +
                        TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_CHECKSUM_SIZE;
    return *encoded_size_out <= TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAX_SIZE &&
           *encoded_size_out <= UINT32_MAX;
}

static inline bool tinydb_schema_catalog_v3_envelope_encode(
    const unsigned char* shape,
    size_t shape_size,
    const unsigned char* identity,
    size_t identity_size,
    unsigned char* output,
    size_t output_capacity,
    size_t* output_size) {
    if (output_size != NULL) *output_size = 0u;
    if (shape == NULL || identity == NULL || output == NULL || output_size == NULL ||
        identity_size > UINT32_MAX || shape_size > UINT32_MAX) {
        return false;
    }

    size_t encoded_size = 0u;
    if (!tinydb_schema_catalog_v3_envelope_size(shape_size,
                                                 identity_size,
                                                 &encoded_size) ||
        output_capacity < encoded_size) {
        return false;
    }

    tinydb_schema_catalog_v3_put_u32(output,
                                     TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAGIC);
    tinydb_schema_catalog_v3_put_u32(output + 4u,
                                     TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_VERSION);
    tinydb_schema_catalog_v3_put_u32(output + 8u, (uint32_t)encoded_size);
    tinydb_schema_catalog_v3_put_u32(output + 12u, (uint32_t)shape_size);
    tinydb_schema_catalog_v3_put_u32(output + 16u, (uint32_t)identity_size);

    size_t offset = TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE;
    memcpy(output + offset, shape, shape_size);
    offset += shape_size;
    memcpy(output + offset, identity, identity_size);
    offset += identity_size;
    tinydb_schema_catalog_v3_put_u64(
        output + offset,
        tinydb_schema_catalog_v3_checksum(output, offset));
    *output_size = encoded_size;
    return true;
}

static inline TinyDBSchemaCatalogV3EnvelopeDecodeResult
 tinydb_schema_catalog_v3_envelope_decode(
    const unsigned char* input,
    size_t input_size,
    TinyDBSchemaCatalogV3EnvelopeView* view_out) {
    tinydb_schema_catalog_v3_envelope_zero_view(view_out);
    if (input == NULL || view_out == NULL) {
        return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID;
    }
    if (input_size < TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE +
                     TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_CHECKSUM_SIZE) {
        return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_TRUNCATED;
    }

    const uint32_t magic = tinydb_schema_catalog_v3_get_u32(input);
    const uint32_t version = tinydb_schema_catalog_v3_get_u32(input + 4u);
    const uint32_t declared_size = tinydb_schema_catalog_v3_get_u32(input + 8u);
    const uint32_t shape_size = tinydb_schema_catalog_v3_get_u32(input + 12u);
    const uint32_t identity_size = tinydb_schema_catalog_v3_get_u32(input + 16u);
    size_t expected_size = 0u;
    if (magic != TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_MAGIC ||
        version != TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_VERSION ||
        !tinydb_schema_catalog_v3_envelope_size((size_t)shape_size,
                                                 (size_t)identity_size,
                                                 &expected_size) ||
        declared_size != expected_size) {
        return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID;
    }
    if (input_size < expected_size) {
        return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_TRUNCATED;
    }
    if (input_size != expected_size) {
        return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID;
    }

    const size_t checksum_offset =
        expected_size - TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_CHECKSUM_SIZE;
    if (tinydb_schema_catalog_v3_get_u64(input + checksum_offset) !=
        tinydb_schema_catalog_v3_checksum(input, checksum_offset)) {
        return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID;
    }

    const size_t shape_offset = TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_HEADER_SIZE;
    const size_t identity_offset = shape_offset + (size_t)shape_size;
    if (identity_offset > checksum_offset ||
        (size_t)identity_size != checksum_offset - identity_offset) {
        return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID;
    }

    view_out->shape = input + shape_offset;
    view_out->shape_size = (size_t)shape_size;
    view_out->identity = input + identity_offset;
    view_out->identity_size = (size_t)identity_size;
    return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK;
}

static inline TinyDBSchemaCatalogV3EnvelopeDecodeResult
 tinydb_schema_catalog_v3_envelope_decode_identity(
    const Catalog* decoded_shape_catalog,
    const unsigned char* input,
    size_t input_size,
    TinyDBSchemaCatalogGenerationSnapshot* snapshot_out,
    TinyDBSchemaCatalogV3EnvelopeView* view_out) {
    tinydb_schema_catalog_generation_zero(snapshot_out);
    TinyDBSchemaCatalogV3EnvelopeView local_view;
    tinydb_schema_catalog_v3_envelope_zero_view(&local_view);
    TinyDBSchemaCatalogV3EnvelopeDecodeResult envelope_result =
        tinydb_schema_catalog_v3_envelope_decode(input, input_size, &local_view);
    if (envelope_result != TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK) {
        return envelope_result;
    }
    if (decoded_shape_catalog == NULL || snapshot_out == NULL ||
        tinydb_schema_catalog_v3_decode(decoded_shape_catalog,
                                        local_view.identity,
                                        local_view.identity_size,
                                        snapshot_out) !=
            TINYDB_SCHEMA_CATALOG_V3_DECODE_OK) {
        tinydb_schema_catalog_generation_zero(snapshot_out);
        return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_INVALID;
    }
    if (view_out != NULL) *view_out = local_view;
    return TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_DECODE_OK;
}

#endif /* TINYDB_SCHEMA_CATALOG_V3_ENVELOPE_H */

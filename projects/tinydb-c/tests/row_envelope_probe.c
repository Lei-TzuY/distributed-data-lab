#include "record.h"
#include "record_payload.h"
#include "row_envelope.h"

#include <stdio.h>
#include <string.h>

static TableSchema make_schema(void) {
    TableSchema schema;
    memset(&schema, 0, sizeof(schema));
    snprintf(schema.name, sizeof(schema.name), "%s", "products");
    schema.num_columns = 3u;
    snprintf(schema.columns[0].name, sizeof(schema.columns[0].name), "%s", "id");
    schema.columns[0].type = COL_TYPE_INT;
    schema.columns[0].offset = 0u;
    schema.columns[0].size = 4u;
    snprintf(schema.columns[1].name, sizeof(schema.columns[1].name), "%s", "name");
    schema.columns[1].type = COL_TYPE_VARCHAR;
    schema.columns[1].offset = 4u;
    schema.columns[1].size = 65u;
    snprintf(schema.columns[2].name, sizeof(schema.columns[2].name), "%s", "price");
    schema.columns[2].type = COL_TYPE_INT;
    schema.columns[2].offset = 69u;
    schema.columns[2].size = 4u;
    schema.row_size = 73u;
    return schema;
}

static bool build_payload(const TableSchema* schema,
                          const char* name,
                          TinyDBRecordPayload* payload) {
    TinyDBValue values[3];
    memset(values, 0, sizeof(values));
    values[0].type = COL_TYPE_INT;
    values[0].int_value = 7u;
    values[1].type = COL_TYPE_VARCHAR;
    snprintf(values[1].text, sizeof(values[1].text), "%s", name);
    values[2].type = COL_TYPE_INT;
    values[2].int_value = 42u;

    TinyDBRecord record;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_encode(schema,
                              values,
                              3u,
                              &record,
                              message,
                              sizeof(message))) {
        fprintf(stderr, "record encode failed: %s\n", message);
        return false;
    }
    if (!tinydb_record_payload_from_record(schema,
                                           &record,
                                           payload,
                                           message,
                                           sizeof(message))) {
        fprintf(stderr, "payload conversion failed: %s\n", message);
        return false;
    }
    return true;
}

static bool expect_roundtrip(const TableSchema* schema,
                             const TinyDBRecordPayload* expected,
                             const unsigned char* stored,
                             uint32_t stored_length) {
    TinyDBRecordPayload decoded;
    if (!tinydb_row_envelope_decode(schema,
                                    stored,
                                    stored_length,
                                    &decoded) ||
        decoded.length != expected->length ||
        memcmp(decoded.bytes, expected->bytes, expected->length) != 0) {
        fprintf(stderr, "compact envelope payload roundtrip failed\n");
        return false;
    }

    TinyDBRecord record;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_payload_to_record(schema,
                                         &decoded,
                                         &record,
                                         message,
                                         sizeof(message))) {
        fprintf(stderr, "payload-to-record failed: %s\n", message);
        return false;
    }
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t value_count = 0u;
    if (!tinydb_record_decode(schema,
                              &record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &value_count,
                              message,
                              sizeof(message))) {
        fprintf(stderr, "record decode failed: %s\n", message);
        return false;
    }
    return value_count == 3u &&
           values[0].int_value == 7u &&
           strcmp(values[1].text, "alpha") == 0 &&
           values[2].int_value == 42u;
}

static bool corruption_is_rejected(const TableSchema* schema,
                                   const unsigned char* valid,
                                   uint32_t valid_length,
                                   uint32_t offset,
                                   unsigned char xor_value) {
    unsigned char corrupt[512];
    memcpy(corrupt, valid, valid_length);
    corrupt[offset] ^= xor_value;
    TinyDBRecordPayload decoded;
    return !tinydb_row_envelope_decode(schema,
                                       corrupt,
                                       valid_length,
                                       &decoded);
}

int main(void) {
    TableSchema schema = make_schema();
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(&schema, message, sizeof(message))) {
        fprintf(stderr, "schema unexpectedly unsupported: %s\n", message);
        return 1;
    }

    TinyDBRecordPayload payload;
    if (!build_payload(&schema, "alpha", &payload)) return 1;

    unsigned char v1[512];
    unsigned char v2[512];
    uint32_t v1_length = 0u;
    uint32_t v2_length = 0u;
    if (!tinydb_row_envelope_encode(&schema,
                                    &payload,
                                    v1,
                                    sizeof(v1),
                                    &v1_length) ||
        !tinydb_row_envelope_encode_compact_v2(&schema,
                                               &payload,
                                               v2,
                                               sizeof(v2),
                                               &v2_length)) {
        fprintf(stderr, "row envelope encoding failed\n");
        return 1;
    }
    if (v2_length >= v1_length || v2_length != 62u) {
        fprintf(stderr,
                "compact envelope did not shrink fixed VARCHAR payload: v1=%u v2=%u\n",
                v1_length,
                v2_length);
        return 1;
    }
    if (!expect_roundtrip(&schema, &payload, v2, v2_length)) return 1;

    TinyDBRecordPayload decoded;
    if (!tinydb_row_envelope_decode(&schema, v1, v1_length, &decoded) ||
        decoded.length != payload.length ||
        memcmp(decoded.bytes, payload.bytes, payload.length) != 0) {
        fprintf(stderr, "legacy envelope v1 compatibility regressed\n");
        return 1;
    }

    if (!corruption_is_rejected(&schema,
                                v2,
                                v2_length,
                                TINYDB_ROW_ENVELOPE_VERSION_OFFSET,
                                0x40u) ||
        !corruption_is_rejected(&schema,
                                v2,
                                v2_length,
                                TINYDB_ROW_ENVELOPE_SCHEMA_FINGERPRINT_OFFSET,
                                0x01u) ||
        !corruption_is_rejected(&schema,
                                v2,
                                v2_length,
                                TINYDB_ROW_ENVELOPE_V2_FIELD_COUNT_OFFSET,
                                0x01u)) {
        fprintf(stderr, "compact envelope header corruption was accepted\n");
        return 1;
    }

    unsigned char corrupt[512];
    memcpy(corrupt, v2, v2_length);
    uint32_t second_entry = TINYDB_ROW_ENVELOPE_V2_HEADER_SIZE +
                            TINYDB_ROW_ENVELOPE_V2_DIRECTORY_ENTRY_SIZE;
    corrupt[second_entry] += 1u;
    if (tinydb_row_envelope_decode(&schema, corrupt, v2_length, &decoded)) {
        fprintf(stderr, "non-contiguous compact directory was accepted\n");
        return 1;
    }

    memcpy(corrupt, v2, v2_length);
    uint32_t name_offset = tinydb_row_envelope_read_u32_le(v2 + second_entry);
    uint32_t name_length = tinydb_row_envelope_read_u32_le(v2 + second_entry + 4u);
    if (name_length != 6u || name_offset + name_length > v2_length) {
        fprintf(stderr, "unexpected encoded VARCHAR directory entry\n");
        return 1;
    }
    corrupt[name_offset + name_length - 1u] = 'x';
    if (tinydb_row_envelope_decode(&schema, corrupt, v2_length, &decoded)) {
        fprintf(stderr, "unterminated compact VARCHAR was accepted\n");
        return 1;
    }

    memcpy(corrupt, v2, v2_length);
    corrupt[name_offset + 1u] = '\0';
    if (tinydb_row_envelope_decode(&schema, corrupt, v2_length, &decoded)) {
        fprintf(stderr, "non-canonical compact VARCHAR with interior NUL was accepted\n");
        return 1;
    }

    memcpy(corrupt, v2, v2_length);
    corrupt[v2_length] = 0xa5u;
    if (tinydb_row_envelope_decode(&schema,
                                   corrupt,
                                   v2_length + 1u,
                                   &decoded)) {
        fprintf(stderr, "compact envelope trailing byte was accepted\n");
        return 1;
    }

    TinyDBRecordPayload empty_payload;
    if (!build_payload(&schema, "", &empty_payload)) return 1;
    uint32_t empty_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(&schema,
                                               &empty_payload,
                                               v2,
                                               sizeof(v2),
                                               &empty_length) ||
        empty_length != 57u ||
        !tinydb_row_envelope_decode(&schema,
                                    v2,
                                    empty_length,
                                    &decoded) ||
        decoded.length != empty_payload.length ||
        memcmp(decoded.bytes,
               empty_payload.bytes,
               empty_payload.length) != 0) {
        fprintf(stderr, "empty VARCHAR compact envelope roundtrip failed\n");
        return 1;
    }

    printf("PASS: compact row-envelope V2 stores VARCHAR fields at actual length, preserves V1 compatibility, expands into canonical schema payloads, and rejects malformed directories, strings, identities, versions, and trailing bytes.\n");
    return 0;
}

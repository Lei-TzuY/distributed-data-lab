#include "record_payload.h"
#include "record_payload_key.h"
#include "row_envelope.h"

#include <stdio.h>
#include <string.h>

static void init_column(TableColumn* column,
                        const char* name,
                        ColumnType type,
                        uint32_t offset,
                        uint32_t size) {
    memset(column, 0, sizeof(*column));
    snprintf(column->name, sizeof(column->name), "%s", name);
    column->type = type;
    column->offset = offset;
    column->size = size;
}

int main(void) {
    TableSchema wide;
    memset(&wide, 0, sizeof(wide));
    snprintf(wide.name, sizeof(wide.name), "%s", "wide_notes");
    wide.num_columns = 3u;
    init_column(&wide.columns[0], "id", COL_TYPE_INT, 0u, 4u);
    init_column(&wide.columns[1], "description", COL_TYPE_VARCHAR, 4u, 300u);
    init_column(&wide.columns[2], "score", COL_TYPE_INT, 304u, 4u);
    wide.row_size = 308u;

    if (wide.row_size <= ROW_SIZE) {
        fprintf(stderr, "probe schema must exceed legacy ROW_SIZE\n");
        return 1;
    }

    TinyDBValue input[3];
    memset(input, 0, sizeof(input));
    input[0].type = COL_TYPE_INT;
    input[0].int_value = 7u;
    input[1].type = COL_TYPE_VARCHAR;
    snprintf(input[1].text, sizeof(input[1].text), "%s",
             "schema-sized payload survives compact V2 encoding");
    input[2].type = COL_TYPE_INT;
    input[2].int_value = 99u;

    char message[256];
    TinyDBRecordPayload encoded;
    if (!tinydb_record_payload_schema_supported(&wide, message, sizeof(message)) ||
        !tinydb_record_payload_encode_values(&wide,
                                             input,
                                             3u,
                                             &encoded,
                                             message,
                                             sizeof(message))) {
        fprintf(stderr, "wide payload encode failed: %s\n", message);
        return 2;
    }
    if (encoded.length != wide.row_size || encoded.length <= ROW_SIZE) {
        fprintf(stderr, "logical payload did not preserve wide schema length\n");
        return 3;
    }

    uint32_t primary_key = 0u;
    if (!tinydb_record_payload_primary_key(&wide,
                                           &encoded,
                                           &primary_key,
                                           message,
                                           sizeof(message)) ||
        primary_key != 7u) {
        fprintf(stderr, "schema-sized primary-key decode failed: %s\n", message);
        return 4;
    }

    TinyDBRecordPayload malformed = encoded;
    malformed.length--;
    primary_key = 123u;
    if (tinydb_record_payload_primary_key(&wide,
                                          &malformed,
                                          &primary_key,
                                          message,
                                          sizeof(message)) ||
        primary_key != 0u) {
        fprintf(stderr, "primary-key decoder accepted malformed payload length\n");
        return 5;
    }

    TinyDBValue decoded[3];
    uint32_t decoded_count = 0u;
    if (!tinydb_record_payload_decode_values(&wide,
                                             &encoded,
                                             decoded,
                                             3u,
                                             &decoded_count,
                                             message,
                                             sizeof(message)) ||
        decoded_count != 3u || decoded[0].int_value != 7u ||
        strcmp(decoded[1].text, input[1].text) != 0 ||
        decoded[2].int_value != 99u) {
        fprintf(stderr, "wide payload decode failed: %s\n", message);
        return 6;
    }

    unsigned char stored[PAGE_USABLE_SIZE];
    uint32_t stored_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(&wide,
                                               &encoded,
                                               stored,
                                               sizeof(stored),
                                               &stored_length)) {
        fprintf(stderr, "compact V2 envelope rejected wide logical payload\n");
        return 7;
    }
    if (stored_length >= encoded.length) {
        fprintf(stderr, "compact V2 envelope did not compact reserved VARCHAR bytes\n");
        return 8;
    }

    TinyDBRecordPayload reopened;
    if (!tinydb_row_envelope_decode(&wide,
                                    stored,
                                    stored_length,
                                    &reopened) ||
        reopened.length != encoded.length ||
        memcmp(reopened.bytes, encoded.bytes, encoded.length) != 0) {
        fprintf(stderr, "compact V2 envelope round-trip failed\n");
        return 9;
    }

    TinyDBRecord legacy;
    if (tinydb_record_payload_to_record(&wide,
                                        &reopened,
                                        &legacy,
                                        message,
                                        sizeof(message))) {
        fprintf(stderr, "legacy TinyDBRecord adapter accepted an oversized row\n");
        return 10;
    }

    TableSchema compact;
    memset(&compact, 0, sizeof(compact));
    snprintf(compact.name, sizeof(compact.name), "%s", "compact_metrics");
    compact.num_columns = 4u;
    init_column(&compact.columns[0], "id", COL_TYPE_INT, 0u, 4u);
    init_column(&compact.columns[1], "region", COL_TYPE_VARCHAR, 4u, 24u);
    init_column(&compact.columns[2], "count", COL_TYPE_INT, 28u, 4u);
    init_column(&compact.columns[3], "tag", COL_TYPE_VARCHAR, 32u, 40u);
    compact.row_size = 72u;

    TinyDBValue compact_values[4];
    memset(compact_values, 0, sizeof(compact_values));
    compact_values[0].type = COL_TYPE_INT;
    compact_values[0].int_value = 11u;
    compact_values[1].type = COL_TYPE_VARCHAR;
    snprintf(compact_values[1].text, sizeof(compact_values[1].text), "%s", "north");
    compact_values[2].type = COL_TYPE_INT;
    compact_values[2].int_value = 1234u;
    compact_values[3].type = COL_TYPE_VARCHAR;
    snprintf(compact_values[3].text, sizeof(compact_values[3].text), "%s", "blue");

    TinyDBRecordPayload compact_payload;
    if (!tinydb_record_payload_encode_values(&compact,
                                             compact_values,
                                             4u,
                                             &compact_payload,
                                             message,
                                             sizeof(message)) ||
        compact_payload.length != 72u ||
        tinydb_row_envelope_schema_fingerprint(&wide) ==
            tinydb_row_envelope_schema_fingerprint(&compact)) {
        fprintf(stderr, "independent schema layout was not preserved\n");
        return 11;
    }

    primary_key = 0u;
    if (!tinydb_record_payload_primary_key(&compact,
                                           &compact_payload,
                                           &primary_key,
                                           message,
                                           sizeof(message)) ||
        primary_key != 11u) {
        fprintf(stderr, "compact schema primary-key decode failed: %s\n", message);
        return 12;
    }

    printf("SCHEMA_SIZED_PAYLOAD_OK wide=%u legacy=%u stored=%u compact=%u key=%u\n",
           encoded.length,
           (unsigned)ROW_SIZE,
           stored_length,
           compact_payload.length,
           primary_key);
    return 0;
}
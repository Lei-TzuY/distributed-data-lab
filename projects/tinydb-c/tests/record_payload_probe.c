#include "record_payload.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void init_compact_schema(TableSchema* schema) {
    memset(schema, 0, sizeof(*schema));
    snprintf(schema->name, sizeof(schema->name), "compact");
    schema->num_columns = 3u;
    schema->row_size = 13u;

    snprintf(schema->columns[0].name, sizeof(schema->columns[0].name), "id");
    schema->columns[0].type = COL_TYPE_INT;
    schema->columns[0].offset = 0u;
    schema->columns[0].size = 4u;

    snprintf(schema->columns[1].name, sizeof(schema->columns[1].name), "name");
    schema->columns[1].type = COL_TYPE_VARCHAR;
    schema->columns[1].offset = 4u;
    schema->columns[1].size = 5u;

    snprintf(schema->columns[2].name, sizeof(schema->columns[2].name), "qty");
    schema->columns[2].type = COL_TYPE_INT;
    schema->columns[2].offset = 9u;
    schema->columns[2].size = 4u;
}

static void init_boundary_schema(TableSchema* schema) {
    memset(schema, 0, sizeof(*schema));
    snprintf(schema->name, sizeof(schema->name), "boundary");
    schema->num_columns = 3u;
    schema->row_size = ROW_SIZE;

    snprintf(schema->columns[0].name, sizeof(schema->columns[0].name), "id");
    schema->columns[0].type = COL_TYPE_INT;
    schema->columns[0].offset = 0u;
    schema->columns[0].size = 4u;

    snprintf(schema->columns[1].name, sizeof(schema->columns[1].name), "large");
    schema->columns[1].type = COL_TYPE_VARCHAR;
    schema->columns[1].offset = 4u;
    schema->columns[1].size = 256u;

    snprintf(schema->columns[2].name, sizeof(schema->columns[2].name), "tail");
    schema->columns[2].type = COL_TYPE_VARCHAR;
    schema->columns[2].offset = 260u;
    schema->columns[2].size = 33u;
}

static void fail(const char* message) {
    printf("FAIL: %s\n", message);
    failures++;
}

static void verify_compact_payload(void) {
    TableSchema schema;
    init_compact_schema(&schema);

    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(&schema, message, sizeof(message))) {
        fail(message);
        return;
    }

    TinyDBRecord record;
    memset(&record, 0xA5, sizeof(record));
    for (uint32_t i = 0; i < schema.row_size; i++) {
        record.bytes[i] = (unsigned char)(i + 1u);
    }

    TinyDBRecordPayload payload;
    if (!tinydb_record_payload_from_record(&schema,
                                           &record,
                                           &payload,
                                           message,
                                           sizeof(message))) {
        fail(message);
        return;
    }
    if (payload.length != 13u) fail("compact payload length was not schema-sized");
    if (memcmp(payload.bytes, record.bytes, 13u) != 0) fail("compact payload bytes changed");
    for (uint32_t i = 13u; i < ROW_SIZE; i++) {
        if (payload.bytes[i] != 0u) {
            fail("logical payload retained legacy fixed-slot tail bytes");
            break;
        }
    }

    unsigned char slot[ROW_SIZE];
    memset(slot, 0xCC, sizeof(slot));
    if (!tinydb_record_payload_pack_fixed_slot(&payload,
                                               slot,
                                               sizeof(slot),
                                               message,
                                               sizeof(message))) {
        fail(message);
        return;
    }
    if (memcmp(slot, record.bytes, 13u) != 0) fail("fixed-slot pack changed payload bytes");
    for (uint32_t i = 13u; i < ROW_SIZE; i++) {
        if (slot[i] != 0u) {
            fail("fixed-slot pack did not canonicalize padding to zero");
            break;
        }
    }

    memset(slot + 13u, 0x7E, ROW_SIZE - 13u);
    TinyDBRecordPayload unpacked;
    if (!tinydb_record_payload_unpack_fixed_slot(&schema,
                                                 slot,
                                                 sizeof(slot),
                                                 &unpacked,
                                                 message,
                                                 sizeof(message))) {
        fail(message);
        return;
    }
    if (unpacked.length != 13u || memcmp(unpacked.bytes, slot, 13u) != 0) {
        fail("fixed-slot unpack did not preserve the logical payload");
    }
    for (uint32_t i = 13u; i < ROW_SIZE; i++) {
        if (unpacked.bytes[i] != 0u) {
            fail("fixed-slot unpack leaked physical padding into logical payload");
            break;
        }
    }

    TinyDBRecord restored;
    memset(&restored, 0xDD, sizeof(restored));
    if (!tinydb_record_payload_to_record(&schema,
                                         &unpacked,
                                         &restored,
                                         message,
                                         sizeof(message))) {
        fail(message);
        return;
    }
    if (memcmp(restored.bytes, slot, 13u) != 0) fail("payload-to-record changed logical bytes");
    for (uint32_t i = 13u; i < ROW_SIZE; i++) {
        if (restored.bytes[i] != 0u) {
            fail("payload-to-record did not canonicalize unused record bytes");
            break;
        }
    }

    TinyDBRecordPayload wrong = payload;
    wrong.length = 12u;
    if (tinydb_record_payload_to_record(&schema,
                                        &wrong,
                                        &restored,
                                        message,
                                        sizeof(message))) {
        fail("payload length mismatch was accepted");
    }
    if (tinydb_record_payload_pack_fixed_slot(&payload,
                                              slot,
                                              ROW_SIZE - 1u,
                                              message,
                                              sizeof(message))) {
        fail("non-legacy fixed slot size was accepted");
    }
}

static void verify_boundary_payload(void) {
    TableSchema schema;
    init_boundary_schema(&schema);
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_schema_supports_records(&schema, message, sizeof(message))) {
        fail(message);
        return;
    }

    TinyDBRecord record;
    for (uint32_t i = 0; i < ROW_SIZE; i++) {
        record.bytes[i] = (unsigned char)(i & 0xFFu);
    }

    TinyDBRecordPayload payload;
    if (!tinydb_record_payload_from_record(&schema,
                                           &record,
                                           &payload,
                                           message,
                                           sizeof(message))) {
        fail(message);
        return;
    }
    if (payload.length != ROW_SIZE) fail("293-byte boundary payload length changed");

    unsigned char slot[ROW_SIZE];
    if (!tinydb_record_payload_pack_fixed_slot(&payload,
                                               slot,
                                               sizeof(slot),
                                               message,
                                               sizeof(message))) {
        fail(message);
        return;
    }
    if (memcmp(slot, record.bytes, ROW_SIZE) != 0) {
        fail("293-byte boundary payload did not round-trip exactly");
    }
}

int main(void) {
    verify_compact_payload();
    verify_boundary_payload();

    if (failures != 0) {
        printf("RECORD_PAYLOAD_FAIL %d\n", failures);
        return 1;
    }
    printf("RECORD_PAYLOAD_OK\n");
    return 0;
}

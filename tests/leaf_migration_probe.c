#include "leaf_format.h"
#include "leaf_migration.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMPACT_LENGTH 13u

static uint32_t read_native_u32(const unsigned char* bytes) {
    uint32_t value = 0u;
    memcpy(&value, bytes, sizeof(value));
    return value;
}

static uint32_t read_u32_le(const unsigned char* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static unsigned char* v1_cell(unsigned char page[PAGE_SIZE], uint32_t index) {
    return page + LEAF_NODE_HEADER_SIZE + index * LEAF_NODE_CELL_SIZE;
}

static const unsigned char* const_v1_cell(const unsigned char page[PAGE_SIZE],
                                          uint32_t index) {
    return page + LEAF_NODE_HEADER_SIZE + index * LEAF_NODE_CELL_SIZE;
}

static void make_compact_payload(uint32_t key,
                                 unsigned char output[COMPACT_LENGTH]) {
    memset(output, 0, COMPACT_LENGTH);
    memcpy(output, &key, sizeof(key));
    snprintf((char*)output + sizeof(key),
             COMPACT_LENGTH - sizeof(key),
             "r%07u",
             key);
}

static TableSchema make_compact_schema(void) {
    TableSchema schema;
    memset(&schema, 0, sizeof(schema));
    snprintf(schema.name, sizeof(schema.name), "%s", "docs");
    schema.num_columns = 2u;
    schema.row_size = COMPACT_LENGTH;

    snprintf(schema.columns[0].name,
             sizeof(schema.columns[0].name),
             "%s",
             "id");
    schema.columns[0].type = COL_TYPE_INT;
    schema.columns[0].offset = 0u;
    schema.columns[0].size = sizeof(uint32_t);

    snprintf(schema.columns[1].name,
             sizeof(schema.columns[1].name),
             "%s",
             "payload");
    schema.columns[1].type = COL_TYPE_VARCHAR;
    schema.columns[1].offset = sizeof(uint32_t);
    schema.columns[1].size = COMPACT_LENGTH - sizeof(uint32_t);
    return schema;
}

static void make_v1_page(unsigned char page[PAGE_SIZE],
                         uint32_t count,
                         uint32_t logical_length,
                         unsigned char trailer_marker) {
    memset(page, 0, PAGE_SIZE);
    memset(page + PAGE_USABLE_SIZE, trailer_marker, PAGE_CHECKSUM_SIZE);
    page[NODE_TYPE_OFFSET] = (unsigned char)NODE_LEAF;
    page[IS_ROOT_OFFSET] = 1u;

    uint32_t parent = 42u;
    uint32_t next_leaf = 77u;
    uint32_t prev_leaf = 66u;
    memcpy(page + PARENT_POINTER_OFFSET, &parent, sizeof(parent));
    memcpy(page + LEAF_NODE_NUM_CELLS_OFFSET, &count, sizeof(count));
    memcpy(page + LEAF_NODE_NEXT_LEAF_OFFSET, &next_leaf, sizeof(next_leaf));
    memcpy(page + LEAF_NODE_PREV_LEAF_OFFSET, &prev_leaf, sizeof(prev_leaf));

    for (uint32_t i = 0u; i < count; i++) {
        uint32_t key = i + 1u;
        unsigned char* cell = v1_cell(page, i);
        memcpy(cell + LEAF_NODE_KEY_OFFSET, &key, sizeof(key));
        memset(cell + LEAF_NODE_VALUE_OFFSET, 0xA5, ROW_SIZE);

        if (logical_length == COMPACT_LENGTH) {
            unsigned char payload[COMPACT_LENGTH];
            make_compact_payload(key, payload);
            memcpy(cell + LEAF_NODE_VALUE_OFFSET, payload, sizeof(payload));
        } else {
            for (uint32_t j = 0u; j < logical_length; j++) {
                cell[LEAF_NODE_VALUE_OFFSET + j] =
                    (unsigned char)((key * 17u + j) & 0xffu);
            }
        }
    }
}

static void canonicalize_compact_schema_page(unsigned char page[PAGE_SIZE],
                                             uint32_t count) {
    for (uint32_t i = 0u; i < count; i++) {
        memset(v1_cell(page, i) + LEAF_NODE_VALUE_OFFSET + COMPACT_LENGTH,
               0,
               ROW_SIZE - COMPACT_LENGTH);
    }
}

static bool trailer_is(const unsigned char page[PAGE_SIZE],
                       unsigned char marker) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static bool identity_matches_v2(const unsigned char page[PAGE_SIZE]) {
    return page[IS_ROOT_OFFSET] == 1u &&
           read_native_u32(page + PARENT_POINTER_OFFSET) == 42u &&
           read_u32_le(page + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) == 77u &&
           read_u32_le(page + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) == 66u;
}

static bool identity_matches_v1(const unsigned char page[PAGE_SIZE]) {
    return page[IS_ROOT_OFFSET] == 1u &&
           read_native_u32(page + PARENT_POINTER_OFFSET) == 42u &&
           read_native_u32(page + LEAF_NODE_NEXT_LEAF_OFFSET) == 77u &&
           read_native_u32(page + LEAF_NODE_PREV_LEAF_OFFSET) == 66u;
}

static bool compact_roundtrip(void) {
    unsigned char v1[PAGE_SIZE];
    unsigned char v2[PAGE_SIZE];
    unsigned char roundtrip[PAGE_SIZE];
    make_v1_page(v1, 5u, COMPACT_LENGTH, 0xC1u);
    memset(v2, 0xD2, sizeof(v2));
    memset(roundtrip, 0xE3, sizeof(roundtrip));

    if (tinydb_leaf_format_detect_page(v1, sizeof(v1)) !=
            TINYDB_LEAF_PAGE_FORMAT_FIXED_V1 ||
        !tinydb_leaf_migrate_v1_to_v2(v1,
                                      sizeof(v1),
                                      COMPACT_LENGTH,
                                      v2,
                                      sizeof(v2)) ||
        tinydb_leaf_format_detect_page(v2, sizeof(v2)) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        tinydb_slotted_leaf_v2_count(v2, sizeof(v2)) != 5u ||
        !identity_matches_v2(v2) ||
        !trailer_is(v1, 0xC1u) ||
        !trailer_is(v2, 0xD2u)) {
        return false;
    }

    for (uint32_t key = 1u; key <= 5u; key++) {
        unsigned char expected[COMPACT_LENGTH];
        unsigned char actual[COMPACT_LENGTH];
        uint16_t length = 0u;
        make_compact_payload(key, expected);
        if (!tinydb_slotted_leaf_v2_read(v2,
                                         sizeof(v2),
                                         key,
                                         actual,
                                         sizeof(actual),
                                         &length) ||
            length != COMPACT_LENGTH ||
            memcmp(actual, expected, sizeof(expected)) != 0) {
            return false;
        }
    }

    if (!tinydb_leaf_v2_can_downgrade_to_v1(v2, sizeof(v2)) ||
        !tinydb_leaf_migrate_v2_to_v1(v2,
                                      sizeof(v2),
                                      roundtrip,
                                      sizeof(roundtrip)) ||
        tinydb_leaf_format_detect_page(roundtrip, sizeof(roundtrip)) !=
            TINYDB_LEAF_PAGE_FORMAT_FIXED_V1 ||
        !identity_matches_v1(roundtrip) ||
        !trailer_is(roundtrip, 0xE3u)) {
        return false;
    }

    uint32_t count = read_native_u32(roundtrip + LEAF_NODE_NUM_CELLS_OFFSET);
    if (count != 5u) return false;
    for (uint32_t i = 0u; i < count; i++) {
        const unsigned char* cell = const_v1_cell(roundtrip, i);
        uint32_t key = read_native_u32(cell + LEAF_NODE_KEY_OFFSET);
        unsigned char expected[COMPACT_LENGTH];
        make_compact_payload(key, expected);
        if (key != i + 1u ||
            memcmp(cell + LEAF_NODE_VALUE_OFFSET,
                   expected,
                   COMPACT_LENGTH) != 0) {
            return false;
        }
        for (uint32_t j = COMPACT_LENGTH; j < ROW_SIZE; j++) {
            if (cell[LEAF_NODE_VALUE_OFFSET + j] != 0u) return false;
        }
    }
    return true;
}

static bool compact_row_primitive_is_atomic_and_decodable(void) {
    unsigned char source[PAGE_SIZE];
    unsigned char envelope[PAGE_SIZE];
    unsigned char expected[PAGE_SIZE];
    TableSchema schema = make_compact_schema();
    TinyDBRecordPayload decoded;
    uint32_t envelope_length = 0u;

    make_v1_page(source, 1u, COMPACT_LENGTH, 0x71u);
    canonicalize_compact_schema_page(source, 1u);
    memset(envelope, 0xA7, sizeof(envelope));
    if (!tinydb_fixed_v1_row_encode_compact_v2(
            &schema,
            1u,
            v1_cell(source, 0u) + LEAF_NODE_VALUE_OFFSET,
            ROW_SIZE,
            envelope,
            sizeof(envelope),
            &envelope_length) ||
        envelope_length == 0u ||
        !tinydb_row_envelope_decode(&schema,
                                    envelope,
                                    envelope_length,
                                    &decoded) ||
        decoded.length != COMPACT_LENGTH ||
        memcmp(decoded.bytes,
               v1_cell(source, 0u) + LEAF_NODE_VALUE_OFFSET,
               COMPACT_LENGTH) != 0) {
        return false;
    }

    memset(envelope, 0xB8, sizeof(envelope));
    memcpy(expected, envelope, sizeof(expected));
    envelope_length = 0xDEADBEEFu;
    if (tinydb_fixed_v1_row_encode_compact_v2(
            &schema,
            1u,
            v1_cell(source, 0u) + LEAF_NODE_VALUE_OFFSET,
            ROW_SIZE,
            envelope,
            1u,
            &envelope_length) ||
        envelope_length != 0xDEADBEEFu ||
        memcmp(envelope, expected, sizeof(envelope)) != 0) {
        return false;
    }

    memset(envelope, 0xC9, sizeof(envelope));
    memcpy(expected, envelope, sizeof(expected));
    envelope_length = 0xCAFEBABEu;
    return !tinydb_fixed_v1_row_encode_compact_v2(
               &schema,
               999u,
               v1_cell(source, 0u) + LEAF_NODE_VALUE_OFFSET,
               ROW_SIZE,
               envelope,
               sizeof(envelope),
               &envelope_length) &&
           envelope_length == 0xCAFEBABEu &&
           memcmp(envelope, expected, sizeof(envelope)) == 0;
}

static bool compact_schema_rejects_key_payload_drift(void) {
    unsigned char source[PAGE_SIZE];
    unsigned char destination[PAGE_SIZE];
    unsigned char expected[PAGE_SIZE];
    TableSchema schema = make_compact_schema();

    make_v1_page(source, 2u, COMPACT_LENGTH, 0x61u);
    canonicalize_compact_schema_page(source, 2u);
    memset(destination, 0x72, sizeof(destination));
    if (!tinydb_leaf_migrate_v1_to_compact_v2(source,
                                               sizeof(source),
                                               &schema,
                                               destination,
                                               sizeof(destination)) ||
        tinydb_leaf_format_detect_page(destination, sizeof(destination)) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        return false;
    }

    make_v1_page(source, 2u, COMPACT_LENGTH, 0x61u);
    canonicalize_compact_schema_page(source, 2u);
    uint32_t divergent_payload_key = 999u;
    memcpy(v1_cell(source, 1u) + LEAF_NODE_VALUE_OFFSET,
           &divergent_payload_key,
           sizeof(divergent_payload_key));
    memset(destination, 0x83, sizeof(destination));
    memcpy(expected, destination, sizeof(expected));

    return !tinydb_leaf_migrate_v1_to_compact_v2(source,
                                                  sizeof(source),
                                                  &schema,
                                                  destination,
                                                  sizeof(destination)) &&
           memcmp(destination, expected, sizeof(destination)) == 0;
}

static bool compact_schema_rejects_noncanonical_varchar_padding(void) {
    unsigned char source[PAGE_SIZE];
    unsigned char destination[PAGE_SIZE];
    unsigned char expected[PAGE_SIZE];
    TableSchema schema = make_compact_schema();

    make_v1_page(source, 2u, COMPACT_LENGTH, 0x51u);
    canonicalize_compact_schema_page(source, 2u);
    unsigned char* field = v1_cell(source, 0u) + LEAF_NODE_VALUE_OFFSET +
                           schema.columns[1].offset;
    field[2] = '\0';
    field[3] = 0x7Fu;

    memset(destination, 0x64, sizeof(destination));
    memcpy(expected, destination, sizeof(expected));
    return !tinydb_leaf_migrate_v1_to_compact_v2(source,
                                                  sizeof(source),
                                                  &schema,
                                                  destination,
                                                  sizeof(destination)) &&
           memcmp(destination, expected, sizeof(destination)) == 0;
}

static bool compact_schema_rejects_noncanonical_unused_tail(void) {
    unsigned char source[PAGE_SIZE];
    unsigned char destination[PAGE_SIZE];
    unsigned char expected[PAGE_SIZE];
    TableSchema schema = make_compact_schema();

    make_v1_page(source, 2u, COMPACT_LENGTH, 0x41u);
    canonicalize_compact_schema_page(source, 2u);
    v1_cell(source, 0u)[LEAF_NODE_VALUE_OFFSET + schema.row_size] = 0x5Cu;

    memset(destination, 0x54, sizeof(destination));
    memcpy(expected, destination, sizeof(expected));
    return !tinydb_leaf_migrate_v1_to_compact_v2(source,
                                                  sizeof(source),
                                                  &schema,
                                                  destination,
                                                  sizeof(destination)) &&
           memcmp(destination, expected, sizeof(destination)) == 0;
}

static bool full_v1_upgrade(void) {
    unsigned char v1[PAGE_SIZE];
    unsigned char v2[PAGE_SIZE];
    unsigned char back[PAGE_SIZE];
    make_v1_page(v1, LEAF_NODE_MAX_CELLS, ROW_SIZE, 0x11u);
    memset(v2, 0x22, sizeof(v2));
    memset(back, 0x33, sizeof(back));

    return tinydb_leaf_migrate_v1_to_v2(v1,
                                        sizeof(v1),
                                        ROW_SIZE,
                                        v2,
                                        sizeof(v2)) &&
           tinydb_slotted_leaf_v2_count(v2, sizeof(v2)) ==
               LEAF_NODE_MAX_CELLS &&
           tinydb_leaf_v2_can_downgrade_to_v1(v2, sizeof(v2)) &&
           tinydb_leaf_migrate_v2_to_v1(v2,
                                        sizeof(v2),
                                        back,
                                        sizeof(back)) &&
           read_native_u32(back + LEAF_NODE_NUM_CELLS_OFFSET) ==
               LEAF_NODE_MAX_CELLS &&
           trailer_is(v2, 0x22u) &&
           trailer_is(back, 0x33u);
}

static bool rejected_downgrades_leave_destination_unchanged(void) {
    unsigned char destination[PAGE_SIZE];
    unsigned char expected[PAGE_SIZE];
    unsigned char v2[PAGE_SIZE];
    unsigned char large_value[400];
    memset(large_value, 0x5Au, sizeof(large_value));

    memset(v2, 0, sizeof(v2));
    if (!tinydb_slotted_leaf_v2_init(v2, sizeof(v2)) ||
        !tinydb_slotted_leaf_v2_insert(v2,
                                       sizeof(v2),
                                       1u,
                                       large_value,
                                       (uint16_t)sizeof(large_value))) {
        return false;
    }
    memset(destination, 0x7Bu, sizeof(destination));
    memcpy(expected, destination, sizeof(expected));
    if (tinydb_leaf_v2_can_downgrade_to_v1(v2, sizeof(v2)) ||
        tinydb_leaf_migrate_v2_to_v1(v2,
                                     sizeof(v2),
                                     destination,
                                     sizeof(destination)) ||
        memcmp(destination, expected, sizeof(destination)) != 0) {
        return false;
    }

    memset(v2, 0, sizeof(v2));
    if (!tinydb_slotted_leaf_v2_init(v2, sizeof(v2))) return false;
    unsigned char one = 0x1u;
    for (uint32_t key = 1u; key <= LEAF_NODE_MAX_CELLS + 1u; key++) {
        if (!tinydb_slotted_leaf_v2_insert(v2,
                                           sizeof(v2),
                                           key,
                                           &one,
                                           1u)) {
            return false;
        }
    }
    memset(destination, 0x6Cu, sizeof(destination));
    memcpy(expected, destination, sizeof(expected));
    if (tinydb_leaf_v2_can_downgrade_to_v1(v2, sizeof(v2)) ||
        tinydb_leaf_migrate_v2_to_v1(v2,
                                     sizeof(v2),
                                     destination,
                                     sizeof(destination)) ||
        memcmp(destination, expected, sizeof(destination)) != 0) {
        return false;
    }
    return true;
}

static bool rejected_upgrades_leave_destination_unchanged(void) {
    unsigned char v1[PAGE_SIZE];
    unsigned char destination[PAGE_SIZE];
    unsigned char expected[PAGE_SIZE];
    make_v1_page(v1, 2u, COMPACT_LENGTH, 0x91u);

    uint32_t bad_key = 1u;
    memcpy(v1_cell(v1, 1u) + LEAF_NODE_KEY_OFFSET,
           &bad_key,
           sizeof(bad_key));
    memset(destination, 0x8Du, sizeof(destination));
    memcpy(expected, destination, sizeof(expected));

    if (tinydb_leaf_migrate_v1_to_v2(v1,
                                     sizeof(v1),
                                     COMPACT_LENGTH,
                                     destination,
                                     sizeof(destination)) ||
        memcmp(destination, expected, sizeof(destination)) != 0) {
        return false;
    }

    make_v1_page(v1, 1u, COMPACT_LENGTH, 0x91u);
    return !tinydb_leaf_migrate_v1_to_v2(v1,
                                         sizeof(v1),
                                         0u,
                                         destination,
                                         sizeof(destination)) &&
           !tinydb_leaf_migrate_v1_to_v2(v1,
                                         sizeof(v1),
                                         ROW_SIZE + 1u,
                                         destination,
                                         sizeof(destination)) &&
           !tinydb_leaf_migrate_v1_to_v2(v1,
                                         sizeof(v1),
                                         COMPACT_LENGTH,
                                         v1,
                                         sizeof(v1));
}

int main(void) {
    if (!compact_roundtrip()) {
        fprintf(stderr, "compact V1/V2/V1 roundtrip failed\n");
        return EXIT_FAILURE;
    }
    if (!compact_row_primitive_is_atomic_and_decodable()) {
        fprintf(stderr, "fixed-row compact migration primitive was not atomic/decodable\n");
        return EXIT_FAILURE;
    }
    if (!compact_schema_rejects_key_payload_drift()) {
        fprintf(stderr, "compact schema migration accepted divergent key/payload identity\n");
        return EXIT_FAILURE;
    }
    if (!compact_schema_rejects_noncanonical_varchar_padding()) {
        fprintf(stderr, "compact schema migration silently canonicalized varchar padding\n");
        return EXIT_FAILURE;
    }
    if (!compact_schema_rejects_noncanonical_unused_tail()) {
        fprintf(stderr, "compact schema migration silently discarded nonzero fixed row tail\n");
        return EXIT_FAILURE;
    }
    if (!full_v1_upgrade()) {
        fprintf(stderr, "full V1 page migration failed\n");
        return EXIT_FAILURE;
    }
    if (!rejected_downgrades_leave_destination_unchanged()) {
        fprintf(stderr, "unsafe V2 downgrade was not rejected atomically\n");
        return EXIT_FAILURE;
    }
    if (!rejected_upgrades_leave_destination_unchanged()) {
        fprintf(stderr, "unsafe V1 upgrade was not rejected atomically\n");
        return EXIT_FAILURE;
    }

    printf("LEAF_MIGRATION_OK compact_roundtrip=yes compact_row_primitive=yes compact_key_payload_identity=yes compact_varchar_padding=yes compact_unused_tail=yes full_v1=yes oversize_downgrade_rejected=yes overcount_downgrade_rejected=yes atomic_failure=yes checksum_reserved=yes\n");
    return EXIT_SUCCESS;
}

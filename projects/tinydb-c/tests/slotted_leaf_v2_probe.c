#include "slotted_leaf_v2.h"
#include "slotted_leaf_v2_split.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void fill_pattern(unsigned char* bytes,
                         size_t length,
                         unsigned char seed) {
    for (size_t i = 0u; i < length; i++) {
        bytes[i] = (unsigned char)(seed + (unsigned char)(i % 23u));
    }
}

static uint32_t read_u32_le_probe(const unsigned char* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void write_u32_le_probe(unsigned char* bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    bytes[2] = (unsigned char)((value >> 16) & 0xffu);
    bytes[3] = (unsigned char)((value >> 24) & 0xffu);
}

static bool expect_value(const unsigned char page[PAGE_SIZE],
                         uint32_t key,
                         const unsigned char* expected,
                         uint16_t expected_length) {
    unsigned char output[1600];
    uint16_t actual_length = 0u;
    memset(output, 0, sizeof(output));
    if (expected_length > sizeof(output) ||
        !tinydb_slotted_leaf_v2_read(page,
                                     PAGE_SIZE,
                                     key,
                                     output,
                                     sizeof(output),
                                     &actual_length)) {
        fprintf(stderr, "unable to read key %u\n", key);
        return false;
    }
    if (actual_length != expected_length ||
        memcmp(output, expected, expected_length) != 0) {
        fprintf(stderr, "payload mismatch for key %u\n", key);
        return false;
    }
    return true;
}

static bool checksum_trailer_untouched(const unsigned char page[PAGE_SIZE],
                                       unsigned char marker) {
    for (uint32_t i = PAGE_USABLE_SIZE; i < PAGE_SIZE; i++) {
        if (page[i] != marker) return false;
    }
    return true;
}

static bool exercise_byte_balanced_split(void) {
    unsigned char left[PAGE_SIZE];
    unsigned char right[PAGE_SIZE];
    memset(left, 0xA5, sizeof(left));
    memset(right, 0x5A, sizeof(right));
    if (!tinydb_slotted_leaf_v2_init(left, sizeof(left))) return false;

    left[IS_ROOT_OFFSET] = 0u;
    write_u32_le_probe(left + PARENT_POINTER_OFFSET, 77u);
    write_u32_le_probe(left + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, 40u);
    write_u32_le_probe(left + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET, 60u);

    unsigned char small1[100];
    unsigned char small2[120];
    unsigned char large1[1100];
    unsigned char large2[1300];
    unsigned char small3[140];
    fill_pattern(small1, sizeof(small1), 0x11u);
    fill_pattern(small2, sizeof(small2), 0x22u);
    fill_pattern(large1, sizeof(large1), 0x33u);
    fill_pattern(large2, sizeof(large2), 0x44u);
    fill_pattern(small3, sizeof(small3), 0x55u);

    if (!tinydb_slotted_leaf_v2_insert(left,
                                       sizeof(left),
                                       10u,
                                       small1,
                                       (uint16_t)sizeof(small1)) ||
        !tinydb_slotted_leaf_v2_insert(left,
                                       sizeof(left),
                                       20u,
                                       small2,
                                       (uint16_t)sizeof(small2)) ||
        !tinydb_slotted_leaf_v2_insert(left,
                                       sizeof(left),
                                       30u,
                                       large1,
                                       (uint16_t)sizeof(large1)) ||
        !tinydb_slotted_leaf_v2_insert(left,
                                       sizeof(left),
                                       40u,
                                       large2,
                                       (uint16_t)sizeof(large2)) ||
        !tinydb_slotted_leaf_v2_insert(left,
                                       sizeof(left),
                                       50u,
                                       small3,
                                       (uint16_t)sizeof(small3))) {
        return false;
    }

    uint16_t chosen = tinydb_slotted_leaf_v2_choose_split_index(left,
                                                                 sizeof(left));
    if (chosen != 3u) {
        fprintf(stderr, "byte-balanced split selected row-count midpoint %u\n",
                (unsigned)chosen);
        return false;
    }

    uint16_t actual_split = 0u;
    if (!tinydb_slotted_leaf_v2_split_nonroot(left,
                                               sizeof(left),
                                               50u,
                                               right,
                                               sizeof(right),
                                               51u,
                                               &actual_split) ||
        actual_split != chosen ||
        !tinydb_slotted_leaf_v2_validate(left, sizeof(left)) ||
        !tinydb_slotted_leaf_v2_validate(right, sizeof(right)) ||
        tinydb_slotted_leaf_v2_count(left, sizeof(left)) != 3u ||
        tinydb_slotted_leaf_v2_count(right, sizeof(right)) != 2u ||
        read_u32_le_probe(left + PARENT_POINTER_OFFSET) != 77u ||
        read_u32_le_probe(right + PARENT_POINTER_OFFSET) != 77u ||
        read_u32_le_probe(left + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) != 40u ||
        read_u32_le_probe(left + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) != 51u ||
        read_u32_le_probe(right + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET) != 50u ||
        read_u32_le_probe(right + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET) != 60u ||
        !expect_value(left, 10u, small1, (uint16_t)sizeof(small1)) ||
        !expect_value(left, 20u, small2, (uint16_t)sizeof(small2)) ||
        !expect_value(left, 30u, large1, (uint16_t)sizeof(large1)) ||
        !expect_value(right, 40u, large2, (uint16_t)sizeof(large2)) ||
        !expect_value(right, 50u, small3, (uint16_t)sizeof(small3)) ||
        !checksum_trailer_untouched(left, 0xA5u) ||
        !checksum_trailer_untouched(right, 0x5Au)) {
        fprintf(stderr, "byte-balanced split redistribution/identity failed\n");
        return false;
    }

    unsigned char root_source[PAGE_SIZE];
    unsigned char root_destination[PAGE_SIZE];
    unsigned char root_before[PAGE_SIZE];
    unsigned char destination_before[PAGE_SIZE];
    memset(root_source, 0x17, sizeof(root_source));
    memset(root_destination, 0x29, sizeof(root_destination));
    if (!tinydb_slotted_leaf_v2_init(root_source, sizeof(root_source)) ||
        !tinydb_slotted_leaf_v2_insert(root_source,
                                       sizeof(root_source),
                                       1u,
                                       small1,
                                       (uint16_t)sizeof(small1)) ||
        !tinydb_slotted_leaf_v2_insert(root_source,
                                       sizeof(root_source),
                                       2u,
                                       small2,
                                       (uint16_t)sizeof(small2))) {
        return false;
    }
    root_source[IS_ROOT_OFFSET] = 1u;
    memcpy(root_before, root_source, sizeof(root_before));
    memcpy(destination_before, root_destination, sizeof(destination_before));
    if (tinydb_slotted_leaf_v2_split_nonroot(root_source,
                                              sizeof(root_source),
                                              0u,
                                              root_destination,
                                              sizeof(root_destination),
                                              1u,
                                              NULL) ||
        memcmp(root_source, root_before, sizeof(root_source)) != 0 ||
        memcmp(root_destination,
               destination_before,
               sizeof(root_destination)) != 0) {
        fprintf(stderr, "failed root split modified caller pages\n");
        return false;
    }

    return true;
}

int main(void) {
    unsigned char page[PAGE_SIZE];
    memset(page, 0xCC, sizeof(page));
    if (!tinydb_slotted_leaf_v2_init(page, sizeof(page)) ||
        !tinydb_slotted_leaf_v2_validate(page, sizeof(page)) ||
        tinydb_slotted_leaf_v2_count(page, sizeof(page)) != 0u ||
        tinydb_slotted_leaf_v2_free_bytes(page, sizeof(page)) !=
            PAGE_USABLE_SIZE - TINYDB_SLOTTED_V2_HEADER_SIZE ||
        !checksum_trailer_untouched(page, 0xCCu)) {
        fprintf(stderr, "V2 initialization geometry is invalid\n");
        return EXIT_FAILURE;
    }

    unsigned char value10[600];
    unsigned char value20[900];
    unsigned char value20_grown[1200];
    unsigned char value25[1500];
    const unsigned char value30[] = "thirty";
    fill_pattern(value10, sizeof(value10), 0x10u);
    fill_pattern(value20, sizeof(value20), 0x20u);
    fill_pattern(value20_grown, sizeof(value20_grown), 0x30u);
    fill_pattern(value25, sizeof(value25), 0x40u);

    if (!tinydb_slotted_leaf_v2_insert(page,
                                       sizeof(page),
                                       30u,
                                       value30,
                                       (uint16_t)(sizeof(value30) - 1u)) ||
        !tinydb_slotted_leaf_v2_insert(page,
                                       sizeof(page),
                                       10u,
                                       value10,
                                       (uint16_t)sizeof(value10)) ||
        !tinydb_slotted_leaf_v2_insert(page,
                                       sizeof(page),
                                       20u,
                                       value20,
                                       (uint16_t)sizeof(value20)) ||
        tinydb_slotted_leaf_v2_insert(page,
                                      sizeof(page),
                                      20u,
                                      value20,
                                      (uint16_t)sizeof(value20))) {
        fprintf(stderr, "V2 sorted insertion or duplicate rejection failed\n");
        return EXIT_FAILURE;
    }

    TinyDBSlottedLeafV2Slot slot;
    uint16_t slot_index = 0u;
    if (tinydb_slotted_leaf_v2_count(page, sizeof(page)) != 3u ||
        !tinydb_slotted_leaf_v2_find(page,
                                     sizeof(page),
                                     10u,
                                     &slot,
                                     &slot_index) ||
        slot_index != 0u ||
        !tinydb_slotted_leaf_v2_find(page,
                                     sizeof(page),
                                     20u,
                                     &slot,
                                     &slot_index) ||
        slot_index != 1u ||
        !tinydb_slotted_leaf_v2_find(page,
                                     sizeof(page),
                                     30u,
                                     &slot,
                                     &slot_index) ||
        slot_index != 2u ||
        !expect_value(page, 10u, value10, (uint16_t)sizeof(value10)) ||
        !expect_value(page, 20u, value20, (uint16_t)sizeof(value20)) ||
        !expect_value(page,
                      30u,
                      value30,
                      (uint16_t)(sizeof(value30) - 1u))) {
        fprintf(stderr, "V2 sorted directory/readback failed\n");
        return EXIT_FAILURE;
    }

    uint32_t free_before_grow =
        tinydb_slotted_leaf_v2_free_bytes(page, sizeof(page));
    if (!tinydb_slotted_leaf_v2_update(page,
                                       sizeof(page),
                                       20u,
                                       value20_grown,
                                       (uint16_t)sizeof(value20_grown)) ||
        !expect_value(page,
                      20u,
                      value20_grown,
                      (uint16_t)sizeof(value20_grown)) ||
        tinydb_slotted_leaf_v2_free_bytes(page, sizeof(page)) !=
            free_before_grow - (sizeof(value20_grown) - sizeof(value20))) {
        fprintf(stderr, "V2 grow update failed\n");
        return EXIT_FAILURE;
    }

    uint32_t free_before_delete =
        tinydb_slotted_leaf_v2_free_bytes(page, sizeof(page));
    if (!tinydb_slotted_leaf_v2_delete(page, sizeof(page), 10u) ||
        tinydb_slotted_leaf_v2_find(page,
                                    sizeof(page),
                                    10u,
                                    NULL,
                                    NULL) ||
        tinydb_slotted_leaf_v2_count(page, sizeof(page)) != 2u ||
        tinydb_slotted_leaf_v2_free_bytes(page, sizeof(page)) !=
            free_before_delete + sizeof(value10) + TINYDB_SLOTTED_V2_SLOT_SIZE) {
        fprintf(stderr, "V2 delete/free-space recovery failed\n");
        return EXIT_FAILURE;
    }

    if (!tinydb_slotted_leaf_v2_insert(page,
                                       sizeof(page),
                                       25u,
                                       value25,
                                       (uint16_t)sizeof(value25)) ||
        !expect_value(page, 25u, value25, (uint16_t)sizeof(value25)) ||
        !tinydb_slotted_leaf_v2_compact(page, sizeof(page)) ||
        !tinydb_slotted_leaf_v2_validate(page, sizeof(page)) ||
        !expect_value(page,
                      20u,
                      value20_grown,
                      (uint16_t)sizeof(value20_grown)) ||
        !expect_value(page, 25u, value25, (uint16_t)sizeof(value25)) ||
        !expect_value(page,
                      30u,
                      value30,
                      (uint16_t)(sizeof(value30) - 1u)) ||
        !checksum_trailer_untouched(page, 0xCCu)) {
        fprintf(stderr, "V2 compaction or checksum isolation failed\n");
        return EXIT_FAILURE;
    }

    unsigned char corrupt[PAGE_SIZE];
    memcpy(corrupt, page, sizeof(corrupt));
    corrupt[TINYDB_SLOTTED_V2_MAGIC_OFFSET] ^= 0x80u;
    if (tinydb_slotted_leaf_v2_validate(corrupt, sizeof(corrupt))) {
        fprintf(stderr, "V2 corrupted magic was accepted\n");
        return EXIT_FAILURE;
    }

    unsigned char full_page[PAGE_SIZE];
    memset(full_page, 0x5Au, sizeof(full_page));
    if (!tinydb_slotted_leaf_v2_init(full_page, sizeof(full_page))) {
        fprintf(stderr, "V2 exact-capacity page initialization failed\n");
        return EXIT_FAILURE;
    }
    uint32_t maximum_value = PAGE_USABLE_SIZE -
                             TINYDB_SLOTTED_V2_HEADER_SIZE -
                             TINYDB_SLOTTED_V2_SLOT_SIZE;
    unsigned char* maximum_payload =
        (unsigned char*)malloc(maximum_value);
    if (maximum_payload == NULL) return EXIT_FAILURE;
    fill_pattern(maximum_payload, maximum_value, 0x51u);
    bool exact_fit = tinydb_slotted_leaf_v2_insert(
        full_page,
        sizeof(full_page),
        7u,
        maximum_payload,
        (uint16_t)maximum_value);
    bool no_extra_space = !tinydb_slotted_leaf_v2_insert(
        full_page,
        sizeof(full_page),
        8u,
        value30,
        (uint16_t)(sizeof(value30) - 1u));
    free(maximum_payload);

    if (!exact_fit || !no_extra_space ||
        tinydb_slotted_leaf_v2_free_bytes(full_page, sizeof(full_page)) != 0u ||
        !tinydb_slotted_leaf_v2_validate(full_page, sizeof(full_page)) ||
        !checksum_trailer_untouched(full_page, 0x5Au)) {
        fprintf(stderr, "V2 exact-capacity boundary failed\n");
        return EXIT_FAILURE;
    }

    if (!exercise_byte_balanced_split()) {
        fprintf(stderr, "V2 byte-balanced split regression failed\n");
        return EXIT_FAILURE;
    }

    printf("SLOTTED_LEAF_V2_OK variable_gt_293=yes sorted=yes update=yes delete=yes compact=yes checksum_reserved=yes exact_fit=yes\n");
    return EXIT_SUCCESS;
}

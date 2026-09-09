#include "leaf_migration.h"

#include "leaf_format.h"
#include "row_envelope.h"
#include "slotted_leaf_v2.h"

#include <string.h>

static bool full_page(const void* page, size_t capacity) {
    return page != NULL && capacity >= PAGE_SIZE;
}

static uint16_t read_u16_le(const unsigned char* bytes) {
    return (uint16_t)((uint16_t)bytes[0] |
                      ((uint16_t)bytes[1] << 8));
}

static uint32_t read_u32_le(const unsigned char* bytes) {
    return (uint32_t)bytes[0] |
           ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) |
           ((uint32_t)bytes[3] << 24);
}

static void write_u32_le(unsigned char* bytes, uint32_t value) {
    bytes[0] = (unsigned char)(value & 0xffu);
    bytes[1] = (unsigned char)((value >> 8) & 0xffu);
    bytes[2] = (unsigned char)((value >> 16) & 0xffu);
    bytes[3] = (unsigned char)((value >> 24) & 0xffu);
}

static const unsigned char* v1_cell(const void* page, uint32_t index) {
    return (const unsigned char*)page + LEAF_NODE_HEADER_SIZE +
           index * LEAF_NODE_CELL_SIZE;
}

static unsigned char* mutable_v1_cell(void* page, uint32_t index) {
    return (unsigned char*)page + LEAF_NODE_HEADER_SIZE +
           index * LEAF_NODE_CELL_SIZE;
}

static uint32_t v1_count(const void* page) {
    uint32_t count = 0u;
    memcpy(&count,
           (const unsigned char*)page + LEAF_NODE_NUM_CELLS_OFFSET,
           sizeof(count));
    return count;
}

static uint32_t v1_key(const void* page, uint32_t index) {
    uint32_t key = 0u;
    memcpy(&key,
           v1_cell(page, index) + LEAF_NODE_KEY_OFFSET,
           sizeof(key));
    return key;
}

static const unsigned char* v1_value(const void* page, uint32_t index) {
    return v1_cell(page, index) + LEAF_NODE_VALUE_OFFSET;
}

static TinyDBSlottedLeafV2Slot v2_slot(const void* page, uint16_t index) {
    const unsigned char* bytes = (const unsigned char*)page +
        TINYDB_SLOTTED_V2_HEADER_SIZE +
        (uint32_t)index * TINYDB_SLOTTED_V2_SLOT_SIZE;
    TinyDBSlottedLeafV2Slot slot;
    slot.key = read_u32_le(bytes + TINYDB_SLOTTED_V2_SLOT_KEY_OFFSET);
    slot.value_offset = read_u16_le(bytes + TINYDB_SLOTTED_V2_SLOT_VALUE_OFFSET);
    slot.value_length = read_u16_le(bytes + TINYDB_SLOTTED_V2_SLOT_LENGTH_OFFSET);
    return slot;
}

static uint32_t v2_next_leaf(const void* page) {
    return read_u32_le((const unsigned char*)page +
                       TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET);
}

static uint32_t v2_prev_leaf(const void* page) {
    return read_u32_le((const unsigned char*)page +
                       TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET);
}

static bool v1_keys_strictly_increasing(const void* source) {
    uint32_t count = v1_count(source);
    uint32_t previous = 0u;
    for (uint32_t i = 0u; i < count; i++) {
        uint32_t key = v1_key(source, i);
        if (i > 0u && key <= previous) return false;
        previous = key;
    }
    return true;
}

static bool fixed_payload_key_matches(const TableSchema* schema,
                                      uint32_t cell_key,
                                      const unsigned char* payload) {
    if (schema == NULL || payload == NULL || schema->num_columns == 0u ||
        schema->columns[0].type != COL_TYPE_INT ||
        schema->columns[0].offset > schema->row_size ||
        schema->columns[0].size != sizeof(uint32_t) ||
        schema->columns[0].offset + sizeof(uint32_t) > schema->row_size) {
        return false;
    }

    uint32_t payload_key = 0u;
    memcpy(&payload_key,
           payload + schema->columns[0].offset,
           sizeof(payload_key));
    return payload_key == cell_key;
}

static bool fixed_payload_is_canonical_for_schema(const TableSchema* schema,
                                                   const unsigned char* payload) {
    if (schema == NULL || payload == NULL) return false;

    for (uint32_t i = 0u; i < schema->num_columns; i++) {
        const TableColumn* column = &schema->columns[i];
        if (column->offset > schema->row_size || column->size == 0u ||
            column->size > schema->row_size - column->offset) {
            return false;
        }
        if (column->type == COL_TYPE_INT) {
            if (column->size != sizeof(uint32_t)) return false;
            continue;
        }
        if (column->type != COL_TYPE_VARCHAR) return false;

        const unsigned char* field = payload + column->offset;
        uint32_t terminator = column->size;
        for (uint32_t j = 0u; j < column->size; j++) {
            if (field[j] == '\0') {
                terminator = j;
                break;
            }
        }
        if (terminator == column->size) return false;
        for (uint32_t j = terminator + 1u; j < column->size; j++) {
            if (field[j] != 0u) return false;
        }
    }

    for (uint32_t i = schema->row_size; i < ROW_SIZE; i++) {
        if (payload[i] != 0u) return false;
    }
    return true;
}

static bool compact_envelope_roundtrips(const TableSchema* schema,
                                        const unsigned char* envelope,
                                        uint32_t envelope_length) {
    if (schema == NULL || envelope == NULL || envelope_length == 0u) {
        return false;
    }

    TinyDBRecordPayload decoded;
    if (!tinydb_row_envelope_decode(schema,
                                    envelope,
                                    envelope_length,
                                    &decoded)) {
        return false;
    }

    unsigned char canonical[PAGE_SIZE];
    uint32_t canonical_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                                &decoded,
                                                canonical,
                                                sizeof(canonical),
                                                &canonical_length)) {
        return false;
    }
    return canonical_length == envelope_length &&
           memcmp(canonical, envelope, envelope_length) == 0;
}

bool tinydb_fixed_v1_row_encode_compact_v2(const TableSchema* schema,
                                            uint32_t cell_key,
                                            const void* fixed_payload,
                                            size_t fixed_payload_capacity,
                                            void* destination,
                                            size_t destination_capacity,
                                            uint32_t* destination_length) {
    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    const unsigned char* payload = (const unsigned char*)fixed_payload;
    if (schema == NULL || payload == NULL || destination == NULL ||
        destination_length == NULL || fixed_payload_capacity < ROW_SIZE ||
        schema->row_size == 0u || schema->row_size > ROW_SIZE ||
        !tinydb_record_payload_schema_supported(schema,
                                                schema_message,
                                                sizeof(schema_message)) ||
        !fixed_payload_key_matches(schema, cell_key, payload) ||
        !fixed_payload_is_canonical_for_schema(schema, payload)) {
        return false;
    }

    TinyDBRecordPayload record_payload;
    memset(&record_payload, 0, sizeof(record_payload));
    record_payload.length = schema->row_size;
    memcpy(record_payload.bytes, payload, record_payload.length);

    unsigned char scratch[PAGE_SIZE];
    uint32_t scratch_length = 0u;
    if (!tinydb_row_envelope_encode_compact_v2(schema,
                                                &record_payload,
                                                scratch,
                                                sizeof(scratch),
                                                &scratch_length) ||
        scratch_length == 0u || scratch_length > UINT16_MAX ||
        scratch_length > destination_capacity ||
        !compact_envelope_roundtrips(schema, scratch, scratch_length)) {
        return false;
    }

    memcpy(destination, scratch, scratch_length);
    *destination_length = scratch_length;
    return true;
}

static void copy_v1_identity_to_v2(const void* source, void* destination) {
    const unsigned char* src = (const unsigned char*)source;
    unsigned char* dst = (unsigned char*)destination;

    dst[IS_ROOT_OFFSET] = src[IS_ROOT_OFFSET];
    memcpy(dst + PARENT_POINTER_OFFSET,
           src + PARENT_POINTER_OFFSET,
           PARENT_POINTER_SIZE);

    uint32_t next_leaf = 0u;
    uint32_t prev_leaf = 0u;
    memcpy(&next_leaf,
           src + LEAF_NODE_NEXT_LEAF_OFFSET,
           sizeof(next_leaf));
    memcpy(&prev_leaf,
           src + LEAF_NODE_PREV_LEAF_OFFSET,
           sizeof(prev_leaf));
    write_u32_le(dst + TINYDB_SLOTTED_V2_NEXT_LEAF_OFFSET, next_leaf);
    write_u32_le(dst + TINYDB_SLOTTED_V2_PREV_LEAF_OFFSET, prev_leaf);
}

static void init_v1_identity_from_v2(const void* source,
                                     void* destination,
                                     uint32_t count) {
    const unsigned char* src = (const unsigned char*)source;
    unsigned char* dst = (unsigned char*)destination;

    memset(dst, 0, PAGE_USABLE_SIZE);
    dst[NODE_TYPE_OFFSET] = (unsigned char)NODE_LEAF;
    dst[IS_ROOT_OFFSET] = src[IS_ROOT_OFFSET];
    memcpy(dst + PARENT_POINTER_OFFSET,
           src + PARENT_POINTER_OFFSET,
           PARENT_POINTER_SIZE);
    memcpy(dst + LEAF_NODE_NUM_CELLS_OFFSET, &count, sizeof(count));

    uint32_t next_leaf = v2_next_leaf(source);
    uint32_t prev_leaf = v2_prev_leaf(source);
    memcpy(dst + LEAF_NODE_NEXT_LEAF_OFFSET,
           &next_leaf,
           sizeof(next_leaf));
    memcpy(dst + LEAF_NODE_PREV_LEAF_OFFSET,
           &prev_leaf,
           sizeof(prev_leaf));
}

bool tinydb_leaf_migrate_v1_to_v2(const void* source,
                                   size_t source_capacity,
                                   uint32_t logical_value_length,
                                   void* destination,
                                   size_t destination_capacity) {
    if (!full_page(source, source_capacity) ||
        !full_page(destination, destination_capacity) ||
        source == destination ||
        logical_value_length == 0u ||
        logical_value_length > ROW_SIZE ||
        logical_value_length > UINT16_MAX ||
        tinydb_leaf_format_detect_page(source, source_capacity) !=
            TINYDB_LEAF_PAGE_FORMAT_FIXED_V1 ||
        !v1_keys_strictly_increasing(source)) {
        return false;
    }

    uint32_t count = v1_count(source);
    uint32_t required = TINYDB_SLOTTED_V2_HEADER_SIZE +
                        count * TINYDB_SLOTTED_V2_SLOT_SIZE +
                        count * logical_value_length;
    if (required > PAGE_USABLE_SIZE) return false;

    unsigned char scratch[PAGE_SIZE];
    memset(scratch, 0, sizeof(scratch));
    if (!tinydb_slotted_leaf_v2_init(scratch, sizeof(scratch))) return false;
    copy_v1_identity_to_v2(source, scratch);

    for (uint32_t i = 0u; i < count; i++) {
        if (!tinydb_slotted_leaf_v2_insert(
                scratch,
                sizeof(scratch),
                v1_key(source, i),
                v1_value(source, i),
                (uint16_t)logical_value_length)) {
            return false;
        }
    }

    if (tinydb_leaf_format_detect_page(scratch, sizeof(scratch)) !=
        TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        return false;
    }

    memcpy(destination, scratch, PAGE_USABLE_SIZE);
    return true;
}

bool tinydb_leaf_migrate_v1_to_compact_v2(const void* source,
                                           size_t source_capacity,
                                           const TableSchema* schema,
                                           void* destination,
                                           size_t destination_capacity) {
    char schema_message[TINYDB_RECORD_MESSAGE_MAX];
    if (!full_page(source, source_capacity) ||
        !full_page(destination, destination_capacity) ||
        source == destination || schema == NULL || schema->row_size == 0u ||
        schema->row_size > ROW_SIZE ||
        !tinydb_record_payload_schema_supported(schema,
                                                schema_message,
                                                sizeof(schema_message)) ||
        tinydb_leaf_format_detect_page(source, source_capacity) !=
            TINYDB_LEAF_PAGE_FORMAT_FIXED_V1 ||
        !v1_keys_strictly_increasing(source)) {
        return false;
    }

    unsigned char scratch[PAGE_SIZE];
    memset(scratch, 0, sizeof(scratch));
    if (!tinydb_slotted_leaf_v2_init(scratch, sizeof(scratch))) return false;
    copy_v1_identity_to_v2(source, scratch);

    uint32_t count = v1_count(source);
    for (uint32_t i = 0u; i < count; i++) {
        unsigned char envelope[PAGE_SIZE];
        uint32_t envelope_length = 0u;
        if (!tinydb_fixed_v1_row_encode_compact_v2(schema,
                                                    v1_key(source, i),
                                                    v1_value(source, i),
                                                    ROW_SIZE,
                                                    envelope,
                                                    sizeof(envelope),
                                                    &envelope_length) ||
            !tinydb_slotted_leaf_v2_insert(scratch,
                                           sizeof(scratch),
                                           v1_key(source, i),
                                           envelope,
                                           (uint16_t)envelope_length)) {
            return false;
        }
    }

    if (tinydb_leaf_format_detect_page(scratch, sizeof(scratch)) !=
        TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2 ||
        !tinydb_slotted_leaf_v2_validate(scratch, sizeof(scratch))) {
        return false;
    }

    memcpy(destination, scratch, PAGE_USABLE_SIZE);
    return true;
}

bool tinydb_leaf_v2_can_downgrade_to_v1(const void* source,
                                        size_t source_capacity) {
    if (!full_page(source, source_capacity) ||
        tinydb_leaf_format_detect_page(source, source_capacity) !=
            TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        return false;
    }

    uint16_t count = tinydb_slotted_leaf_v2_count(source, source_capacity);
    if (count > LEAF_NODE_MAX_CELLS) return false;

    for (uint16_t i = 0u; i < count; i++) {
        TinyDBSlottedLeafV2Slot slot = v2_slot(source, i);
        if (slot.value_length == 0u || slot.value_length > ROW_SIZE) {
            return false;
        }
    }
    return true;
}

bool tinydb_leaf_migrate_v2_to_v1(const void* source,
                                   size_t source_capacity,
                                   void* destination,
                                   size_t destination_capacity) {
    if (!full_page(destination, destination_capacity) ||
        source == destination ||
        !tinydb_leaf_v2_can_downgrade_to_v1(source, source_capacity)) {
        return false;
    }

    uint16_t count = tinydb_slotted_leaf_v2_count(source, source_capacity);
    unsigned char scratch[PAGE_SIZE];
    memset(scratch, 0, sizeof(scratch));
    init_v1_identity_from_v2(source, scratch, count);

    for (uint16_t i = 0u; i < count; i++) {
        TinyDBSlottedLeafV2Slot slot = v2_slot(source, i);
        unsigned char* cell = mutable_v1_cell(scratch, i);
        memcpy(cell + LEAF_NODE_KEY_OFFSET, &slot.key, sizeof(slot.key));
        memset(cell + LEAF_NODE_VALUE_OFFSET, 0, ROW_SIZE);
        memcpy(cell + LEAF_NODE_VALUE_OFFSET,
               (const unsigned char*)source + slot.value_offset,
               slot.value_length);
    }

    if (tinydb_leaf_format_detect_page(scratch, sizeof(scratch)) !=
        TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        return false;
    }

    memcpy(destination, scratch, PAGE_USABLE_SIZE);
    return true;
}

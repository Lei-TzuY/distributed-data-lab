#include "record_payload_try_scan.h"

#include "leaf_format.h"
#include "leaf_page_access.h"
#include "pager_try_pin.h"
#include "row_envelope.h"

#include <stdio.h>
#include <string.h>

static void scan_set_message(char* message,
                             size_t message_size,
                             const char* text) {
    if (message == NULL || message_size == 0u) return;
    snprintf(message, message_size, "%s", text != NULL ? text : "");
}

static bool scan_valid_page(const Table* table, uint32_t page_num) {
    return table != NULL && table->pager != NULL &&
           page_num != INVALID_PAGE_NUM && page_num < table->pager->num_pages;
}

static bool scan_pin_read(Table* table,
                          uint32_t page_num,
                          PagerPageHandle* handle,
                          char* message,
                          size_t message_size) {
    PagerTryPinStatus status = pager_try_pin_existing_page_handle(table->pager,
                                                                  page_num,
                                                                  handle);
    if (status != PAGER_TRY_PIN_OK) {
        if (message != NULL && message_size > 0u) {
            snprintf(message,
                     message_size,
                     "could not acquire B+ tree page %u: %s",
                     page_num,
                     pager_try_pin_status_string(status));
        }
        return false;
    }
    if (!pager_page_handle_acquire_read(handle)) {
        (void)pager_release_page_handle(handle);
        scan_set_message(message,
                         message_size,
                         "could not acquire B+ tree page read lock");
        return false;
    }
    return true;
}

static bool scan_release_read(PagerPageHandle* handle,
                              char* message,
                              size_t message_size) {
    if (!pager_page_handle_release_read(handle)) {
        scan_set_message(message,
                         message_size,
                         "could not release B+ tree page read lock");
        return false;
    }
    if (!pager_release_page_handle(handle)) {
        scan_set_message(message,
                         message_size,
                         "could not release B+ tree page pin");
        return false;
    }
    return true;
}

static bool scan_internal_child(Table* table,
                                uint32_t page_num,
                                const void* node,
                                uint32_t key,
                                uint32_t* child_page) {
    if (table == NULL || node == NULL || child_page == NULL ||
        get_node_type((void*)node) != NODE_INTERNAL) {
        return false;
    }

    uint32_t num_keys = *internal_node_num_keys((void*)node);
    if (num_keys == 0u || num_keys > INTERNAL_NODE_MAX_KEYS) return false;

    uint32_t right_child = *internal_node_right_child((void*)node);
    if (!scan_valid_page(table, right_child) || right_child == page_num) {
        return false;
    }

    uint32_t previous_key = 0u;
    for (uint32_t i = 0u; i < num_keys; i++) {
        uint32_t separator = *internal_node_key((void*)node, i);
        if (i > 0u && separator <= previous_key) return false;
        previous_key = separator;

        uint32_t candidate = *internal_node_child((void*)node, i);
        if (!scan_valid_page(table, candidate) ||
            candidate == page_num || candidate == right_child) {
            return false;
        }
        for (uint32_t j = 0u; j < i; j++) {
            if (*internal_node_child((void*)node, j) == candidate) return false;
        }
    }

    uint32_t min_index = 0u;
    uint32_t max_index = num_keys;
    while (min_index < max_index) {
        uint32_t index = min_index + (max_index - min_index) / 2u;
        if (*internal_node_key((void*)node, index) >= key) {
            max_index = index;
        } else {
            min_index = index + 1u;
        }
    }

    *child_page = min_index == num_keys
        ? right_child
        : *internal_node_child((void*)node, min_index);
    return scan_valid_page(table, *child_page) && *child_page != page_num;
}

static bool scan_decode_cell(const TableSchema* schema,
                             const void* page,
                             uint32_t cell_index,
                             uint32_t* key,
                             TinyDBRecordPayload* payload,
                             char* message,
                             size_t message_size) {
    if (schema == NULL || page == NULL || key == NULL || payload == NULL) {
        scan_set_message(message, message_size, "invalid payload scan cell arguments");
        return false;
    }

    const void* value = NULL;
    uint32_t stored_length = 0u;
    if (!tinydb_leaf_page_key_at(page, PAGE_SIZE, cell_index, key) ||
        !tinydb_leaf_page_value_at(page,
                                   PAGE_SIZE,
                                   cell_index,
                                   &value,
                                   &stored_length) ||
        value == NULL) {
        scan_set_message(message, message_size, "leaf row is not decodable");
        return false;
    }

    TinyDBRecordPayload local;
    memset(&local, 0, sizeof(local));
    TinyDBLeafPageFormat format = tinydb_leaf_format_detect_page(page, PAGE_SIZE);
    if (format == TINYDB_LEAF_PAGE_FORMAT_FIXED_V1) {
        if (stored_length < schema->row_size) {
            scan_set_message(message,
                             message_size,
                             "fixed V1 row is shorter than the logical schema payload");
            return false;
        }
        local.length = schema->row_size;
        memcpy(local.bytes, value, local.length);
    } else if (format == TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2) {
        if (stored_length == schema->row_size) {
            local.length = stored_length;
            memcpy(local.bytes, value, local.length);
        } else if (!tinydb_row_envelope_decode(schema,
                                               value,
                                               stored_length,
                                               &local)) {
            scan_set_message(message,
                             message_size,
                             "slotted V2 row envelope is invalid for the logical schema");
            return false;
        }
    } else {
        scan_set_message(message, message_size, "leaf page uses an unsupported format");
        return false;
    }

    *payload = local;
    return true;
}

static bool scan_seek_lower_bound(Table* table,
                                  const TableSchema* schema,
                                  uint32_t key,
                                  uint32_t* leaf_page,
                                  uint32_t* cell_index,
                                  char* message,
                                  size_t message_size) {
    uint32_t page_num = schema->root_page_num;
    for (uint32_t depth = 0u; depth <= table->pager->num_pages; depth++) {
        PagerPageHandle handle;
        if (!scan_pin_read(table,
                           page_num,
                           &handle,
                           message,
                           message_size)) {
            return false;
        }

        NodeType type = get_node_type(handle.data);
        if (type == NODE_INTERNAL) {
            uint32_t child_page = INVALID_PAGE_NUM;
            bool valid = scan_internal_child(table,
                                             page_num,
                                             handle.data,
                                             key,
                                             &child_page);
            if (!scan_release_read(&handle, message, message_size)) return false;
            if (!valid) {
                scan_set_message(message,
                                 message_size,
                                 "internal B+ tree routing metadata is invalid");
                return false;
            }
            page_num = child_page;
            continue;
        }

        if (type != NODE_LEAF) {
            if (!scan_release_read(&handle, message, message_size)) return false;
            scan_set_message(message,
                             message_size,
                             "B+ tree page has an invalid node type");
            return false;
        }

        uint32_t count = 0u;
        uint32_t lower = 0u;
        bool exact_match = false;
        bool valid = tinydb_leaf_page_count(handle.data, PAGE_SIZE, &count) &&
                     tinydb_leaf_page_lower_bound(handle.data,
                                                  PAGE_SIZE,
                                                  key,
                                                  &lower,
                                                  &exact_match) &&
                     lower <= count;
        (void)exact_match;
        if (!scan_release_read(&handle, message, message_size)) return false;
        if (!valid) {
            scan_set_message(message,
                             message_size,
                             "leaf page has an invalid searchable layout");
            return false;
        }

        *leaf_page = page_num;
        *cell_index = lower;
        return true;
    }

    scan_set_message(message,
                     message_size,
                     "B+ tree traversal exceeded the allocated page count");
    return false;
}

uint32_t tinydb_record_payload_try_scan_range(Table* table,
                                              const TableSchema* schema,
                                              uint32_t min_id,
                                              uint32_t max_id,
                                              TinyDBRecordPayloadVisitor visitor,
                                              void* context,
                                              bool* scan_complete,
                                              char* message,
                                              size_t message_size) {
    if (scan_complete != NULL) *scan_complete = false;
    if (message != NULL && message_size > 0u) message[0] = '\0';

    if (table == NULL || table->pager == NULL || schema == NULL) {
        scan_set_message(message, message_size, "table and schema are required");
        return 0u;
    }
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return 0u;
    }
    if (!scan_valid_page(table, schema->root_page_num)) {
        scan_set_message(message, message_size, "schema root page is invalid");
        return 0u;
    }
    if (min_id > max_id) {
        if (scan_complete != NULL) *scan_complete = true;
        return 0u;
    }

    uint32_t page_num = INVALID_PAGE_NUM;
    uint32_t cell_index = 0u;
    if (!scan_seek_lower_bound(table,
                               schema,
                               min_id,
                               &page_num,
                               &cell_index,
                               message,
                               message_size)) {
        return 0u;
    }

    uint32_t rows = 0u;
    uint32_t transitions = 0u;
    uint32_t expected_prev = INVALID_PAGE_NUM;
    bool require_prev = false;
    uint32_t global_last_key = 0u;
    bool have_global_last_key = false;

    while (true) {
        PagerPageHandle handle;
        if (!scan_pin_read(table,
                           page_num,
                           &handle,
                           message,
                           message_size)) {
            return 0u;
        }

        if (get_node_type(handle.data) != NODE_LEAF) {
            if (!scan_release_read(&handle, message, message_size)) return 0u;
            scan_set_message(message,
                             message_size,
                             "leaf-chain scan reached a non-leaf page");
            return 0u;
        }

        uint32_t leaf_count = 0u;
        uint32_t prev_page = 0u;
        uint32_t next_page = 0u;
        if (!tinydb_leaf_page_count(handle.data, PAGE_SIZE, &leaf_count) ||
            !tinydb_leaf_page_prev(handle.data, PAGE_SIZE, &prev_page) ||
            !tinydb_leaf_page_next(handle.data, PAGE_SIZE, &next_page) ||
            cell_index > leaf_count) {
            if (!scan_release_read(&handle, message, message_size)) return 0u;
            scan_set_message(message,
                             message_size,
                             "leaf-chain traversal metadata is invalid");
            return 0u;
        }

        if (require_prev) {
            uint32_t first_key = 0u;
            bool ordered = prev_page == expected_prev;
            if (ordered && leaf_count > 0u && have_global_last_key) {
                ordered = tinydb_leaf_page_key_at(handle.data,
                                                  PAGE_SIZE,
                                                  0u,
                                                  &first_key) &&
                          first_key > global_last_key;
            }
            if (!ordered) {
                if (!scan_release_read(&handle, message, message_size)) return 0u;
                scan_set_message(message,
                                 message_size,
                                 "leaf sibling reciprocity or key order is invalid");
                return 0u;
            }
            require_prev = false;
        }

        if (cell_index < leaf_count) {
            uint32_t key = 0u;
            TinyDBRecordPayload payload;
            memset(&payload, 0, sizeof(payload));
            bool decoded = scan_decode_cell(schema,
                                            handle.data,
                                            cell_index,
                                            &key,
                                            &payload,
                                            message,
                                            message_size);
            if (!decoded) {
                if (!scan_release_read(&handle, message, message_size)) return 0u;
                return 0u;
            }
            if (key < min_id) {
                if (!scan_release_read(&handle, message, message_size)) return 0u;
                scan_set_message(message,
                                 message_size,
                                 "payload range scan moved before its lower bound");
                return 0u;
            }
            if (key > max_id) {
                if (!scan_release_read(&handle, message, message_size)) return 0u;
                if (scan_complete != NULL) *scan_complete = true;
                if (message != NULL && message_size > 0u) message[0] = '\0';
                return rows;
            }
            if (cell_index > 0u) {
                uint32_t previous_key = 0u;
                if (!tinydb_leaf_page_key_at(handle.data,
                                             PAGE_SIZE,
                                             cell_index - 1u,
                                             &previous_key) ||
                    previous_key >= key) {
                    if (!scan_release_read(&handle, message, message_size)) return 0u;
                    scan_set_message(message,
                                     message_size,
                                     "leaf keys are not strictly increasing");
                    return 0u;
                }
            }

            if (!scan_release_read(&handle, message, message_size)) return 0u;
            if (rows == UINT32_MAX) {
                scan_set_message(message,
                                 message_size,
                                 "payload scan row count exceeds API capacity");
                return 0u;
            }
            rows++;
            if (visitor != NULL && !visitor(schema, &payload, context)) {
                if (scan_complete != NULL) *scan_complete = true;
                if (message != NULL && message_size > 0u) message[0] = '\0';
                return rows;
            }
            cell_index++;
            continue;
        }

        uint32_t leaf_last_key = 0u;
        bool leaf_has_key = leaf_count > 0u;
        if (leaf_has_key &&
            !tinydb_leaf_page_key_at(handle.data,
                                     PAGE_SIZE,
                                     leaf_count - 1u,
                                     &leaf_last_key)) {
            if (!scan_release_read(&handle, message, message_size)) return 0u;
            scan_set_message(message,
                             message_size,
                             "leaf tail key is not decodable");
            return 0u;
        }

        if (!scan_release_read(&handle, message, message_size)) return 0u;

        if (leaf_has_key) {
            global_last_key = leaf_last_key;
            have_global_last_key = true;
        }
        if (next_page == 0u) {
            if (scan_complete != NULL) *scan_complete = true;
            if (message != NULL && message_size > 0u) message[0] = '\0';
            return rows;
        }
        if (!scan_valid_page(table, next_page) || next_page == page_num ||
            transitions >= table->pager->num_pages) {
            scan_set_message(message,
                             message_size,
                             "leaf sibling chain is cyclic or out of range");
            return 0u;
        }

        expected_prev = page_num;
        require_prev = true;
        page_num = next_page;
        cell_index = 0u;
        transitions++;
    }
}

uint32_t tinydb_record_payload_try_scan(Table* table,
                                        const TableSchema* schema,
                                        TinyDBRecordPayloadVisitor visitor,
                                        void* context,
                                        bool* scan_complete,
                                        char* message,
                                        size_t message_size) {
    return tinydb_record_payload_try_scan_range(table,
                                                schema,
                                                0u,
                                                UINT32_MAX,
                                                visitor,
                                                context,
                                                scan_complete,
                                                message,
                                                message_size);
}

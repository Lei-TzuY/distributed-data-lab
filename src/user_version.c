#include "user_version.h"

#include "pager_try_pin.h"

#include <stdio.h>

static void set_message(char* message,
                        size_t message_size,
                        const char* text) {
    if (message == NULL || message_size == 0u) return;
    snprintf(message, message_size, "%s", text != NULL ? text : "");
}

static void set_pin_message(char* message,
                            size_t message_size,
                            const char* action,
                            PagerTryPinStatus status) {
    if (message == NULL || message_size == 0u) return;
    snprintf(message,
             message_size,
             "%s: %s",
             action,
             pager_try_pin_status_string(status));
}

bool db_try_get_user_version(Table* table,
                             uint32_t* version,
                             char* message,
                             size_t message_size) {
    if (version != NULL) *version = 0u;
    set_message(message, message_size, "");

    if (version == NULL) {
        set_message(message, message_size, "invalid user_version output");
        return false;
    }
    if (table == NULL || table->pager == NULL) {
        set_message(message, message_size, "invalid database handle");
        return false;
    }
    if (table->pager->num_pages == 0u) return true;

    PagerPageHandle root_handle;
    PagerTryPinStatus pin_status = pager_try_pin_existing_page_handle(
        table->pager,
        table->root_page_num,
        &root_handle);
    if (pin_status != PAGER_TRY_PIN_OK) {
        set_pin_message(message,
                        message_size,
                        "could not acquire root page",
                        pin_status);
        return false;
    }

    if (!pager_page_handle_acquire_read(&root_handle)) {
        (void)pager_release_page_handle(&root_handle);
        set_message(message,
                    message_size,
                    "could not acquire root-page read lock");
        return false;
    }

    uint32_t value = *node_parent(root_handle.data);
    if (!pager_page_handle_release_read(&root_handle)) {
        (void)pager_release_page_handle(&root_handle);
        set_message(message,
                    message_size,
                    "could not release root-page read lock");
        return false;
    }
    if (!pager_release_page_handle(&root_handle)) {
        set_message(message,
                    message_size,
                    "could not release root-page pin");
        return false;
    }

    *version = value;
    return true;
}

bool db_try_set_user_version(Table* table,
                             uint32_t version,
                             char* message,
                             size_t message_size) {
    set_message(message, message_size, "");

    if (table == NULL || table->pager == NULL) {
        set_message(message, message_size, "invalid database handle");
        return false;
    }
    if (table->pager->num_pages == 0u ||
        table->root_page_num >= table->pager->num_pages) {
        set_message(message, message_size, "database has no allocated root page");
        return false;
    }

    PagerPageHandle root_handle;
    PagerTryPinStatus pin_status = pager_try_pin_existing_page_handle(
        table->pager,
        table->root_page_num,
        &root_handle);
    if (pin_status != PAGER_TRY_PIN_OK) {
        set_pin_message(message,
                        message_size,
                        "could not acquire root page",
                        pin_status);
        return false;
    }

    if (!pager_page_handle_acquire_write(&root_handle)) {
        (void)pager_release_page_handle(&root_handle);
        set_message(message,
                    message_size,
                    "could not acquire root-page write lock");
        return false;
    }

    *node_parent(root_handle.data) = version;
    mark_page_dirty(table->pager, table->root_page_num);

    if (!pager_page_handle_release_write(&root_handle)) {
        (void)pager_release_page_handle(&root_handle);
        set_message(message,
                    message_size,
                    "user_version updated but root-page write lock could not be released");
        return false;
    }
    if (!pager_release_page_handle(&root_handle)) {
        set_message(message,
                    message_size,
                    "user_version updated but root-page pin could not be released");
        return false;
    }

    return true;
}

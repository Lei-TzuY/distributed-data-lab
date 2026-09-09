#ifndef PAGER_TRY_PIN_H
#define PAGER_TRY_PIN_H

#include "pager.h"

typedef enum {
    PAGER_TRY_PIN_OK = 0,
    PAGER_TRY_PIN_BUSY,
    PAGER_TRY_PIN_INVALID_PAGE,
    PAGER_TRY_PIN_IO_ERROR,
    PAGER_TRY_PIN_CORRUPT_PAGE,
    PAGER_TRY_PIN_NO_MEMORY
} PagerTryPinStatus;

static inline const char* pager_try_pin_status_string(PagerTryPinStatus status) {
    switch (status) {
        case PAGER_TRY_PIN_OK: return "ok";
        case PAGER_TRY_PIN_BUSY: return "buffer pool busy";
        case PAGER_TRY_PIN_INVALID_PAGE: return "invalid page";
        case PAGER_TRY_PIN_IO_ERROR: return "page I/O error";
        case PAGER_TRY_PIN_CORRUPT_PAGE: return "page checksum mismatch";
        case PAGER_TRY_PIN_NO_MEMORY: return "out of memory while spilling dirty page";
    }
    return "unknown pager try-pin status";
}

/*
 * Non-fatal existing-page acquisition. This does not allocate logical pages.
 * Cache lookup/replacement, dirty no-steal spill, page-image load/checksum
 * validation, LRU mutation, and publication of the returned pin are atomic
 * with respect to Pager metadata. A fully owned pool reports BUSY rather than
 * terminating the process.
 *
 * The implementation is compiled once in the Pager extension translation
 * unit instead of being emitted independently into every consumer.
 */
PagerTryPinStatus pager_try_pin_existing_page_handle(
    Pager* pager,
    uint32_t page_num,
    PagerPageHandle* handle);

#endif /* PAGER_TRY_PIN_H */

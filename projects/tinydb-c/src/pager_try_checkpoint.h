#ifndef TINYDB_PAGER_TRY_CHECKPOINT_H
#define TINYDB_PAGER_TRY_CHECKPOINT_H

#include "pager.h"

#include <stddef.h>

/*
 * Backpressure-aware companion to pager_checkpoint().
 *
 * The historical checkpoint path uses get_page() while flushing dirty pages
 * and can therefore terminate the process when every buffer-pool frame is
 * externally pinned and a dirty page is nonresident. This additive API first
 * proves that every currently dirty logical page can be acquired through the
 * non-fatal existing-page try-pin seam. Only after the complete preflight
 * succeeds does it invoke the historical checkpoint implementation.
 *
 * A false return means no checkpoint flush was started by this function;
 * message receives bounded detail when supplied. A true return preserves the
 * historical checkpoint semantics, including its existing I/O error policy.
 *
 * The preflight is intentionally non-blocking and is designed for static pin
 * pressure. It does not make checkpoint atomic with arbitrary concurrent Pager
 * pin/mutation activity; callers still need their normal higher-level
 * serialization when sharing a Pager across concurrent operations.
 */
bool pager_try_checkpoint(Pager* pager,
                          char* message,
                          size_t message_size);

#endif /* TINYDB_PAGER_TRY_CHECKPOINT_H */

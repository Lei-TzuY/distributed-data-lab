#ifndef RECORD_PAYLOAD_H
#define RECORD_PAYLOAD_H

#include "record.h"

#include <stddef.h>

/* A logical generic row is no longer sized by the historical users Row slot.
 * The largest schema currently expressible by TinyDBValue is sixteen VARCHAR
 * fields (including their terminating NUL); INT fields consume less space.
 * Physical leaf formats remain free to impose tighter per-value limits. */
#define TINYDB_RECORD_PAYLOAD_MAX \
    (MAX_COLUMNS_PER_TABLE * (TINYDB_RECORD_TEXT_MAX + 1u))

typedef struct {
    uint32_t length;
    unsigned char bytes[TINYDB_RECORD_PAYLOAD_MAX];
} TinyDBRecordPayload;

typedef bool (*TinyDBRecordPayloadVisitor)(const TableSchema* schema,
                                           const TinyDBRecordPayload* payload,
                                           void* context);

/* Schema-aware logical codecs. These operate directly on the sized payload
 * carrier so generic rows can be wider than the legacy ROW_SIZE value slot.
 * They do not imply that every physical leaf format can store every payload. */
bool tinydb_record_payload_schema_supported(const TableSchema* schema,
                                            char* message,
                                            size_t message_size);

bool tinydb_record_payload_encode_values(const TableSchema* schema,
                                         const TinyDBValue* values,
                                         uint32_t value_count,
                                         TinyDBRecordPayload* payload,
                                         char* message,
                                         size_t message_size);

bool tinydb_record_payload_decode_values(const TableSchema* schema,
                                         const TinyDBRecordPayload* payload,
                                         TinyDBValue* values,
                                         uint32_t value_capacity,
                                         uint32_t* value_count,
                                         char* message,
                                         size_t message_size);

/* Payload-native record reads are the wide-schema query seam. They use the
 * dual-format leaf cursor and decode either raw V1/V2 logical bytes or compact
 * V2 row envelopes without forcing the result back through TinyDBRecord.
 * scan_complete is set false when traversal detects corruption or a malformed
 * value so callers can distinguish an empty table from a truncated scan. */
bool tinydb_record_payload_find(Table* table,
                                const TableSchema* schema,
                                uint32_t id,
                                TinyDBRecordPayload* payload,
                                char* message,
                                size_t message_size);

uint32_t tinydb_record_payload_scan(Table* table,
                                    const TableSchema* schema,
                                    TinyDBRecordPayloadVisitor visitor,
                                    void* context,
                                    bool* scan_complete,
                                    char* message,
                                    size_t message_size);

/* Inclusive primary-key range scan for schema-sized payloads. The cursor seeks
 * directly to min_id and stops before the first key greater than max_id, so
 * wide generic query paths do not need to fall back through TinyDBRecord.
 * An inverted range is a valid empty scan. */
uint32_t tinydb_record_payload_scan_range(Table* table,
                                          const TableSchema* schema,
                                          uint32_t min_id,
                                          uint32_t max_id,
                                          TinyDBRecordPayloadVisitor visitor,
                                          void* context,
                                          bool* scan_complete,
                                          char* message,
                                          size_t message_size);

/* Payload-native INSERT writes a schema-sized payload directly into compact V2
 * storage without narrowing through TinyDBRecord. Proven production growth
 * paths include an empty/single root leaf, atomic root-leaf split, non-root
 * leaf split under a parent with space, one-level growth when that parent is
 * the full stable root (including root page zero), splitting a full non-root
 * parent into a non-full grandparent, and bounded recursive cascades through
 * consecutive full non-root internal ancestors into an available ancestor.
 * Tail leaves may grow their global maximum; non-tail separators, reciprocal
 * sibling links, ancestor identity, allocator claims, and mutation epoch
 * ordering are validated before publication. Stable root page zero is valid in
 * the bounded recursive path, including the two- and three-full-level cases
 * covered by live rollback/commit/reopen regressions. Recursive growth remains
 * deliberately bounded by the current 16-page atomic publication capacity;
 * topologies whose complete staged page set exceeds that limit fail closed. */
bool tinydb_record_payload_insert(Table* table,
                                  const TableSchema* schema,
                                  const TinyDBRecordPayload* payload,
                                  char* message,
                                  size_t message_size);

/* Payload-native point UPDATE is the wide-schema replacement seam. It keeps
 * the primary key stable, invalidates generic secondary-index snapshots before
 * mutation, supports fixed V1 rows only when the logical payload fits ROW_SIZE,
 * and updates existing compact-envelope V2 rows in place when the replacement
 * fits the page. It never routes through the legacy TinyDBRecord carrier. */
bool tinydb_record_payload_update(Table* table,
                                  const TableSchema* schema,
                                  uint32_t id,
                                  const TinyDBRecordPayload* payload,
                                  char* message,
                                  size_t message_size);

/* DELETE has no replacement row to carry, but exposing it on the payload API
 * keeps wide-schema CRUD callers on one schema-sized surface. The generic
 * delete implementation is already schema-aware and format-aware; validate the
 * wider logical schema contract here before delegating so invalid layouts fail
 * before any mutation or generic-index epoch publication. */
static inline bool tinydb_record_payload_delete(Table* table,
                                                const TableSchema* schema,
                                                uint32_t id,
                                                char* message,
                                                size_t message_size) {
    if (!tinydb_record_payload_schema_supported(schema, message, message_size)) {
        return false;
    }
    return tinydb_record_delete(table, schema, id, message, message_size);
}

/* Compatibility adapters for callers that still use the historical fixed
 * TinyDBRecord carrier. They intentionally reject rows wider than ROW_SIZE. */
bool tinydb_record_payload_from_record(const TableSchema* schema,
                                       const TinyDBRecord* record,
                                       TinyDBRecordPayload* payload,
                                       char* message,
                                       size_t message_size);

bool tinydb_record_payload_to_record(const TableSchema* schema,
                                     const TinyDBRecordPayload* payload,
                                     TinyDBRecord* record,
                                     char* message,
                                     size_t message_size);

/* Legacy fixed leaf slots remain exactly ROW_SIZE bytes. These helpers make
 * padding an explicit physical-format concern instead of part of logical row
 * encoding. */
bool tinydb_record_payload_pack_fixed_slot(const TinyDBRecordPayload* payload,
                                           void* slot,
                                           size_t slot_size,
                                           char* message,
                                           size_t message_size);

bool tinydb_record_payload_unpack_fixed_slot(const TableSchema* schema,
                                             const void* slot,
                                             size_t slot_size,
                                             TinyDBRecordPayload* payload,
                                             char* message,
                                             size_t message_size);

#endif /* RECORD_PAYLOAD_H */

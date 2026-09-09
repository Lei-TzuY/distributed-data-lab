#ifndef RECORD_H
#define RECORD_H

#include "table.h"

#include <stddef.h>

#define TINYDB_RECORD_MESSAGE_MAX 256
#define TINYDB_RECORD_TEXT_MAX 256

typedef struct {
    ColumnType type;
    uint32_t int_value;
    char text[TINYDB_RECORD_TEXT_MAX + 1];
} TinyDBValue;

typedef struct {
    unsigned char bytes[ROW_SIZE];
} TinyDBRecord;

typedef bool (*TinyDBRecordVisitor)(const TableSchema* schema,
                                    const TinyDBRecord* record,
                                    void* context);

bool tinydb_schema_supports_records(const TableSchema* schema,
                                    char* message,
                                    size_t message_size);

bool tinydb_record_encode(const TableSchema* schema,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          TinyDBRecord* record,
                          char* message,
                          size_t message_size);

bool tinydb_record_decode(const TableSchema* schema,
                          const TinyDBRecord* record,
                          TinyDBValue* values,
                          uint32_t value_capacity,
                          uint32_t* value_count,
                          char* message,
                          size_t message_size);

bool tinydb_record_insert(Table* table,
                          const TableSchema* schema,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size);

bool tinydb_record_update(Table* table,
                          const TableSchema* schema,
                          uint32_t id,
                          const TinyDBValue* values,
                          uint32_t value_count,
                          char* message,
                          size_t message_size);

bool tinydb_record_delete(Table* table,
                          const TableSchema* schema,
                          uint32_t id,
                          char* message,
                          size_t message_size);

uint32_t tinydb_record_delete_all(Table* table,
                                  const TableSchema* schema,
                                  char* message,
                                  size_t message_size);

bool tinydb_record_find(Table* table,
                        const TableSchema* schema,
                        uint32_t id,
                        TinyDBRecord* record);

uint32_t tinydb_record_scan(Table* table,
                            const TableSchema* schema,
                            TinyDBRecordVisitor visitor,
                            void* context);

void tinydb_record_print(const TableSchema* schema,
                         const TinyDBRecord* record);

#endif /* RECORD_H */

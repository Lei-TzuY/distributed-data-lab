#ifndef COLUMN_TYPE_H
#define COLUMN_TYPE_H

#include "table.h"

#include <stddef.h>

/* Canonical interpretation of the SQL type strings currently supported by
 * executable generic records. VARCHAR storage includes one mandatory NUL byte,
 * so declared_capacity and storage_size intentionally differ by one. */
typedef struct {
    ColumnType type;
    uint32_t storage_size;
    uint32_t declared_capacity;
    bool explicitly_sized;
} TinyDBColumnTypeSpec;

/* Parse one complete type string. Leading/trailing whitespace is accepted. */
bool tinydb_column_type_parse(const char* text, TinyDBColumnTypeSpec* spec);

/* Parse one type expression from the front of a larger SQL fragment and return
 * the first byte after the type expression. This is the shared lexical source
 * for CREATE/ALTER VARCHAR(n) normalization; callers remain responsible for
 * validating the SQL delimiter that follows end_out. */
bool tinydb_column_type_parse_prefix(const char* text,
                                     TinyDBColumnTypeSpec* spec,
                                     const char** end_out);

bool tinydb_column_type_is_int(const char* text);
bool tinydb_column_type_is_varchar(const char* text);

/* Format catalog metadata using SQL-facing character capacity rather than the
 * physical serialized byte count. */
bool tinydb_column_type_format(const TableColumn* column,
                               char* output,
                               size_t output_size);

#endif /* COLUMN_TYPE_H */

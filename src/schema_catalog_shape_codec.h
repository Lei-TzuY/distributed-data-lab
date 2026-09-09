#ifndef TINYDB_SCHEMA_CATALOG_SHAPE_CODEC_H
#define TINYDB_SCHEMA_CATALOG_SHAPE_CODEC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "table.h"

#define TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE 32768u

typedef struct TinyDBSchemaShapeWriter {
    unsigned char* data;
    size_t capacity;
    size_t position;
    bool ok;
} TinyDBSchemaShapeWriter;

typedef struct TinyDBSchemaShapeReader {
    const unsigned char* data;
    size_t size;
    size_t position;
    bool ok;
} TinyDBSchemaShapeReader;

static inline void tinydb_schema_shape_put_u32(unsigned char out[4], uint32_t v) {
    out[0] = (unsigned char)(v & 0xffu);
    out[1] = (unsigned char)((v >> 8) & 0xffu);
    out[2] = (unsigned char)((v >> 16) & 0xffu);
    out[3] = (unsigned char)((v >> 24) & 0xffu);
}

static inline uint32_t tinydb_schema_shape_get_u32(const unsigned char in[4]) {
    return ((uint32_t)in[0]) | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static inline void tinydb_schema_shape_write(TinyDBSchemaShapeWriter* w,
                                              const void* data,
                                              size_t size) {
    if (w == NULL || !w->ok || size > w->capacity - w->position) {
        if (w != NULL) w->ok = false;
        return;
    }
    memcpy(w->data + w->position, data, size);
    w->position += size;
}

static inline void tinydb_schema_shape_write_u32(TinyDBSchemaShapeWriter* w,
                                                  uint32_t value) {
    unsigned char bytes[4];
    tinydb_schema_shape_put_u32(bytes, value);
    tinydb_schema_shape_write(w, bytes, sizeof(bytes));
}

static inline void tinydb_schema_shape_read(TinyDBSchemaShapeReader* r,
                                             void* out,
                                             size_t size) {
    if (r == NULL || !r->ok || size > r->size - r->position) {
        if (r != NULL) r->ok = false;
        return;
    }
    memcpy(out, r->data + r->position, size);
    r->position += size;
}

static inline uint32_t tinydb_schema_shape_read_u32(TinyDBSchemaShapeReader* r) {
    unsigned char bytes[4] = {0};
    tinydb_schema_shape_read(r, bytes, sizeof(bytes));
    return r != NULL && r->ok ? tinydb_schema_shape_get_u32(bytes) : 0u;
}

static inline bool tinydb_schema_shape_fixed_string(const char* s, size_t n) {
    return s != NULL && memchr(s, '\0', n) != NULL;
}

static inline bool tinydb_schema_catalog_shape_valid(const Catalog* catalog) {
    if (catalog == NULL || catalog->num_tables == 0u ||
        catalog->num_tables > MAX_TABLES || catalog->num_views > MAX_VIEWS) {
        return false;
    }
    for (uint32_t i = 0; i < catalog->num_tables; i++) {
        const TableSchema* schema = &catalog->schemas[i];
        if (!tinydb_schema_shape_fixed_string(schema->name, sizeof(schema->name)) ||
            schema->num_columns == 0u || schema->num_columns > MAX_COLUMNS_PER_TABLE) {
            return false;
        }
        for (uint32_t j = 0; j < schema->num_columns; j++) {
            const TableColumn* col = &schema->columns[j];
            if (!tinydb_schema_shape_fixed_string(col->name, sizeof(col->name)) ||
                (col->type != COL_TYPE_INT && col->type != COL_TYPE_VARCHAR) ||
                col->offset > schema->row_size ||
                col->size > schema->row_size - col->offset) {
                return false;
            }
        }
        if (!tinydb_schema_shape_fixed_string(schema->fk_col, sizeof(schema->fk_col)) ||
            !tinydb_schema_shape_fixed_string(schema->fk_parent_table, sizeof(schema->fk_parent_table)) ||
            !tinydb_schema_shape_fixed_string(schema->fk_parent_col, sizeof(schema->fk_parent_col))) {
            return false;
        }
    }
    for (uint32_t i = 0; i < catalog->num_views; i++) {
        if (!tinydb_schema_shape_fixed_string(catalog->views[i].name, sizeof(catalog->views[i].name)) ||
            !tinydb_schema_shape_fixed_string(catalog->views[i].select_sql, sizeof(catalog->views[i].select_sql))) {
            return false;
        }
    }
    return true;
}

static inline bool tinydb_schema_catalog_shape_encode(const Catalog* catalog,
                                                       unsigned char* out,
                                                       size_t capacity,
                                                       size_t* size_out) {
    if (size_out != NULL) *size_out = 0u;
    if (!tinydb_schema_catalog_shape_valid(catalog) || out == NULL || size_out == NULL) return false;
    TinyDBSchemaShapeWriter w = {out, capacity, 0u, true};
    tinydb_schema_shape_write_u32(&w, catalog->num_tables);
    tinydb_schema_shape_write_u32(&w, catalog->num_views);
    for (uint32_t i = 0; i < catalog->num_tables; i++) {
        const TableSchema* schema = &catalog->schemas[i];
        tinydb_schema_shape_write(&w, schema->name, sizeof(schema->name));
        tinydb_schema_shape_write_u32(&w, schema->root_page_num);
        tinydb_schema_shape_write_u32(&w, schema->num_columns);
        for (uint32_t j = 0; j < schema->num_columns; j++) {
            const TableColumn* col = &schema->columns[j];
            tinydb_schema_shape_write(&w, col->name, sizeof(col->name));
            tinydb_schema_shape_write_u32(&w, (uint32_t)col->type);
            tinydb_schema_shape_write_u32(&w, col->size);
            tinydb_schema_shape_write_u32(&w, col->offset);
        }
        tinydb_schema_shape_write_u32(&w, schema->row_size);
        { unsigned char b = schema->has_fk ? 1u : 0u; tinydb_schema_shape_write(&w, &b, 1u); }
        tinydb_schema_shape_write(&w, schema->fk_col, sizeof(schema->fk_col));
        tinydb_schema_shape_write(&w, schema->fk_parent_table, sizeof(schema->fk_parent_table));
        tinydb_schema_shape_write(&w, schema->fk_parent_col, sizeof(schema->fk_parent_col));
        { unsigned char b = schema->fk_on_delete_cascade ? 1u : 0u; tinydb_schema_shape_write(&w, &b, 1u); }
    }
    for (uint32_t i = 0; i < catalog->num_views; i++) {
        tinydb_schema_shape_write(&w, catalog->views[i].name, sizeof(catalog->views[i].name));
        tinydb_schema_shape_write(&w, catalog->views[i].select_sql, sizeof(catalog->views[i].select_sql));
    }
    if (!w.ok) return false;
    *size_out = w.position;
    return true;
}

static inline bool tinydb_schema_catalog_shape_decode(const unsigned char* data,
                                                       size_t size,
                                                       Catalog* catalog_out) {
    if (catalog_out == NULL) return false;
    memset(catalog_out, 0, sizeof(*catalog_out));
    if (data == NULL || size == 0u || size > TINYDB_SCHEMA_CATALOG_SHAPE_MAX_SIZE) return false;
    Catalog candidate;
    memset(&candidate, 0, sizeof(candidate));
    TinyDBSchemaShapeReader r = {data, size, 0u, true};
    candidate.num_tables = tinydb_schema_shape_read_u32(&r);
    candidate.num_views = tinydb_schema_shape_read_u32(&r);
    if (!r.ok || candidate.num_tables == 0u || candidate.num_tables > MAX_TABLES || candidate.num_views > MAX_VIEWS) return false;
    for (uint32_t i = 0; i < candidate.num_tables; i++) {
        TableSchema* schema = &candidate.schemas[i];
        tinydb_schema_shape_read(&r, schema->name, sizeof(schema->name));
        schema->root_page_num = tinydb_schema_shape_read_u32(&r);
        schema->num_columns = tinydb_schema_shape_read_u32(&r);
        if (!r.ok || schema->num_columns == 0u || schema->num_columns > MAX_COLUMNS_PER_TABLE) return false;
        for (uint32_t j = 0; j < schema->num_columns; j++) {
            TableColumn* col = &schema->columns[j];
            tinydb_schema_shape_read(&r, col->name, sizeof(col->name));
            col->type = (ColumnType)tinydb_schema_shape_read_u32(&r);
            col->size = tinydb_schema_shape_read_u32(&r);
            col->offset = tinydb_schema_shape_read_u32(&r);
        }
        schema->row_size = tinydb_schema_shape_read_u32(&r);
        unsigned char has_fk = 0u;
        tinydb_schema_shape_read(&r, &has_fk, 1u);
        tinydb_schema_shape_read(&r, schema->fk_col, sizeof(schema->fk_col));
        tinydb_schema_shape_read(&r, schema->fk_parent_table, sizeof(schema->fk_parent_table));
        tinydb_schema_shape_read(&r, schema->fk_parent_col, sizeof(schema->fk_parent_col));
        unsigned char cascade = 0u;
        tinydb_schema_shape_read(&r, &cascade, 1u);
        if (!r.ok || has_fk > 1u || cascade > 1u) return false;
        schema->has_fk = has_fk != 0u;
        schema->fk_on_delete_cascade = cascade != 0u;
    }
    for (uint32_t i = 0; i < candidate.num_views; i++) {
        tinydb_schema_shape_read(&r, candidate.views[i].name, sizeof(candidate.views[i].name));
        tinydb_schema_shape_read(&r, candidate.views[i].select_sql, sizeof(candidate.views[i].select_sql));
    }
    if (!r.ok || r.position != r.size || !tinydb_schema_catalog_shape_valid(&candidate)) return false;
    *catalog_out = candidate;
    return true;
}

#endif /* TINYDB_SCHEMA_CATALOG_SHAPE_CODEC_H */

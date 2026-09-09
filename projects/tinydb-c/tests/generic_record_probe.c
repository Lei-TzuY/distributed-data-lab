#include "engine.h"
#include "generic_index_candidates.h"
#include "record.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool exec_ok(TinyDB* db, const char* sql) {
    TinyDBSqlResult result;
    TinyDBSqlStatus status = tinydb_execute_sql(db, sql, &result);
    if (status != TINYDB_SQL_SUCCESS) {
        fprintf(stderr, "SQL failed: %s (%s)\n", sql, result.message);
        return false;
    }
    return true;
}

static TinyDBValue int_value(uint32_t value) {
    TinyDBValue result;
    memset(&result, 0, sizeof(result));
    result.type = COL_TYPE_INT;
    result.int_value = value;
    return result;
}

static TinyDBValue text_value(const char* value) {
    TinyDBValue result;
    memset(&result, 0, sizeof(result));
    result.type = COL_TYPE_VARCHAR;
    snprintf(result.text, sizeof(result.text), "%s", value);
    return result;
}

static bool expect_layout_validation(const TableSchema* schema) {
    char message[TINYDB_RECORD_MESSAGE_MAX];
    TableSchema invalid = *schema;

    invalid.columns[2].offset--;
    if (tinydb_schema_supports_records(&invalid, message, sizeof(message)) ||
        strstr(message, "contiguous non-overlapping") == NULL) {
        fprintf(stderr, "overlapping generic layout was not rejected: %s\n", message);
        return false;
    }

    invalid = *schema;
    invalid.columns[2].offset++;
    invalid.row_size++;
    if (tinydb_schema_supports_records(&invalid, message, sizeof(message)) ||
        strstr(message, "contiguous non-overlapping") == NULL) {
        fprintf(stderr, "gapped generic layout was not rejected: %s\n", message);
        return false;
    }

    invalid = *schema;
    invalid.row_size++;
    if (tinydb_schema_supports_records(&invalid, message, sizeof(message)) ||
        strstr(message, "row size must match") == NULL) {
        fprintf(stderr, "trailing generic layout gap was not rejected: %s\n", message);
        return false;
    }

    if (!tinydb_schema_supports_records(schema, message, sizeof(message))) {
        fprintf(stderr, "valid generic layout was rejected: %s\n", message);
        return false;
    }
    return true;
}

static bool expect_unterminated_varchar_rejected(const TableSchema* schema) {
    int varchar_index = -1;
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        if (schema->columns[i].type == COL_TYPE_VARCHAR) {
            varchar_index = (int)i;
            break;
        }
    }
    if (varchar_index < 0) return true;

    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    for (uint32_t i = 0; i < schema->num_columns; i++) {
        values[i] = schema->columns[i].type == COL_TYPE_INT
            ? int_value(i + 1u)
            : text_value("valid");
    }

    TinyDBRecord record;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_record_encode(schema,
                              values,
                              schema->num_columns,
                              &record,
                              message,
                              sizeof(message))) {
        fprintf(stderr, "unable to encode corruption probe record: %s\n", message);
        return false;
    }

    const TableColumn* column = &schema->columns[(uint32_t)varchar_index];
    memset(record.bytes + column->offset, 'x', column->size);

    TinyDBValue decoded[MAX_COLUMNS_PER_TABLE];
    uint32_t decoded_count = 123u;
    if (tinydb_record_decode(schema,
                             &record,
                             decoded,
                             MAX_COLUMNS_PER_TABLE,
                             &decoded_count,
                             message,
                             sizeof(message))) {
        fprintf(stderr, "unterminated VARCHAR corruption was accepted\n");
        return false;
    }
    if (decoded_count != 0u || strstr(message, "not NUL-terminated") == NULL) {
        fprintf(stderr,
                "unexpected unterminated VARCHAR failure: count=%u message=%s\n",
                decoded_count,
                message);
        return false;
    }
    return true;
}

static bool expect_product(Table* table,
                           TableSchema* schema,
                           uint32_t id,
                           const char* name,
                           uint32_t price) {
    TinyDBRecord record;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];

    if (!tinydb_record_find(table, schema, id, &record)) {
        fprintf(stderr, "product %u not found\n", id);
        return false;
    }
    if (!tinydb_record_decode(schema,
                              &record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &count,
                              message,
                              sizeof(message))) {
        fprintf(stderr, "product decode failed: %s\n", message);
        return false;
    }
    if (count != 3 ||
        values[0].int_value != id ||
        strcmp(values[1].text, name) != 0 ||
        values[2].int_value != price) {
        fprintf(stderr, "product %u decoded values do not match\n", id);
        return false;
    }

    uint32_t raw_price = 0;
    memcpy(&raw_price,
           record.bytes + schema->columns[2].offset,
           sizeof(raw_price));
    if (raw_price != price) {
        fprintf(stderr, "product %u raw price offset is incorrect\n", id);
        return false;
    }
    for (uint32_t i = schema->row_size; i < ROW_SIZE; i++) {
        if (record.bytes[i] != 0) {
            fprintf(stderr, "product %u has nonzero fixed-slot tail at byte %u\n", id, i);
            return false;
        }
    }
    return true;
}

static bool expect_order(Table* table,
                         TableSchema* schema,
                         uint32_t id,
                         uint32_t product_id,
                         uint32_t quantity) {
    TinyDBRecord record;
    TinyDBValue values[MAX_COLUMNS_PER_TABLE];
    uint32_t count = 0;
    char message[TINYDB_RECORD_MESSAGE_MAX];

    if (!tinydb_record_find(table, schema, id, &record)) {
        fprintf(stderr, "order %u not found\n", id);
        return false;
    }
    if (!tinydb_record_decode(schema,
                              &record,
                              values,
                              MAX_COLUMNS_PER_TABLE,
                              &count,
                              message,
                              sizeof(message))) {
        fprintf(stderr, "order decode failed: %s\n", message);
        return false;
    }
    if (count != 3 ||
        values[0].int_value != id ||
        values[1].int_value != product_id ||
        values[2].int_value != quantity) {
        fprintf(stderr, "order %u decoded values do not match\n", id);
        return false;
    }
    for (uint32_t i = schema->row_size; i < ROW_SIZE; i++) {
        if (record.bytes[i] != 0) {
            fprintf(stderr, "order %u has nonzero fixed-slot tail at byte %u\n", id, i);
            return false;
        }
    }
    return true;
}

static GenericSecondaryIndex* find_index(Table* table, const char* name) {
    for (uint32_t i = 0; i < table->num_sec_indexes; i++) {
        if (table->sec_indexes[i].enabled &&
            strcmp(table->sec_indexes[i].name, name) == 0) {
            return &table->sec_indexes[i];
        }
    }
    return NULL;
}

static bool expect_price_window(Table* table, TableSchema* products) {
    GenericSecondaryIndex* index = find_index(table, "idx_products_price");
    if (index == NULL) {
        fprintf(stderr, "price index not found\n");
        return false;
    }

    TinyDBGenericPredicate predicates[2];
    memset(predicates, 0, sizeof(predicates));
    predicates[0].column_index = 2;
    predicates[0].op = TINYDB_GENERIC_COMPARE_GTE;
    predicates[0].value = int_value(1000u);
    predicates[1].column_index = 2;
    predicates[1].op = TINYDB_GENERIC_COMPARE_LTE;
    predicates[1].value = int_value(1500u);

    TinyDBGenericIndexCandidates candidates;
    char message[TINYDB_RECORD_MESSAGE_MAX];
    if (!tinydb_generic_index_collect_conjunctive_candidates(table,
                                                             products,
                                                             index,
                                                             predicates,
                                                             2,
                                                             &candidates,
                                                             message,
                                                             sizeof(message))) {
        fprintf(stderr, "conjunctive candidates failed: %s\n", message);
        return false;
    }

    bool ok = candidates.count == 6u;
    for (uint32_t i = 0; ok && i < candidates.count; i++) {
        if (candidates.ids[i] != 10u + i) ok = false;
    }
    if (!ok) {
        fprintf(stderr, "unexpected conjunctive candidate window count=%u\n", candidates.count);
    }
    tinydb_generic_index_candidates_free(&candidates);
    return ok;
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s DATABASE\n", argv[0]);
        return EXIT_FAILURE;
    }

    TinyDB* db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;

    if (!exec_ok(db, "CREATE TABLE products (id INT, name VARCHAR, price INT);") ||
        !exec_ok(db, "CREATE TABLE orders (id INT, product_id INT, quantity INT);")) {
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    Table* table = tinydb_table(db);
    TableSchema* products = table_get_schema(table, "products");
    TableSchema* orders = table_get_schema(table, "orders");
    if (products == NULL || orders == NULL || products == orders) {
        fprintf(stderr, "generic schemas were not created\n");
        tinydb_close(db);
        return EXIT_FAILURE;
    }
    if (products->row_size != 264u || orders->row_size != 12u ||
        products->columns[2].offset != 260u || orders->columns[2].offset != 8u) {
        fprintf(stderr,
                "unexpected layouts: products=%u price_offset=%u orders=%u qty_offset=%u\n",
                products->row_size,
                products->columns[2].offset,
                orders->row_size,
                orders->columns[2].offset);
        tinydb_close(db);
        return EXIT_FAILURE;
    }
    if (!expect_layout_validation(products) || !expect_layout_validation(orders) ||
        !expect_unterminated_varchar_rejected(products)) {
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    char message[TINYDB_RECORD_MESSAGE_MAX];
    for (uint32_t i = 1; i <= 30; i++) {
        char name[64];
        snprintf(name, sizeof(name), "product_%u", i);
        TinyDBValue values[3];
        values[0] = int_value(i);
        values[1] = text_value(name);
        values[2] = int_value(i * 100u);
        if (!tinydb_record_insert(table, products, values, 3, message, sizeof(message))) {
            fprintf(stderr, "product insert %u failed: %s\n", i, message);
            tinydb_close(db);
            return EXIT_FAILURE;
        }
    }

    for (uint32_t i = 1; i <= 20; i++) {
        TinyDBValue values[3];
        values[0] = int_value(i);
        values[1] = int_value((i % 30u) + 1u);
        values[2] = int_value((i % 5u) + 1u);
        if (!tinydb_record_insert(table, orders, values, 3, message, sizeof(message))) {
            fprintf(stderr, "order insert %u failed: %s\n", i, message);
            tinydb_close(db);
            return EXIT_FAILURE;
        }
    }

    if (!exec_ok(db, "CREATE INDEX idx_products_price ON products(price);")) {
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    if (tinydb_record_scan(table, products, NULL, NULL) != 30u ||
        tinydb_record_scan(table, orders, NULL, NULL) != 20u ||
        !expect_product(table, products, 29u, "product_29", 2900u) ||
        !expect_order(table, orders, 19u, 20u, 5u) ||
        !expect_price_window(table, products)) {
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("GENERIC_LAYOUT products_row_size=%u price_offset=%u orders_row_size=%u quantity_offset=%u\n",
           products->row_size,
           products->columns[2].offset,
           orders->row_size,
           orders->columns[2].offset);
    printf("GENERIC_LAYOUT_GUARD overlap=reject gap=reject trailing_gap=reject\n");
    printf("GENERIC_RECORD_CORRUPTION unterminated_varchar=reject\n");
    printf("GENERIC_INDEX_WINDOW price=1000..1500 candidates=6\n");

    tinydb_close(db);
    db = tinydb_open(argv[1]);
    if (db == NULL) return EXIT_FAILURE;
    table = tinydb_table(db);
    products = table_get_schema(table, "products");
    orders = table_get_schema(table, "orders");

    if (products == NULL || orders == NULL ||
        !expect_layout_validation(products) ||
        !expect_layout_validation(orders) ||
        !expect_unterminated_varchar_rejected(products) ||
        tinydb_record_scan(table, products, NULL, NULL) != 30u ||
        tinydb_record_scan(table, orders, NULL, NULL) != 20u ||
        !expect_product(table, products, 29u, "product_29", 2900u) ||
        !expect_order(table, orders, 19u, 20u, 5u) ||
        !expect_price_window(table, products) ||
        !exec_ok(db, "PRAGMA integrity_check;")) {
        tinydb_close(db);
        return EXIT_FAILURE;
    }

    printf("GENERIC_RECORD_OK products=30 orders=20 reopen=yes conjunctive_index=yes layout_guard=yes varchar_corruption_guard=yes\n");
    tinydb_close(db);
    return EXIT_SUCCESS;
}

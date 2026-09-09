#include "column_type.h"

#include <stdio.h>
#include <string.h>

static int failures = 0;

static void expect_parse(const char* text,
                         ColumnType type,
                         uint32_t storage_size,
                         uint32_t declared_capacity,
                         bool explicitly_sized) {
    TinyDBColumnTypeSpec spec;
    if (!tinydb_column_type_parse(text, &spec)) {
        printf("FAIL parse: %s\n", text);
        failures++;
        return;
    }
    if (spec.type != type ||
        spec.storage_size != storage_size ||
        spec.declared_capacity != declared_capacity ||
        spec.explicitly_sized != explicitly_sized) {
        printf("FAIL values: %s type=%d storage=%u capacity=%u sized=%d\n",
               text,
               (int)spec.type,
               spec.storage_size,
               spec.declared_capacity,
               spec.explicitly_sized ? 1 : 0);
        failures++;
    }
}

static void expect_prefix(const char* text,
                          ColumnType type,
                          uint32_t storage_size,
                          uint32_t declared_capacity,
                          bool explicitly_sized,
                          const char* expected_suffix) {
    TinyDBColumnTypeSpec spec;
    const char* end = NULL;
    if (!tinydb_column_type_parse_prefix(text, &spec, &end)) {
        printf("FAIL prefix parse: %s\n", text);
        failures++;
        return;
    }
    if (end == NULL || strcmp(end, expected_suffix) != 0 ||
        spec.type != type ||
        spec.storage_size != storage_size ||
        spec.declared_capacity != declared_capacity ||
        spec.explicitly_sized != explicitly_sized) {
        printf("FAIL prefix values: %s suffix=%s type=%d storage=%u capacity=%u sized=%d\n",
               text,
               end != NULL ? end : "(null)",
               (int)spec.type,
               spec.storage_size,
               spec.declared_capacity,
               spec.explicitly_sized ? 1 : 0);
        failures++;
    }
}

static void expect_reject(const char* text) {
    TinyDBColumnTypeSpec spec;
    if (tinydb_column_type_parse(text, &spec)) {
        printf("FAIL accepted invalid type: %s\n", text);
        failures++;
    }
}

static void expect_format(ColumnType type,
                          uint32_t size,
                          const char* expected) {
    TableColumn column;
    memset(&column, 0, sizeof(column));
    column.type = type;
    column.size = size;

    char output[32];
    if (!tinydb_column_type_format(&column, output, sizeof(output))) {
        printf("FAIL format rejected type=%d size=%u\n", (int)type, size);
        failures++;
        return;
    }
    if (strcmp(output, expected) != 0) {
        printf("FAIL format type=%d size=%u got=%s expected=%s\n",
               (int)type,
               size,
               output,
               expected);
        failures++;
    }
}

int main(void) {
    expect_parse("INT", COL_TYPE_INT, 4u, 0u, false);
    expect_parse("integer", COL_TYPE_INT, 4u, 0u, false);
    expect_parse("  InTeGeR  ", COL_TYPE_INT, 4u, 0u, false);

    expect_parse("VARCHAR", COL_TYPE_VARCHAR, 256u, 255u, false);
    expect_parse("varchar(1)", COL_TYPE_VARCHAR, 2u, 1u, true);
    expect_parse("VARCHAR(32)", COL_TYPE_VARCHAR, 33u, 32u, true);
    expect_parse(" varchar ( 255 ) ", COL_TYPE_VARCHAR, 256u, 255u, true);

    expect_prefix("VARCHAR(32), next INT",
                  COL_TYPE_VARCHAR,
                  33u,
                  32u,
                  true,
                  ", next INT");
    expect_prefix(" varchar ( 7 ) ;",
                  COL_TYPE_VARCHAR,
                  8u,
                  7u,
                  true,
                  " ;");
    expect_prefix("INTEGER, x",
                  COL_TYPE_INT,
                  4u,
                  0u,
                  false,
                  ", x");
    expect_prefix("VARCHAR, x",
                  COL_TYPE_VARCHAR,
                  256u,
                  255u,
                  false,
                  ", x");

    expect_reject("");
    expect_reject("CHAR");
    expect_reject("INT extra");
    expect_reject("VARCHAR(0)");
    expect_reject("VARCHAR(256)");
    expect_reject("VARCHAR(-1)");
    expect_reject("VARCHAR() ");
    expect_reject("VARCHAR(32) extra");

    if (!tinydb_column_type_is_int("INTEGER") ||
        tinydb_column_type_is_int("VARCHAR(4)")) {
        printf("FAIL integer classifier\n");
        failures++;
    }
    if (!tinydb_column_type_is_varchar("VARCHAR(4)") ||
        tinydb_column_type_is_varchar("INT")) {
        printf("FAIL varchar classifier\n");
        failures++;
    }

    expect_format(COL_TYPE_INT, 4u, "INT");
    expect_format(COL_TYPE_VARCHAR, 33u, "VARCHAR(32)");
    expect_format(COL_TYPE_VARCHAR, 256u, "VARCHAR(255)");

    TableColumn invalid;
    memset(&invalid, 0, sizeof(invalid));
    invalid.type = COL_TYPE_INT;
    invalid.size = 8u;
    char output[32];
    if (tinydb_column_type_format(&invalid, output, sizeof(output))) {
        printf("FAIL accepted invalid INT storage size\n");
        failures++;
    }
    invalid.type = COL_TYPE_VARCHAR;
    invalid.size = 0u;
    if (tinydb_column_type_format(&invalid, output, sizeof(output))) {
        printf("FAIL accepted zero-width VARCHAR storage\n");
        failures++;
    }

    if (failures != 0) {
        printf("COLUMN_TYPE_FAIL %d\n", failures);
        return 1;
    }

    printf("COLUMN_TYPE_OK\n");
    return 0;
}

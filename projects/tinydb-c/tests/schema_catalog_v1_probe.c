#include "multitable.h"
#include "table.h"

#include <stdio.h>

bool multitable_catalog_save_v1_base(Table* table, const char* database_filename);

int main(int argc, char* argv[]) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s <database>\n", argv[0]);
        return 2;
    }

    Table* table = db_open(argv[1]);
    if (table == NULL) {
        fprintf(stderr, "unable to open database\n");
        return 1;
    }
    if (!multitable_catalog_load(table, argv[1])) {
        fprintf(stderr, "unable to load current schema catalog\n");
        db_close(table);
        return 1;
    }
    if (!multitable_catalog_save_v1_base(table, argv[1])) {
        fprintf(stderr, "unable to write legacy schema catalog\n");
        db_close(table);
        return 1;
    }

    db_close(table);
    printf("SCHEMA_CATALOG_V1_OK\n");
    return 0;
}

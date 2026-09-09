#ifndef COMPILER_H
#define COMPILER_H

#include "common.h"
#include "table.h"

typedef enum {
    PREPARE_SUCCESS,
    PREPARE_SYNTAX_ERROR,
    PREPARE_UNRECOGNIZED_STATEMENT
} PrepareResult;

typedef enum {
    STATEMENT_INSERT,
    STATEMENT_SELECT,
    STATEMENT_DELETE,
    STATEMENT_BEGIN,
    STATEMENT_COMMIT,
    STATEMENT_ROLLBACK,
    STATEMENT_UPDATE,
    STATEMENT_CREATE_INDEX,
    STATEMENT_DROP_INDEX,
    STATEMENT_VACUUM,
    STATEMENT_SAVEPOINT,
    STATEMENT_ROLLBACK_TO,
    STATEMENT_RELEASE_SAVEPOINT,
    STATEMENT_CHECKPOINT,
    STATEMENT_PRAGMA_INTEGRITY_CHECK,
    STATEMENT_PRAGMA_USER_VERSION,
    STATEMENT_PRAGMA_SET_USER_VERSION,
    STATEMENT_PRAGMA_TABLE_INFO,
    STATEMENT_PRAGMA_INDEX_LIST,
    STATEMENT_CREATE_TABLE,
    STATEMENT_PREPARE,
    STATEMENT_EXECUTE_PREPARED,
    STATEMENT_ALTER_TABLE,
    STATEMENT_CREATE_VIEW,
    STATEMENT_DROP_VIEW
} StatementType;

typedef struct {
    char table_name[32];
    uint32_t num_columns;
    char col_names[16][32];
    char col_types[16][16];
    bool has_fk;
    char fk_col[32];
    char fk_parent_table[32];
    char fk_parent_col[32];
    bool fk_on_delete_cascade;
} CreateTableStatement;

/* COMPARE_EQ must be 0 so zero-initialised statements default to = */
typedef enum {
    COMPARE_EQ = 0,
    COMPARE_GT,
    COMPARE_GTE,
    COMPARE_LT,
    COMPARE_LTE
} CompareOp;

/* AGGREGATE_NONE must be 0 so zero-initialised statements default to no aggregate */
typedef enum {
    AGGREGATE_NONE = 0,
    AGGREGATE_COUNT,
    AGGREGATE_MIN,
    AGGREGATE_MAX,
    AGGREGATE_SUM,
    AGGREGATE_AVG
} AggregateType;

typedef enum {
    STRING_FUNC_NONE = 0,
    STRING_FUNC_LENGTH,
    STRING_FUNC_UPPER,
    STRING_FUNC_LOWER,
    STRING_FUNC_CONCAT
} StringFuncType;

typedef enum {
    SYS_FUNC_NONE = 0,
    SYS_FUNC_VERSION,
    SYS_FUNC_DATABASE
} SysFuncType;

typedef enum {
    MATH_FUNC_NONE = 0,
    MATH_FUNC_ABS,
    MATH_FUNC_MOD
} MathFuncType;

typedef struct SelectStatement {
    char          table_name[32];
    bool          has_id_filter;
    uint32_t      id;
    CompareOp     id_op;       /* operator for the WHERE id clause */
    bool          has_id_min_filter;
    uint32_t      id_min;
    CompareOp     id_min_op;   /* > or >= */
    bool          has_id_max_filter;
    uint32_t      id_max;
    CompareOp     id_max_op;   /* < or <= */
    bool          has_username_filter;
    char          username[COLUMN_USERNAME_SIZE + 1];
    bool          has_username_like;
    char          username_like[COLUMN_USERNAME_SIZE + 1];
    bool          has_email_filter;
    char          email[COLUMN_EMAIL_SIZE + 1];
    bool          has_email_like;
    char          email_like[COLUMN_EMAIL_SIZE + 1];
    AggregateType aggregate;
    bool          has_order_desc; /* ORDER BY id DESC */
    bool          has_limit;
    uint32_t      limit;
    bool          has_join;
    char          join_table[32];
    char          join_left_col[32];
    char          join_right_col[32];
    bool          has_group_by;
    char          group_by_col[32];
    bool          has_project_col;
    char          project_col[32];
    bool          has_having;
    AggregateType having_agg;
    CompareOp     having_op;
    uint32_t      having_val;
    bool          has_in_subquery;
    struct SelectStatement* in_subquery;
    bool          has_exists_subquery;
    struct SelectStatement* exists_subquery;
    bool          is_distinct;
    bool          has_match_filter;
    char          match_keyword[64];
    bool          has_window_func;
    char          window_func_name[32];
    bool          has_partition_by;
    char          partition_col[32];
    bool          has_window_order_by;
    char          window_order_col[32];
    bool          is_union;
    bool          is_union_all;
    char          union_second_select[256];
    bool          has_is_null_filter;
    bool          has_is_not_null_filter;
    char          null_target_col[32];
    bool          has_order_by_col;
    char          order_by_col[32];
    bool          has_secondary_order_by;
    char          secondary_order_col[32];
    bool          secondary_order_desc;
    bool          has_in_list;
    uint32_t      in_list_ids[32];
    uint32_t      in_list_count;
    bool          has_between_filter;
    uint32_t      between_min;
    uint32_t      between_max;
    bool          is_not_in_list;
    bool          is_not_like;
    bool          is_ilike;
    bool          has_scalar_subquery;
    char          scalar_subquery_sql[256];
    bool          is_catalog_query;
    bool          has_offset;
    uint32_t      offset;
    StringFuncType str_func;
    char           str_func_target_col[32];
    char           str_func_second_col[32];
    SysFuncType    sys_func;
    MathFuncType   math_func;
    char           math_target_col[32];
    uint32_t       math_operand;
} SelectStatement;

typedef struct {
    bool set_username;
    char username[COLUMN_USERNAME_SIZE + 1];
    bool set_email;
    char email[COLUMN_EMAIL_SIZE + 1];
    uint32_t id;
} UpdateStatement;

typedef struct {
    char name[64];
} SavepointStatement;

typedef struct {
    uint32_t user_version;
} PragmaStatement;

typedef struct {
    char name[32];
    char sql_template[256];
} PrepareStatement;

typedef struct {
    char name[32];
    uint32_t param_val;
} ExecutePreparedStatement;

typedef struct {
    char name[64];
    char table_name[32];
    char column_name[32];
    char column_name2[32];
    uint32_t num_columns;
} CreateIndexStatement;

typedef struct {
    char name[64];
} DropIndexStatement;

typedef struct {
    char table_name[32];
    bool is_rename;
    char new_table_name[32];
    bool is_add_column;
    char new_col_name[32];
    char new_col_type[16];
} AlterTableStatement;

typedef struct {
    char view_name[32];
    char select_sql[256];
} CreateViewStatement;

typedef struct {
    char view_name[32];
} DropViewStatement;

typedef struct {
    bool has_into;
    char into_filename[256];
} VacuumStatement;

typedef struct {
    StatementType type;
    bool explain;
    char table_name[32];
    Row row_to_insert;
    bool is_auto_id;
    SelectStatement select;
    CreateTableStatement create_table;
    CreateIndexStatement create_index;
    DropIndexStatement drop_index;
    PrepareStatement prepare;
    ExecutePreparedStatement execute_prepared;
    uint32_t delete_id;
    bool delete_all;
    UpdateStatement update;
    SavepointStatement savepoint;
    PragmaStatement pragma;
    AlterTableStatement alter_table;
    CreateViewStatement create_view;
    DropViewStatement drop_view;
    VacuumStatement vacuum;
    bool has_cte;
    char cte_name[32];
    char cte_select_sql[256];
} Statement;

PrepareResult prepare_statement(const char* input, Statement* statement);

#endif // COMPILER_H

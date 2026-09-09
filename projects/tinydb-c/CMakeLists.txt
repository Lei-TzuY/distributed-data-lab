cmake_minimum_required(VERSION 3.10)
project(TinyDB C)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED True)

if(MSVC)
    add_compile_options(/W4 /WX /utf-8)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Werror)
endif()

set(TINYDB_CORE_SOURCES
    src/alter_varchar_policy.c
    src/analyze_sql.c
    src/catalog_pragmas.c
    src/column_type.c
    src/compiler.c
    src/create_table_policy.c
    src/database_diagnostics.c
    src/diagnostics.c
    src/diagnostics_inspection.c
    src/engine.c
    src/engine_open_guard.c
    src/integrity_compat.c
    src/user_version.c
    src/generic_boolean.c
    src/generic_compound_plan_guard.c
    src/generic_error_status.c
    src/generic_wide_range_route.c
    src/generic_wide_or_route.c
    src/generic_wide_grouped_route.c
    src/generic_wide_statement_atomicity.c
    src/generic_wide_plan_route.c
    src/generic_wide_boolean_index_route.c
    src/generic_wide_boolean_cost_route.c
    src/generic_wide_order_route.c
    src/generic_index_anchor_v2_sql.c
    src/generic_index_candidates.c
    src/generic_index_correlation.c
    src/generic_index_correlation_sql.c
    src/generic_index_intersection_forced_sql.c
    src/generic_index_stats.c
    src/generic_index_stats_refresh.c
    src/generic_index_estimate_policy.c
    src/generic_index_ddl.c
    src/generic_index_drop.c
    src/generic_index_epoch.c
    src/generic_index_intersection_sql.c
    src/generic_index_policy.c
    src/generic_index_range_v2_sql.c
    src/generic_index_single_cost_sql.c
    src/generic_index_record_guard.c
    src/generic_index_snapshot_sql.c
    src/generic_index_sql.c
    src/generic_index_union_sql.c
    src/generic_index_union_cost_sql.c
    src/generic_like_route_sql.c
    src/generic_or_mutation_sql.c
    src/generic_or_sql.c
    src/generic_parenthesized_sql.c
    src/generic_predicate.c
    src/generic_predicate_plan_sql.c
    src/generic_predicate_sql.c
    src/generic_range_mutation_sql.c
    src/generic_range_plan_sql.c
    src/generic_range_select_sql.c
    src/generic_sql.c
    src/join_plan.c
    src/leaf_cursor_read.c
    src/leaf_format.c
    src/leaf_migration.c
    src/leaf_mutation_policy.c
    src/leaf_page_access.c
    src/leaf_value.c
    src/multi_vacuum_into.c
    src/multitable.c
    src/schema_catalog_v2.c
    src/schema_catalog_guard.c
    src/sized_table_alter.c
    src/sized_table_create.c
    src/pager.c
    src/pager_checkpoint_order.c
    src/prepared_route.c
    src/profile.c
    src/record.c
    src/record_delete_v2.c
    src/record_delete_v2_empty_route.c
    src/record_insert_v2_split.c
    src/record_insert_v2_nonroot_overflow.c
    src/record_insert_v2_recursive_overflow.c
    src/record_insert_v2_root_overflow.c
    src/record_payload.c
    src/record_payload_try_scan.c
    src/record_payload_insert.c
    src/record_payload_nonroot_internal_split.c
    src/record_payload_nonroot_overflow_route.c
    src/record_payload_nonroot_split.c
    src/record_payload_recursive_chain.c
    src/record_payload_recursive_route.c
    src/record_payload_root_internal_split.c
    src/record_payload_root_split.c
    src/record_read_v2.c
    src/record_update_payload_route.c
    src/slotted_leaf_v2.c
    src/table.c
    src/varchar_width_parser.c
    src/vm.c
)

set_source_files_properties(
    src/compiler.c
    PROPERTIES COMPILE_DEFINITIONS
    "prepare_statement=prepare_statement_legacy_base"
)

set_source_files_properties(
    src/engine.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_execute_sql=tinydb_execute_sql_base;tinydb_open=tinydb_open_legacy_base"
)
set_source_files_properties(
    src/create_table_policy.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_execute_sql=tinydb_execute_sql_create_policy_base"
)
set_source_files_properties(
    src/multi_vacuum_into.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_execute_sql=tinydb_execute_sql_alter_delegate_base"
)

set_source_files_properties(
    src/generic_index_candidates.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_index_estimate_candidates=tinydb_generic_index_estimate_candidates_exact_base;tinydb_generic_index_estimate_conjunctive_candidates=tinydb_generic_index_estimate_conjunctive_candidates_exact_base"
)
set_source_files_properties(
    src/generic_index_stats.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_index_estimate_candidates=tinydb_generic_index_estimate_candidates_stats_base;tinydb_generic_index_estimate_conjunctive_candidates=tinydb_generic_index_estimate_conjunctive_candidates_stats_base"
)

set_source_files_properties(
    src/generic_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_base"
)
set_source_files_properties(
    src/generic_range_mutation_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_range_mutations"
)
set_source_files_properties(
    src/generic_range_select_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_predicate_base"
)
set_source_files_properties(
    src/generic_predicate_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_range_select"
)
set_source_files_properties(
    src/generic_error_status.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_wide_base"
)
set_source_files_properties(
    src/generic_wide_range_route.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_wide_range_base"
)
set_source_files_properties(
    src/generic_wide_or_route.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_wide_or_base"
)
set_source_files_properties(
    src/generic_wide_grouped_route.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_wide_grouped_base"
)
set_source_files_properties(
    src/generic_wide_statement_atomicity.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_wide_atomic_base"
)
set_source_files_properties(
    src/generic_wide_plan_route.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_wide_boolean_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_wide_boolean_base"
)
set_source_files_properties(
    src/generic_wide_boolean_index_route.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_wide_boolean_index_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_wide_boolean_index_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_wide_boolean_index_base"
)
set_source_files_properties(
    src/generic_wide_boolean_cost_route.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_wide_order_base"
)
set_source_files_properties(
    src/generic_range_plan_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_range_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_range_base"
)
set_source_files_properties(
    src/generic_predicate_plan_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_range;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_range;tinydb_generic_sql_build_select_plan_predicate_base=tinydb_generic_sql_build_select_plan_range_base;tinydb_generic_sql_print_plan_predicate_base=tinydb_generic_sql_print_plan_range_base"
)
set_source_files_properties(
    src/generic_index_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_index_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_index_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_index_base"
)
set_source_files_properties(
    src/generic_index_snapshot_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_snapshot_base"
)
if(NOT MSVC)
    set_source_files_properties(
        src/generic_index_snapshot_sql.c
        PROPERTIES COMPILE_OPTIONS "-Wno-type-limits"
    )
endif()
set_source_files_properties(
    src/generic_index_range_v2_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_range_index_uncosted_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_range_index_uncosted_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_range_index_uncosted_base"
)
set_source_files_properties(
    src/generic_index_anchor_v2_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_single_anchor_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_single_anchor_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_single_anchor_base"
)
set_source_files_properties(
    src/generic_index_intersection_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_intersection_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_intersection_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_intersection_base"
)
set_source_files_properties(
    src/generic_index_correlation_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_anchor_index_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_anchor_index_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_anchor_index_base"
)
set_source_files_properties(
    src/generic_or_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_or_scan_base;tinydb_generic_sql_try_execute_index_base=tinydb_generic_sql_try_execute_anchor_index_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_or_scan_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_or_scan_base;tinydb_generic_sql_build_select_plan_or_base=tinydb_generic_sql_build_select_plan_anchor_index_base;tinydb_generic_sql_print_plan_or_base=tinydb_generic_sql_print_plan_anchor_index_base"
)
set_source_files_properties(
    src/generic_index_union_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute_or_select_base=tinydb_generic_sql_try_execute_union_uncosted_base;tinydb_generic_sql_build_select_plan_parenthesized_base=tinydb_generic_sql_build_select_plan_union_uncosted_base;tinydb_generic_sql_print_plan_parenthesized_base=tinydb_generic_sql_print_plan_union_uncosted_base"
)
set_source_files_properties(
    src/generic_like_route_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_or_select_base;tinydb_generic_sql_try_execute_like_base=tinydb_generic_sql_try_execute_union_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_parenthesized_base;tinydb_generic_sql_build_select_plan_like_base=tinydb_generic_sql_build_select_plan_union_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_parenthesized_base;tinydb_generic_sql_print_plan_like_base=tinydb_generic_sql_print_plan_union_base"
)
set_source_files_properties(
    src/generic_or_mutation_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_parenthesized_base"
)
set_source_files_properties(
    src/generic_parenthesized_sql.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_try_execute=tinydb_generic_sql_try_execute_diagnostic_base;tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_wide_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_wide_base"
)
set_source_files_properties(
    src/generic_compound_plan_guard.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_generic_sql_build_select_plan=tinydb_generic_sql_build_select_plan_compound_base;tinydb_generic_sql_print_plan=tinydb_generic_sql_print_plan_compound_base"
)

set_source_files_properties(
    src/generic_index_record_guard.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_record_insert=tinydb_record_insert_mixed_base;tinydb_record_update=tinydb_record_update_mixed_legacy_base;tinydb_record_delete=tinydb_record_delete_mixed_base"
)
set_source_files_properties(
    src/record.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_record_insert=tinydb_record_insert_base;tinydb_record_update=tinydb_record_update_base;tinydb_record_delete=tinydb_record_delete_base;tinydb_record_delete_all=tinydb_record_delete_all_base;tinydb_record_find=tinydb_record_find_v1_base;tinydb_record_scan=tinydb_record_scan_v1_base"
)
set_source_files_properties(
    src/record_delete_v2.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_record_delete=tinydb_record_delete_v2_empty_base"
)
set_source_files_properties(
    src/record_insert_v2_split.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_record_insert=tinydb_record_insert_v2_split_base"
)
set_source_files_properties(
    src/record_insert_v2_nonroot_overflow.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_record_insert=tinydb_record_insert_v2_nonroot_overflow_base"
)
set_source_files_properties(
    src/record_insert_v2_recursive_overflow.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_record_insert=tinydb_record_insert_v2_recursive_overflow_base"
)
set_source_files_properties(
    src/record_payload_nonroot_split.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_record_payload_try_nonroot_split=tinydb_record_payload_try_nonroot_split_nonfull_base"
)
set_source_files_properties(
    src/record_payload_nonroot_overflow_route.c
    PROPERTIES COMPILE_DEFINITIONS
    "tinydb_record_payload_try_nonroot_split=tinydb_record_payload_try_nonroot_split_recursive_preflight_base"
)

set_source_files_properties(
    src/multitable.c
    PROPERTIES COMPILE_DEFINITIONS
    "multitable_catalog_load=multitable_catalog_load_v1_base;multitable_catalog_save=multitable_catalog_save_v1_base;multitable_index_target_supported=multitable_index_target_supported_legacy"
)
set_source_files_properties(
    src/schema_catalog_v2.c
    PROPERTIES COMPILE_DEFINITIONS
    "multitable_catalog_load=multitable_catalog_load_checksums_base"
)

set_source_files_properties(
    src/table.c
    PROPERTIES COMPILE_DEFINITIONS
    "table_create_table=table_create_table_legacy_base;table_add_column=table_add_column_legacy_base;table_create_index=table_create_index_legacy;table_drop_index=table_drop_index_legacy;table_find=table_find_v1_base;table_start=table_start_v1_base;cursor_value=cursor_value_v1_base;cursor_advance=cursor_advance_v1_base;table_end=table_end_v1_base;cursor_retreat=cursor_retreat_v1_base;db_integrity_check=db_integrity_check_legacy_base"
)
set_source_files_properties(
    src/pager.c
    PROPERTIES COMPILE_DEFINITIONS
    "pager_checkpoint=pager_checkpoint_legacy_base"
)

add_library(tinydb_core STATIC ${TINYDB_CORE_SOURCES})
target_include_directories(tinydb_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_executable(tinydb src/main.c)
target_link_libraries(tinydb PRIVATE tinydb_core)

add_executable(tinydb_bench bench/benchmark.c)
target_link_libraries(tinydb_bench PRIVATE tinydb_core)

add_executable(tinydb_generic_bench bench/generic_benchmark.c)
target_link_libraries(tinydb_generic_bench PRIVATE tinydb_core)

add_executable(tinydb_pager_growth_probe tests/pager_growth_probe.c)
target_link_libraries(tinydb_pager_growth_probe PRIVATE tinydb_core)

add_executable(tinydb_engine_api_probe tests/engine_api_probe.c)
target_link_libraries(tinydb_engine_api_probe PRIVATE tinydb_core)

add_executable(tinydb_page_ownership_probe tests/page_ownership_probe.c)
target_link_libraries(tinydb_page_ownership_probe PRIVATE tinydb_core)

add_executable(tinydb_integrity_api_pin_probe tests/integrity_api_pin_probe.c)
target_link_libraries(tinydb_integrity_api_pin_probe PRIVATE tinydb_core)

add_executable(tinydb_user_version_api_pin_probe tests/user_version_api_pin_probe.c)
target_link_libraries(tinydb_user_version_api_pin_probe PRIVATE tinydb_core)

add_executable(tinydb_generic_record_probe tests/generic_record_probe.c)
target_link_libraries(tinydb_generic_record_probe PRIVATE tinydb_core)

add_executable(tinydb_schema_catalog_v1_probe tests/schema_catalog_v1_probe.c)
target_link_libraries(tinydb_schema_catalog_v1_probe PRIVATE tinydb_core)

add_executable(tinydb_committed_crash_probe tests/committed_crash_probe.c)
target_link_libraries(tinydb_committed_crash_probe PRIVATE tinydb_core)

add_executable(tinydb_column_type_probe tests/column_type_probe.c)
target_link_libraries(tinydb_column_type_probe PRIVATE tinydb_core)

add_executable(tinydb_record_payload_probe tests/record_payload_probe.c)
target_link_libraries(tinydb_record_payload_probe PRIVATE tinydb_core)

add_executable(tinydb_leaf_value_probe tests/leaf_value_probe.c)
target_link_libraries(tinydb_leaf_value_probe PRIVATE tinydb_core)

add_executable(tinydb_slotted_leaf_v2_probe tests/slotted_leaf_v2_probe.c)
target_link_libraries(tinydb_slotted_leaf_v2_probe PRIVATE tinydb_core)

add_executable(tinydb_leaf_format_discriminator_probe tests/leaf_format_discriminator_probe.c)
target_link_libraries(tinydb_leaf_format_discriminator_probe PRIVATE tinydb_core)

add_executable(tinydb_leaf_migration_probe tests/leaf_migration_probe.c)
target_link_libraries(tinydb_leaf_migration_probe PRIVATE tinydb_core)

add_executable(tinydb_dual_leaf_read_probe tests/dual_leaf_read_probe.c)
target_link_libraries(tinydb_dual_leaf_read_probe PRIVATE tinydb_core)

add_executable(tinydb_row_envelope_probe tests/row_envelope_probe.c)
target_link_libraries(tinydb_row_envelope_probe PRIVATE tinydb_core)

add_executable(tinydb_compact_v2_read_probe tests/compact_v2_read_probe.c)
target_link_libraries(tinydb_compact_v2_read_probe PRIVATE tinydb_core)

add_executable(tinydb_slotted_v2_split_insert_probe tests/slotted_v2_split_insert_probe.c)
target_link_libraries(tinydb_slotted_v2_split_insert_probe PRIVATE tinydb_core)
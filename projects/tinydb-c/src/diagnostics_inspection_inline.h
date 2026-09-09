#ifndef TINYDB_DIAGNOSTICS_INSPECTION_INLINE_H
#define TINYDB_DIAGNOSTICS_INSPECTION_INLINE_H

/*
 * Diagnostics-aware source callers keep the historical .page/.btree routing
 * macros, but the non-fatal implementations are real tinydb_core symbols.
 * table.c retains the original void print_page()/print_tree() ABI symbols for
 * callers that do not opt into diagnostics.h.
 */
bool tinydb_print_tree_nonfatal(Pager* pager,
                                uint32_t page_num,
                                uint32_t indentation_level);

bool tinydb_print_page_nonfatal(Table* table,
                                uint32_t page_num);

#define print_tree tinydb_print_tree_nonfatal
#define print_page tinydb_print_page_nonfatal

#endif /* TINYDB_DIAGNOSTICS_INSPECTION_INLINE_H */

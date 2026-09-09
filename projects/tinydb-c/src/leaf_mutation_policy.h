#ifndef LEAF_MUTATION_POLICY_H
#define LEAF_MUTATION_POLICY_H

#include "table.h"

#include <stddef.h>

/* Returns true only while every leaf reachable through the tree's ordered
 * sibling chain is the production fixed-V1 format. V2 remains read-only until
 * its mutation/split/WAL semantics are production-ready. */
bool tinydb_leaf_tree_mutation_supported(Table* table,
                                         uint32_t root_page_num,
                                         char* message,
                                         size_t message_size);

#endif /* LEAF_MUTATION_POLICY_H */

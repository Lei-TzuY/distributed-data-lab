import os
import re
import sys


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def function_body(source, name):
    marker = f"static bool {name}("
    start = source.find(marker)
    require(start >= 0, f"missing {name}")
    brace = source.find("{", start)
    require(brace >= 0, f"missing body for {name}")
    depth = 0
    for index in range(brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[brace + 1:index]
    raise AssertionError(f"unterminated body for {name}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    source_path = os.path.join(repo_root, "src", "leaf_cursor_read.c")
    with open(source_path, "r", encoding="utf-8") as handle:
        source = handle.read()

    guard = function_body(source, "internal_routing_valid")
    require("num_keys == 0u || num_keys > INTERNAL_NODE_MAX_KEYS" in guard,
            "internal route guard must reject empty/oversized internal nodes")
    require("separator <= previous_key" in guard,
            "internal route guard must reject duplicate/unsorted separators")
    require("child_page == page_num || child_page == right_child" in guard,
            "internal route guard must reject self/duplicate-right child identities")
    require(re.search(r"internal_node_child\(node, j\) == child_page", guard) is not None,
            "internal route guard must reject duplicate non-rightmost children")
    require("get_page(" not in guard,
            "internal route guard must not fetch children and evict its own node frame")
    require("node_parent(" not in guard,
            "read routing must not reject otherwise valid trees solely on maintenance backlinks")

    internal_find_start = source.find("static Cursor* internal_find(")
    internal_find_end = source.find("Cursor* tinydb_leaf_read_find", internal_find_start)
    require(internal_find_start >= 0 and internal_find_end > internal_find_start,
            "missing mixed-format internal find path")
    internal_find = source[internal_find_start:internal_find_end]
    guard_pos = internal_find.find("internal_routing_valid(table, page_num, node)")
    route_pos = internal_find.find("internal_node_find_child(node, key)")
    load_pos = internal_find.find("get_page(table->pager, child_page)")
    require(guard_pos >= 0 and route_pos > guard_pos and load_pos > route_pos,
            "mixed-format lookup must validate and route before loading only the selected child")
    require("get_node_type(child) == NODE_LEAF" in internal_find and
            "get_node_type(child) == NODE_INTERNAL" in internal_find,
            "the selected child must still be type-checked after it is loaded")

    end_start = source.find("Cursor* tinydb_leaf_read_end(")
    end_end = source.find("bool tinydb_leaf_read_retreat_checked", end_start)
    require(end_start >= 0 and end_end > end_start,
            "missing mixed-format rightmost traversal path")
    end_body = source[end_start:end_end]
    guard_pos = end_body.find("internal_routing_valid(table, page_num, node)")
    right_pos = end_body.find("internal_node_right_child(node)")
    load_pos = end_body.find("get_page(table->pager, page_num)", right_pos)
    require(guard_pos >= 0 and right_pos > guard_pos and load_pos > right_pos,
            "rightmost traversal must validate the internal image before loading its selected child")

    print(
        "PASS: mixed-format lookup/rightmost traversal reject corrupt local internal routing "
        "metadata without walking every child and invalidating the bounded Pager frame."
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)

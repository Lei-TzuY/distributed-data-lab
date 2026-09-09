import os
import sys


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    source_path = os.path.join(repo_root, "src", "leaf_cursor_read.c")
    with open(source_path, "r", encoding="utf-8") as handle:
        source = handle.read()

    required = [
        "tinydb_leaf_page_prev(next_page, PAGE_SIZE, &back_link)",
        "back_link == current_page",
        "tinydb_leaf_page_next(prev_page, PAGE_SIZE, &forward_link)",
        "forward_link == current_page",
        "!reciprocal_forward_transition(table, current_page, next)",
        "!reciprocal_backward_transition(table, current_page, prev)",
    ]
    missing = [fragment for fragment in required if fragment not in source]
    if missing:
        raise AssertionError(
            "mixed-format cursor lost reciprocal sibling validation: "
            + ", ".join(missing)
        )

    forward_guard = source.index(
        "!reciprocal_forward_transition(table, current_page, next)"
    )
    forward_order = source.index(
        "!ordered_forward_transition(table,", forward_guard
    )
    backward_guard = source.index(
        "!reciprocal_backward_transition(table, current_page, prev)"
    )
    backward_order = source.index(
        "!ordered_backward_transition(table,", backward_guard
    )
    if forward_guard >= forward_order or backward_guard >= backward_order:
        raise AssertionError(
            "reciprocal sibling checks must run before cross-leaf key-order checks"
        )

    print(
        "PASS: mixed-format cursor traversal requires reciprocal next/prev sibling "
        "links in both directions before accepting cross-leaf key ordering."
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)

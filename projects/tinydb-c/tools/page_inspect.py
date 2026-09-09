#!/usr/bin/env python3
"""Offline TinyDB page inspector.

This tool does not open the database through TinyDB. It reads the on-disk
4096-byte pages directly, verifies FNV-1a checksums, decodes B+ tree headers,
and performs lightweight pointer/key sanity checks. That makes it useful when
the engine itself refuses to open a damaged database.
"""

from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

PAGE_SIZE = 4096
PAGE_CHECKSUM_SIZE = 4
PAGE_USABLE_SIZE = PAGE_SIZE - PAGE_CHECKSUM_SIZE

NODE_INTERNAL = 0
NODE_LEAF = 1

NODE_TYPE_OFFSET = 0
IS_ROOT_OFFSET = 1
PARENT_POINTER_OFFSET = 2
COMMON_NODE_HEADER_SIZE = 6

LEAF_NUM_CELLS_OFFSET = COMMON_NODE_HEADER_SIZE
LEAF_NEXT_OFFSET = LEAF_NUM_CELLS_OFFSET + 4
LEAF_PREV_OFFSET = LEAF_NEXT_OFFSET + 4
LEAF_HEADER_SIZE = COMMON_NODE_HEADER_SIZE + 12
ROW_SIZE = 4 + 33 + 256
LEAF_CELL_SIZE = 4 + ROW_SIZE
LEAF_MAX_CELLS = (PAGE_USABLE_SIZE - LEAF_HEADER_SIZE) // LEAF_CELL_SIZE

INTERNAL_NUM_KEYS_OFFSET = COMMON_NODE_HEADER_SIZE
INTERNAL_RIGHT_CHILD_OFFSET = INTERNAL_NUM_KEYS_OFFSET + 4
INTERNAL_HEADER_SIZE = COMMON_NODE_HEADER_SIZE + 8
INTERNAL_CELL_SIZE = 8
INTERNAL_MAX_KEYS = (PAGE_USABLE_SIZE - INTERNAL_HEADER_SIZE) // INTERNAL_CELL_SIZE


@dataclass
class PageInfo:
    page: int
    node_type: str
    is_root: bool
    parent: int
    checksum_stored: str
    checksum_computed: str
    checksum_ok: bool
    cells_or_keys: int
    next_leaf: int | None = None
    prev_leaf: int | None = None
    right_child: int | None = None
    min_key: int | None = None
    max_key: int | None = None


def u32(page: bytes, offset: int) -> int:
    return struct.unpack_from("<I", page, offset)[0]


def fnv1a_32(data: bytes) -> int:
    value = 2166136261
    for byte in data:
        value ^= byte
        value = (value * 16777619) & 0xFFFFFFFF
    return value


def decode_page(raw: bytes, page_num: int, page_count: int) -> tuple[PageInfo, list[str]]:
    problems: list[str] = []
    stored = u32(raw, PAGE_USABLE_SIZE)
    computed = fnv1a_32(raw[:PAGE_USABLE_SIZE])
    node_kind = raw[NODE_TYPE_OFFSET]
    is_root = bool(raw[IS_ROOT_OFFSET])
    parent = u32(raw, PARENT_POINTER_OFFSET)

    if node_kind == NODE_LEAF:
        num_cells = u32(raw, LEAF_NUM_CELLS_OFFSET)
        next_leaf = u32(raw, LEAF_NEXT_OFFSET)
        prev_leaf = u32(raw, LEAF_PREV_OFFSET)
        min_key = None
        max_key = None

        if num_cells > LEAF_MAX_CELLS:
            problems.append(
                f"page {page_num}: leaf cell count {num_cells} exceeds {LEAF_MAX_CELLS}"
            )
        else:
            keys = [
                u32(raw, LEAF_HEADER_SIZE + index * LEAF_CELL_SIZE)
                for index in range(num_cells)
            ]
            if keys:
                min_key, max_key = keys[0], keys[-1]
                if keys != sorted(keys):
                    problems.append(f"page {page_num}: leaf keys are not sorted")
                if len(keys) != len(set(keys)):
                    problems.append(f"page {page_num}: duplicate leaf keys detected")

        for label, pointer in (("next_leaf", next_leaf), ("prev_leaf", prev_leaf)):
            if pointer != 0 and pointer >= page_count:
                problems.append(
                    f"page {page_num}: {label} points outside file ({pointer} >= {page_count})"
                )

        info = PageInfo(
            page=page_num,
            node_type="leaf",
            is_root=is_root,
            parent=parent,
            checksum_stored=f"0x{stored:08x}",
            checksum_computed=f"0x{computed:08x}",
            checksum_ok=stored == computed,
            cells_or_keys=num_cells,
            next_leaf=next_leaf,
            prev_leaf=prev_leaf,
            min_key=min_key,
            max_key=max_key,
        )
    elif node_kind == NODE_INTERNAL:
        num_keys = u32(raw, INTERNAL_NUM_KEYS_OFFSET)
        right_child = u32(raw, INTERNAL_RIGHT_CHILD_OFFSET)
        min_key = None
        max_key = None

        if num_keys > INTERNAL_MAX_KEYS:
            problems.append(
                f"page {page_num}: internal key count {num_keys} exceeds {INTERNAL_MAX_KEYS}"
            )
        else:
            keys: list[int] = []
            children: list[int] = []
            for index in range(num_keys):
                cell_offset = INTERNAL_HEADER_SIZE + index * INTERNAL_CELL_SIZE
                children.append(u32(raw, cell_offset))
                keys.append(u32(raw, cell_offset + 4))
            if keys:
                min_key, max_key = keys[0], keys[-1]
                if keys != sorted(keys):
                    problems.append(f"page {page_num}: internal separator keys are not sorted")
            children.append(right_child)
            for child in children:
                if child >= page_count:
                    problems.append(
                        f"page {page_num}: child pointer {child} points outside file"
                    )

        info = PageInfo(
            page=page_num,
            node_type="internal",
            is_root=is_root,
            parent=parent,
            checksum_stored=f"0x{stored:08x}",
            checksum_computed=f"0x{computed:08x}",
            checksum_ok=stored == computed,
            cells_or_keys=num_keys,
            right_child=right_child,
            min_key=min_key,
            max_key=max_key,
        )
    else:
        problems.append(f"page {page_num}: unknown node type byte {node_kind}")
        info = PageInfo(
            page=page_num,
            node_type=f"unknown({node_kind})",
            is_root=is_root,
            parent=parent,
            checksum_stored=f"0x{stored:08x}",
            checksum_computed=f"0x{computed:08x}",
            checksum_ok=stored == computed,
            cells_or_keys=0,
        )

    if stored != computed:
        problems.append(
            f"page {page_num}: checksum mismatch stored=0x{stored:08x} computed=0x{computed:08x}"
        )
    if is_root and page_num != 0:
        problems.append(f"page {page_num}: nonzero page is marked as root")
    if page_num == 0 and not is_root:
        problems.append("page 0: root page is not marked as root")

    return info, problems


def inspect_database(path: Path) -> dict[str, object]:
    size = path.stat().st_size
    if size == 0:
        raise ValueError("database file is empty")
    if size % PAGE_SIZE != 0:
        raise ValueError(
            f"file size {size} is not a multiple of TinyDB page size {PAGE_SIZE}"
        )

    page_count = size // PAGE_SIZE
    pages: list[PageInfo] = []
    problems: list[str] = []

    with path.open("rb") as handle:
        for page_num in range(page_count):
            raw = handle.read(PAGE_SIZE)
            if len(raw) != PAGE_SIZE:
                raise ValueError(f"short read while loading page {page_num}")
            info, page_problems = decode_page(raw, page_num, page_count)
            pages.append(info)
            problems.extend(page_problems)

    leaf_pages = [page for page in pages if page.node_type == "leaf"]
    internal_pages = [page for page in pages if page.node_type == "internal"]
    checksum_failures = sum(not page.checksum_ok for page in pages)
    total_rows = sum(page.cells_or_keys for page in leaf_pages)

    return {
        "path": str(path),
        "file_size": size,
        "page_size": PAGE_SIZE,
        "page_count": page_count,
        "leaf_pages": len(leaf_pages),
        "internal_pages": len(internal_pages),
        "root_pages": sum(page.is_root for page in pages),
        "total_rows": total_rows,
        "checksum_failures": checksum_failures,
        "ok": not problems,
        "problems": problems,
        "pages": [asdict(page) for page in pages],
    }


def print_text(report: dict[str, object], show_pages: bool) -> None:
    print(
        "TinyDB page report: "
        f"pages={report['page_count']} leaf={report['leaf_pages']} "
        f"internal={report['internal_pages']} rows={report['total_rows']} "
        f"checksum_failures={report['checksum_failures']} ok={str(report['ok']).lower()}"
    )

    if show_pages:
        for page in report["pages"]:
            assert isinstance(page, dict)
            line = (
                f"page={page['page']} type={page['node_type']} root={page['is_root']} "
                f"parent={page['parent']} count={page['cells_or_keys']} "
                f"checksum_ok={page['checksum_ok']}"
            )
            if page.get("min_key") is not None:
                line += f" keys={page['min_key']}..{page['max_key']}"
            if page.get("next_leaf") is not None:
                line += f" prev={page['prev_leaf']} next={page['next_leaf']}"
            if page.get("right_child") is not None:
                line += f" right_child={page['right_child']}"
            print(line)

    problems = report["problems"]
    assert isinstance(problems, list)
    for problem in problems:
        print(f"PROBLEM: {problem}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("database", type=Path)
    parser.add_argument("--json", action="store_true", dest="as_json")
    parser.add_argument("--pages", action="store_true", help="print one line per page")
    parser.add_argument(
        "--strict",
        action="store_true",
        help="return exit code 1 when checksum/header/pointer checks find a problem",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if not args.database.is_file():
        print(f"database file not found: {args.database}", file=sys.stderr)
        return 2

    try:
        report = inspect_database(args.database)
    except (OSError, ValueError) as exc:
        print(f"inspection failed: {exc}", file=sys.stderr)
        return 2

    if args.as_json:
        print(json.dumps(report, indent=2, sort_keys=True))
    else:
        print_text(report, args.pages)

    return 1 if args.strict and not report["ok"] else 0


if __name__ == "__main__":
    raise SystemExit(main())

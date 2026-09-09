from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]
HEADER = (ROOT / "src" / "record_payload.h").read_text(encoding="utf-8")
TRY_HEADER = (ROOT / "src" / "record_payload_try_find.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "src" / "record_read_v2.c").read_text(encoding="utf-8")
TRY_SOURCE = (ROOT / "src" / "record_payload.c").read_text(encoding="utf-8")


def function_body(name: str, source: str = SOURCE) -> str:
    marker = name + "("
    start = source.find(marker)
    if start < 0:
        raise AssertionError(f"missing function {name}")
    brace = source.find("{", start)
    if brace < 0:
        raise AssertionError(f"missing body for {name}")
    depth = 0
    for i in range(brace, len(source)):
        if source[i] == "{":
            depth += 1
        elif source[i] == "}":
            depth -= 1
            if depth == 0:
                return source[brace : i + 1]
    raise AssertionError(f"unterminated body for {name}")


def require(text: str, needle: str, context: str) -> None:
    if needle not in text:
        raise AssertionError(f"{context}: missing {needle!r}")


def main() -> int:
    require(HEADER, "TinyDBRecordPayloadVisitor", "payload visitor API")
    require(HEADER, "tinydb_record_payload_find", "payload point-read API")
    require(HEADER, "tinydb_record_payload_scan", "payload scan API")
    require(TRY_HEADER, "tinydb_record_payload_try_find", "non-fatal payload point-read API")

    raw = function_body("raw_value_to_payload")
    require(raw, "schema->row_size > sizeof(payload->bytes)", "payload capacity guard")
    if "schema->row_size > ROW_SIZE" in raw:
        raise AssertionError("payload-native decoder regressed to the legacy ROW_SIZE ceiling")
    require(raw, "tinydb_row_envelope_decode", "compact V2 decode")
    require(raw, "TINYDB_LEAF_PAGE_FORMAT_FIXED_V1", "V1 compatibility")
    require(raw, "TINYDB_LEAF_PAGE_FORMAT_SLOTTED_V2", "V2 compatibility")

    find = function_body("tinydb_record_payload_find")
    require(find, "tinydb_record_payload_schema_supported", "payload find schema guard")
    require(find, "tinydb_leaf_read_find", "format-aware point lookup")
    require(find, "cursor_to_payload", "payload point decode")
    if "tinydb_schema_supports_records" in find or "tinydb_record_payload_to_record" in find:
        raise AssertionError("payload find must not round-trip through the legacy TinyDBRecord carrier")

    safe_find = function_body("tinydb_record_payload_try_find", TRY_SOURCE)
    require(safe_find, "pager_try_pin_existing_page_handle", "non-fatal page acquisition")
    require(safe_find, "pager_page_handle_acquire_read", "stable read ownership")
    require(safe_find, "pager_page_handle_release_read", "read-lock release")
    require(safe_find, "pager_release_page_handle", "pin release")
    require(safe_find, "try_find_internal_child", "one-page-at-a-time internal traversal")
    if "get_page(" in safe_find or "tinydb_leaf_read_find" in safe_find:
        raise AssertionError("non-fatal payload find must not re-enter fatal page/cursor lookup")

    scan = function_body("tinydb_record_payload_scan")
    require(scan, "tinydb_record_payload_schema_supported", "payload scan schema guard")
    require(scan, "tinydb_leaf_read_start", "format-aware scan start")
    require(scan, "tinydb_leaf_read_advance_checked", "checked mixed-leaf traversal")
    require(scan, "cursor_to_payload", "payload scan decode")
    require(scan, "scan_complete", "truncation/corruption status")
    if "TinyDBRecord record" in scan or "tinydb_record_payload_to_record" in scan:
        raise AssertionError("payload scan must not narrow rows into TinyDBRecord")

    legacy_find = function_body("tinydb_record_find")
    require(legacy_find, "tinydb_schema_supports_records", "legacy compatibility ceiling")
    require(legacy_find, "cursor_to_record", "legacy adapter")

    print("PASS: payload-native reads retain the legacy cursor surface while exposing a linked try-pin point lookup that avoids fatal get_page() acquisition")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        raise SystemExit(1)

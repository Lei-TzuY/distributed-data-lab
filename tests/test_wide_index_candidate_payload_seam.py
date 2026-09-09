import os
import sys


def require(source, marker, description):
    if marker not in source:
        raise AssertionError(f"missing {description}: {marker!r}")


def main():
    repo_root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    src_dir = os.path.join(repo_root, "src")

    with open(os.path.join(src_dir, "generic_index_candidates.c"), encoding="utf-8") as handle:
        candidates = handle.read()
    with open(os.path.join(src_dir, "generic_index_epoch.h"), encoding="utf-8") as handle:
        epoch_header = handle.read()
    with open(
        os.path.join(src_dir, "generic_index_payload_scan_shim.h"), encoding="utf-8"
    ) as handle:
        shim = handle.read()

    # The candidate builder must continue to share one entry construction path.
    require(candidates, "tinydb_record_scan(table, schema", "candidate snapshot scan seam")
    require(candidates, "tinydb_record_decode(schema", "candidate entry decode seam")

    # The adapter is intentionally injected only into the candidate implementation
    # include boundary rather than changing record.h or its public ABI globally.
    require(
        epoch_header,
        "#ifdef GENERIC_INDEX_CANDIDATES_H",
        "candidate-only payload shim guard",
    )
    require(
        epoch_header,
        '#include "generic_index_payload_scan_shim.h"',
        "candidate payload shim include",
    )

    # Wide rows must use payload-native traversal and decoding. Narrow rows retain
    # the historical TinyDBRecord route so existing index snapshots stay compatible.
    require(shim, "schema->row_size <= ROW_SIZE", "narrow-row compatibility branch")
    require(shim, "return tinydb_record_scan(table, schema, visitor, context);", "narrow scan delegation")
    require(shim, "tinydb_record_payload_scan(", "wide payload scan")
    require(shim, "tinydb_record_payload_decode_values(", "wide payload decode")
    require(shim, "bridge == NULL || bridge->visitor == NULL", "defensive bridge visitor guard")
    require(shim, "visitor == NULL ? NULL : tinydb_generic_index_payload_scan_bridge", "NULL visitor scan preservation")
    require(shim, "visitor == NULL ? NULL : &bridge", "NULL visitor context preservation")

    # A truncated/corrupt payload traversal must never become a valid partial index
    # snapshot. The NULL sentinel deliberately drives the existing builder failure bit.
    require(shim, "bool scan_complete = false;", "payload scan completion state")
    require(shim, "if (!scan_complete && visitor != NULL)", "incomplete scan guard")
    require(shim, "(void)visitor(schema, NULL, context);", "candidate failure sentinel")
    require(shim, '"payload candidate scan did not complete"', "fail-closed decode diagnostic")

    # The macros are below the helper definitions: their narrow delegation therefore
    # resolves to the real record functions instead of recursively calling the shim.
    scan_macro = shim.index("#define tinydb_record_scan")
    decode_macro = shim.index("#define tinydb_record_decode")
    scan_helper = shim.index("tinydb_generic_index_payload_compatible_scan(")
    decode_helper = shim.index("tinydb_generic_index_payload_compatible_decode(")
    if scan_macro <= scan_helper or decode_macro <= decode_helper:
        raise AssertionError("payload compatibility macros precede their helper definitions")

    print(
        "PASS: generic index candidate rebuilds preserve the narrow record path while "
        "wide schemas use fail-closed payload-native scan/decode semantics."
    )


if __name__ == "__main__":
    try:
        main()
    except AssertionError as exc:
        print("FAIL:", exc)
        sys.exit(1)

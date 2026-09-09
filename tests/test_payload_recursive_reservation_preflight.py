from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROUTE = (ROOT / "src" / "record_payload_nonroot_overflow_route.c").read_text(encoding="utf-8")
RESERVATION = (ROOT / "src" / "record_payload_overflow_reservation.h").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    require("TinyDBPayloadOverflowReservation" in RESERVATION,
            "recursive payload overflow must expose an explicit reservation budget")
    require("new_leaf_pages" in RESERVATION and
            "new_internal_pages" in RESERVATION and
            "total_pages" in RESERVATION,
            "reservation budget must distinguish leaf, internal, and total pages")
    require("tinydb_record_payload_size_overflow_reservation" in RESERVATION,
            "recursive overflow must size all page claims before mutation")

    root_case = RESERVATION.index("if (plan->requires_root_growth)")
    root_formula = RESERVATION.index(
        "new_internal_pages = plan->full_internal_levels + 1u", root_case)
    nonroot_case = RESERVATION.index("} else {", root_case)
    nonroot_formula = RESERVATION.index(
        "new_internal_pages = plan->full_internal_levels", nonroot_case)
    require(root_case < root_formula < nonroot_case < nonroot_formula,
            "stable-root growth must budget one more internal page than ordinary ancestor absorption")

    require("plan->full_internal_levels != chain->count" in RESERVATION,
            "root-growth sizing must require every collected internal ancestor to be full")
    require("plan->stopping_ancestor_index != plan->full_internal_levels" in RESERVATION,
            "non-root sizing must agree with the preflight stopping level")
    require("plan->full_internal_levels == UINT32_MAX" in RESERVATION and
            "new_internal_pages == UINT32_MAX" in RESERVATION,
            "reservation arithmetic must fail closed before uint32 overflow")
    require("reservation->new_leaf_pages = 1u" in RESERVATION,
            "every leaf overflow cascade must reserve exactly one new right leaf")
    require("reservation->total_pages = new_internal_pages + 1u" in RESERVATION,
            "total reservation must include the split leaf as well as internal cascade pages")

    collect = ROUTE.index("tinydb_record_payload_collect_ancestor_chain")
    plan = ROUTE.index("tinydb_record_payload_plan_overflow_chain", collect)
    size = ROUTE.index("tinydb_record_payload_size_overflow_reservation", plan)
    release = ROUTE.index("tinydb_record_payload_ancestor_chain_release", size)
    require(collect < plan < size < release,
            "recursive fallback must collect, plan, and size reservations before releasing validated ancestry")
    require("reservation.new_leaf_pages != 1u" in ROUTE and
            "reservation.new_internal_pages < 2u" in ROUTE and
            "reservation.total_pages < 3u" in ROUTE,
            "full-grandparent boundary must reject an undersized recursive reservation plan")


if __name__ == "__main__":
    main()
    print("PASS: payload recursive overflow reservation preflight")

from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROUTE = (ROOT / "src" / "record_payload_nonroot_overflow_route.c").read_text(encoding="utf-8")
CLAIMS = (ROOT / "src" / "record_payload_page_claim_plan.h").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    require("TinyDBPayloadPageClaimPlan" in CLAIMS,
            "recursive payload overflow must expose a concrete page-claim plan")
    for field in ("page_nums", "count", "original_num_pages", "original_free_page_count"):
        require(field in CLAIMS, f"claim plan must preserve {field}")
    require("tinydb_record_payload_prepare_page_claim_plan" in CLAIMS,
            "recursive overflow must materialize exact page identities before mutation")
    require("pager->free_pages[free_count - 1u - i]" in CLAIMS,
            "claim preflight must mirror the allocator free-stack order")
    require("pager->num_pages + (i - free_count)" in CLAIMS,
            "claim preflight must model append-only page allocation after the free list")
    require("tinydb_record_payload_claim_page_is_live_ancestor" in CLAIMS,
            "prospective claims must reject aliases with the selected live topology")
    require("page_nums[j] == page_num" in CLAIMS,
            "prospective claims must reject duplicate allocator entries")
    require("pager->num_pages > INVALID_PAGE_NUM - appended" in CLAIMS,
            "claim preflight must guard append arithmetic")
    require("reservation->new_leaf_pages > UINT32_MAX - reservation->new_internal_pages" in CLAIMS,
            "reservation component arithmetic must be overflow checked before summing")
    require("tinydb_record_payload_page_claim_plan_release" in CLAIMS,
            "claim-plan ownership must have an explicit release boundary")

    collect = ROUTE.index("tinydb_record_payload_collect_ancestor_chain")
    size = ROUTE.index("tinydb_record_payload_size_overflow_reservation", collect)
    mapped = ROUTE.index("tinydb_record_payload_prepare_page_claim_plan", size)
    release_chain = ROUTE.index("tinydb_record_payload_ancestor_chain_release", mapped)
    require(collect < size < mapped < release_chain,
            "router must map concrete claims while validated ancestry is still available")
    require("claim_plan.count != reservation.total_pages" in ROUTE,
            "router must verify the concrete claim count matches its reservation")
    require("claim_plan.original_num_pages != table->pager->num_pages" in ROUTE and
            "claim_plan.original_free_page_count != table->pager->free_page_count" in ROUTE,
            "read-only claim planning must prove it did not mutate allocator state")
    require(ROUTE.count("tinydb_record_payload_page_claim_plan_release(&claim_plan)") >= 3,
            "all recursive preflight exits must release concrete claim-plan ownership")


if __name__ == "__main__":
    main()
    print("PASS: payload recursive overflow concrete page-claim preflight")

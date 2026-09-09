from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROUTE = (ROOT / "src" / "record_payload_nonroot_overflow_route.c").read_text(encoding="utf-8")
PLAN = (ROOT / "src" / "record_payload_page_claim_plan.h").read_text(encoding="utf-8")
TXN = (ROOT / "src" / "record_payload_page_claim_transaction.h").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    require("tinydb_record_payload_claim_prepared_pages" in TXN,
            "recursive payload overflow needs a concrete multi-page claim primitive")
    require("tinydb_record_payload_rollback_claimed_pages" in TXN,
            "recursive payload overflow needs an explicit allocator rollback primitive")
    require("tinydb_record_payload_next_prepared_claim_matches" in TXN,
            "recursive payload overflow must validate the next allocator identity before claiming it")
    require("pager->free_pages[pager->free_page_count - 1u]" in TXN and
            ": pager->num_pages" in TXN,
            "pre-claim validation must mirror the pager's free-stack-then-append allocation order")
    precheck = TXN.index("tinydb_record_payload_next_prepared_claim_matches(",
                         TXN.index("for (uint32_t i = 0u; i < claim_plan->count; i++)"))
    consume = TXN.index("get_unused_page_num(pager)", precheck)
    require(precheck < consume,
            "prepared page identity must be checked before get_unused_page_num mutates allocator state")
    require("*claimed_count_out = 0u;" in TXN[precheck:consume],
            "a pre-claim mismatch must leave the caller with no outstanding reservation")
    require("get_unused_page_num(pager)" in TXN,
            "claim transaction must consume the real pager allocator")
    require("(void)get_page(pager, claimed)" in TXN,
            "append claims must materialize page identity exactly as production allocation does")
    require("pager_shrink(pager, claim_plan->original_num_pages)" in TXN,
            "rollback must discard append-only claims back to the original high-water mark")
    require("pager->free_pages[pager->free_page_count++] = claim_plan->page_nums[i - 1u]" in TXN,
            "rollback must restore reused free pages in reverse claim order")
    require("restored != claim_plan->page_nums[i]" in TXN,
            "rollback must verify exact free-stack restoration")
    require("pager->num_pages != claim_plan->original_num_pages" in TXN and
            "pager->free_page_count != claim_plan->original_free_page_count" in TXN,
            "claim transaction must reject stale allocator snapshots and verify restoration")

    require("pager->free_page_count > pager->num_pages" in PLAN,
            "claim planning must reject impossible in-memory free-page counts")
    require("pager->free_page_count > 0u && pager->free_pages == NULL" in PLAN,
            "claim planning must reject a missing free-page stack")
    require("reuses_free_page && page_num >= pager->num_pages" in PLAN,
            "claim planning must reject free-list entries outside the live pager range")

    mapped = ROUTE.index("tinydb_record_payload_prepare_page_claim_plan")
    claim = ROUTE.index("tinydb_record_payload_claim_prepared_pages", mapped)
    rollback = ROUTE.index("tinydb_record_payload_rollback_claimed_pages", claim)
    release = ROUTE.index("tinydb_record_payload_page_claim_plan_release(&claim_plan)", rollback)
    require(mapped < claim < rollback < release,
            "recursive boundary must claim and roll back the prepared reservation before releasing it")
    require("table->pager->num_pages != claim_plan.original_num_pages" in ROUTE and
            "table->pager->free_page_count != claim_plan.original_free_page_count" in ROUTE,
            "router must prove the reversible claim transaction restored allocator counts")


if __name__ == "__main__":
    main()
    print("PASS: reversible recursive payload page-claim transaction")

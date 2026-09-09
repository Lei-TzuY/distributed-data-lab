from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
ROUTE = (ROOT / "src" / "record_payload_nonroot_overflow_route.c").read_text(encoding="utf-8")
ANCESTRY = (ROOT / "src" / "record_payload_ancestor_chain.h").read_text(encoding="utf-8")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    recursive_boundary = (
        '"payload-native recursive internal overflow beyond the grandparent remains fail-closed"'
    )
    require(recursive_boundary in ROUTE,
            "router must classify the explicit full-grandparent capacity boundary")

    handler = ROUTE.index("tinydb_record_payload_try_full_nonroot_parent_split")
    collect = ROUTE.index("tinydb_record_payload_collect_ancestor_chain", handler)
    plan = ROUTE.index("tinydb_record_payload_plan_overflow_chain", collect)
    release = ROUTE.index("tinydb_record_payload_ancestor_chain_release", plan)
    final_message = ROUTE.rindex("set_message(message, message_size, nonroot_parent_message)")
    require(handler < collect < plan < release < final_message,
            "recursive fallback must collect, preflight, and release ancestry before returning the capacity boundary")
    require("plan.full_internal_levels < 2u" in ROUTE,
            "full-grandparent fallback must reject a preflight that does not actually contain two full internal levels")

    require("typedef struct {" in ANCESTRY and "TinyDBPayloadAncestorChain" in ANCESTRY,
            "payload overflow ancestry must have a reusable detached chain type")
    require("TinyDBPayloadOverflowPlan" in ANCESTRY,
            "recursive overflow must expose a reusable read-only cascade plan")
    require("full_internal_levels" in ANCESTRY and "stopping_ancestor_index" in ANCESTRY,
            "cascade preflight must retain full depth and the first non-full ancestor")
    require("requires_root_growth" in ANCESTRY,
            "cascade preflight must distinguish ancestor absorption from stable-root growth")
    require("tinydb_record_payload_collect_ancestor_chain" in ANCESTRY,
            "ancestor validation must expose the validated page-number chain for recursive staging")
    require("leaf_page_num" in ANCESTRY and "internal_pages" in ANCESTRY and "count" in ANCESTRY,
            "collected ancestry must retain the leaf identity and parent-first internal page numbers")
    require("capacity > SIZE_MAX / sizeof(uint32_t)" in ANCESTRY,
            "ancestor-chain allocation must reject size_t overflow")
    require("internal_pages[count++] = parent_page_num" in ANCESTRY,
            "ancestor collection must record each validated internal parent")
    require("chain->internal_pages = internal_pages" in ANCESTRY,
            "validated ancestry must transfer detached page-number ownership to the caller")
    require("tinydb_record_payload_ancestor_chain_release" in ANCESTRY,
            "collected ancestry must have an explicit release helper")

    require("depth < table->pager->num_pages" in ANCESTRY,
            "ancestor walk must be bounded by the database page count")
    require("tinydb_payload_parent_references_child(parent, current_page_num)" in ANCESTRY,
            "ancestor walk must verify reciprocal parent->child references")
    require("parent_page_num == schema->root_page_num" in ANCESTRY,
            "ancestor walk must terminate only at the catalog-stable root")
    require("parent[IS_ROOT_OFFSET] == 0u" in ANCESTRY,
            "catalog root must be explicitly marked as root")
    require("parent[IS_ROOT_OFFSET] != 0u" in ANCESTRY,
            "foreign roots encountered below the catalog root must fail closed")
    require("memcpy(parent, get_page(table->pager, parent_page_num), PAGE_SIZE)" in ANCESTRY,
            "ancestor validation must use local page images rather than retain Pager pointers across traversal")

    planner = ANCESTRY.index("tinydb_record_payload_plan_overflow_chain")
    require("keys > INTERNAL_NODE_MAX_KEYS" in ANCESTRY[planner:],
            "overflow preflight must reject already-overfull internal ancestors")
    require("keys < INTERNAL_NODE_MAX_KEYS" in ANCESTRY[planner:],
            "overflow preflight must stop at the first ancestor that can absorb a separator")
    require("plan->stopping_ancestor_index = i" in ANCESTRY[planner:],
            "overflow preflight must identify the first non-full ancestor by chain index")
    require("plan->full_internal_levels++" in ANCESTRY[planner:],
            "overflow preflight must count consecutive full internal levels bottom-up")
    require("plan->requires_root_growth = true" in ANCESTRY[planner:],
            "an all-full chain must explicitly request stable-root growth")
    require("chain->internal_pages[chain->count - 1u] != schema->root_page_num" in ANCESTRY[planner:],
            "root-growth classification must still prove that the detached chain terminates at the catalog root")
    require("tinydb_record_payload_validate_ancestor_chain" not in ANCESTRY,
            "the obsolete validation-only static wrapper must not remain after the router consumes collected ancestry directly")


if __name__ == "__main__":
    main()
    print("PASS: payload recursive ancestor guard and overflow preflight")

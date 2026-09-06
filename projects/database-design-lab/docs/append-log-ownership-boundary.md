# Append-log ownership boundary

The retained generation directory uses a **cooperative ownership contract**, not filesystem sandboxing.

Supported application and maintenance paths acquire `append_log_generation_writer_lock_v1` before authority-sensitive mutation. `GenerationLogEngine` holds that lease across authority refresh and each routed operation; compact-switch, cleanup, migration/cutover, and related maintenance paths use the same coordination boundary. Ordinary standalone `LogEngine::open` / `create_new` calls reject canonical generation filenames before mutation, and ownership constructors reject symlink indirection.

The contract intentionally does **not** attempt to revoke operating-system filesystem authority from a process that deliberately bypasses the routed APIs. A caller that explicitly invokes `LogEngine::open_managed_generation` or `create_new_managed_generation` on a canonical generation path is asserting that it is generation infrastructure and is responsible for satisfying the surrounding lease/authority preconditions. Likewise, a process with direct filesystem access can mutate or replace retained files outside the Rust API entirely.

This boundary is deliberate for the educational storage laboratory: correctness claims cover cooperating repository APIs and their deterministic crash/fault model. Preventing hostile same-user processes from opening or replacing files would require an external isolation mechanism such as OS permissions, a broker/service boundary, sandboxing, or a capability-bearing filesystem design; adding more pathname checks inside the process would not provide that guarantee.

The regression `log_generation_ownership_contract_integration` pins both sides of the contract: a held writer lease excludes another coordinated writer, while a deliberately uncoordinated managed raw-path caller remains possible and therefore cannot be confused with a supported coordinated writer.

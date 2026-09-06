# Windows write-through namespace primitive

The repository's append-log generation lifecycle intentionally fails closed on Windows wherever it would otherwise need to claim durable namespace publication without an implemented mechanism. `db_cli::windows_durable::move_no_replace_write_through` is the first narrow platform primitive intended to reduce that gap.

## Contract

On Windows the function calls `MoveFileExW` with **only** `MOVEFILE_WRITE_THROUGH`.

That deliberately means:

- the source is moved to the target name;
- the call requests write-through completion before returning success;
- `MOVEFILE_REPLACE_EXISTING` is not enabled, so an existing target is not overwritten;
- `MOVEFILE_COPY_ALLOWED` is not enabled, so the function does not accept a cross-volume copy/delete fallback;
- paths are passed as NUL-terminated UTF-16 and Unicode path behavior is covered by the Windows test suite.

On non-Windows targets the function returns `Unsupported` before filesystem access.

The raw Win32 FFI is isolated to this module. The rest of `db-cli` keeps `unsafe_code = "deny"`; `lib.rs` grants a local allowance only for this audited boundary.

## First upper-level use: generation reservations

`append_log_generation_reservation_windows_v1` uses the primitive to publish a synchronized, unique sibling staging file into the canonical zero-byte `reserve-%020d.frontier` name while holding the shared generation writer lease. The staging object lives outside the strict retained generation namespace; successful write-through move consumes that staging name, and retained-state verification must then observe the canonical reservation before success is returned.

This is a deliberately narrow lift of the former Windows fail-closed boundary. Reservation publication fits the primitive exactly because the complete retained object can be created and synchronized under a non-authoritative staging name before its one-way no-overwrite namespace transition.

## Why this is not yet full Windows generation durability

The primitive solves only one namespace transition: **an already-synchronized staging object to a previously absent canonical name**. Other generation lifecycle operations have additional ordering and retention requirements, including compact-generation construction, final commit-marker publication, cleanup/orphan retirement, migration, and cutover.

Those upper-level Windows paths remain unsupported until each operation is explicitly redesigned around a complete Windows-safe ordering. In particular, enabling durable reservation does not prove that an already-existing canonical generation filename preceded marker authority durably.

## Evidence boundary

Windows CI proves the actual API call succeeds for a fresh target, rejects replacement without altering either retained object, handles Unicode paths, and now exercises the reservation CLI's monotonic frontier / lease / corruption behavior through this primitive. That is executable API/contract evidence on the hosted Windows filesystem.

It is **not** physical power-loss testing and does not claim that every Windows filesystem, storage controller, or device honors persistence identically. The project will only lift each upper-level Windows fail-closed boundary after the complete operation ordering is defensible, not merely because this primitive exists.

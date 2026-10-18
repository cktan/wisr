# Known issues

Problems found and left unfixed, each with what it would take
to fix. The code it concerns points here by number.

---

## Issue 1: a library a session loads shares its statics

A PostgreSQL extension keeps its state in static variables and
sets it in `_PG_init()`, once per backend: each backend is a
process, with its own copy. digres's sessions are threads of
one process, and `dlopen` loads a library once for all of them,
so its statics are one for the process. A library a session
loads after it starts, by `LOAD` or a C function's first call,
breaks:

- **Each session's load runs `_PG_init()` again** on the same
  statics. The usual hook chain, `prev_planner_hook =
  planner_hook; planner_hook = my_planner;`, keeps one
  `prev_planner_hook`: the last session to load the library
  sets it, from its own hooks (`tcb->hooks`), and every other
  session's `my_planner` then calls that session's previous
  hook, a library it may never have loaded.
- **The write is a data race** with any other session calling
  the hook meanwhile.

`shared_preload_libraries` is safe: `_PG_init()` runs once, in
the postmaster, and each session starts with a copy of what it
set (`session_thread_start()`: `tcb->hooks`, `tcb->dfmgr`,
`tcb->explain`).

No library is built in the tree, so nothing loads one. dfmgr.c's
`internal_load_library()` asserts that no session thread loads
a library, so a test that does fails loudly rather than racing.

**The fix, not planned soon**, is one of:

- **Load libraries only through `shared_preload_libraries`**,
  and refuse a session's `LOAD` and later loads with an error,
  not an assert. Simple and safe; a session loses `LOAD`.
- **Require a digres extension to keep its state
  `thread_local`.** `LOAD` stays; every extension must change.
- **Run `_PG_init()` once per process**, with process-wide
  hooks under a lock. Undoes the per-session hooks
  (`WORKNOTE.md` step 222).

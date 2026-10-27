# Known issues

Problems found and left unfixed, each with what it would take
to fix. The code it concerns points here by number. A fixed
issue is removed, and its number is not used again: issue 2,
ON CONFLICT DO UPDATE affecting a row twice, was fixed in
`WORKNOTE.md` step 237; issue 3, a numeric range in an array
or a multirange losing its bound, in step 300; issue 6, a
duplicate key in a table made in a savepoint saying the wrong
error, in step 302; issue 7, ON COMMIT DELETE ROWS ignoring a
foreign key into the table, in step 303; issue 4, a cursor
seeing its own transaction's later DELETE, in step 304; issue
5, a FOR UPDATE cursor returning a row updated after it was
declared, in step 305; issue 9, ON CONFLICT above READ
COMMITTED saying sm1's error, in step 306; issue 8, a table
made in pg_catalog, in step 307; and issue 11, a foreign key
keeping a SET DEFAULT column twice, in step 308.

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

**Loading is disabled until it is redesigned.** dfmgr.c's
`internal_load_library()` refuses every library with an error,
`loading library "..." is not supported yet`: `LOAD`, a C
function, `shared_preload_libraries` and the session's preload
settings alike. It asserted that no session thread loaded one,
which took the server down on PostgreSQL's
`create_function_c`. No library is built in the tree, so
nothing digres runs loaded one.

**The fix is to redesign dynamic loading and redo it**, from
one of:

- **Load libraries only through `shared_preload_libraries`**,
  and refuse a session's `LOAD` and later loads with an error,
  not an assert. Simple and safe; a session loses `LOAD`.
- **Require a digres extension to keep its state
  `thread_local`.** `LOAD` stays; every extension must change.
- **Run `_PG_init()` once per process**, with process-wide
  hooks under a lock. Undoes the per-session hooks
  (`WORKNOTE.md` step 222).

---

The issues below were found by triaging pgregress's
`--dg-differs` statements (`test/pgregress/TRIAGE.md`). Each
repro runs in psql on an empty database, the statements on
psql's input (`psql -f -`), not in one `-c`; each says what
PostgreSQL 18 answers and what digres does. None is diagnosed
yet: "where to start" is where the difference shows, not a
cause found.

---

## Issue 10: CREATE TABLE (LIKE a, LIKE a) says sm's error

```sql
create table inhx (xx text default 'text');
create table inhf (like inhx, like inhx);
-- PostgreSQL: ERROR:  column "xx" specified more than once
-- digres:     ERROR:  could not create a table: create table: a
--             column without a name, or two with one: xx
```

pgregress: `create_table_like`. **Where to start**: digres
does not check the merged column list for a repeated name
before it hands it to sm, as PostgreSQL's
`MergeAttributes()` does.

## Issue 12: the hash access method says it cannot exclude

```sql
select pg_indexam_has_property(a.oid, 'can_exclude')
  from pg_am a where amname = 'hash';
-- PostgreSQL: t
-- digres:     f
```

pgregress: `amutils`. **Where to start**: the `hash` row of
`pg_am` and the routine its handler returns
(`amcanexclude`).

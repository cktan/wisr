# Known issues

Problems found and left unfixed, each with what it would take to
fix. The code it concerns points here by number. A fixed issue is
removed, and its number is not used again: issue 2, ON CONFLICT
DO UPDATE affecting a row twice, was fixed in
[`WORKNOTE.md`](WORKNOTE.md) step
237; issue 3, a numeric range in an array or a multirange losing
its bound, in step 300; issue 6, a duplicate key in a table made
in a savepoint saying the wrong error, in step 302; issue 7, ON
COMMIT DELETE ROWS ignoring a foreign key into the table, in step
303; issue 4, a cursor seeing its own transaction's later DELETE,
in step 304; issue 5, a FOR UPDATE cursor returning a row updated
after it was declared, in step 305; issue 9, ON CONFLICT above
READ COMMITTED saying sm1's error, in step 306; issue 8, a table
made in pg_catalog, in step 307b; issue 11, a foreign key keeping
a SET DEFAULT column twice, in step 308; issue 10, CREATE TABLE
(LIKE a, LIKE a) saying sm's error, in step 309; issue 13, a FOR
UPDATE cursor now and then reading no rows, in step 315; issue
14, a dropped column staying in its own transaction, in step 384;
issue 16, lock_timeout ending CREATE INDEX's wait for its build,
in step 385; issue 20, ALTER COLUMN TYPE's lock timeout saying
57014, in step 386; issue 18, nextval() in a read-only
transaction, in step 387; issue 23, a table renamed to its own
name, in step 388; issue 25, OWNED BY saying 22023, in step 389;
issue 26, a column's NULL constraint refused, in step 390; issue
24, a missing schema reported as a missing relation, in step 391;
issue 19, ALTER SCHEMA RENAME failing with an internal error, in
step 392, where it is now refused; and issues 15 and 17, ALTER
COLUMN TYPE losing a committed update and ADD COLUMN NOT NULL
counting deleted rows, in step 394. Issue 12, the hash access
method saying it cannot exclude, was closed as digres's answer,
not a bug, in step 310: digres has no hash index. Issue 21, a
bare numeric rounding its places past 17 digits, was closed so in
step 393: the code does it on purpose, and
[`DECIMAL.md`](DECIMAL.md) now says
so.

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
  ([`WORKNOTE.md`](WORKNOTE.md) step 222).

## Issue 22: the purge of a mass delete stalls every writer

Found by DuckDB's large DELETE and UPDATE files, which time out
on digres. After a DELETE and an UPDATE of 500,000 rows each in
a table of 1,000,000, writes to every table of the database
were six times slower for more than 20 minutes. A DELETE of
100,000 rows took 14 s, against 2 s on a new server.

- `purge_one()` (`../sm/sm1/src/gc.cc`) writes one RocksDB
  `DeleteRange` for each purged key.
- A store's memtable is flushed every 1,000 range tombstones
  (`store_max_range_deletions`), so the purge makes a level-0
  file every 1,000 keys.
- RocksDB then stalls writes for the level-0 files and the
  immutable memtables ("Stalling writes because we have 21
  level-0 files"): the purge thread's writes, and each
  session's, which join the same write group.

To see it: on a new server, make a table of 1,000,000 rows,
DELETE half and UPDATE the rest, then time a DELETE of 100,000
rows in another table.

What a profile of the slow DELETE shows: the session's thread
waits on RocksDB's write group behind the purge thread, which
sleeps in `DBImpl::DelayWrite()`; the busy threads fragment range
tombstones (`FragmentedRangeTombstoneList`) in flushes and
compactions. A DELETE on a new server is linear: 2 s for 100,000
rows, 4.1 s for 200,000. DuckDB's
`index/art/storage/test_art_reclaim_storage.test_slow` meets it
too: after DROP INDEX erases an index's 1,000,000 entries, a
CREATE INDEX of the same table takes longer than a minute.
In a full `--tree` run, `insert_update_delete/
test_art_random_deletion.test_slow` once timed out at line 16,
a count of 1,567 rows: "a wait for a page interrupted". Run
alone, the file is clean in 41 s. That it waits on an earlier
file's purge is likely, not proven.

The fix is a choice in sm1's GC
([`../sm/GC.md`](../sm/GC.md#5-the-tombstone-purge) §5), not made
here:

- **Erase each version with a point delete**, found by an
  iterator over the purge's range, `[tombstone, userkey | FF x
  12)`, instead of one `DeleteRange`. No range tombstone, so no
  forced flush; a seek for each purged key, and point
  tombstones that compaction drops at the bottom level. The
  range holds only that key's versions older than the
  tombstone, and no write makes a new one there.
- **Purge more slowly**, at a rate that keeps level-0 below
  RocksDB's slowdown trigger. Simple, but a mass delete then
  takes longer to reclaim.
- **Raising `store_max_range_deletions`** is no fix: a memtable
  with many range tombstones makes each read fragment them
  again, which [`GC.md`](../sm/GC.md#5-the-tombstone-purge) §5
  flushes to avoid.

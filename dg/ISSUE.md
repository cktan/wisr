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
step 392, where it is now refused; issue 22, the purge of a mass
delete stalling every writer, in step 419; issue 27, sm's log
messages dropped, by `datadir_set_sm_log()`, which the postmaster
and initdb install; and issues 15 and 17, ALTER COLUMN TYPE
losing a committed update and ADD COLUMN NOT NULL counting
deleted rows, in step 394. Issue 12, the hash access
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

---

## Issue 28: an index scan with a null key reads the whole index

Found 2026-11-09 by `bench/soak.sh`, in a delete that took 25 ms
a statement and grew slower as the table grew.

A key of an index scan can be null at run time: the value of
an InitPlan (a subquery run once) that returns no row. A
comparison with null is never true, so such a scan meets no row.
PostgreSQL sees the null key when the scan starts and returns at
once. digres reads the whole index and checks each entry.

**To reproduce.** On a table `enrollment (student_id, class_id,
grade)` of 100,000 rows with `create index on enrollment using
btree (class_id, student_id)`:

    delete from enrollment where student_id = 5 and class_id =
      (select class_id from enrollment where false);

- digres: 141 ms. The plan is `Index Scan using enrollment_class`
  with `Index Cond: ((class_id = (InitPlan 1).col1) AND
  (student_id = 5))`, and the scan reads all 100,000 entries.
- PostgreSQL 18: under 1 ms.
- The time grows with the table. `class_id IN (subquery)` has no
  null key, and takes 1.4 ms.
- A real workload meets it as `class_id = (select … offset N)`,
  when the student has N rows or fewer.

**The cause, from reading `smstore_index_beginscan()`**
(`dg/src/backend/access/sm/smstore.c`):

1. `plain_key()` refuses a key with `SK_ISNULL`, so the key
   gives the scan no bound.
2. With no bound on `class_id`, the next column, `student_id`,
   bounds nothing either: the leading column comes first.
3. The scan then reads every entry, and `keys_met()` refuses
   each one for the null key (`SK_ISNULL`).

**The fix** is to end the scan before it reads. In
`smstore_index_beginscan()`, if a key has `SK_ISNULL` and neither
`SK_SEARCHNULL` nor `SK_SEARCHNOTNULL`, make the scan return no
row, as it does now for a null array key ("it meets no row").
A btree operator is strict, so a null argument never holds. The
cost is a loop over the keys, which the function makes already.
The same holds for a scan on a table's primary key: check it
too.

**Tests when it is fixed:**

- `dg/test`: the delete above, and a select with the same
  condition, on a table of 100,000 rows, each under a time limit
  that a whole-index scan cannot meet.
- `sm/rel/test`: a scan with a null key returns no row. This
  needs the layer to take a null key, which it may not.
- `bench/soak/drop_class.sql` may then use `class_id = (…)`
  again; it uses `IN` to avoid this issue.

# NEXT — what to build next, and in what order

This file holds only what is still to do, across levels. What
is built or settled is in the design documents: `DESIGN.md`
§3 has where each level stands, and `sm3/SM3_DESIGN.md` §8
has sm3's own order of work.

---

## 1. Next

**The highest priority is to bring the stack to where work on
hooking it up to PostgreSQL can begin** (`PART4.md` §7). What
is listed here is ordered by that, and what does not serve it
waits. The first target is a spike that inserts into and
scans a table of integer columns, from one backend, and
`PART4.md` §9 has the stack's API read against the executor
for it: nothing in that target waits on the stack, and §9.3
there lists what the fork would meet next.

The rest of `sm3/SM3_DESIGN.md` §8.1 step 5.

1. **Statistics for a planner.** It has a strategy,
   `STATISTICS.md`, whose measurements are done (§6 there):
   RocksDB's size estimates are exact once flushed and
   compacted, and as much as 7x high for rows still
   unflushed. Its design is `SM3_DESIGN.md` §6, and the
   stack's part is built: `sm_tbl_analyze` samples a table,
   within a fixed budget, and records PostgreSQL's statistics
   of it in sm3's catalog, and `sm_tbl_stats` reads them. Next
   is the host's: digres's ANALYZE and its planner reading
   them in place of `pg_statistic` and `pg_class`, designed in
   `../dg/STATISTICS.md`.

Low priority, and after it:

2. **Bulk load, the rest of it.** File mode into an empty
   table is built, and digres's `COPY` uses it (`BULKLOAD.md`
   §13). Not built: a load from files into a table that has
   rows, which inserts a row at a time today; rows stored out
   of line, which end a load's files; a `rel` verb; and batch
   mode and mini-batch, which matter less now that no write
   keeps a row lock.

---

## 2. Loose ends

Owed, and blocking nothing above:

- **The commit cache's size** (`sm1/COMMITCACHE.md` §7).
  65,536 entries reaches back under a second at 100,000
  transactions a second, while a version needs a lookup until
  compaction stamps it. Size the cache by memory with a larger
  default, 16 MB for a million slots; and measure how
  long stamping takes under load, which is what the size
  should be set from.

- **Check `EvalPlanQual` against PostgreSQL's source.**
  `SM3_DESIGN.md` §3.1 says the re-check of a join uses the
  row of the other table that was already joined, not its
  newest version. That is from memory.
- **A parent's other columns wait for its children.** sm1 has
  no pair of modes like PostgreSQL's `FOR KEY SHARE` and `FOR
  NO KEY UPDATE`, so an update of a parent row that leaves its
  key alone still waits for a child's transaction, which holds
  the row shared.
- **`lock_timeout`, and the rest of no-wait.** A lock that
  does not wait is built for a row and for a table
  (`sm1::trylock`, `sm_tbl_trylock`), and digres's NOWAIT and
  SKIP LOCKED use it. Not built: a write that does not wait,
  which nothing in SQL asks for; `LOCK TABLE ... NOWAIT`; and
  `lock_timeout`, which needs no more than an `sm_sys.h` timer
  that interrupts the waiting thread. sm1's `set_qa_nowait` is
  still for QA only (`sm1/SM1_DESIGN.md` §5.1).
- **The evaluator's cost is unmeasured**: what a call a row
  adds to a write on a table with a partial or an expression
  index.
- **The index build is unmeasured.** Four workers is a guess:
  builds contend on sm1's lock table, one mutex, and on
  RocksDB's write path. Two indexes built on one table walk
  its heap twice where one pass could fill both.
- **Nothing reads a range at its newest version.** An
  over-long key in a unique index is refused instead
  (`SM3_DESIGN.md` §7). The primitive is built when a workload
  asks.

- **A point read should pin its row, not lend it.** The bytes
  `sm1::read`, `sm1::read_latest` and the old row of a write
  hand out are valid only until the next call on the session
  (`sm1/API.md`), so a caller holding two rows read by rowno
  must copy one. PostgreSQL pins a page a row and holds any
  number at once. sm should give the same: a read that
  returns a handle the caller releases, over RocksDB's
  `PinnableSlice`, which keeps the block in memory and copies
  nothing. sm1's point read is an iterator's `Seek` today
  (`sm1/SM1_DESIGN.md` §10.1), so the handle would hold the
  iterator, or the read would move to `Get`. Until then
  digres copies: the row an index scan, a bitmap scan or an
  UPDATE fetches by its rowno is copied once a column is
  passed by reference.

---

## 3. Not blocking

- **TOAST in digres.** sm2's and sm3's are built
  (`sm2/SM2_DESIGN.md` §4.2, `sm3/SM3_DESIGN.md` §3.4), and
  the C API has `sm_set_toast` and `sm_tbl_fetch`. digres
  leaves it off: it reads a value straight out of a tuple. It
  is to fetch a pointer where it reads a value, and give a
  column's pointer back to an update that leaves the column
  alone. Also not built: compression, and a fetch of part of a
  value.
- **What temporary tables left for later** (`TEMPTABLE.md`
  §9). They are built, in digres too. Not built: skipping the
  sync of a commit that wrote only temporary stores,
  statistics of a temporary table, `CREATE TEMPORARY
  SEQUENCE`, and a bulk load into one.
- **Dependency checks**, such as refusing `DROP COLLATION` in
  use and `DROP SCHEMA … CASCADE`: low priority. They wait
  for the dependency index of `sm3/CATALOG3.md` §0.5, and
  nothing is waiting for them.
- **The unmeasured startup cost for thousands of tables** —
  open in `sm3/CATALOG.md` §7.
- **The rest of the catalog's layout** (`sm3/CATALOG3.md`). A
  key for each column and each index is built. Still
  proposed: an index's own key, include and read columns as
  entries, which removes the last counted framing; the
  dependency index, which the checks above wait for; and keys
  as words with tagged values.
- **A table in a namespace other than `public`, from `rel`.**
  `rel` creates and drops namespaces but has no way to name
  one for a table.
- **Savepoints without an undo list** (`sm1/SAVEPOINT.md`):
  a rollback would mark the `seq` range it undid as dead and
  delete nothing. Designed, and not to be built for now: we
  lean towards the capped undo list, for the reasons at the
  top of that file.

---

## 4. What this file is not

Not a replacement for `sm3/SM3_DESIGN.md` §8, which holds
sm3's order of work and the reasoning behind it, nor for the
design documents, where a protocol is argued before it is
built and recorded once it is. A decision settled here moves
into one of those and leaves this file.

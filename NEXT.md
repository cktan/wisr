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
waits.

The rest of `sm3/SM3_DESIGN.md` §8.1 step 5.

1. **Statistics for a planner.** It has a strategy,
   `STATISTICS.md`, whose measurements are done (§6 there):
   RocksDB's size estimates are exact once flushed and
   compacted, and as much as 7x high for rows still
   unflushed. It has no design yet: `SM3_DESIGN.md` §6 is the
   next thing to write, from the strategy and what was
   measured (`STATISTICS.md` §8, step 2). The stack analyzes:
   `sm_analyze` samples a table and records statistics of an
   LSM in sm3's catalog, and the host fits them to the
   planner.

Low priority, and after it:

2. **Bulk load**: a path that batches within sm1's memory
   bound. It has a design, `BULKLOAD.md`, and is not built. A
   load is one transaction in one of three modes: mini-batch,
   100 rows a write under IX, keeping no row locks;
   batch mode, a chunk a write under SIX; and file mode,
   sorted files that RocksDB ingests. Its checks against
   RocksDB are done, and batch mode is its first piece.

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
- **`NOWAIT` and `SKIP LOCKED` as flags on the calls.** sm1's
  `set_qa_nowait` is for QA only (`sm1/SM1_DESIGN.md` §5.1).
  PostgreSQL passes a wait policy with each call, and sm1 is to
  take one the same way, through sm2, sm3 and `include/sm.h`,
  `sm_tbl_lock` first among the calls. `lock_timeout` needs
  no more than a timer in the host that calls `sm_cancel`.
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

---

## 3. Not blocking

- **sm2's TOAST** (`sm2/SM2_DESIGN.md` §8.2, step 3). The
  `0xFFFF` relid is reserved per table and unused. Wholly
  independent of the above, so it can run in parallel.
- **Temporary tables** (`sm3/CATALOG.md` §7): a temporary
  store, its definition in the session's own temporary catalog
  and never written to store 0.
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

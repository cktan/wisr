# NEXT — what to build next, and in what order

This file holds only what is still to do, across levels. What
is built or settled is in the design documents: `DESIGN.md`
§3 has where each level stands, and `sm3/SM3_DESIGN.md` §8
has sm3's own order of work.

---

## 1. Next

The rest of `sm3/SM3_DESIGN.md` §8.1 step 5, and one change
to sm1. The order among them is not settled.

1. **Statistics for a planner.** It has a strategy,
   `STATISTICS.md`, and no design yet: `SM3_DESIGN.md` §6 is
   still to be written. The stack analyzes: `sm_analyze`
   samples a table and records statistics of an LSM in sm3's
   catalog, and the host fits them to the planner. The
   strategy's first step is to measure how good RocksDB's size
   estimates are, which nothing has done.
2. **Bulk load**: a path that batches within sm1's memory
   bound. It has a design, `BULKLOAD.md`, and is not built:
   a load holds the table's schema lock SIX in place of row
   locks, writes its rows to sorted files, and has RocksDB
   ingest them, in one transaction. Its first step is to
   check what it assumes of RocksDB.

3. **Savepoints without an undo list**
   (`sm1/SAVEPOINT.md`): a rollback marks the `seq` range it
   undid as dead and deletes nothing, so holding a savepoint
   costs no memory. Designed, not built. It changes sm1's
   visibility rule, status record and compaction filter, and
   lets a bulk load be rolled back to a savepoint.

---

## 2. Loose ends

Owed, and blocking nothing above:

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
  `sm_tbl_lock` first among the calls.
  `lock_timeout` needs a timed wait besides.
- **A lock wait cannot be cancelled.** A build's worker that
  waits on a lock held by a prepared, in-doubt transaction
  holds up `cat_close`, and so a detach, until that transaction
  is resolved. sm1 needs a way to cancel a wait.
- **Marking an index valid takes the schema lock X**, as every
  catalog change does, so a long transaction on the table
  delays it. The step only flips a state writers do not depend
  on; a weaker lock would do.
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
  `0xFFFF` prefix is reserved per table and unused. Wholly
  independent of the above, so it can run in parallel.
- **Temporary tables** (`sm3/CATALOG.md` §7): a temporary
  store, its definition in the session's own temporary catalog
  and never written to store 0.
- **Namespaces past the two built in**, dependency checks such
  as refusing `DROP COLLATION` in use, and the unmeasured
  startup cost for thousands of tables — all three open in
  `sm3/CATALOG.md` §7.

---

## 4. What this file is not

Not a replacement for `sm3/SM3_DESIGN.md` §8, which holds
sm3's order of work and the reasoning behind it, nor for the
design documents, where a protocol is argued before it is
built and recorded once it is. A decision settled here moves
into one of those and leaves this file.

# NEXT — what to build next, and in what order

What was here about index maintenance, the READ COMMITTED
re-check and the index build is built, and has moved into the
design documents: `sm3/SM3_DESIGN.md` §3, with its worked
examples in §3.1 and its tests in §3.2; `sm1/SM1_DESIGN.md`
§5.1 and §5.4; `sm2/SM2_DESIGN.md` §3.2 and §5; `DESIGN.md` §3
to §5; and `rel/REL_DESIGN.md`. This file holds only what is
still to do, across levels. `sm3/PLAN.md` stays sm3's own
order of work.

---

## 0. Where the stack stands

| Level | State |
| --- | --- |
| sm1 | built and tested |
| sm2 | built but for TOAST (`sm2/SM2_DESIGN.md` §8.2) |
| sm3 | catalog, schema locks, object mapping, indexes of every kind with their maintenance and their concurrent build, and scans built; constraints and `SM3_DESIGN.md` §6, statistics, are design |
| top | `include/sm.h` over all three, with the `sm_cat_*` and `sm_tbl_*` calls; `rel` reads and writes its tables through them |

`DESIGN.md` §3 has the same table at more length.

---

## 1. Next: foreign keys

What is left of constraints in `sm3/PLAN.md` step 5. `NOT
NULL` is enforced by sm3, and the `varchar(n)` limit is the
host's to apply (`DESIGN.md` §5, `SM3_DESIGN.md` §3).

A foreign key needs a shared lock on the parent row, held to
the end of the child's transaction, which sm2's `rel_lock`
takes. Whether the check is sm3's, or the host's as PostgreSQL
runs it from triggers, is the first thing to settle.

---

## 2. After it

The rest of `sm3/PLAN.md` step 5: statistics for a planner,
whose section of `SM3_DESIGN.md`, §6, is still to be written;
and a bulk-load path that batches within sm1's memory bound.

---

## 3. Loose ends

Owed, and blocking nothing above:

- **Check `EvalPlanQual` against PostgreSQL's source.**
  `SM3_DESIGN.md` §3.1 says the re-check of a join uses the
  row of the other table that was already joined, not its
  newest version. That is from memory.
- **`NOWAIT` and `SKIP LOCKED` as flags on the calls.** sm1's
  `set_qa_nowait` is for QA only (`sm1/SM1_DESIGN.md` §5.1).
  PostgreSQL passes a wait policy with each call, and sm1 is to
  take one the same way, through sm2, sm3 and `include/sm.h`.
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

## 4. Not blocking

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

## 5. What this file is not

Not a replacement for `sm3/PLAN.md`, which holds sm3's order
of work and the reasoning behind it, nor for the design
documents, where a protocol is argued before it is built and
recorded once it is. A decision settled here moves into one of
those and leaves this file.

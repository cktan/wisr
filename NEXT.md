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
| sm3 | catalog, schema locks, object mapping, index maintenance and the concurrent index build built; partial and expression indexes and `SM3_DESIGN.md` §6 are design |
| top | `include/sm.h` over all three, with the `sm_cat_*` and `sm_tbl_*` calls; `rel` reads and writes its tables through them |

`DESIGN.md` §3 has the same table at more length.

---

## 1. Next: partial and expression indexes, by callback

The last of `sm3/PLAN.md` step 3. sm3 evaluates nothing: the
host does, through a callback it registers
(`SM3_DESIGN.md` §3, `DESIGN.md` §5). That decision also
settled step 4: a scan takes key bounds and filters nothing,
so there is no predicate scan to build (`SM3_DESIGN.md` §5).

In this order, tests first:

1. **Settle what §3 left open** — **done**: what the callback
   computes, who owns the values it hands back, how an update
   leaves such an index alone, how the predicate and the
   expressions are kept, and where the evaluator is
   registered (`SM3_DESIGN.md` §3).
2. **The catalog**: an index's record keeps its blob, the
   columns it reads, and each expression key column's type,
   typmod and collation; `cat_create_index` takes them.
   → verify: they load back after a restart.
   **Done**, with `cat_set_evaluator`. Until 3, a table with
   such an index refuses every write and every build.
3. **Maintenance and the build** call the evaluator: on
   insert, on the old and the new image of an update and a
   delete, and in each batch of a build.
   → verify: the index agrees with a brute-force scan of the
   heap put through the same evaluator, at every isolation
   level, with a writer racing the build. An evaluator that
   fails makes the write fail, and a build keep the reason.
4. **The C API and `rel`**: registering an evaluator, and a
   `rel` script supplying one as a Janet function.

What it costs a row on the write path is unmeasured; measure
it once step 3 runs.

---

## 2. After it

`sm3/PLAN.md` step 5: statistics for a planner; constraints;
a bulk-load path that batches within sm1's memory bound.
Among the constraints are two that `DESIGN.md` §5 already
gives to sm3 and nothing enforces today: `NOT NULL`, and the
length of a `varchar(n)`.

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
- **The index build is unmeasured.** Four workers is a guess:
  builds contend on sm1's lock table, one mutex, and on
  RocksDB's write path. Two indexes built on one table walk
  its heap twice where one pass could fill both.
- **`include/sm.h` still has its C forms.** It is C++ in a C
  style now, and `std::string` and `std::vector` may cross it
  (`DESIGN.md` §5). The calls that predate that keep a list as
  one malloc'd block freed with `sm_free`, an owned result as
  bytes valid "until the next call", and a C mirror of each
  table definition. Each moves when it is next touched, or all
  at once if that proves cleaner.
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

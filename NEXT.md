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
| sm3 | catalog, schema locks, object mapping, index maintenance and the concurrent index build built; `SM3_DESIGN.md` §5 and §6 are design |
| top | `include/sm.h` over all three, with the `sm_cat_*` and `sm_tbl_*` calls; `rel` reads and writes its tables through them |

`DESIGN.md` §3 has the same table at more length.

---

## 1. Next: predicate scans, and the evaluator they need

`sm3/PLAN.md` step 4, and `SM3_DESIGN.md` §5, which still
opens "*To be written.*" A predicate goes in, a key range
comes out of it, and the residual filter is evaluated below
the API. Partial and expression indexes wait on the same
evaluator: maintaining one means evaluating the index's own
predicate or expression against a row image
(`SM3_DESIGN.md` §3).

The evaluator is not needed for the READ COMMITTED re-check.
That is the executor's, because the qual is
(`SM3_DESIGN.md` §3.1).

### 1.1 What an evaluator in sm3 would take

- **A predicate representation across the C API.**
  `include/sm.h` has none: a scan takes a `rowno` range or an
  `sm_bound_t`. It needs a serialized tree — `Var` by attnum,
  `Const` as an `sm_datum_t` with its type code and typmod,
  operator application by a stable code, `AND` / `OR` / `NOT`,
  `IS [NOT] NULL`, and a collation on every comparison. This is
  the same decision as §5's open "API shape", not a second one.
- **The evaluator itself**, the cheap part: sm2 already has one
  ADT descriptor per type (`sm2/SM2_DESIGN.md` §2.3),
  collation-aware comparison (§3.3) and per-ordinal decode, so
  a filter on one column of a wide row decodes one column. sm3
  adds three-valued logic and operator dispatch.
- **A pushdown-safe subset, defined and enforced.** Three
  things can never go down: user-defined functions and
  operators, because sm3 cannot call PL code; subqueries and
  joins, because `EvalPlanQual` re-evaluates a plan subtree
  with the new tuple substituted and sm3 has no plan; and
  volatile functions, which are wrong to re-evaluate at all. So
  a classifier above decides pushdown or fallback, and the
  executor's own path stays for the fallback. Pushing a filter
  down does not remove that path, it adds one beside it, and
  that is the honest cost.
- **Semantic agreement with PostgreSQL**, which is where the
  bugs will be: NULL propagation, collation (`SM3_DESIGN.md`
  §3 already requires sm3's own comparisons to follow it, and
  to hash the sort key as `hashtext` does under a
  non-deterministic one), decimal comparison, coercion. A
  divergence surfaces as wrong rows, the failure mode a test
  suite is worst at catching.

The subset to start from: equality and range comparisons on
sm2's scalar types with collation, conjunctions, and
`IS [NOT] NULL`; no `OR` and no functions.

One consequence for indexes, recorded in `DESIGN.md` §5: an
index whose predicate or expression falls outside what the
evaluator covers is refused. sm3 cannot evaluate a PostgreSQL
expression without calling back into C that may `longjmp`
across its frames.

### 1.2 What it buys

- **§5 needs the evaluator regardless**: "the residual filter
  is evaluated below the API instead of returning rows for a
  caller to discard" (`SM3_DESIGN.md` §5).
- **Partial and expression indexes come with it.** The catalog
  cannot record a predicate or an expression today:
  `cat_create_index` takes key columns and INCLUDE columns
  only.
- **`rel` and any non-PostgreSQL consumer get predicate
  scans.** `OBJECTIVE.md` keeps a consumer other than the fork
  possible, and without an evaluator below the API such a
  consumer filters rows itself, which is the waste §5 exists
  to remove.
- **The chosen precedent did this.** YugabyteDB "replaced whole
  executor nodes to push work down — sequential scan, bitmap
  scan, batched nested loop, expression evaluation"
  (`PART4.md`).

### 1.3 Order

This repo argues a protocol before writing it, so:

1. **`SM3_DESIGN.md` §5's prose**: the API shape, one tuple at
   a time as sm2 is or sets of rows; the predicate's
   representation; and the pushdown subset.
   → verify: the section no longer says "to be written".
2. **The evaluator**, tested by property against sm2's own
   `cmp` for every type.
3. **Predicate scans**: a key range derived from the
   predicate, the residual filter evaluated below the API.
   → verify: for generated predicates, the rows returned match
   a brute-force scan-and-filter.
4. **Partial and expression indexes**: the predicate or the
   expression in the index's catalog record, maintenance and
   the build evaluating it.
   → verify: the index agrees with a brute-force scan of the
   heap, filtered, at every isolation level.

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

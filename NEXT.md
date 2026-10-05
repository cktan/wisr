# NEXT — what to build next, and in what order

Levels 1 and 2 are built, sm3's catalog with them, and the C
wrapper and the `rel` driver run on it. This file bridges that
state and `sm3/PLAN.md` step 3, index maintenance: what stands
in the way, which of those are decisions rather than code, and
an order that clears them.

`sm3/PLAN.md` stays sm3's order of work. This file is
cross-level, because one candidate for the next primitive is
sm1's (§2 below), and because two gaps that block nothing
here sit below and beside sm3 (§5 below).

---

## 0. Where the stack stands

| Level | State |
| --- | --- |
| sm1 | built and tested |
| sm2 | built but for TOAST (`sm2/SM2_DESIGN.md` §8.2) |
| sm3 | catalog, schema locks and object mapping built; `SM3_DESIGN.md` §3 to §6 are design |
| top | `include/sm.h` over all three, the `sm_cat_*` calls included; `rel` opens its tables through them |

`sm3/PLAN.md` steps 1 and 2 are both done. Step 2's object
mapping arrived inside the catalog's DDL rather than as a step
of its own, which is why `PLAN.md` still reads as though it
were pending:

| What | Where |
| --- | --- |
| one sm1 store per table | `sm3/src/catalog/ddl.cc:293` |
| `DROP TABLE` as one `sm1::rmstore` | `sm3/src/catalog/ddl.cc:339` |
| index prefixes in increasing order, never reused | `sm3/src/catalog/ddl.cc:488` |
| `DROP INDEX` as a range delete over the prefix | `sm3/src/catalog/ddl.cc:526` |

Everything below is about step 3.

---

## 1. Who owns re-evaluation

**Start from what `SM3_DESIGN.md` §3 said when this section
was written**, because it was not silent. Its "Read committed
writes" bullet gave the re-check to sm3: sm2 "does not resolve
it, because the resolution needs a filter", so sm3 would call
`rel_read_latest` on the base row, re-check the statement's
filter against that image, and repeat the write only if the
row still qualified. sm3 owning re-evaluation was the design
of record. §3's closing "Open" — whether PostgreSQL's executor
drives maintenance instead — was a proposal to deviate from
that, not a blank slate.

The rest of this section argues the split that §3 now records
(§4's step 1, done): the qual check is the executor's, the
index fix-up is sm3's, and sm3 drives maintenance.

### 1.1 Why the re-check cannot be left to sm1

sm1 forces the re-read and can do no more. `read_latest` marks
the lock entry, and a write without a marked lock is refused
again (`sm1/SM1_DESIGN.md` §5.4, "The re-read is enforced, not
requested"). What sm1 cannot know is the statement's
predicate, so it cannot tell a caller that the row has stopped
qualifying. §5.4 names the case that proves it: `UPDATE R1 SET
n = 1 WHERE n = 0`, where "T2's re-check must fail and T2 must
write nothing, so letting the write through is not enough".

Two workers claiming one job, both at READ COMMITTED:

```sql
-- row 7: state='free', owner=NULL
UPDATE jobs SET state='taken', owner=:me WHERE state='free';
```

| Step | What happens |
| --- | --- |
| 1 | both statements take a snapshot in which row 7 is `free` |
| 2 | T1 writes row 7 and holds the row lock |
| 3 | T2's write blocks on that lock |
| 4 | T1 commits; T2's write is refused with `SM_ERR_UPDATED` |
| 5 | T2 reads the latest image: `state='taken'` |
| 6 | T2 re-evaluates `state='free'` against it, false, and writes nothing |

Without that last step T2 writes `owner='w2'`, the job is
claimed twice, and nothing raises an error anywhere: the
statement reports one row and the application believes it won
the race.

### 1.2 Why the index makes it sm3's

With an index on `state`, T2's snapshot image puts the entry to
remove at key `'free'`, while the entry that exists is at
`'taken'`. Repeating the write off the stale image erases a
`'free'` entry that is already gone and leaves `'taken'`
pointing at a row just overwritten — the heap and the index
disagree, which `PLAN.md` step 3 calls the property that
matters most. §3 says it too: sm3 takes the entries to remove
from the old image the repeated write hands back, "so the
entries removed are the ones that are actually there".

The same race, with an index on `state` and a second statement
whose filter row 7 still passes after T1, so the re-check of
1.1 lets the write through and only the index can go wrong:

```sql
-- row 7: state='free', index entry ('free', 7)
UPDATE jobs SET state='taken', owner=:me WHERE state='free'; -- T1
UPDATE jobs SET state='done' WHERE state<>'done';            -- T2
```

| Step | What happens |
| --- | --- |
| 1 | both statements take a snapshot in which row 7 is `free` |
| 2 | T1 writes row 7, replacing `('free', 7)` with `('taken', 7)` |
| 3 | T1 commits; T2's write is refused with `SM_ERR_UPDATED` |
| 4 | T2 reads the latest image, `state='taken'`; `'taken' <> 'done'` still holds, so it repeats the write |
| 5 | off its snapshot image T2 removes `('free', 7)`, which is gone, and inserts `('done', 7)` |
| 6 | the heap says `done`; the index holds `('taken', 7)` and `('done', 7)` |

Step 5 is the bug: T2 has two old images of row 7 and uses
the wrong one. The first is what its scan read at the
snapshot, `state='free'`. The second is what the repeated
`rel_update` returns, the version it actually overwrote, which
is T1's `state='taken'`. The index entry to remove must be
computed from the second. Then step 5 removes `('taken', 7)`
and inserts `('done', 7)`, and the index holds one entry that
matches the heap.

With the first, the index is invalid from step 5 on: every
later snapshot sees two entries for row 7 where the invariant
is one visible entry per row per index. Until then it held,
since T1 swapped one entry for another. What a query sees
depends on the key it asks for. A scan for `state='done'` is
right, and a scan for `state='taken'` returns a row that is
not taken. Nothing rechecks the leftover entry against the
base row, and again nothing raises an error.

A partial index sharpens it. For `jobs(owner) WHERE
state='free'`, deciding whether to remove the entry means
evaluating the *index's* predicate against the new image, not
the statement's. That is a second and independent need for an
evaluator inside sm3.

Two jobs are in play, and "owning" one means being the layer
whose code does it. *Evaluation* decides whether a predicate
holds for a row image: the statement's filter in 1.1, the
index's predicate here. *Maintenance* removes and inserts
index entries when a base row changes, as in steps 2 and 5
above.

For a plain index the two jobs split cleanly, and 1.6 splits
them: the executor evaluates the statement's filter and
re-issues the write, and sm1 hands sm3 the overwritten image
whoever asked for the write. Only a partial or expression
index ties them together, because maintaining one means
evaluating its predicate, so the layer that maintains it must
be able to evaluate.

### 1.3 What an evaluator in sm3 would take

1.6 settles that the READ COMMITTED re-check does not need
one. This is what an evaluator costs where it *is* wanted:
§5's residual filters, and partial and expression index
maintenance.

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
  a classifier above decides pushdown or fallback, and
  `EvalPlanQual` stays for the fallback. Pushing a filter down
  does not remove the executor's path, it adds one beside it,
  and that is the honest cost.
- **Semantic agreement with PostgreSQL**, which is where the
  bugs will be: NULL propagation, collation (§3 already
  requires sm3's own comparisons to follow it, and to hash the
  sort key as `hashtext` does under a non-deterministic one),
  decimal comparison, coercion. A divergence surfaces as wrong
  rows, the failure mode a test suite is worst at catching.

One bound on the work, worth writing down because it keeps the
re-check per-row: **re-evaluation only ever drops rows.** A row
that did not qualify at the statement snapshot is never
visited, so the check can remove a row from the update set and
never add one. No re-scan is needed.

One bound on the subset:

```sql
UPDATE orders o SET status='hold' FROM customers c
 WHERE o.cust_id = c.id AND c.region = 'EU';
```

The newest image of `orders` says nothing about whether
`c.region` matches: that takes the customer row the executor
joined, which sm3 was never given. That qual is the executor's
at every isolation level. 1.8 works it through.

### 1.4 What it buys

- **§5 needs the evaluator regardless**: "the residual filter
  is evaluated below the API instead of returning rows for a
  caller to discard" (`SM3_DESIGN.md:430`-432). The evaluator
  is owed to §5 whatever becomes of the re-check.
- **Partial and expression indexes come with it**, which was
  most of the case for handing maintenance to the executor.
- **`rel` and any non-PostgreSQL consumer get predicate
  scans.** `OBJECTIVE.md` keeps a consumer other than the fork
  possible, and without an evaluator below the API such a
  consumer filters rows itself — which is the waste §5 exists
  to remove.
- **The chosen precedent did this.** YugabyteDB "replaced whole
  executor nodes to push work down — sequential scan, bitmap
  scan, batched nested loop, expression evaluation"
  (`PART4.md:199`-201).

### 1.5 Recommendation

**Re-evaluation stays above sm3; sm3 takes the index
fix-up.** 1.6 has the decomposition this rests on: deciding
whether a row still qualifies needs the statement's qual and
is the executor's, while removing the index entries that are
actually there needs only the newest old image, which sm1
hands back on the retried write. sm3 needs no evaluator to be
correct at READ COMMITTED.

**An evaluator is still wanted, for two things that are not
the re-check:** §5's residual filters, and partial and
expression index maintenance. Wherever it lands, the subset is
1.3's — equality and range comparisons on sm2's scalar types
with collation, conjunctions, and `IS [NOT] NULL`, no `OR` and
no functions — because UDFs, joins and volatile functions
cannot descend at all.

Unchanged either way: the rules of a unique index's key (§2),
the online build under the schema lock (§4 below, step 5) and
the invariant that the index and the heap never disagree are
all sm3's.

**The consequence for order:** §5's API shape is not a
precondition of index maintenance, so it stays where
`PLAN.md` has it, after step 3, and the evaluator arrives with
it. §4 below is ordered that way.

### 1.6 The decomposition

The re-check is two jobs rather than one, and they need
different knowledge:

| | Needs | Whose |
| --- | --- | --- |
| (a) decide whether the row still qualifies, then write or skip | the statement's qual | above sm3: the executor's |
| (b) remove the index entries that are actually there | the newest old image | sm3's |

**(b) needs no predicate at all.** §3 supplies the mechanism:
"The old image the repeated write hands back is the newest
version's, not the one the statement's scan read", so the
entries removed are the ones that are actually there. So when
the executor re-checks the qual and re-issues the write, sm1
hands sm3 the newest old image, and index maintenance is
correct with sm3 never learning what a qual is.

That is what 1.5 rests on: **sm3 needs no evaluator for the
READ COMMITTED path.** An evaluator
is still wanted, but for two independent features — §5's
residual filters, and partial and expression index maintenance
— and neither of those is the re-check.

Nothing passes a qual across `include/sm.h` today in any case:
a scan takes a `rowno` range or an `sm_bound_t`. And
`sm1/SM1_DESIGN.md` §5.4 says plainly where the job sits —
"The executor's side of the exchange belongs to the PostgreSQL
fork, not to sm1".

`SM3_DESIGN.md` §3 was ambivalent, and read both ways.
One sentence had sm3 "re-checks the statement's filter against
that image"; four sentences later, "sm1 refuses a write
repeated without the read, so **an executor** cannot forget
it". It gave the check to sm3 while assuming an executor was
the one making it, and never said where sm3 would get a
filter.

**The rule: ownership of re-evaluation follows ownership of the
qual, and the qual lives above.** sm3's share is the index
fix-up from the newest image.

That correction has been made, in `SM3_DESIGN.md` and not in
this file: §3's "Read committed writes" bullet now gives the
qual check to the executor and the index fix-up to sm3 (§4's
step 1).

### 1.7 The fix-up, step by step

1.2's race again, with the split of 1.6 applied. One index on
`state`, and row 7 starts as `(id=7, state='free',
owner=NULL)`:

```sql
CREATE INDEX jobs_state ON jobs(state);   -- holds ('free', 7)

-- T1, worker w1
UPDATE jobs SET state='taken', owner='w1' WHERE state='free';
-- T2, a reaper, READ COMMITTED
UPDATE jobs SET state='done' WHERE state <> 'done';
```

The executor needs a way into sm3, and there is none today.
Its write call is `sm_rel_update` in `include/sm.h`, on a heap
relation that `src/sm.cc` hands straight to sm2, so the call
knows of no index. `sm_idx_insert` and `sm_idx_delete` are
separate calls, and `sm3/include/sm3.h` has the catalog and
the transaction calls but nothing that writes a row. The steps
below assume a table-level write that sm3 owns, which updates
the heap and maintains every index of the table in one call.
The name and shape are placeholders:

```c
int sm_tbl_update(sm_tbl_t* tbl, uint64_t rowno,
                  const sm_datum_t* values, const bool* isnull,
                  size_t n);
```

**The write case: the row still qualifies.**

| Step | Layer | What happens |
| --- | --- | --- |
| 1 | executor | T2's scan reads row 7 at its snapshot: `(7, 'free', NULL)`. It builds the new tuple `(7, 'done', NULL)`. |
| 2 | sm1 | T1 updates row 7 and commits. The heap holds `(7, 'taken', 'w1')` and the index holds `('taken', 7)`. |
| 3 | executor | T2 calls `sm_tbl_update(jobs, 7, (7, 'done', NULL))`. |
| 3a | sm3 | It calls sm2's `rel_update` on the heap. sm1 refuses it with `SM_ERR_UPDATED`. sm3 touches no index and returns the status to the executor as a non-error. |
| 4 | executor | It calls `sm_rel_read_latest(7)`, which marks the lock and returns `(7, 'taken', 'w1')`. sm3 has no part in it: no index is involved. |
| 5 | executor | It evaluates T2's qual against that image, whose `state` is now `'taken'` and no longer the `'free'` its scan saw. `'taken' <> 'done'` is true, so the row still qualifies. It rebuilds the new tuple from the image: `(7, 'done', 'w1')`. |
| 6 | executor | It calls `sm_tbl_update(jobs, 7, (7, 'done', 'w1'))`. Steps 6a to 6c run inside that one call. |
| 6a | sm3 | It calls sm2's `rel_update` on the heap again. sm1 accepts it and hands back the old image `(7, 'taken', 'w1')`. |
| 6b | sm3 | It copies that old image, because the next sm2 call on the session reuses the memory. |
| 6c | sm3 | For each index in the table's definition, it takes the old key from the copy and the new key from the new values. For `jobs_state`, `'taken'` and `'done'` differ, so `idx_delete('taken', 7)` then `idx_insert('done', 7)`. It returns 0 to the executor. |
| 7 | — | The heap holds `(7, 'done', 'w1')` and the index holds `('done', 7)` only. |

Steps 6a to 6c are the fix-up, and they are the same code as an
ordinary update. Step 3a is that code stopping early, before
it reaches an index. The point is in step 6c: the old key comes
from the image returned in step 6a, not from the `'free'`
image the scan read in step 1. An index on `owner` would be left
alone in step 6c, since its old and new keys are both `'w1'`.

**What "marks the lock" means in step 4.** The mark is a
one-bit flag recording that this transaction has looked at
the newest version of the row, and sm1 refuses a repeated
write without it (`sm1/SM1_DESIGN.md` §5.4). T2 holds the
exclusive lock on row 7 from the refused write in step 3a
onward. The bit sits on T2's entry for that lock in its own
session's lock list, not on the shared lock table.

| Event | Effect on the mark |
| --- | --- |
| first write refused with `SM_ERR_UPDATED` | lock taken, mark not set |
| `read_latest` on that row | mark set |
| write repeated with the mark set | write goes through; mark cleared |
| write repeated without the mark | refused again with `SM_ERR_UPDATED` |
| rollback to a savepoint taken before the lock | lock released, mark gone with it |

The mark makes the re-read mandatory. Without it an executor
could take `SM_ERR_UPDATED`, call the write again, and
overwrite T1's change with no re-check: 1.1's double claim.

- **The image read cannot go stale.** T2 holds the row lock
  throughout, so nobody changes row 7 between its
  `read_latest` and its repeated write.
- **The mark is single-use.** A successful write clears it, so
  a later write to the row in the same transaction needs a
  fresh read.
- **sm1 forces the read, not the re-check.** Whether the qual
  is evaluated against that image is still up to the
  executor.

**The skip case: the row no longer qualifies.** Replace T2
with a second worker running 1.1's claim statement:

```sql
-- T2, worker w2
UPDATE jobs SET state='taken', owner='w2' WHERE state='free';
```

Steps 1 to 4 are the same. Then:

| Step | Layer | What happens |
| --- | --- | --- |
| 5 | executor | It checks `state='free'` against `(7, 'taken', 'w1')`: false. It skips the row. |
| 6 | sm3 | Nothing. No write is issued and no index call is made. |
| 7 | — | The heap holds `(7, 'taken', 'w1')`, the index holds `('taken', 7)`, and T2 reports zero rows. |

In both cases sm3 never sees a predicate: the executor
decides, and sm3 acts only on the image sm1 hands back.
`rel_update`, `idx_delete` and `idx_insert` are the sm2 calls
sm3 would make, and `sm_rel_read_latest` is the executor's own
call through `include/sm.h`.

The alternative to a table-level call is to keep
`sm_rel_update` and have the executor call `sm_idx_*` itself,
as PostgreSQL's executor does natively. It adds no API, but
then the fix-up is the executor's and not sm3's, against 1.5,
and an executor can forget an index or use the wrong old
image. §4's step 4 has to design the entry point before it
writes the maintenance.

### 1.8 A qual that spans two tables

1.3's join, worked through as 1.7 was, with the same
placeholder `sm_tbl_update`:

```sql
-- orders:    row 42 = (id=42, cust_id=5, status='new')
-- customers: row 5  = (id=5, region='EU')
--            row 9  = (id=9, region='US')
CREATE INDEX orders_status ON orders(status); -- holds ('new', 42)

-- T2, READ COMMITTED
UPDATE orders o SET status='hold' FROM customers c
 WHERE o.cust_id = c.id AND c.region = 'EU';
```

A tuple below is written with its table's name in front, as
in `orders(42, 5, 'new')` or `customers(5, 'EU')`. Only
`orders` is written. `customers` is read at T2's
statement snapshot and never locked.

**Case A: the order row changes under T2.** T1 runs `UPDATE
orders SET cust_id=9 WHERE id=42` and commits after T2's
snapshot.

| Step | Layer | What happens |
| --- | --- | --- |
| 1 | executor | T2's join pairs order 42 `orders(42, 5, 'new')` with customer 5 `customers(5, 'EU')` at its snapshot. It builds the new tuple `orders(42, 5, 'hold')`. |
| 2 | sm1 | T1 commits. The heap holds `orders(42, 9, 'new')`; the index is unchanged. |
| 3 | executor | T2 calls `sm_tbl_update(orders, 42, (42, 5, 'hold'))`. |
| 3a | sm3 | sm2's `rel_update` is refused with `SM_ERR_UPDATED`. sm3 touches no index and returns the status. |
| 4 | executor | It calls `sm_rel_read_latest(orders, 42)` and gets `orders(42, 9, 'new')`. |
| 5 | executor | It re-runs the join for this one row: the latest order image against the customer row it had already joined, `customers(5, 'EU')`. `o.cust_id = c.id` is `9 = 5`: false. It skips the row. |
| 6 | sm3 | Nothing. No write is issued and no index call is made. |
| 7 | — | The heap holds `orders(42, 9, 'new')`, the index `orders_status` holds `('new', 42)`, and T2 reports zero rows. |

Step 5 is the point. The qual needs the customer row, and
`sm_tbl_update` carries a row of `orders` and nothing else. An
evaluator in sm3 holding only the latest order image could not
decide it.

**Case B: only the customer changes.** T1 runs `UPDATE
customers SET region='US' WHERE id=5` and commits after T2's
snapshot.

- Order 42 was not touched, so `sm_tbl_update` succeeds the
  first time and there is no `SM_ERR_UPDATED`.
- T2 sets the order to `'hold'` on the strength of
  `region='EU'` as its snapshot saw it.
- sm3 does an ordinary update: the old image is `orders(42,
  5, 'new')`, so `idx_delete('new', 42)` and
  `idx_insert('hold', 42)`.

No re-check happens at all, and that is correct at READ
COMMITTED: only a conflict on the row being written triggers
one.

**Case C: the order changes and still qualifies.** T1 runs
`UPDATE orders SET status='shipped' WHERE id=42` and commits.

- Steps 1 to 4 are case A's, and the latest image is
  `orders(42, 5, 'shipped')`.
- At step 5 the join holds, `5 = 5` and `'EU'`, so the
  executor rebuilds the new tuple `orders(42, 5, 'hold')` and
  calls `sm_tbl_update` again.
- sm3's fix-up is 1.7's: the old image handed back is
  `orders(42, 5, 'shipped')`, so it removes `('shipped', 42)`
  and inserts `('hold', 42)`.

What the three cases show:

- **sm3's share is unchanged.** It sees one row of `orders`
  and the image sm1 hands back, as in 1.7.
- **The executor's share is larger.** It keeps the joined
  customer row and re-runs the join for one row, which is the
  work of PostgreSQL's `EvalPlanQual`.
- **The re-check uses the customer row already joined**, not
  the newest one. In case A the order is skipped even if
  customer 9 were in the EU. This is `EvalPlanQual`'s
  treatment of non-target tables as recalled here, and it is
  to be checked against the PostgreSQL source before the fork
  relies on it.

### 1.9 What the C API shows the executor

Once the table-level write exists, the heap-level write calls
leave `include/sm.h`:

- **They let a caller break the invariant.** With
  `sm_rel_update` exposed, any caller can change a row and
  leave its index entries behind. The table-level write keeps
  the index and the heap in agreement only if it is the sole
  way to write a row.
- **The header already calls them temporary.** The comment on
  `sm_rel_open` reads "Scaffolding, until sm3 opens tables".
- **Nothing is lost.** On a table with no index the
  table-level write does what `sm_rel_update` does.

| Call | Verdict |
| --- | --- |
| `sm_rel_insert`, `sm_rel_update`, `sm_rel_delete` and their `_tuple` forms | remove; the table-level writes replace them |
| `sm_idx_insert`, `sm_idx_delete` | remove; only sm3 writes index entries |
| `sm_rel_open` | remove; a table is opened through the catalog |
| `sm_rel_read`, `sm_rel_read_latest`, `sm_rel_scan`, the index scans | keep the operations, on the table handle |

sm2's own `rel_update` and `idx_*` stay as they are, and sm3
calls them.

**The `rel` driver follows the same rule**: it keeps the raw
reads and scans and loses the raw writes. `rel/src/rel_ops.cc`
and `rel/src/rel_catalog.cc` use the heap-level calls and move
to the table-level ones in the same change.

- **`rel` exists to drive the public API** (`rel/REL.md`). A
  private write path would test something no consumer runs.
- **`rel` does sm3's job by hand today.** `rel_ops.cc` pairs
  `sm_rel_update` with `sm_idx_delete` and `sm_idx_insert`
  itself. Once sm3 owns maintenance that is a second
  implementation, and it can pass while sm3's is broken.
- **sm2 is tested where it lives.** `sm2/test/test_rel.cc` and
  `sm2/test/test_index.cc` test the heap and the index in
  C++, so `rel` gives up no coverage of sm2.
- **The check that matters needs only reads.** §4's step 4
  asks that the index agree with a brute-force scan of the
  heap, which takes a raw heap scan and a raw index scan.

The one case for raw writes in QA is deliberate corruption:
writing a heap row without its index entry to prove that a
consistency checker catches it. Those calls would go in a
test-only header the fork never includes, not back in
`include/sm.h`, and not before there is a checker to test.

**Re-evaluation loses nothing by this.** It never calls
`sm_rel_update` or `sm_idx_*`. These are the calls the
executor makes in 1.7:

| Step | What the executor does | Call it needs |
| --- | --- | --- |
| 3 | tries the write and learns of the conflict | `sm_tbl_update`, which returns `SM_ERR_UPDATED` |
| 4 | fetches the latest image of the row | `sm_rel_read_latest`, kept |
| 5 | evaluates the qual against that image and rebuilds the new tuple | none; its own code on its own memory |
| 6 | writes again, or skips | `sm_tbl_update` again, or nothing |

The executor's job is to take an error code, read one row,
decide, and call the same write again or not. The removed
calls do sm3's share and not the executor's: `rel_update` at
steps 3a and 6a, `idx_delete` and `idx_insert` at step 6c.

One thing must stay true. sm1 accepts the repeated write only
if `read_latest` marked the lock first
(`sm1/SM1_DESIGN.md` §5.4). `sm_rel_read_latest` does that
today, and whatever read ends up on the table handle must keep
doing it.

### 1.10 Bottom line

**sm guarantees that the index and the heap never disagree;
the executor decides which rows get written.**

- **Tuple and index together are sm's alone.** Once a write is
  issued, sm3 updates the heap, takes the old image sm1 hands
  back, and fixes every index from it (1.7, steps 6a to 6c).
  It needs nothing from the executor for that.
- **Whether the row still qualifies is the executor's.** After
  `SM_ERR_UPDATED`, the statement's qual is re-checked against
  the latest image, and the outcome is write or skip. sm never
  sees the qual, and in 1.8's join it does not have the other
  table's row either.

The line is not drawn by the index. 1.1's double claim happens
on a table with no index at all.

Two caveats:

- **Today the executor would have to do both.** The
  table-level write does not exist, so a caller must pair
  `sm_rel_update` with `sm_idx_*` itself to keep the two in
  step. §4's step 4 closes that gap.
- **Partial and expression indexes are the exception.**
  Maintaining one means evaluating the index's own predicate
  or expression, so sm needs an evaluator of its own (§4's
  step 6), though still no help from the executor per write.

---

## 2. The missing primitive, and how much it is really needed

**Settled (§4's step 2, done).** Both recommendations below
were taken, and the line numbers this section cited are gone
with the text they pointed at. `SM3_DESIGN.md` §3 now says one
thing about NULL keys, an over-long key in a unique index is
refused whether or not a key column is NULL, and §7 records
the trade-off. The primitive is unbuilt. What follows is the
argument as it was made.

`SM3_DESIGN.md` §3 flagged it: **"Open: nothing reads a range
at its newest version."** The chain that led there:

1. A unique index is keyed `encode(sk)` with no `rowno`
   suffix, so the KV key is the uniqueness unit, and sm1's
   `SM_ERR_DUPKEY` plus its write-write conflict enforce
   uniqueness with no check of sm3's own.
2. An entry whose key sm2 had to cut cannot lean on that: two
   different originals can share the cut bytes. It carries the
   `rowno` suffix, so `DUPKEY` never fires, and uniqueness
   needs an explicit check that compares originals over the
   cut key's prefix range.
3. At READ COMMITTED that check has to read the newest
   committed versions rather than the statement snapshot,
   because the snapshot can predate a duplicate that committed
   while the X lock was awaited.

Nothing in the stack reads a *range* that way:

| Call | Reads | Where |
| --- | --- | --- |
| `sm1::read_latest` | one key, newest committed | `sm1/include/sm1.h:551` |
| `sm2::catkv_get_latest` | one key, newest committed | `sm2/include/sm2.h:659` |
| `sm1::scan` | a range, at the snapshot | `sm1/include/sm1.h:896` |
| `sm2::idx_scan` | a range, at the snapshot | `sm2/include/sm2.h:604` |

### 2.1 Settle a contradiction first — it sets the urgency

`SM3_DESIGN.md` §3 said two kinds of entry "still need the
existence check": one with a NULL key column, and one whose
key was cut. Its next bullet then said an entry with a NULL
key column "skips the uniqueness check, as SQL requires by
default". Both could not hold.

**The second is right.** SQL's default is that NULLs are
distinct, `NULLS NOT DISTINCT` is refused in v1, and a
NULL-bearing entry carries the `rowno` suffix — so no other
entry can collide with it and there is nothing to check. The
first sentence over-states by including the NULL case.

That matters well beyond tidiness. If only cut keys need the
check, the newest-version range read is wanted on **one rare
path** — an over-long index key — and not on the common one.
It stops being a precondition for step 3 at all.

### 2.2 Two ways to take it

- **Build the primitive.** A range scan at the newest
  committed version, in sm1, surfaced through sm2's index
  `rel`. Its hard question is locking: `read_latest` requires
  a lock already held on the key it reads
  (`sm1/include/sm1.h:541`-543), and a range has no single key
  to lock, so the primitive needs a rule of its own — an X
  lock on the cut key's prefix, or a lock per key visited.
- **Reshape the check**, which `SM3_DESIGN.md` §3 already
  left room for. The cheapest shape: refuse an over-long key
  in a unique index in v1, and the path disappears until
  someone needs it. PostgreSQL itself refuses an over-long
  B-tree index row rather than cutting it, so this is not a
  wisr-specific limitation — sm2 cuts keys for the benefit of
  non-unique indexes, where a recheck against the base row
  already handles it.

**Recommendation: settle 2.1, then refuse over-long unique
keys in v1 and leave the primitive unbuilt.** It is a new
concurrency primitive at level 1 — the level where being wrong
costs the most — bought for the rarest path in step 3. Record
it in `SM3_DESIGN.md` §7 as an accepted trade-off, where the
next reader will find it, and build it when a workload asks.

---

## 3. The scenario suite: sm3 needs a driver of its own

`PLAN.md` deferred this "for the first scenario that needs it,
not before". Step 3 is that scenario: an index build against a
concurrent writer, and DDL against DML.

**It cannot be sm1's driver extended.** `build/driver` is one
verb per sm1 API call (`sm1/mvcc/DRIVER.md` §0) and links
`sm1/build/libwisr.a` alone. Teaching it `create table`, or an
index insert, would make sm1's test binary link sm2 and sm3 —
which inverts the stack, since a level may not depend on one
above it. So either a driver in `sm3/` over sm3's calls, or
the driver moves somewhere level-neutral and takes verbs per
level. The first is smaller and leaves sm1's 92 scripts alone.

What to copy verbatim, because `DRIVER.md` §0 argues for it
and it earned its keep in sm1: comments echoed into the
transcript, `check` steps in the script rather than only in
the expected output, and `accept` as a target that `test`
never runs. The hazard it names — "a diff is not an
intention" — bites harder here, because an index-build
transcript is long and nobody reads all of it.

---

## 4. Then the code, in this order

`SM3_DESIGN.md` §3 opened "*To be written.*" though its
bullets were detailed. This repo argues a protocol before
writing it, so the prose and the decisions come before code.

1. **Finish `SM3_DESIGN.md` §3's prose** against 1.5's split,
   settling §3's own ambivalence about who re-checks the qual
   (1.6).
   → verify: the section no longer says "to be written", and
   says plainly that the qual check is the executor's and the
   index fix-up sm3's.
   **Done.** §3 also says the executor writes through one
   table-level call (1.7, 1.9), and closes its "Open" on who
   drives maintenance in favour of sm3.
2. **Settle the NULL/cut contradiction** (§2.1) and record the
   over-long-key decision in `SM3_DESIGN.md` §7.
   → verify: §3 says one thing about NULL keys, not two.
   **Done.** An over-long key in a unique index is refused,
   with or without a NULL column, in `SM3_DESIGN.md` §3 and
   §7, `sm2/SM2_DESIGN.md` §3.2 and §5, and `DESIGN.md` §5.
3. **sm3's scenario driver** (§3 above), with one scenario
   passing: two sessions, DDL against DML, over the catalog
   that is already built.
   → verify: `make -C sm3 test` runs it and diffs a transcript.
4. **Index maintenance** — first the entry point the executor
   calls, a table-level write that sm3 owns (1.7), which
   `include/sm.h` lacks, and the heap-level writes it retires
   (1.9). Then insert, update and delete, the old
   image copied before the next call on the session
   (`SM3_DESIGN.md:279`-283), and the index fix-up from the
   newest old image after `SM_ERR_UPDATED`, with no predicate
   of sm3's own. sm2's index `rel` already refuses an
   over-long key in a unique index with `SM_ERR_KEYTOOLONG`
   (`sm2/src/rel/index.cc`, `entry_key`).
   → verify: scenarios at every isolation level in which the
   index and a brute-force scan of the heap agree, the
   claim-once shape of 1.1 among them.
5. **The online build under S**, which `CATALOG.md` §7.1 waits
   on: `cat_create_index` takes X today and takes S once the
   build exists, with the `indcheckxmin` guard for a reader
   whose snapshot predates the index, and DDL in a prepared
   transaction refused.
   → verify: a writer in flight when the build starts is not
   missed; a reader holding IS is never blocked.
6. **§5's predicate scans, with the evaluator** (1.3): the API
   shape, the pushdown subset, and the residual filter
   evaluated below the API. Partial and expression indexes
   follow it.
   → verify: §5's own criterion — for generated predicates,
   the rows returned match a brute-force scan-and-filter.

Step 5 carries the sharpest cost already written down: the
schema row is a hot key, every reader and writer of the table
locks it, and the lock table is one mutex rather than sharded
(`SM3_DESIGN.md` §3). That is where the first benchmark should
point.

---

## 5. Not blocking step 3

- **sm2's TOAST** (`sm2/SM2_DESIGN.md` §8.2, step 3). The
  `0xFFFF` prefix is reserved per table and unused. Wholly
  independent of the above, so it can run in parallel.
- **Temporary tables** (`CATALOG.md` §7.2): a temporary store,
  its definition in the session's own temporary catalog and
  never written to store 0.
- **Namespaces past the two built in**, dependency checks such
  as refusing `DROP COLLATION` in use, and the unmeasured
  startup cost for thousands of tables — all three open in
  `CATALOG.md` §7.

---

## 6. What this file is not

Not a replacement for `sm3/PLAN.md`, which holds sm3's order
of work and the reasoning behind it, nor for `SM3_DESIGN.md`,
where the protocols are argued before they are built. A
decision settled here moves into one of those and leaves this
file.

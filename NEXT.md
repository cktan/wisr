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

**Start from what `SM3_DESIGN.md` §3 already says**, because it
is not silent here. Lines 292-297 give the READ COMMITTED
re-check to sm3: sm2 "does not resolve it, because the
resolution needs a filter", so sm3 calls `rel_read_latest` on
the base row, re-checks the statement's filter against that
image, and repeats the write only if the row still qualifies.
sm3 owning re-evaluation is the design of record. §3's closing
"Open" — whether PostgreSQL's executor drives maintenance
instead — is a proposal to deviate from that, not a blank
slate, and moving maintenance up moves the re-check with it.

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
matters most. §3 says it too: "the old image the retried write
hands back is the newest version's, so the index entries
removed are the ones that are actually there".

The same race, with an index on `state` and a second statement
whose filter row 7 still passes after T1, so the re-check of
1.1 lets the write through and only the index can go wrong:

```sql
-- row 7: state='free', index entry ('free', 7)
UPDATE jobs SET state='taken', owner=:me WHERE state='free'; -- T1
UPDATE jobs SET state='done' WHERE id=7;                     -- T2
```

| Step | What happens |
| --- | --- |
| 1 | both statements take a snapshot in which row 7 is `free` |
| 2 | T1 writes row 7, replacing `('free', 7)` with `('taken', 7)` |
| 3 | T1 commits; T2's write is refused with `SM_ERR_UPDATED` |
| 4 | T2 reads the latest image; `id=7` still holds, so it repeats the write |
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
`c.region` still matches, and `c` may have changed too. That
qual is the executor's at every isolation level.

### 1.4 What it buys

- **§5 needs the evaluator regardless**: "the residual filter
  is evaluated below the API instead of returning rows for a
  caller to discard" (`SM3_DESIGN.md:406`-408). The evaluator
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

Unchanged either way: the cut-key unique check (§2), the
online build under the schema lock (§4 below, step 5) and the
invariant that the index and the heap never disagree are all
sm3's.

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

**(b) needs no predicate at all.** §3 already supplies the
mechanism: "the old image the retried write hands back is the
newest version's, so the index entries removed are the ones
that are actually there". So when the executor re-checks the
qual and re-issues the write, sm1 hands sm3 the newest old
image, and index maintenance is correct with sm3 never
learning what a qual is.

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

`SM3_DESIGN.md` §3 is ambivalent, and reads both ways.
One sentence has sm3 "re-checks the statement's filter against
that image"; four sentences later, "sm1 refuses a write
repeated without the read, so **an executor** cannot forget
it". It gives the check to sm3 while assuming an executor is
the one making it, and never says where sm3 would get a
filter.

**The rule: ownership of re-evaluation follows ownership of the
qual, and the qual lives above.** sm3's share is the index
fix-up from the newest image.

One correction is still owed, to `SM3_DESIGN.md` rather than
to this file: §3's bullet gives the re-check to sm3 while
assuming an executor makes it. §4's step 1 rewrites that prose
and should settle it there.

### 1.7 The fix-up, step by step

1.2's race again, with the split of 1.6 applied. One index on
`state`, and row 7 starts as `(id=7, state='free',
owner=NULL)`:

```sql
CREATE INDEX jobs_state ON jobs(state);   -- holds ('free', 7)

-- T1, worker w1
UPDATE jobs SET state='taken', owner='w1' WHERE state='free';
-- T2, a reaper, READ COMMITTED
UPDATE jobs SET state='done' WHERE id=7;
```

**The write case: the row still qualifies.**

| Step | Layer | What happens |
| --- | --- | --- |
| 1 | executor | T2's scan reads row 7 at its snapshot: `(7, 'free', NULL)`. It builds the new tuple `(7, 'done', NULL)`. |
| 2 | sm1 | T1 updates row 7 and commits. The heap holds `(7, 'taken', 'w1')` and the index holds `('taken', 7)`. |
| 3 | sm3 | T2 calls `rel_update(7, (7, 'done', NULL))`. sm1 refuses it with `SM_ERR_UPDATED`. sm3 touches no index and returns the status to the executor as a non-error. |
| 4 | executor | It calls `sm_rel_read_latest(7)`, which marks the lock and returns `(7, 'taken', 'w1')`. sm3 has no part in it: no index is involved. |
| 5 | executor | It checks `id=7` against that image: true. It rebuilds the new tuple from it: `(7, 'done', 'w1')`. |
| 6 | sm3 | It calls `rel_update(7, (7, 'done', 'w1'))` again. sm1 accepts it and hands back the old image `(7, 'taken', 'w1')`. |
| 7 | sm3 | It copies that old image before making any index call. |
| 8 | sm3 | For `jobs_state`: old key from the copy is `'taken'`, new key from the new tuple is `'done'`. They differ, so `idx_delete('taken', 7)` then `idx_insert('done', 7)`. |
| 9 | — | The heap holds `(7, 'done', 'w1')` and the index holds `('done', 7)` only. |

Steps 6 to 8 are the fix-up, and they are the same code as an
ordinary update. The point is in step 8: the old key comes
from the image returned in step 6, not from the `'free'` image
the scan read in step 1. An index on `owner` would be left
alone in step 8, since its old and new keys are both `'w1'`.

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
decides, and sm3 acts only on the image sm1 hands back. The
call shapes are illustrative. `rel_update`, `idx_delete` and
`idx_insert` are the sm2 calls sm3 would make, and
`sm_rel_read_latest` is the executor's own call through
`include/sm.h`. sm3's index maintenance is §4's step 4 and is
not written yet.

---

## 2. The missing primitive, and how much it is really needed

`SM3_DESIGN.md:251` flags it: **"Open: nothing reads a range
at its newest version."** The chain that leads there:

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

`SM3_DESIGN.md:244`-247 says two kinds of entry "still need
the existence check": one with a NULL key column, and one
whose key was cut. `SM3_DESIGN.md:258`-261 then says an entry
with a NULL key column "skips the uniqueness check, as SQL
requires by default". Both cannot hold.

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
- **Reshape the check**, which `SM3_DESIGN.md:256` already
  leaves room for. The cheapest shape: refuse an over-long key
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

`SM3_DESIGN.md` §3 still opens "*To be written.*" though its
bullets are detailed. This repo argues a protocol before
writing it, so the prose and the decisions come before code.

1. **Finish `SM3_DESIGN.md` §3's prose** against 1.5's split,
   settling §3's own ambivalence about who re-checks the qual
   (1.6).
   → verify: the section no longer says "to be written", and
   says plainly that the qual check is the executor's and the
   index fix-up sm3's.
2. **Settle the NULL/cut contradiction** (§2.1) and record the
   over-long-key decision in `SM3_DESIGN.md` §7.
   → verify: §3 says one thing about NULL keys, not two.
3. **sm3's scenario driver** (§3 above), with one scenario
   passing: two sessions, DDL against DML, over the catalog
   that is already built.
   → verify: `make -C sm3 test` runs it and diffs a transcript.
4. **Index maintenance** — insert, update and delete, the old
   image copied before the next call on the session
   (`SM3_DESIGN.md:283`-287), and the index fix-up from the
   newest old image after `SM_ERR_UPDATED`, with no predicate
   of sm3's own.
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

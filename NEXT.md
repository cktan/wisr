# NEXT — what to build next, and in what order

Levels 1 and 2 are built, sm3's catalog with them, and the C
wrapper and the `rel` driver run on it. This file bridges that
state and `sm3/PLAN.md` step 3, index maintenance: what stands
in the way, which of those are decisions rather than code, and
an order that clears them.

`sm3/PLAN.md` stays sm3's order of work. This file is
cross-level, because the next piece of code is most likely
sm1's rather than sm3's (§2 below), and because two gaps that
block nothing here sit below and beside sm3 (§5 below).

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

## 1. The decision that sizes step 3

**Open, and it comes first: does sm3 drive index maintenance,
or does PostgreSQL's executor?** `SM3_DESIGN.md` §3 raises it
and leaves it open. Nothing else in step 3 can be scoped until
it is answered, so it is not a thing to discover halfway
through an implementation.

The case for the executor: it already writes index entries
through its index access method, evaluates expression and
partial index keys, and re-checks rows under READ COMMITTED
with `EvalPlanQual`. If it keeps that work it reaches sm2's
index `rel` through the C wrapper, and sm3 never grows a
predicate evaluator or an expression evaluator at all.

What cannot move up, whichever way this goes —
`SM3_DESIGN.md` §3 names all three:

- the unique check on a cut key (§2 below);
- the online build under the schema lock (§4 below, step 5);
- the invariant that the index and the heap never disagree.

**Recommendation: the executor drives maintenance, and sm3
owns those three.** It keeps partial and expression indexes
out of sm3 entirely, which is most of the surface named in
`PLAN.md` step 3, and it matches the division the rest of the
stack already follows: sm3 owns protocol, not evaluation.

The cost, which should be written down before it surprises
someone: wisr's own API then cannot maintain an index without
an executor above it, so `rel` grows index maintenance of its
own for testing, or its tests drive sm2's index `rel`
directly. That is test scaffolding, and it should be labelled
as such where it lands.

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
bullets are detailed. This repo argues a protocol before it
writes one, so the prose comes first.

1. **Finish `SM3_DESIGN.md` §3's prose**, against §1's answer.
   → verify: the section no longer says "to be written", and
   says plainly what sm3 owns and what the executor owns.
2. **Settle the NULL/cut contradiction** (§2.1) and record the
   over-long-key decision in `SM3_DESIGN.md` §7.
   → verify: §3 says one thing about NULL keys, not two.
3. **sm3's scenario driver** (§3), with one scenario passing:
   two sessions, DDL against DML, over the catalog that is
   already built.
   → verify: `make -C sm3 test` runs it and diffs a transcript.
4. **Index maintenance** — insert, update and delete, the old
   image copied before the next call on the session
   (`SM3_DESIGN.md:283`-287), and the READ COMMITTED re-check
   through `rel_read_latest`.
   → verify: scenarios at every isolation level in which the
   index and a brute-force scan of the heap agree.
5. **The online build under S**, which `CATALOG.md` §7.1 waits
   on: `cat_create_index` takes X today and takes S once the
   build exists, with the `indcheckxmin` guard for a reader
   whose snapshot predates the index, and DDL in a prepared
   transaction refused.
   → verify: a writer in flight when the build starts is not
   missed; a reader holding IS is never blocked.

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

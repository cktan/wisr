# PLAN — sm3

The order of work, and why it is this order. `SM3_OBJECTIVE.md`
says what sm3 is for; `SM3_DESIGN.md` is where each step's
argument goes before its code does. It starts where sm2's
build order (`sm2/SM2_DESIGN.md` §8) stops: sm2's types,
encodings and tuple relation exist before anything here does.

The principle: **the formats were sm2's problem; the
concurrency is this one's.** Almost nothing sm3 writes is
permanent on disk — the catalog's own layout is the exception
— so what gets the scrutiny is the protocols, and the tests
that matter most are the ones with two sessions in them.

## 1. Catalog

Everything else needs a definition to work from, so it comes
first.

**Status: built** — the store, keys, values, load, DDL, a
session's overlay, and the schema locks with publishing under
them, as `CATALOG.md` documents.

- Tables, indexes, columns, types, collations, and each
  column's dropped mark and missing value, in sm3's own sm2
  relations. No schema versions: a row records its attribute
  count instead (`sm2/SM2_DESIGN.md` §4).
- In memory, written through to sm2, loaded at startup.
- The schema-row lock protocol of `SM3_DESIGN.md` §2, which is
  what makes an in-memory catalog safe.
- Decide the open question there: how a DDL change stays
  private until it commits. Settled: the overlay
  (`CATALOG.md` §4).

**Verify:** DDL rolls back with its transaction, including the
in-memory copy; a transaction that holds the shared schema
lock never sees a definition change under it; two sessions
doing concurrent DDL behave as sm1's isolation says they must.

## 2. Object mapping

**Status: built**, inside the catalog's DDL
(`SM3_DESIGN.md` §4, `src/catalog/ddl.cc`).

- One sm1 store per table (`SM3_DESIGN.md` §4): its rows under
  prefix `0x0000`, each index under a prefix of its own,
  allocated in increasing order and never reused, its TOAST
  data under `0xFFFF`.
- `DROP TABLE` is one `sm1::rmstore`; `DROP INDEX` is a range
  delete over the index's prefix.

**Verify:** a table's rows and indexes share its store without
meeting; `DROP TABLE` removes the store, indexes and all; a
dropped index's prefix is never handed out again, so its
lingering versions are never read as another index's.

## 3. Indexes

**Status: built** (`SM3_DESIGN.md` §3).

- Unique and non-unique entries, built with sm2's encoder, and
  INCLUDE columns; maintenance from the old image sm2 hands
  back, through the table-level writes that are the one way to
  write a row.
- The READ COMMITTED re-check is the executor's, since the
  filter is. sm3's share is the index fix-up from the newest
  old image (`SM3_DESIGN.md` §3.1).
- The index build: concurrent, in the background, with the
  index building, valid or failed.
- Partial and expression indexes. sm3 evaluates nothing: the
  host does, through a callback it registers, which says
  whether a row belongs in the index and computes its
  expression key columns.

**Verified:** the index and the base relation agree with a
brute-force scan, under concurrency, at every isolation level;
the build does not miss a row written while it runs
(`SM3_DESIGN.md` §3.2).

## 4. Scans

**Status: settled, and nothing left to build.** A scan takes
key bounds and filters nothing, tuple at a time
(`SM3_DESIGN.md` §5). This step was to take a predicate,
derive a key range and evaluate the residual below the API.
Deriving the range is the kept planner's, and the residual is
the executor's, since sm3 has no evaluator and gets none.

## 5. Statistics, constraints, bulk load

**Status: not started.** `NEXT.md` has constraints first.

- Row counts, sizes, sampling.
- `NOT NULL`: built, in the table-level writes
  (`SM3_DESIGN.md` §3). The length of a `varchar(n)` is the
  host's to apply, not sm3's (`DESIGN.md` §5).
- Foreign keys are the host's to check (`DESIGN.md` §5). The
  stack's share is a row locked without being written, which
  sm2's `rel_lock` takes and sm3 is still to hand out
  (`NEXT.md` §1).
- A bulk path that batches within sm1's RAM bound.

---

## Testing

sm1's two-layer arrangement is kept: C++ tests for contract
and single-session mechanics, and something declarative for
anything where two sessions interleave. sm3's concurrency
questions — an index build against a concurrent writer, DDL
against DML — are the second kind.

sm3 did not get a scenario driver of its own, nor extend
sm1's, which links sm1 alone. sm1's QA-only `set_qa_nowait`
lets one thread play several sessions: a step that would wait
fails with `SM_ERR_BLOCKED` and is repeated later. sm3's
two-session cases are written that way in `test/test_table.cc`
and as `rel` scripts; what needs real threads, deadlocks and
the background build, has them (`SM3_DESIGN.md` §3.2,
`DESIGN.md` §4).

## What is deliberately not here

- No types, no key encoding, no tuple format. All of it is
  sm2's (`sm2/SM2_DESIGN.md` §8), and sm3 calls it rather than
  repeating it.
- No SQL, no planner, no executor.
- No PostgreSQL dependency. `PART4.md` explains what the PG
  target forces on the decisions above; none of it requires a
  PostgreSQL source tree to build or test sm3.

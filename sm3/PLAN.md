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

**Status: built, but for partial and expression indexes**
(`SM3_DESIGN.md` §3).

- Unique and non-unique entries, built with sm2's encoder, and
  INCLUDE columns; maintenance from the old image sm2 hands
  back, through the table-level writes that are the one way to
  write a row.
- The READ COMMITTED re-check is the executor's, since the
  filter is. sm3's share is the index fix-up from the newest
  old image (`SM3_DESIGN.md` §3.1).
- The index build: concurrent, in the background, with the
  index building, valid or failed.
- **Not built:** partial and expression indexes. They need an
  evaluator, which arrives with step 4.

**Verified:** the index and the base relation agree with a
brute-force scan, under concurrency, at every isolation level;
the build does not miss a row written while it runs
(`SM3_DESIGN.md` §3.2).

## 4. Scan by predicate

**Status: not started.** `NEXT.md` §1 has what the evaluator
takes, and the order.

- A predicate in, a key range derived, the residual filter
  evaluated below the API.
- Decide the API shape: tuple at a time, as sm2 is, or sets
  of rows for pushdown (`SM3_DESIGN.md` §5).

**Verify:** the rows returned match a brute-force scan-and-filter
over the same data, for generated predicates.

## 5. Statistics, constraints, bulk load

**Status: not started.**

- Row counts, sizes, sampling.
- `NOT NULL` and the length of a `varchar(n)`, which
  `DESIGN.md` §5 gives to sm3 and nothing enforces yet.
- Foreign keys via `sm1::lock(SHARED)` on the parent row, which
  sm2's `rel_lock` now takes.
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

# PLAN — sm2

The order of work, and why it is this order. `OBJECTIVE.md` says
what sm2 is for; `DESIGN.md` is where each step's argument goes
before its code does.

The principle: **the two on-disk formats come first and get the
most scrutiny.** A key encoding and a record layout are
permanent the moment any data exists, and neither can be fixed
later by a patch. Everything else in sm2 is code that can be
rewritten.

## 1. Key encoding

The foundation. sm1 orders keys bytewise only, so every ordered
thing sm2 does rests on this.

- Order-preserving encodings per type: integers (sign-flipped
  big-endian), floats (IEEE-754 total order), decimals,
  timestamps, text (collation sort key).
- Multi-column keys, ascending and descending per column, nulls
  first or last.
- Decide the open questions in `OBJECTIVE.md` first: over-long
  keys against `SM_MAX_KEYLEN`, collation version stability,
  non-deterministic collations.

**Verify:** property-based tests, not examples. For every type
and every column order, `encode(a) < encode(b)` iff `a < b`,
over generated values including the boundaries (min, max, zero,
negative zero, NaN, empty string, the longest legal key). Round
trip where the encoding is reversible, and prove the documented
loss where it is not.

## 2. Record format

- Null bitmap, fixed/variable split, alignment.
- Schema version per row, so `ADD COLUMN` is O(1) and a dropped
  column does not shift the ones after it.
- Chunking for large values, below sm1's 64MB ceiling.

**Verify:** a row written under schema version N reads correctly
under N+1, N+2 and after a column drop; the golden-bytes test
that a record written today still decodes tomorrow.

## 3. Catalog

- Tables, indexes, columns, types, schema versions, in sm2's own
  sm1 relations.
- A cache with transactional invalidation.

**Verify:** DDL rolls back with its transaction, including the
cache; two sessions doing concurrent DDL behave as sm1's
isolation says they must.

## 4. Object mapping

- sm2 object id as a key prefix, many objects per sm1 relation.
- Decide what `DROP TABLE` costs once a table is a key range
  rather than a column family.

**Verify:** a schema far past sm1's "low thousands of relations"
target opens, and a dropped table's space is reclaimed.

## 5. Indexes

- Unique and non-unique entries, maintenance from the old image,
  `SM_ERR_DUPKEY` for uniqueness.
- The online build protocol from `DESIGN.md` §5.
- Partial, expression and INCLUDE indexes.

**Verify:** the index and the base relation never disagree,
under concurrency, at every isolation level — the property that
matters most here. The online build must not miss a row written
by a transaction that was in flight when it started; `sm1`'s
scenario suite has that shape already and sm2's version of it
belongs in the same style.

## 6. Scan interface

- A predicate in, a key range derived, the residual filter
  evaluated below the API.
- Batch-shaped: sets of rows in and out (`OBJECTIVE.md`).

**Verify:** the rows returned match a brute-force scan-and-filter
over the same data, for generated predicates.

## 7. Statistics, constraints, bulk load

- Row counts, sizes, sampling.
- Foreign keys via `sm_lock(SHARED)` on the parent row.
- A bulk path that batches within sm1's RAM bound.

---

## Testing

sm1's two-layer arrangement is worth copying, because it earned
its keep: C++ tests for contract and single-session mechanics,
and a declarative scenario suite for anything where two sessions
interleave. sm2's concurrency questions — an index build against
a concurrent writer, DDL against DML — are exactly the second
kind, and `sm1/mvcc/DRIVER.md` explains why that language exists.

Whether sm2 gets its own driver or extends sm1's is a decision
for the first scenario that needs it, not before.

## What is deliberately not here

- No SQL, no planner, no executor.
- No PostgreSQL dependency. `PART3.md` explains what the PG
  target forces on the decisions above; none of it requires a
  PostgreSQL source tree to build or test sm2.

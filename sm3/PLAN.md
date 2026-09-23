# PLAN — sm3

The order of work, and why it is this order. `OBJECTIVE.md`
says what sm3 is for; `SM3_DESIGN.md` is where each step's
argument goes before its code does. It starts where
`sm2/PLAN.md` stops: sm2's types, encodings and tuple
relation exist before anything here does.

The principle: **the formats were sm2's problem; the
concurrency is this one's.** Almost nothing sm3 writes is
permanent on disk — the catalog's own layout is the exception
— so what gets the scrutiny is the protocols, and the tests
that matter most are the ones with two sessions in them.

## 1. Catalog

Everything else needs a definition to work from, so it comes
first.

- Tables, indexes, columns, types, collations, schema
  versions, in sm3's own sm2 relations.
- In memory, written through to sm2, loaded at startup.
- The schema-row lock protocol of `SM3_DESIGN.md` §2, which is
  what makes an in-memory catalog safe.
- Decide the open question there: how a DDL change stays
  private until it commits.

**Verify:** DDL rolls back with its transaction, including the
in-memory copy; a transaction that holds the shared schema
lock never sees a definition change under it; two sessions
doing concurrent DDL behave as sm1's isolation says they must.

## 2. Object mapping

- An sm3 object id as sm2's key prefix, many objects per sm1
  store.
- Decide what `DROP TABLE` costs once a table is a key range
  rather than a column family.

**Verify:** a schema far past sm1's "low thousands of
stores" target opens; two objects sharing an sm1 store
never see each other's rows; a dropped table's space is
reclaimed.

## 3. Indexes

- Unique and non-unique entries, built with sm2's encoder;
  maintenance from the old image sm2 hands back.
- The READ COMMITTED re-check: `rel_read_latest`, the filter
  again, then the write.
- The online build protocol from `SM3_DESIGN.md` §3.
- Partial, expression and INCLUDE indexes.

**Verify:** the index and the base relation never disagree,
under concurrency, at every isolation level — the property that
matters most here. The online build must not miss a row written
by a transaction that was in flight when it started; `sm1`'s
scenario suite has that shape already and sm3's version of it
belongs in the same style.

## 4. Scan by predicate

- A predicate in, a key range derived, the residual filter
  evaluated below the API.
- Decide the API shape: tuple at a time, as sm2 is, or sets
  of rows for pushdown (`SM3_DESIGN.md` §5).

**Verify:** the rows returned match a brute-force scan-and-filter
over the same data, for generated predicates.

## 5. Statistics, constraints, bulk load

- Row counts, sizes, sampling.
- Foreign keys via `sm1_lock(SHARED)` on the parent row.
- A bulk path that batches within sm1's RAM bound.

---

## Testing

sm1's two-layer arrangement is worth copying, because it earned
its keep: C++ tests for contract and single-session mechanics,
and a declarative scenario suite for anything where two sessions
interleave. sm3's concurrency questions — an index build against
a concurrent writer, DDL against DML — are exactly the second
kind, and `sm1/mvcc/DRIVER.md` explains why that language exists.

Whether sm3 gets its own driver or extends sm1's is a decision
for the first scenario that needs it, not before.

## What is deliberately not here

- No types, no key encoding, no tuple format. All of it is
  `sm2/PLAN.md`, and sm3 calls it rather than repeating it.
- No SQL, no planner, no executor.
- No PostgreSQL dependency. `PART4.md` explains what the PG
  target forces on the decisions above; none of it requires a
  PostgreSQL source tree to build or test sm3.

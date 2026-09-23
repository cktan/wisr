# FEATURELIST — the PostgreSQL subset wisr targets

`OBJECTIVE.md` says wisr's PostgreSQL target is a subset —
the essential parts, not full compatibility. This is that
subset, spelled out feature by feature so it can be decided
once and referenced afterward, instead of re-litigated every
time it comes up.

**How to edit this file.** Check a box for everything wisr
should support. Leave a box unchecked for everything it
should not. Add a line under any item if the decision needs
a reason attached — a reason written down here is worth more
later than the box itself. Once this file stops changing,
its decisions move into `OBJECTIVE.md` and `PART4.md`, and
this file either stays as the working list or is retired.

Scope note: `OBJECTIVE.md` already excludes a query
language, a planner and an executor from wisr itself. Most
SQL-language features (window functions, CTEs, stored
procedures, triggers) are decided by whatever sits above
wisr, not by wisr, so they are left out of this list except
where they force something onto storage. What is listed here
is the part wisr itself must decide to support: types, index
access methods, constraints, table storage features, and the
ecosystem axis PART4.md §7 raises but does not settle.

**Already settled by sm1 — not on this list.** RC / RR /
SERIALIZABLE isolation with SSI, two-phase commit, nested
savepoints, deadlock detection, sequences, and a single
ordered key as the only access path below sm2.

---

## 1. Data types

- [x] int / float / number
- [x] number (this is a new implementation of decimal ala arrow;
  `NUMERIC` and `DECIMAL` are not supported)
- [x] text / varchar
- [x] bool
- [x] timestamp / date / time
- [x] UUID
- [x] bytea
- [ ] JSON / JSONB
      — needs its own storage, and typically GIN indexing
- [ ] arrays (any element type)
      — needs its own encoding
- [x] enums
      — sm3's, not an sm2 type: `int4` on disk, labels
        in sm3's catalog (sm2/DESIGN.md §2)
- [ ] range types (int4range, tsrange, ...)
      — usually paired with GiST and exclusion constraints
- [ ] composite types
- [ ] domains
      — a type plus a constraint, not a storage format
- [ ] geometric types (point, box, polygon, ...)
- [ ] network types (inet, cidr, macaddr)
- [ ] bit strings
- [ ] money
- [ ] XML
- [ ] hstore
      — an extension, not core PostgreSQL

## 2. Index access methods

- [x] B-tree
      — already assumed; the primary key is a B-tree index
        over the heap (sm2/DESIGN.md §5)
- [ ] Hash
- [ ] GiST
      — needed for range / geometric / exclusion
- [ ] GIN
      — needed for JSONB containment, full-text, arrays
- [ ] SP-GiST
- [ ] BRIN
      — cheap, block-range, for huge append-mostly tables
- [x] covering / INCLUDE indexes
- [x] partial indexes
      — sm3's predicate scan interface already assumes this
- [x] expression indexes
      — needs an expression evaluator

## 3. Constraints

- [x] PRIMARY KEY / UNIQUE / NOT NULL
- [x] FOREIGN KEY, with CASCADE / RESTRICT / SET NULL
      — sm3/PLAN.md already names this as future work, via
        sm_lock(SHARED) on the parent row
- [x] CHECK
      — needs an expression evaluator
- [ ] EXCLUSION
      — needs GiST
- [ ] deferred constraints
      — checked at commit, not per statement

## 4. Table storage features

- [x] partitioning (range / list / hash)
- [ ] table inheritance (legacy)
- [x] TOAST (large datums stored out of line)
      — sm2's own TOAST, in its own format, in the table's
        sm1 store (sm2/DESIGN.md §4)
- [x] generated / computed columns
- [x] temporary tables
- [ ] unlogged tables
- [ ] materialized views
      — storage and refresh only; no query semantics needed
        from wisr

## 5. Full-text search

- [ ] tsvector / tsquery + GIN
      — its own subsystem: a type, a GIN opclass, and a
        ranking function

## 6. Concurrency / online DDL

- [x] online index build
      — already closed, sm3/DESIGN.md §3
- [x] online ADD COLUMN
      — already assumed, via a schema-version stamp per row
- [x] other online ALTER (type change, drop column)
      — not yet decided

## 7. Replication / backup / ops

- [ ] streaming (physical) replication
      — already out of scope, sm1/DESIGN.md
- [x] logical replication / CDC decoding
- [ ] point-in-time recovery / continuous archiving
      — already out of scope ("online backup")
- [x] row-level security

## 8. Ecosystem axis (PART4.md §7's open question)

- [x] wire protocol (libpq)
      — only needed for drop-in client compatibility
- [ ] C extension ABI
      — only needed to run existing PostgreSQL extensions
- [ ] foreign data wrappers

---

## Open notes

Space for anything that does not fit a checkbox: a feature
half-in (needed for one type but not another), a feature
whose answer depends on another row above, or a reason worth
keeping next to the decision.

# DESIGN — sm2, records and indexes

The level that turns sm1's ordered byte strings into rows. This
document is the design of record; it follows the decisions in
`OBJECTIVE.md`:

| Decision | Choice |
| --- | --- |
| Row identity | the primary key, clustered, as sm1 relations already are |
| Index entry | `encode(index columns) ‖ encode(pk)` → empty or INCLUDE columns |
| API shape | batch-oriented: sets of rows, filters pushed down |
| Relation mapping | many sm2 objects multiplexed onto few sm1 relations |
| PostgreSQL | a target that shapes decisions, never a build dependency |

**Status: nothing here is implemented.** This file exists so the
formats are argued before they are written, because two of them
— the key encoding and the record layout — are on disk and
effectively permanent once any data exists. `PLAN.md` has the
order of work.

---

## 1. What sm1 provides, and what it costs

sm2 is written against the API in `sm1/include/sm.h`. The parts
that matter here, and the constraints that come with them:

| sm1 gives sm2 | and the constraint it brings |
| --- | --- |
| relations as ordered key→value maps, clustered on the key | keys are ordered **bytewise only**; there is no comparator to install (`sm1/OBJECTIVE.md`) |
| transactions at RC / RR / SERIALIZABLE | index maintenance is atomic with the base write for free, because both are in one transaction |
| `sm_update` / `sm_delete` returning the **old image** | the old index entries can be computed without a read of their own |
| `SM_ERR_DUPKEY` from `sm_insert` | unique enforcement needs no separate existence check |
| `sm_lock(SHARED\|EXCLUSIVE)` | the online index build protocol (§5) |
| nested savepoints | a statement can be undone without losing the transaction |
| forward scans with `sm_scan_seek` | probe loops cost one cursor, not one per probe |
| `SM_MAX_KEYLEN` 4096, `SM_MAX_DATALEN` 64MB | an index entry's `sk ‖ pk` must fit 4096 bytes together |
| no backward scan, ever | a descending index is a second relation with inverted bytes |
| one column family per relation, hundreds to low thousands | sm2 must multiplex (§6) |

---

## 2. Key encoding

*To be written.* The rules it must satisfy:

- `encode(a) < encode(b)` bytewise **iff** `a < b` in the type's
  own order, for every supported type.
- Multi-column keys concatenate without ambiguity: no encoded
  value may be a prefix of another that should sort after it.
- Per column: ascending or descending, and nulls first or last.
  A descending column is the ascending encoding with its bytes
  inverted, since sm1 will not scan backwards.
- Text ordering is a **collation sort key**, not the UTF-8
  bytes. The transform is lossy, so a collated key column stores
  the sort key *and* the original value.
- The encoder's signature takes, per column,
  `(type, collation, direction, nulls-order)`. Baking in
  "ascending, nulls low, bytewise" makes half of SQL's `ORDER
  BY` clauses unrepresentable.

Open: over-long keys against `SM_MAX_KEYLEN`, collation version
stability, and non-deterministic collations — see
`OBJECTIVE.md`.

---

## 3. Record format

*To be written.* The rules it must satisfy:

- A null bitmap, a fixed/variable split, and enough alignment
  discipline that reading a column does not require decoding
  every column before it.
- A **schema version stamp per row**, so `ADD COLUMN` is O(1):
  an old row is read under its own layout and the missing column
  comes from its default.
- Tolerance for dropped columns, since the column at a given
  position may no longer exist.
- Large values are **chunked out of line** above a threshold.
  sm1 permits a 64MB value, but a transaction's undo, lock and
  read sets are linear in the keys it touches (`sm1/DESIGN.md`
  §13), and very large values hurt LSM compaction, so the
  threshold is well below sm1's ceiling.

---

## 4. Catalog

*To be written.* Tables, indexes, columns, types and schema
versions, stored in sm2's own relations and written in the same
sm1 transaction as the DDL that changes them — which is what
makes DDL transactional for free.

Needs a cache with transactional invalidation. This is the usual
place such a design goes wrong: a cached definition that outlives
the transaction that changed it.

---

## 5. Secondary indexes

*To be written.* The shape is already argued in
`sm1/SECINDEX.md`, which was an assessment of what a layer like
this one needs; every gap it found is now closed in sm1.

- **Entry:** `encode(sk) ‖ encode(pk)` → empty, or the INCLUDE
  columns. The pk suffix makes every entry unique, so a
  non-unique index needs no further disambiguation.
- **Unique indexes:** the uniqueness unit is the key *without*
  the pk suffix, so it is a separate existence check under
  `sm_lock(EXCLUSIVE)` rather than a property of the KV key.
- **Maintenance:** on update and delete, the old image from
  `sm_update`/`sm_delete` gives the old index entries to remove.
- **Online build on a populated relation**, from
  `sm1/SECINDEX.md` gap 1, the protocol `sm_lock` exists for:
  1. every writer takes `sm_lock(SHARED)` on the table's schema
     row and reads it, so what it maintains is current;
  2. the builder takes the schema row **exclusive** by updating
     it, which waits out the writers in flight and blocks new
     ones;
  3. the builder scans the base relation — its snapshot is taken
     after the lock, so it sees every committed row — fills the
     index, and commits;
  4. blocked writers then fail their conflict check, retry, and
     read the new schema.
  `sm1/mvcc/in/` has this end to end as a scenario.
- Partial, expression and INCLUDE indexes mean maintenance takes
  a predicate and an expression evaluator, not a column list.

---

## 6. Object mapping

*To be written.* sm1 gives each relation a column family and
targets hundreds to low thousands of them; a SQL schema with
partitions and indexes passes that easily. So an sm2 object id
is a key prefix inside a shared sm1 relation, and the number of
sm1 relations is a tuning decision rather than a function of the
schema.

This interacts with `sm_rmrel`: dropping an sm2 table is then a
range delete inside a shared relation, not sm1's O(1) column
family drop. The trade has to be made explicitly.

---

## 7. Statistics

*To be written.* Row counts, sizes, and a sampling primitive.
A planner above needs distributions, and for an index-organized
table the primary key's correlation is 1.0 by construction,
which is worth reporting rather than measuring.

---

## 8. Known trade-offs

*To be filled in as the design settles, in the manner of
`sm1/DESIGN.md` §13 — the costs this design accepts, written
down where the next reader will find them.*

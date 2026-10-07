# CATALOG2 — what a catalog has to hold

`CATALOG.md` documents the catalog **as built**: a key-value
map in store 0 under `"U"`, seven key kinds, and a value that
is a sequence of hand-framed sm2 tuples. This file is the
input to replacing it. It is an inventory and not a layout:
what the catalog has to hold, and which of it is held today.
The layout comes after, and the statistics of
`STATISTICS.md` §3.6 are deliberately left out of here.

Doing this now is cheap: there are no users, so there is no
migration to pay for, and the format sm3 writes is "the one
format sm3 writes that lasts" (`SM3_DESIGN.md` §2).

---

## 0. The question that sizes everything

`PART4.md` §4 commits the catalog to **replacing**
PostgreSQL's heap catalogs, and names the cost as "the code
that reads the catalogs — the relation cache, syscache and
system-table scans". Read strictly, that is the whole of
`pg_catalog` in time: `pg_proc`, `pg_type`, `pg_operator`,
`pg_constraint`, `pg_depend`, `pg_amop` and the rest — some
sixty object kinds, not seven.

Nothing has decided how far down §1 to §7 below the catalog
is meant to go, and the answer changes the design rather than
sizing it:

- **The ten currently-supported-but-unstored items** (§8) are
  a format change to one subsystem.
- **The whole `pg_catalog` surface** is a different shape
  entirely. A seven-value enum, a blob per kind, and
  everything about a table nested in one value does not
  reach it.

`SM3_DESIGN.md` §2 already says as much in its open list:
"A table's definition in one row, its columns' missing values
included, could pass [64 KB]; a row per column, as
PostgreSQL's `pg_attribute` has, keeps each row small."

**Decide this first.** The rest of this file is the same
either way; what it is used for is not.

---

## 1. Catalog-wide

| What | Today |
| --- | --- |
| the layout's format version | yes, `META` |
| the oid allocator | yes, but as sm1's sequence `sm3.oid`, not a catalog entry |
| **bootstrap**: how the catalog's own schemas are known before it is loaded | **no** — open in `SM3_DESIGN.md` §2; PostgreSQL hard-codes its bootstrap catalogs |

The index-valid watermark and the count of publishes are
**deliberately not stored**: no snapshot survives a restart
(`CATALOG.md` §4).

## 2. Namespace

| What | Today |
| --- | --- |
| oid, name | yes |
| owner, ACL, comment | no |

## 3. Table

| Group | What | Today |
| --- | --- | --- |
| identity | oid, namespace oid, name | yes |
| storage | storeid, rowno sequence name, next index relid | yes |
| schema lock | nothing to store: an `sm1::LOCKTYPE_OID` lock on the table's oid | n/a |
| kind | relkind (table, matview, partitioned); persistence (permanent, unlogged, temporary) | **no**. Partitioning needs relkind; persistence does not need storing yet, since a temporary table is never written to store 0 and unlogged tables and matviews are unsupported |
| partitioning | strategy, partition key, bounds, parent and child links | **no**, though `FEATURELIST.md` §4 marks partitioning supported |
| constraints | CHECK expressions; FOREIGN KEY columns, referenced table and actions; EXCLUSION | **no**, though CHECK and FOREIGN KEY are both marked supported |
| options | reloptions such as fillfactor; row-level security policies; triggers; rules | no |
| metadata | owner, ACL, comment | no |

## 4. Column

The strongest candidate for a row of its own, for the reason
`SM3_DESIGN.md` §2 gives.

| What | Today |
| --- | --- |
| name, type code, typmod, not null, dropped, missing value, collation oid | yes |
| attnum | implicit: the column's position, with `dropped` holding the slot |
| **DEFAULT expression** — not the same as `missing`, which only serves rows written before the column was added | **no** |
| generated or identity expression, and the sequence it owns | **no**, though generated columns are marked supported |
| per-column statistics target (`ALTER TABLE … SET STATISTICS`) | **no** — and `STATISTICS.md` §3.3 needs it |
| storage mode (plain, external, extended); compression method | no |
| comment, ACL | no |

## 5. Index

| What | Today |
| --- | --- |
| oid, name, relid, unique | yes |
| key attnums, and per key: desc, nulls first, collation oid, and type code and typmod for an expression | yes |
| include attnums | yes |
| build state, the code its build failed with, and that error's message | yes |
| the eval blob — predicate and expressions as the host serialized them — and the columns they read | yes |
| access method: btree, hash, gist, gin | no — btree implicitly, which is true while `FEATURELIST.md` §2 marks only B-tree supported |
| operator class per key column | no — sm2's type and collation stand in for it today |
| NULLS NOT DISTINCT; reloptions; which constraint it implements | no |

## 6. Collation

| What | Today |
| --- | --- |
| name, locale, deterministic, ICU version | yes |
| provider (ICU or libc), owner | no |

## 7. Objects with no kind at all today

| Object | Why it is needed |
| --- | --- |
| **sequence**, as an sm3 object | `CREATE SEQUENCE` needs an oid, a namespace, a name, start, increment, min, max, cache, cycle, and the column it is owned by. sm1's sequences are keyed by a bare name under `"S"` and carry none of it |
| **type** | domains (base type, constraints, default), **enums** (their labels and the order of them), composites, ranges. sm2's frozen type codes cover its built-ins only |
| **dependencies** | `CATALOG.md` §7's open item: refusing `DROP COLLATION` in use, "by a scan of the table blobs until it earns a key kind". `DROP … RESTRICT` and `CASCADE` need it |
| **materialized view** | its storage and its refresh state; `FEATURELIST.md` §4 wants storage and refresh, no query semantics |

## 8. The gaps that are not speculative

Sorted by what is already true, since that is what decides
whether a gap blocks anything.

**A feature `FEATURELIST.md` marks supported has nowhere to
put its definition.** These are the ones that are wrong
today, not merely unbuilt.

1. partition strategy, key and bounds (§4: range, list, hash)
2. CHECK expressions (§3)
3. FOREIGN KEY definitions and their actions (§3)
4. generated and identity column expressions (§4)
5. relkind, enough to tell a partitioned parent from a leaf,
   which item 1 needs anyway

Persistence is **not** on that list, though it looks as though
it should be. A temporary table is supported, but its
definition "lives only in the session's own temporary catalog
... never written to store 0" (`CATALOG.md` §7), so there is
nothing persisted to mark. Unlogged tables and materialized
views are both unchecked in `FEATURELIST.md` §4, so a stored
persistence or a matview relkind is wanted only when they
arrive.

**Something already written needs it.**

6. column DEFAULT expressions. Not in `FEATURELIST.md`, which
   does not go down to a clause this ordinary, but `missing`
   is not it: `missing` serves rows written before a column
   was added, and a DEFAULT serves inserts that omit it.
7. the per-column statistics target, which
   `STATISTICS.md` §3.3 passes to `sm_analyze` and nothing can
   record.
8. dependencies, enough to refuse a `DROP` that would break
   one — `CATALOG.md` §7's own open item.

**Needed only once a feature arrives.** Listed to be designed
for, not to be built: an index's access method and the
operator class of each of its keys. `FEATURELIST.md` §2 marks
only B-tree supported, so "btree implicitly" is true today and
costs nothing; it stops being true with the first hash, GiST
or GIN index. The same holds for everything in §7 above and
for the metadata rows of §2 to §6.

## 9. Derived, and not to be stored

Stated so a redesign does not persist them. `table_def_t`
holds all four in memory and the blob holds none:

- the sm2 `schema` vector and its `tup_desc_t`, built from
  the columns;
- an index's `key_column_t` and `attribute_t` vectors, built
  from its attnums and the column types.

---

## 10. What the inventory shows

**The table entry is where the present design breaks.** Nine
of §3 to §7's gaps hang off a table: partitions,
constraints, policies, triggers, defaults, generation
expressions, statistics targets. Under one value a table,
each of them inflates the single value that every
`ALTER TABLE` rewrites and that has to stay inside the 64 KB
a part is held to (`sm2/TUPLE.md` §1). That is an argument
for a row per column and a row per constraint which stands
on its own, whatever is done about the framing.

**Five gaps are live bugs, not missing features.** §8's first
group: a table can be partitioned, carry a CHECK or a foreign
key, or hold a generated column, and the catalog has nowhere
to write any of it down. Two more block work in
flight: the per-column statistics target, without which
statistics cannot honour `SET STATISTICS`, and a column
DEFAULT, which `missing` is not a substitute for.

**The framing is the lesser problem, and the cheaper one.**
A value's parts carry no tag, and how many of each follows is
read from count fields in an earlier part —
`natts` and `nindex` from the table part, `nkey`, `ninclude`
and `nread` from each index part (`src/catalog/blob.cc`). A
reader that has a count wrong reads one kind of part as
another, and whether that is caught is luck. Tagging each
part with one byte would let the reader walk parts to the end
of the value and dispatch on the tag, which deletes all five
counts. It is worth doing, but it is not why the design needs
replacing.

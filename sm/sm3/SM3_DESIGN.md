# DESIGN — sm3, schema and index maintenance

sm3 is level 3 of the stack. It turns sm2's tuples into a
schema. sm3:

- gives relations names and definitions;
- decides which indexes a table has;
- keeps the indexes in step with the table's rows;
- keeps statistics.

This document is an overview. Each module has its own design
document (§3). [`SM3_OBJECTIVE.md`](SM3_OBJECTIVE.md) says what
the level is for.

## 1. What the level provides

| Decision | Choice |
| --- | --- |
| Catalog | in memory, written through to sm2. DDL works by locks |
| Index entry | `encode(index columns) ‖ row key`, or a B+tree entry. The primary key has no entries of its own: the rows are keyed by it |
| Encoder | sm2's, called with the same arguments for each column |
| Relation mapping | one sm1 store for each table. The store holds several rels, told apart by sm2's 2-byte relid: one for the rows, one for each index, and one for TOAST |
| API shape | one tuple at a time. A scan takes key bounds and filters nothing. The host evaluates expressions, through a callback |
| Index builds | online: the table's readers and writers do not wait |

Built: the catalog, object mapping, index maintenance and
builds, scans, and statistics. Bulk load is built for files into
an empty table. [`../NEXT.md`](../NEXT.md) has what is left.

The split of constraints is settled:

- sm3 enforces `NOT NULL`.
- The host enforces the `varchar(n)` limit and a foreign key's
  checks. The stack's part of a foreign key is `tbl_lock`.
- sm3 keeps a check constraint's definition, and the host
  evaluates it ([`../CHECK.md`](../CHECK.md)).

The API is `include/sm3.h`, in `namespace sm3`.

## 2. What the level depends on

sm3 uses sm2's tuple relation. For locking it calls sm1 directly,
through `../sm1/include/sm1.h`, because sm2 adds nothing to
locks.

| The stack gives sm3 | and the constraint it brings |
| --- | --- |
| sm2's rows, keyed by their primary key or by `_id` | a row's identity is its key. Every index names the row by its key. A row with a primary key has no `_id` |
| sm2's index `rel` | sm2 lays out an entry and the scan bounds. sm3 decides which indexes exist, and writes their entries on every row write |
| sm2's 2-byte relid | a table's rows, indexes and TOAST data are rels in the table's one store |
| `rel_update` and `rel_delete` returning the old image | removing old index entries needs no extra read. The image is pinned, so sm3 copies nothing |
| sm2's codec over `datum_t` values and null flags | sm3 never handles a value's bytes. Rows, keys and bounds are `datum_t`s |
| a tuple of at most 65535 bytes, with long datums out of line | after the host reads the TOAST pointers, a row is as long as its datums. A longer index entry fails with `SM_ERR_ROWTOOLONG` |
| `SM_ERR_DUPKEY` from sm1's insert | the primary key, and a unique index keyed without its row's key, need no existence check |
| `SM_ERR_UPDATED` and `rel_read_latest` at READ COMMITTED | a filtered update or delete checks its predicate again, and computes its entries again from the newest version |
| transactions at the three isolation levels | index maintenance is atomic with the base write, because both are in one transaction |
| `sm1::lock_oid` and `sm1::lock_row` in Gray's modes | readers and writers share a table-oid lock, and DDL waits for them. After an index build records its index, it stops neither readers nor writers |
| `sm1::max_keylen` 4096 | an entry's key must fit in 4094 bytes after the relid. A longer key is cut and flagged, and sm3 checks it again |
| no backward scan, permanently | a descending column is stored as inverted bytes and scanned forward, in the same index |
| one column family for each store | the number of tables, partitions included, is limited by what sm1 handles: hundreds to low thousands of stores |

sm3 also uses `../sm0` and `../include`. In `../include`,
`sm_stats.h` declares the statistics that sm3 records. sm3 builds
without a PostgreSQL source tree.

## 3. Modules

| Module | Document | Summary | Code in `src/` |
| --- | --- | --- | --- |
| Catalog | [`../CATALOG.md`](../CATALOG.md) | definitions, their keys and values, a session's overlay, publishing, DDL, object mapping | `catalog/`, `ddl/` |
| Namespace | [`../NAMESPACE.md`](../NAMESPACE.md) | SQL's `SCHEMA`: how a name is taken and found | `catalog/`, `ddl/` |
| Sequence | [`../SEQUENCE.md`](../SEQUENCE.md) | SQL's `CREATE SEQUENCE`, over sm1's counters | `catalog/ddl.cc`, `ddl/ddl.cc` |
| LSM Index | [`../LSMINDEX.md`](../LSMINDEX.md) | index maintenance by table writes, unique checks, scans, partial and expression indexes | `table/table.cc` |
| B-tree Index | [`../BTREEINDEX.md`](../BTREEINDEX.md) | the row check, the unique check, `VACUUM` and the btree reaper of a tree | `table/table.cc`, `btree_reaper/` |
| Index Builder | [`../INDEXBUILD.md`](../INDEXBUILD.md) | fills an index on a table that has rows, while its readers and writers go on | `builder/`, `table/table.cc` |
| Row Store | [`../ROWSTORE.md`](../ROWSTORE.md) | the row calls, and rows read as pins | `table/` |
| TOAST | [`../TOAST.md`](../TOAST.md) | values fetched for indexes | `table/table.cc` |
| Statistics | [`../STATISTICS.md`](../STATISTICS.md) | what the planner needs: a sample, PostgreSQL's estimators, `STATS` entries | `analyze/`, `catalog/ddl.cc` |

Five designs span levels:

- [`../VIEW.md`](../VIEW.md)
- [`../FOREIGNKEY.md`](../FOREIGNKEY.md)
- [`../CLUSTER.md`](../CLUSTER.md)
- [`../TEMPTABLE.md`](../TEMPTABLE.md)
- [`../BULKLOAD.md`](../BULKLOAD.md)

## 4. Structure

**Principle: sm2 owns the formats; sm3 owns the concurrency.**
Almost nothing that sm3 writes is permanent on disk. The one
exception is the catalog's own layout. So sm3's protocols get the
most scrutiny, and its most important tests have two sessions.

### 4.1 Source layout

```
Makefile        the same targets as sm2's
include/sm3.h   the public C++ API
src/catalog/    keys, values, the load, puts and dels, a session
src/ddl/        what a CREATE, a DROP or an ALTER does
src/table/      table writes, index maintenance, scans, pins
src/builder/    the index build's queue and workers
src/btree_reaper/    the queue of B-tree leaves to reap
src/analyze/    the sampler, the estimators, tbl_analyze
test/           doctest, as in sm1
```

### 4.2 Build

```
make -j4              libsm3.a and the test binary
DEBUG=1 make -j4      the same, -O0 -g, with assert()
make format           clang-format src/ and test/
```

When sm1's or sm2's library is missing or out of date, the build
makes it with that level's own Makefile.

### 4.3 Tests

```
DEBUG=1 make -j4 test    the unit tests
make tsan                the btree reaper's tests, under the thread sanitizer
```

sm3 has no scenario driver of its own. Instead, sm1's
`set_qa_nowait`, for QA only, lets one thread act as several
sessions. So sm3's cases with two sessions are in
`test/test_table.cc` and in `../rel/test`. The tests of deadlocks
and of the background build use real threads
([`../DESIGN.md`](../DESIGN.md#4-testing-across-levels) §4).

Each module document says what its tests prove. The tests of the
level as a whole prove these rules:

1. DDL rolls back with its transaction, and so does the
   in-memory copy of the catalog.
2. A transaction that holds the shared table-oid lock never sees
   the table's definition change.
3. A table's rows and indexes share its store without
   overlapping, and `DROP TABLE` removes the store.
4. A dropped index's relid is never handed out again.
5. The index and the base relation agree with a brute-force scan,
   under concurrency, at every isolation level.

## 5. Cross-level decisions

- **What is deliberately not here.** sm3 has no types, no key
  encoding and no tuple format: it calls sm2's. It has no SQL, no
  planner and no executor. It has no PostgreSQL dependency
  ([`../PART4.md`](../PART4.md)).
- **A scan filters nothing.** sm3 has no expression evaluator,
  and will not get one. The host's planner derives the key range,
  and the host's executor applies the rest of the predicate
  ([`../LSMINDEX.md`](../LSMINDEX.md#55-what-a-scan-leaves-to-the-host)
  §5.5).
- **The host evaluates.** For partial and expression indexes,
  sm3 calls the host through a callback
  ([`../LSMINDEX.md`](../LSMINDEX.md#6-partial-and-expression-indexes)
  §6).
- **Deployment.** All the levels link into one server process
  ([`../sm2/SM2_DESIGN.md`](../sm2/SM2_DESIGN.md#52-deployment)
  §5.2). The boundary between the executor and sm3 is a function
  call, so sm3's interface is one tuple at a time.

## 6. Known trade-offs

- **The version lag is permanent.** digres will always lag
  behind PostgreSQL's releases, and
  [`../PART4.md`](../PART4.md) already counts that lag as a
  liability. The fork replaces the storage and transaction
  layers, and makes PostgreSQL thread safe. So tracking upstream
  releases is harder still.

The modules own the other trade-offs:

- [`../LSMINDEX.md`](../LSMINDEX.md#9-costs-accepted) §9 and
  §4.3: lost plans and over-long unique keys;
- [`../CATALOG.md`](../CATALOG.md#1-design) §1: a catalog with no
  versions, and PITR;
- [`../DBMGR.md`](../DBMGR.md) §14: the limit on the number of
  tables.

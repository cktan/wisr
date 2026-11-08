# DESIGN — sm3, schema and index maintenance

sm3 is level 3 of the stack. It turns sm2's tuples into a schema.
It gives relations names and definitions, decides which indexes a
table has, keeps them in step with its rows, and adds statistics.
This document is an overview. Each module document holds its own
design. [`SM3_OBJECTIVE.md`](SM3_OBJECTIVE.md) says what the
level is for.

## 1. What the level provides

| Decision | Choice |
| --- | --- |
| Catalog | in memory, written through to sm2, with lock-based DDL |
| Index entry | `encode(index columns) ‖ row key`, or a B+tree entry; the primary key has no entries, the rows being keyed by it |
| Encoder | sm2's, called with the same arguments for each column |
| Relation mapping | one sm1 store a table, a collection of rels told apart by sm2's 2-byte relid: its rows, one for each index, one for TOAST |
| API shape | a tuple at a time; a scan takes key bounds and filters nothing, and whatever needs evaluating is the host's, by callback |
| Index builds | online: readers and writers of the table go on |

The catalog, object mapping, index maintenance and builds, scans
and statistics are built. Constraints are settled: `NOT NULL` is
sm3's, the `varchar(n)` limit and the checks of a foreign key are
the host's, and `tbl_lock` is the stack's share of a foreign key.
Bulk load is built for files into an empty table.
[`../NEXT.md`](../NEXT.md)
has what is left. The API is `include/sm3.h`, in `namespace sm3`.

## 2. What the level depends on

sm3 is written against sm2's tuple relation, and against
`../sm1/include/sm1.h` for locking, the one thing sm2 adds
nothing to.

| The stack gives sm3 | and the constraint it brings |
| --- | --- |
| sm2's rows, keyed by their primary key or by `_id` | a row's identity is its key; every index names the row by it, and a keyed row has no `_id` |
| sm2's index `rel` | sm2 lays out an entry and scan bounds; which indexes exist, and writing their entries on every row write, are sm3's |
| sm2's 2-byte relid | a table's rows, indexes and TOAST data are rels in its one store |
| `rel_update` and `rel_delete` returning the old image | old index entries need no read of their own; the image is pinned, so sm3 copies nothing |
| sm2's codec over `datum_t` values and null flags | sm3 never handles a value's bytes: rows, keys and bounds are `datum_t`s |
| a tuple of at most 65535 bytes, long datums out of line | a row is as long as its datums once the host reads TOAST pointers; a longer index entry fails with `SM_ERR_ROWTOOLONG` |
| `SM_ERR_DUPKEY` from sm1's insert | a unique index keyed without its row's key, and the primary key, need no existence check |
| `SM_ERR_UPDATED` and `rel_read_latest` at READ COMMITTED | a filtered update or delete re-checks its predicate and recomputes its entries from the newest version |
| transactions at the three levels | index maintenance is atomic with the base write, as both are in one transaction |
| `sm1::lock_oid` and `sm1::lock_row` in Gray's modes | readers and writers coexist on a table-oid lock, DDL waits them out, and a build stops neither once its index is recorded |
| `sm1::max_keylen` 4096 | an entry's key must fit 4094 bytes after the relid; an over-long one is cut and flagged, and sm3 rechecks it |
| no backward scan, ever | a descending column is inverted bytes scanned forward, in the same index |
| one column family a store | tables, partitions included, are bounded by the hundreds to low thousands of stores sm1 handles |

sm3 also uses `../sm0` and `../include`, where `sm_stats.h`
declares the statistics it records. It builds with no PostgreSQL
source tree.

## 3. Modules

| Module | Document | Summary | Code in `src/` |
| --- | --- | --- | --- |
| Catalog | [`../CATALOG.md`](../CATALOG.md) | definitions, their keys and values, a session's overlay, publishing, DDL, object mapping | `catalog/`, `ddl/` |
| Namespace | [`../NAMESPACE.md`](../NAMESPACE.md) | SQL's `SCHEMA`: how a name is taken and found | `catalog/`, `ddl/` |
| Sequence | [`../SEQUENCE.md`](../SEQUENCE.md) | SQL's `CREATE SEQUENCE`, over sm1's counters | `catalog/ddl.cc`, `ddl/ddl.cc` |
| LSM Index | [`../LSMINDEX.md`](../LSMINDEX.md) | index maintenance by table writes, unique checks, scans, partial and expression indexes | `table/table.cc` |
| B-tree Index | [`../BTREEINDEX.md`](../BTREEINDEX.md) | the row check, the unique check, `VACUUM` and the cleaner of a tree | `table/table.cc`, `cleaner/` |
| Index Builder | [`../INDEXBUILD.md`](../INDEXBUILD.md) | fills an index on a table that has rows, while its readers and writers go on | `builder/`, `table/table.cc` |
| Row Store | [`../ROWSTORE.md`](../ROWSTORE.md) | the row calls, and rows read as pins | `table/` |
| TOAST | [`../TOAST.md`](../TOAST.md) | values fetched for indexes | `table/table.cc` |
| Statistics | [`../STATISTICS.md`](../STATISTICS.md) | what the planner needs: a sample, PostgreSQL's estimators, `STATS` entries | `analyze/`, `catalog/ddl.cc` |

Five designs span levels: [`../VIEW.md`](../VIEW.md),
[`../FOREIGNKEY.md`](../FOREIGNKEY.md),
[`../CLUSTER.md`](../CLUSTER.md),
[`../TEMPTABLE.md`](../TEMPTABLE.md) and
[`../BULKLOAD.md`](../BULKLOAD.md).

## 4. Structure

**Principle: the formats were sm2's problem, and the concurrency
is this level's.** Almost nothing sm3 writes is permanent on
disk. The catalog's own layout is the exception. So the protocols
get the scrutiny, and the tests that matter most have two
sessions in them.

### 4.1 Source layout

```
Makefile        the same targets as sm2's
include/sm3.h   the public C++ API
src/catalog/    keys, values, the load, puts and dels, a session
src/ddl/        what a CREATE, a DROP or an ALTER does
src/table/      table writes, index maintenance, scans, pins
src/builder/    the index build's queue and workers
src/cleaner/    the queue of B-tree leaves to clean
src/analyze/    the sampler, the estimators, tbl_analyze
test/           doctest, as in sm1
```

### 4.2 Build

```
make -j4              libsm3.a and the test binary
DEBUG=1 make -j4      the same, -O0 -g, with assert()
make format           clang-format src/ and test/
```

The build makes sm1's and sm2's libraries through their own
Makefiles when they are missing or out of date.

### 4.3 Tests

```
DEBUG=1 make -j4 test    the unit tests
make tsan                the cleaner's tests, under the thread sanitizer
```

sm3 has no scenario driver of its own. sm1's QA-only
`set_qa_nowait` lets one thread play several sessions, so sm3's
two-session cases are in `test/test_table.cc` and in
`../rel/test`. Deadlocks and the background build use real
threads ([`../DESIGN.md`](../DESIGN.md#4-testing-across-levels)
§4).

Each module document says what its tests prove. The level's own
rules:

1. DDL rolls back with its transaction, the in-memory copy
   included.
2. A transaction holding the shared table-oid lock never sees a
   definition change under it.
3. A table's rows and indexes share its store without meeting,
   and `DROP TABLE` removes the store.
4. A dropped index's relid is never handed out again.
5. The index and the base relation agree with a brute-force scan,
   under concurrency, at every isolation level.

## 5. Cross-level decisions

- **What is deliberately not here.** No types, no key encoding
  and no tuple format: sm3 calls sm2's. No SQL, no planner and no
  executor. No PostgreSQL dependency
  ([`../PART4.md`](../PART4.md)).
- **A scan filters nothing.** sm3 has no evaluator, and gets
  none. Deriving a key range is the kept planner's, and the
  residual is the executor's
  ([`../LSMINDEX.md`](../LSMINDEX.md#55-what-a-scan-leaves-to-the-host)
  §5.5).
- **The host evaluates.** Partial and expression indexes reach it
  by a callback
  ([`../LSMINDEX.md`](../LSMINDEX.md#6-partial-and-expression-indexes)
  §6).
- **Deployment.** All the levels link into one server process
  ([`../sm2/SM2_DESIGN.md`](../sm2/SM2_DESIGN.md#52-deployment)
  §5.2). The boundary between the
  executor and sm3 is a function call, so sm3's interface is one
  tuple at a time.

## 6. Known trade-offs

- **The version lag is permanent.** The fork replaces the storage
  and transaction layers and makes PostgreSQL thread safe. So
  tracking upstream releases is harder than the version lag it
  already counts as a liability ([`../PART4.md`](../PART4.md)).

The other trade-offs have module owners:
[`../LSMINDEX.md`](../LSMINDEX.md#9-costs-accepted) §9 and
§4.3 for lost plans and over-long unique keys,
[`../CATALOG.md`](../CATALOG.md#1-design) §1
for a catalog with no versions and for PITR, and
[`../DBMGR.md`](../DBMGR.md)
§14 for the bound on tables.

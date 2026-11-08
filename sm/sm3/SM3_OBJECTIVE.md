# OBJECTIVE — sm3

Build the level that turns sm2's tuples into a **schema**: a
catalog of tables and indexes, secondary indexes maintained
transactionally with the base row, scans between key bounds,
and the statistics a planner above needs.

sm2 stops exactly where this begins.
[`sm2/SM2_OBJECTIVE.md`](../sm2/SM2_OBJECTIVE.md) says
sm2 is "handed a schema", that it "maintains no index", that
it "has no filter and counts nothing", and that an sm2
relation is one sm1 store with an opaque relid
reserved for the level above. sm3 is that level, and this
document is what it is for.

## Scope

- **A catalog.** Table and index definitions, column types,
  collations, dropped columns and missing values, held in
  memory and persisted through sm2 in the same transactions as
  the data. The aim was sm2 relations; they are kept instead
  in sm1's catalog store, store 0, under `"U"`, through sm2's
  `catkv` ([`../CATALOG.md`](../CATALOG.md#31-where-it-lives)
  §3.1).
  It also replaces PostgreSQL's heap catalogs and sequences,
  which the fork can no longer keep on heap
  ([`PART4.md`](../PART4.md#4-the-fork-and-what-it-costs) §4).
- **Secondary indexes.** Unique and non-unique, maintained
  transactionally with the base row, built online on a
  populated relation.
- **Object mapping.** Logical tables and indexes onto sm1
  stores, through the key prefix sm2 offers. The aim was many
  onto few; as decided below, each table has a store of its
  own, its indexes and TOAST data inside it.
- **A scan interface** over a table and over each of its
  indexes, between key bounds. It began as one that takes a
  predicate and derives the key range from it; deriving the
  range is the planner's, and sm3 evaluates nothing
  ([`../INDEXBUILD.md`](../INDEXBUILD.md#2-the-protocol) §2,
  [`../LSMINDEX.md`](../LSMINDEX.md#5-scans) §5).
- **Statistics** good enough for a cost-based planner above.
  They are not optional: the planner is kept, so it needs row
  counts, sizes, histograms and correlations, and `ANALYZE`
  needs a sampling path.

## Not in scope

- **Types, encodings and the tuple format.** They are sm2's.
  sm3 calls sm2's key encoder for its index entries rather
  than writing a second one, and never looks inside a tuple
  except through sm2.
- A query language, a planner, or an executor. sm3 answers
  questions about rows; it does not decide which questions to
  ask.
- Anything sm1 already does: transactions, isolation, locking,
  durability, recovery, garbage collection. sm3 reaches them
  through sm2's session, or directly through `sm1/include/sm1.h`
  where sm2 has nothing to add — `sm1::lock_oid` is the case that
  matters.
- Replication, point-in-time recovery, and logical decoding.
  They read PostgreSQL's WAL, which does not carry data held
  in RocksDB
  ([`PART4.md`](../PART4.md#4-the-fork-and-what-it-costs) §4);
  rebuilding them is a subsystem
  of its own, at the storage tier.
- A PostgreSQL dependency. sm3 must be useful and testable on
  its own, and buildable without a PostgreSQL source tree. The
  PG target shapes sm3's *decisions* (see
  [`PART4.md`](../PART4.md) at the
  top level), never its *build*.

## Decisions

These are settled and drive [`SM3_DESIGN.md`](SM3_DESIGN.md).

- **An index entry is `encode(sk) ‖ row key`**, mapping to a
  tuple of whether the key was cut, the INCLUDE columns and the
  row's key; a unique index drops the suffix. The decision was
  `encode(sk) ‖ rowno`, over a heap keyed by `rowno` with the
  primary key one of these indexes, as in PostgreSQL
  ([`sm2/SM2_OBJECTIVE.md`](../sm2/SM2_OBJECTIVE.md)). The table
  is now clustered: a row
  is keyed by its primary key, or by its `_id` without one,
  and the primary key has no entries of its own
  ([`../CLUSTER.md`](../CLUSTER.md),
  [`../ROWSTORE.md`](../ROWSTORE.md)). A lookup is an index scan
  followed by a read by the row's key.
- **sm2's encoder is the only encoder.** sm3 builds index keys
  from the same function, with the same per-column arguments
  ([`sm2/SM2_OBJECTIVE.md`](../sm2/SM2_OBJECTIVE.md)).
- **The catalog is held in memory** and written through to sm2
  for persistence. It replaces PostgreSQL's heap catalogs
  ([`PART4.md`](../PART4.md#4-the-fork-and-what-it-costs) §4), so
  sm1's status record is the one commit
  point, and DDL is transactional for free.
- **Schema changes are lock-based, as in PostgreSQL.** The
  catalog is not versioned; a transaction is kept away from a
  concurrent DDL with an sm1 lock on the table's oid, not
  given an old definition
  ([`../CATALOG.md`](../CATALOG.md#1-design) §1).
- **One sm1 store per table.** A table's heap, its indexes
  and its TOAST data are rels in one sm1 store, told apart by
  sm2's 2-byte relid (§4), so `DROP TABLE` is
  sm1's O(1) column family drop. sm1 targets "hundreds to low
  thousands of stores per database"
  ([`../XCTMGR.md`](../XCTMGR.md#114-trade-offs) §11.4),
  so tables, partitions included, are bounded by that;
  accepted for v1
  ([`../CATALOG.md`](../CATALOG.md#2-object-mapping) §2).
- **The eventual consumer is a forked PostgreSQL**, keeping
  its parser, planner and executor and replacing its storage
  and transaction layers. The fork is **threaded**
  ([`PART4.md`](../PART4.md#2-the-blocker-the-process-model) §2),
  so sm1, sm2 and sm3 are linked into the one
  server process, every backend thread shares sm1's
  transaction table, lock manager and SSI graph, and one
  shared in-memory catalog is coherent without cross-process
  invalidation.
- **Writes at READ COMMITTED follow sm1's protocol.** A write
  that meets a row committed after the transaction began is
  refused with `SM_ERR_UPDATED`; sm2 surfaces the error
  without resolving it, so sm3 reads the newest image with
  `rel_read_latest`, re-checks the statement's filter, and
  only then repeats the write
  ([`PART4.md`](../PART4.md#5a-the-second-conflict-read-committed-writes)
  §5a,
  [`../LOCKMGR.md`](../LOCKMGR.md#19-read-committed-write-re-read-write)
  §1.9). Index entries are computed from that
  newest image.

## Open questions

- **Batch-shaped API.** It was decided because a PostgreSQL
  backend was a separate process per connection that could not
  link sm1, and behind an IPC boundary a per-row API is
  unsalvageable. The fork is now threaded, so the boundary
  between the executor and sm3 is a function call and the
  shape is no longer forced
  ([`PART4.md`](../PART4.md#2-the-blocker-the-process-model) §2).
  Settled in
  [`../LSMINDEX.md`](../LSMINDEX.md#5-scans) §5: tuple at a time.
  Filter pushdown, the
  other reason for sets of rows, went when sm3 was given no
  evaluator. Rows read by key, many in one call, are built
  ([`../ROWSTORE.md`](../ROWSTORE.md)); a scan stays tuple at a
  time.
- **Privacy of a catalog change until commit** — settled and
  built: a session's overlay keeps its transaction's DDL to
  itself, undone on abort and savepoint rollback, and publishes
  it at commit ([`../CATALOG.md`](../CATALOG.md#4-in-memory) §4,
  §6).
- **How good RocksDB's approximate statistics are** for
  `reltuples` and `relpages` — unmeasured when this was
  written, with
  [`../STATISTICS.md`](../STATISTICS.md#9-the-design) §9
  depending on the answer.
  Settled: measured
  ([`../STATISTICS.md`](../STATISTICS.md#6-what-was-measured)
  §6): exact at rest,
  within 8% by relid, and 7x over for rows not yet flushed;
  so sm1 now counts a relid's memtable bytes exactly itself
  ([`../STATISTICS.md`](../STATISTICS.md#92-sizes-and-the-unflushed-rows)
  §9.2).

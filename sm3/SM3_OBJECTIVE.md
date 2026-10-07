# OBJECTIVE — sm3

Build the level that turns sm2's tuples into a **schema**: a
catalog of tables and indexes, secondary indexes maintained
transactionally with the base row, scans between key bounds,
and the statistics a planner above needs.

sm2 stops exactly where this begins. `sm2/SM2_OBJECTIVE.md` says
sm2 is "handed a schema", that it "maintains no index", that
it "has no filter and counts nothing", and that an sm2
relation is one sm1 store with an opaque relid
reserved for the level above. sm3 is that level, and this
document is what it is for.

## Scope

- **A catalog.** Table and index definitions, column types,
  collations, dropped columns and missing values, held in
  memory and persisted through sm2 relations in the same
  transactions as the data.
  It also replaces PostgreSQL's heap catalogs and sequences,
  which the fork can no longer keep on heap (`PART4.md` §4).
- **Secondary indexes.** Unique and non-unique, maintained
  transactionally with the base row, built online on a
  populated relation.
- **Object mapping.** Many logical tables and indexes onto few
  sm1 stores, through the key prefix sm2 offers.
- **A scan interface** over a table and over each of its
  indexes, between key bounds. It began as one that takes a
  predicate and derives the key range from it; deriving the
  range is the planner's, and sm3 evaluates nothing
  (`SM3_DESIGN.md` §3, §5).
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
  where sm2 has nothing to add — `sm1::lock` is the case that
  matters.
- Replication, point-in-time recovery, and logical decoding.
  They read PostgreSQL's WAL, which does not carry data held
  in RocksDB (`PART4.md` §4); rebuilding them is a subsystem
  of its own, at the storage tier.
- A PostgreSQL dependency. sm3 must be useful and testable on
  its own, and buildable without a PostgreSQL source tree. The
  PG target shapes sm3's *decisions* (see `PART4.md` at the
  top level), never its *build*.

## Decisions

These are settled and drive `SM3_DESIGN.md`.

- **An index entry is `encode(sk) ‖ rowno`**, mapping to a tuple
  of the `rowno`, whether the key was cut, and the INCLUDE
  columns; a unique index drops the suffix. The table is a heap
  keyed by `rowno` and the primary key is one of these indexes,
  as in PostgreSQL (`sm2/SM2_OBJECTIVE.md`). A lookup is an index
  scan followed by a read by `rowno`.
- **sm2's encoder is the only encoder.** sm3 builds index keys
  from the same function, with the same per-column arguments
  (`sm2/SM2_OBJECTIVE.md`).
- **The catalog is held in memory** and written through to sm2
  for persistence. It replaces PostgreSQL's heap catalogs
  (`PART4.md` §4), so sm1's status record is the one commit
  point, and DDL is transactional for free.
- **Schema changes are lock-based, as in PostgreSQL.** The
  catalog is not versioned; a transaction is kept away from a
  concurrent DDL with sm1's row locks on the schema row, not
  given an old definition (`SM3_DESIGN.md` §2).
- **One sm1 store per table.** A table's rows, its
  indexes and its TOAST data share one sm1 store,
  told apart by sm2's 2-byte relid, so `DROP TABLE` is
  sm1's O(1) column family drop. sm1 targets "hundreds to low
  thousands of stores per database" (`sm1/SM1_DESIGN.md` §13),
  so tables, partitions included, are bounded by that;
  accepted for v1 (`SM3_DESIGN.md` §4).
- **The eventual consumer is a forked PostgreSQL**, keeping
  its parser, planner and executor and replacing its storage
  and transaction layers. The fork is **threaded**
  (`PART4.md` §2), so sm1, sm2 and sm3 are linked into the one
  server process, every backend thread shares sm1's
  transaction table, lock manager and SSI graph, and one
  shared in-memory catalog is coherent without cross-process
  invalidation.
- **Writes at READ COMMITTED follow sm1's protocol.** A write
  that meets a row committed after the transaction began is
  refused with `SM_ERR_UPDATED`; sm2 surfaces the error
  without resolving it, so sm3 reads the newest image with
  `rel_read_latest`, re-checks the statement's filter, and
  only then repeats the write (`PART4.md` §5a,
  `sm1/SM1_DESIGN.md` §5.4). Index entries are computed from that
  newest image.

## Open questions

- **Batch-shaped API.** It was decided because a PostgreSQL
  backend was a separate process per connection that could not
  link sm1, and behind an IPC boundary a per-row API is
  unsalvageable. The fork is now threaded, so the boundary
  between the executor and sm3 is a function call and the
  shape is no longer forced (`PART4.md` §2). Settled in
  `SM3_DESIGN.md` §5: tuple at a time. Filter pushdown, the
  other reason for sets of rows, went when sm3 was given no
  evaluator, and fewer calls per scan waits for a profile.
- **Privacy of a catalog change until commit** — settled and
  built: a session's overlay keeps its transaction's DDL to
  itself, undone on abort and savepoint rollback, and publishes
  it at commit (`CATALOG.md` §4, §5).
- **How good RocksDB's approximate statistics are** for
  `reltuples` and `relpages`. It has not been measured, and
  `SM3_DESIGN.md` §6 depends on the answer.

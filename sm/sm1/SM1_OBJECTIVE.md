# OBJECTIVE

Design and build a **storage manager** backed by RocksDB that supports
full MVCC, for OLTP workloads.

## Scope

- This project adds only an **MVCC storage manager** on top of
  RocksDB. Nothing above the storage manager is in scope.
- Each store is a **clustered index on a single primary
  key**: the value is stored with its key, rows are ordered by
  key, and the key is the only access path.
- Keys are ordered **bytewise (`memcmp`)**; there is no custom
  comparator of user keys (a store CF's, `wisr.store.v2`, only
  orders each key's versions, `SM1_DESIGN.md` §3.1). Callers
  encode composite or typed keys into byte strings whose
  `memcmp` order is the order they want.
- **Secondary indexes are not provided.** They can be built by
  another layer on top of this one, for example as separate
  stores maintained in the same transaction.
- **Backward scans are not supported.** Iteration is forward
  only, in ascending bytewise key order. This holds for every
  index: the clustered primary key here, and any secondary
  index a layer above builds out of stores. It is a
  permanent decision, not a phase-1 limitation. A caller that
  needs descending order builds it into the key: an index
  whose key bytes are inverted, scanned forward. The stack
  above does that per index column, and keeps no mirror copy
  of an index to serve the other direction; an order an index
  does not have is a sort (`../PART4.md` §5).

## Goals

- Use RocksDB purely as a key-value storage engine underneath — all
  MVCC and transactional semantics are implemented in the storage
  manager itself (RocksDB has no native multi-version row concept).
- Simulate full MVCC on top of RocksDB's key-value model.
- Support the standard transaction isolation levels:
  - read committed
  - repeatable read
  - serializable

## API

- The `sm1::` Storage Manager API (see `API.md`) is the interface
  to this engine. It already sketches the primitives needed:
  - database lifecycle: `sm1::mkdb` / `sm1::rmdb`
  - sessions: `sm1::opendb` / `sm1::closedb`
  - transactions: `sm1::begin` (with `sm1::xctlevel_t`) /
    `sm1::prepare` / `sm1::commit` / `sm1::abort`
  - nested savepoints: `sm1::savepoint` / `sm1::rollback` /
    `sm1::forget`
  - statement boundaries: `sm1::stmt_begin`
  - row locks without writing, in Gray's modes (IS, IX, S,
    SIX, X): `sm1::lock`
  - cursor reposition: `sm1::scan_seek`
  - stores: `sm1::mkstore` / `sm1::rmstore` / `sm1::lsstore`
  - sequences: `sm1::mkseq` / `sm1::rmseq` / `sm1::nextseq`
  - data ops: `sm1::insert` / `sm1::update` / `sm1::erase` /
    `sm1::read` / `sm1::scan` / `sm1::scannxt` / `sm1::scanend`

## Decisions

These were open; they are now settled and drive `SM1_DESIGN.md`.

- **Concurrency control.** Snapshot isolation for all levels, with
  serializable implemented as **SSI (optimistic)** — read/write
  dependency tracking with SIREAD predicate locks and pivot detection.
  No read locks; serialization failures surface as errors at commit.
  Write-write conflicts are still prevented eagerly
  (first-committer-wins), with no lock kept for a write: a second
  writer of a key waits for the transaction whose uncommitted
  version it meets, and fails if that one commits
  (`WRITELOCK.md`). At read committed a write that meets a newer
  commit fails with `SM_ERR_UPDATED` instead, which does not
  abort: the caller reads the newest version with
  `sm1::read_latest` and writes again (`SM1_DESIGN.md` §5.4).
- **Uncommitted writes go eagerly into RocksDB**, stamped with the
  writing transaction's id, and are made visible through a durable
  commit-status table. This keeps transaction size unbounded by RAM,
  at the cost of a status lookup on read and a garbage-collection pass
  for versions left behind by aborts.
- **`sm1::prepare` is real two-phase commit.** Prepare durably
  persists the prepared-transaction record (with a global
  transaction id), so a prepared transaction survives a crash and
  can be committed or aborted after restart by an external
  coordinator. Recovery must restore prepared transactions and
  re-acquire their explicit locks (a write keeps none,
  `WRITELOCK.md`).
- **Deployment is a single process, multi-threaded.** All
  sessions on a database in a process share its one open
  RocksDB instance (the root db and temporary tables have one
  each, `SM1_DESIGN.md` §3.5); the transaction table,
  lock manager, and SSI structures are ordinary in-process data
  structures. This is what RocksDB's single-writer-process rule
  effectively forces.

## Notes

- `API.md` is the specification of `include/sm1.h`, the C++
  interface; `SM1_DESIGN.md` is the design behind it.

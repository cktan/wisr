# OBJECTIVE

Design and build a **storage manager** backed by RocksDB that supports
full MVCC, for OLTP workloads.

## Scope

- This project adds only an **MVCC storage manager** on top of
  RocksDB. Nothing above the storage manager is in scope.
- Each relation is a **clustered index on a single primary
  key**: the value is stored with its key, rows are ordered by
  key, and the key is the only access path.
- Keys are ordered **bytewise (`memcmp`)**; there is no custom
  comparator. Callers encode composite or typed keys into byte
  strings whose `memcmp` order is the order they want.
- **Secondary indexes are not provided.** They can be built by
  another layer on top of this one, for example as separate
  relations maintained in the same transaction.
- **Backward scans are not supported.** Iteration is forward
  only, in ascending bytewise key order. This holds for every
  index: the clustered primary key here, and any secondary
  index a layer above builds out of relations. It is a
  permanent decision, not a phase-1 limitation. A caller that
  needs descending order keeps a second relation whose key
  bytes are inverted, and scans that one forward.

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

- The `sm_*` Storage Manager API (see `API.md`) is the interface to
  this engine. It already sketches the primitives needed:
  - database lifecycle: `sm_mkdb` / `sm_rmdb`
  - sessions: `sm_opendb` / `sm_closedb`
  - transactions: `sm_begin` (with `sm_xctlevel_t`) / `sm_prepare` /
    `sm_commit` / `sm_abort`
  - nested savepoints: `sm_savepoint` / `sm_rollback` /
    `sm_forget`
  - relations: `sm_mkrel` / `sm_rmrel` / `sm_lsrel`
  - sequences: `sm_mkseq` / `sm_rmseq` / `sm_nextseq`
  - data ops: `sm_insert` / `sm_update` / `sm_delete` / `sm_read` /
    `sm_scan` / `sm_scannxt` / `sm_scanend`

## Decisions

These were open; they are now settled and drive `DESIGN.md`.

- **Concurrency control.** Snapshot isolation for all levels, with
  serializable implemented as **SSI (optimistic)** — read/write
  dependency tracking with SIREAD predicate locks and pivot detection.
  No read locks; serialization failures surface as errors at commit.
  Write-write conflicts are still prevented eagerly by in-memory row
  locks (first-writer-wins).
- **Uncommitted writes go eagerly into RocksDB**, stamped with the
  writing transaction's id, and are made visible through a durable
  commit-status table. This keeps transaction size unbounded by RAM,
  at the cost of a status lookup on read and a garbage-collection pass
  for versions left behind by aborts.
- **`sm_prepare` is real two-phase commit.** Prepare durably persists
  the prepared-transaction record (with a global transaction id), so a
  prepared transaction survives a crash and can be committed or
  aborted after restart by an external coordinator. Recovery must
  restore prepared transactions and re-acquire their row locks.
- **Deployment is a single process, multi-threaded.** All sessions in
  a process share one open RocksDB instance; the transaction table,
  lock manager, and SSI structures are ordinary in-process data
  structures. This is what RocksDB's single-writer-process rule
  effectively forces.

## Notes

- `API.md` is a sketch. `DESIGN.md` proposes concrete C signatures
  and flags where it must diverge — notably the read/scan calls, and
  the extra 2PC entry points (`sm_prepare` taking a global id, plus
  recovery of in-doubt transactions).

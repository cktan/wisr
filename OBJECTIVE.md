# OBJECTIVE

Design and build a **storage manager** backed by RocksDB that supports
full MVCC, for OLTP workloads.

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

- The `sm_*` Storage Manager API (see `API.txt`) is the interface to
  this engine. It already sketches the primitives needed:
  - database lifecycle: `sm_mkdb` / `sm_rmdb`
  - sessions: `sm_attach` / `sm_detach`
  - transactions: `sm_begin` (with `sm_xctlevel_t`) / `sm_prepare` /
    `sm_commit` / `sm_abort`
  - a single savepoint per session: `sm_savepoint` / `sm_rollback`
  - relations: `sm_mkrel` / `sm_rmrel`
  - data ops: `sm_insert` / `sm_update` / `sm_delete` / `sm_read` /
    `sm_scan` / `sm_scan_next` / `sm_scan_end`

## Open questions

- Concurrency control strategy for serializable isolation (e.g.
  optimistic/SSI vs. locking) is not yet decided.

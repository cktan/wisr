# OBJECTIVE

Design and build **pgrock**: a PostgreSQL-compatible frontend backed by
RocksDB for storage, targeting OLTP workloads.

## Goals

- Speak enough of the Postgres wire protocol / SQL surface to work as a
  drop-in frontend for OLTP clients.
- Use RocksDB purely as a key-value storage engine underneath — all
  relational and transactional semantics are implemented in pgrock.
- Simulate full MVCC on top of RocksDB's key-value model (RocksDB itself
  has no native multi-version row/tuple concept for SQL rows).
- Support the standard SQL transaction isolation levels:
  - read committed
  - repeatable read
  - serializable

## Storage layer

- The `sm_*` Storage Manager API (see `API.txt`) is the interface between
  pgrock and RocksDB. It already sketches the primitives needed:
  - database lifecycle: `sm_mkdb` / `sm_rmdb`
  - sessions: `sm_attach` / `sm_detach`
  - transactions: `sm_begin` (with `sm_xctlevel_t`) / `sm_prepare` /
    `sm_commit` / `sm_abort`
  - a single savepoint per session: `sm_savepoint` / `sm_rollback`
  - relations: `sm_mkrel` / `sm_rmrel`
  - data ops: `sm_insert` / `sm_update` / `sm_delete` / `sm_read` /
    `sm_scan` / `sm_scan_next` / `sm_scan_end`
- MVCC and isolation-level enforcement (visibility rules, conflict
  detection, savepoint rollback) live above this API, in pgrock — the
  Storage Manager itself is isolation-level-agnostic key/value storage.

## Open questions

- How much of the Postgres wire protocol / SQL dialect is in scope
  initially (just enough for basic CRUD + transactions, or broader)?
- Concurrency control strategy for serializable isolation (e.g.
  optimistic/SSI vs. locking) is not yet decided.

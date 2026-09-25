# OBJECTIVE — wisr

Build a storage stack for OLTP that could, eventually, sit
under a subset of PostgreSQL — the essential parts, not full
compatibility — in place of its own storage and transaction
layers. `FEATURELIST.md` says which parts; `PART4.md` argues
what the target costs. Neither is a dependency of anything
below, but they shape the decisions at every level, because a
design that could not host the subset would be wasted work at
this project's scale.

"wisr" is Wisconsin storage on rocks: a stack of storage
levels, each one built on the level below it, in the
tradition of the database systems built at Wisconsin.
`README.md` has the map of the stack and where to read about
each level.

## Scope

The stack, as a whole:

| Level | What it is | Owns |
| --- | --- | --- |
| 0 | RocksDB | bytes, ordering, durability, crash atomicity |
| 1 | `sm1/` — the MVCC storage manager | row version, transaction, isolation, visibility |
| 2 | `sm2/` — types and tuples | column, type, row, index entry |
| 3 | `sm3/` — schema and index maintenance | table, index definition, predicate, statistic |

Each level is built on the level directly below it, through
that level's own interface — `sm1` on RocksDB's API, `sm2` on
sm1's, `sm3` on sm2's tuple relation. Two callers may also
reach further down: `sm3` calls sm1's interface as well as
sm2's, and the top level calls sm1 for sessions,
transactions, savepoints and 2PC and sm3 for everything else
(`DESIGN.md` §1). Nothing calls upward. A notion that belongs
to level N is never reimplemented at level N+1, and level N+1's
internals are never reached into from level N-1. This is the
discipline that lets `sm2/SM2_OBJECTIVE.md` say "anything sm1
already does ... sm2 calls it; it does not reimplement it",
and it is the rule this document exists to state once, for
the whole stack, instead of per level.

**Not in scope, at any level, yet.** A query language, a
planner, an executor, multi-process deployment. sm1 keeps
its transaction table, lock manager and SSI graph in one
process's memory, and RocksDB permits one writer process, so
`PART4.md` §2 makes multi-process the hard blocker for the
PostgreSQL target, not a phase-1 omission. The answer is to
change the target, not sm1: the PostgreSQL fork moves from a
process per connection to a thread per connection, so every
backend shares sm1's process.

## The PostgreSQL subset

`FEATURELIST.md` is the source of truth: a checkbox per
PostgreSQL feature, decided once. This document does not
repeat it, because a copy would drift. The shape of it, as
of this writing: scalar types and enums, B-tree indexes with
partial and expression forms, keys and foreign keys,
partitioning, TOAST, online DDL, row-level security, logical
decoding and the libpq wire protocol are in; the extensible
type and index machinery — JSON, arrays, GiST, GIN, and the
C extension ABI — is out.

The target is a fork of PostgreSQL, not an extension, and the
fork will be changed substantially: besides swapping the
storage and transaction layers, it moves from a process per
connection to a thread per connection so that sm1 is linked
into it (`PART4.md` §2).

## Goals

- A storage engine usable on its own, for OLTP workloads
  that do not need SQL: `sm1` alone is a complete, tested
  MVCC key-value store today.
- A tuples layer, `sm2`, that turns `sm1`'s ordered byte
  strings into typed rows, and a schema layer, `sm3`, that
  gives those rows a catalog, secondary indexes and
  predicate scans — usable by a hand-written engine or a
  query layer above them.
- Every decision at every level checked against the
  PostgreSQL target in `PART4.md`, so that the parts of that
  program which are hard — the process model, the tuple
  format, the collation and type system — are found early,
  not after a level is built around an assumption that
  forecloses them.

## Decisions

These are settled; they bind every level below.

- **Language is C++, and so is every level's interface.**
  `sm1`, `sm2` and `sm3` each expose a C++ interface in a
  namespace of their own (`sm1::`, `sm2::`, `sm3::`). Only
  the top level — `include/` and `src/` at the root — wraps
  the stack in a C API, because only PostgreSQL's C code
  needs one, and it calls the stack nowhere else. Four
  rules keep a C++ interface as safe as the C one it
  replaces:
  - *Every level reports failure as sm1 does.* A call
    returns 0 or -1, and the code and message are read from
    the thread-local `sm1::errnum()` and `sm1::errmsg()` — one
    errno namespace for the whole stack, every code defined
    in `sm1/include/sm1.h` (`DESIGN.md` §5).
  - *Only the top level catches.* An exception may pass
    through sm1, sm2 and sm3 unchanged; the C wrapper catches
    everything, because a PostgreSQL `longjmp` across C++
    frames, or a C++ exception through PostgreSQL's C frames,
    is undefined behaviour (`DESIGN.md` §5).
  - *A public header is opaque.* It declares handles, not
    layouts — no private members and no RocksDB type — so a
    caller cannot come to depend on a level's internals.
  - *The error taxonomy is kept.* A retryable error still
    means the engine has already aborted the transaction,
    and sm1's `UPDATED` still means it has not.
- **RocksDB is a key-value store, nothing more.** No level
  may treat it as anything but ordered, durable,
  crash-atomic storage. Every notion of row version,
  transaction, isolation and visibility belongs to `sm1`;
  every notion of column, type and row belongs to `sm2`, and
  every notion of table, index and predicate to `sm3`.
  `README.md` states this rule for readers; it is
  repeated here because it is the one rule a change at
  either level must never cross.
- **One process, many threads.** Forced by RocksDB's
  single-writer-process rule and accepted for the reasons in
  `sm1/SM1_OBJECTIVE.md`. The PostgreSQL fork adapts to it, by
  moving to a thread per connection, rather than the stack
  moving behind IPC (`PART4.md` §2).
- **The catalog is sm3's, held in memory.** PostgreSQL's
  heap catalogs are replaced, not kept beside the stack
  (`PART4.md` §4), so sm1's status record is the one commit
  point. The catalog is loaded into memory at startup and
  written through to sm2 for persistence
  (`sm3/SM3_DESIGN.md` §2).
- **Each level ships its own `SMn_OBJECTIVE.md` and
  `SMn_DESIGN.md`.** This document and `DESIGN.md` at the top
  level cover only what spans levels: the stack's shape, its
  build, and its cross-level decisions. Anything specific to
  one level's internals belongs in that level's own
  documents, not here.

## Where to read next

- `sm1/SM1_OBJECTIVE.md`, `sm2/SM2_OBJECTIVE.md`,
  `sm3/SM3_OBJECTIVE.md` — what each level is for, and the
  decisions that settled its own design.
- `FEATURELIST.md` — which PostgreSQL features wisr supports.
- `PART4.md` — the PostgreSQL target in full, including the
  parts of it that are not yet verified.
- `DESIGN.md` (this directory) — how the stack fits
  together.

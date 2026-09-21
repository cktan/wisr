# OBJECTIVE — wisr

Build a storage stack for OLTP that could, eventually, sit
under PostgreSQL in place of its own storage and transaction
layers. That target is argued in full in `PART3.md`; it is
not a dependency of anything below, but it shapes the
decisions at every level, because a design that could not
host it would be wasted work at this project's scale.

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
| 2 | `sm2/` — records and indexes | column, type, row, index |

Each level speaks only to the level directly below it,
through that level's own API — `sm1` through RocksDB's,
`sm2` through `sm1/include/sm.h`. A notion that belongs to
level N is never reimplemented at level N+1, and level N+1's
internals are never reached into from level N-1. This is the
discipline that lets `sm2/OBJECTIVE.md` say "anything sm1
already does ... sm2 calls it; it does not reimplement it",
and it is the rule this document exists to state once, for
the whole stack, instead of per level.

**Not in scope, at any level, yet.** A query language, a
planner, an executor, multi-process deployment. `PART3.md`
§2 explains why multi-process is the hard blocker for the
PostgreSQL target, not a phase-1 omission: sm1 keeps its
transaction table, lock manager and SSI graph in one
process's memory, and RocksDB permits one writer process.
Getting past that is a separate, later decision — an IPC
layer, most likely — and nothing in sm1 or sm2's design
forecloses it.

## Goals

- A storage engine usable on its own, for OLTP workloads
  that do not need SQL: `sm1` alone is a complete, tested
  MVCC key-value store today.
- A records-and-indexes layer, `sm2`, that turns `sm1`'s
  ordered byte strings into typed rows and secondary
  indexes, usable by a hand-written engine or a query layer
  above it.
- Every decision at every level checked against the
  PostgreSQL target in `PART3.md`, so that the parts of that
  program which are hard — the process model, the record
  format, the collation and type system — are found early,
  not after a level is built around an assumption that
  forecloses them.

## Decisions

These are settled; they bind every level below.

- **Language is C++,** exposing a C ABI at each level's
  public surface (`sm1/include/sm.h` today; `sm2`'s
  equivalent when it has one). A C ABI is what makes the
  "separate process, IPC" escape from `PART3.md` §2 possible
  later without a rewrite.
- **RocksDB is a key-value store, nothing more.** No level
  may treat it as anything but ordered, durable,
  crash-atomic storage. Every notion of row version,
  transaction, isolation and visibility belongs to `sm1`;
  every notion of column, type, row and index belongs to
  `sm2`. `README.md` states this rule for readers; it is
  repeated here because it is the one rule a change at
  either level must never cross.
- **One process, many threads, for now.** Forced by
  RocksDB's single-writer-process rule and accepted for the
  reasons in `sm1/OBJECTIVE.md`. Revisiting it is a project
  of its own, argued in `PART3.md` §2 before it is started.
- **Each level ships its own `OBJECTIVE.md` and
  `DESIGN.md`.** This document and `DESIGN.md` at the top
  level cover only what spans levels: the stack's shape, its
  build, and its cross-level decisions. Anything specific to
  one level's internals belongs in that level's own
  documents, not here.

## Where to read next

- `sm1/OBJECTIVE.md`, `sm2/OBJECTIVE.md` — what each level
  is for, and the decisions that settled its own design.
- `PART3.md` — the PostgreSQL target in full, including the
  parts of it that are not yet verified.
- `DESIGN.md` (this directory) — how the stack fits
  together.

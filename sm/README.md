# wisr

Wisconsin storage on rocks: a stack of storage levels, each one
built on the level below it.

| Level | Where | What it is |
| --- | --- | --- |
| 0 | — | RocksDB: an ordered, durable, crash-atomic key-value store |
| 1 | `sm1/` | the MVCC storage manager: stores, transactions, isolation, locks, 2PC, recovery |
| 2 | `sm2/` | types and tuples: a type system, a key encoding, a tuple format, heap and index relations |
| 3 | `sm3/` | schema and index maintenance: a catalog, index maintenance and builds, scans, statistics |
| top | `include/`, `src/` | the API over the stack, the only one, for PostgreSQL to call: C in style, compiled as C++ |

Level 0 is used only as a place to put bytes. Every notion of
*row version*, *transaction*, *isolation* and *visibility* is
implemented in level 1, every notion of *column*, *type*,
*row* and *index entry* belongs to level 2, and every notion
of *table*, *index definition* and *predicate* belongs to
level 3.

## Building

```
make            build every level
make test       build and test every level
make -C sm1     build level 1 alone
```

Each level has a Makefile of its own with the real targets; the
top-level one delegates to them. It also builds the C wrapper,
in the root's `include/` and `src/`, which belongs to no level,
into `build/libsm.a`: one archive of every level's objects and
the wrapper's, so a C++ program includes `include/sm.h` and
links that (`DESIGN.md` §2, §5). `make test` ends by driving
the wrapper through the `rel/` test suite.

## Where to read

- `LOCK.md` — what gets locked and how: the two mechanisms,
  the lock subjects, and the scenarios that are easy to get
  wrong
- `GLOSSARY.md` — the words this stack uses, the ones that
  mean more than one thing first, and the identifiers side
  by side
- `OBJECTIVE.md` — what wisr as a whole is for, and the
  decisions that span every level
- `DESIGN.md` — how the levels fit together: the stack, the
  build, and the decisions that cross level boundaries
- `NEXT.md` — what to build next and in what order, across
  levels, with the loose ends and the gaps that block nothing
- `sm1/SM1_OBJECTIVE.md` — what level 1 is for, and the decisions
  that settled its design
- `sm1/SM1_DESIGN.md` — how level 1 works; the design of record
- `sm1/API.md` — sm1's C++ API, call by call, in
  `namespace sm1`
- `sm1/INTERRUPT.md` — a design, not built yet: interrupting
  a `std::thread` by bits, interruptible waits, and timers
- `sm1/MVCC.md` — a digest of sm1's MVCC: its timestamps, a
  version's bytes, the status table, and how a reader turns a
  writer's id into a commit timestamp
- `sm1/WRITELOCK.md` — writes that keep no row lock: the
  uncommitted version stands for the lock, as in PostgreSQL
- `sm1/COMMITCACHE.md` — the commit cache: what it holds,
  the life of an entry, and that it starts empty after a
  restart
- `sm1/SAVEPOINT.md` — a rollback to a savepoint that
  deletes nothing: rolled-back writes marked dead by their
  `seq`; designed, not built
- `sm1/SIREAD.md` — SSI's SIREAD locks and rw-edges, explained
- `sm1/RCOMPACT.md` — whether compaction can run on another
  machine; research, not built
- `sm1/mvcc/MVCC.md` — the isolation rules, as a table of
  scenarios that are executed on every build
- `sm1/mvcc/POSTGRES.md` — PostgreSQL's isolation test suite,
  listed, and set against the scenarios here
- `sm2/SM2_OBJECTIVE.md` — what level 2 is for
- `sm2/SM2_DESIGN.md` — the type system, the two on-disk
  encodings, and the tuple relation
- `sm2/TYPE.md` — every type, one by one: native
  representation, order, and key bytes
- `sm2/TUPLE.md` — a row's bytes: the tuple format
- `sm2/VARLENA.md` — a variable-width datum's bytes
- `sm2/QUESTIONS.md` — what is still open in sm2, and where
  each settled answer went
- `sm3/SM3_OBJECTIVE.md` — what level 3 is for
- `sm3/SM3_DESIGN.md` — the catalog, index maintenance and
  builds, scans and statistics
- `STATISTICS.md` — the strategy for statistics: the stack
  analyzes a table and records what is true of an LSM, and
  the host fits that to PostgreSQL's planner
- `BULKLOAD.md` — the design of a bulk load: one
  transaction in one of three modes, mini-batch, batch and
  file, the last writing sorted files that RocksDB ingests
- `FEATURELIST.md` — which PostgreSQL features wisr supports
- `PART4.md` — what it would take to put PostgreSQL on this
  stack, and what that target forces on level 2's design

Paths inside a level's documents are relative to that level, so
`src/db.cc` in `sm1/SM1_DESIGN.md` means `sm1/src/db.cc`.

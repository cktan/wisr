# wisr

Wisconsin storage on rocks: a stack of storage levels, each one
built on the level below it.

| Level | Where | What it is |
| --- | --- | --- |
| 0 | — | RocksDB: an ordered, durable, crash-atomic key-value store |
| 1 | `sm1/` | the MVCC storage manager: stores, transactions, isolation, locks, 2PC, recovery |
| 2 | `sm2/` | types and tuples: a type system, a key encoding, a tuple format, heap and index relations |
| 3 | `sm3/` | schema and index maintenance: a catalog, index maintenance, predicate scans, statistics |
| top | `include/`, `src/` | the C API over the stack, the only one, for PostgreSQL to call |

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
top-level one delegates to them. It will also build the C
wrapper, in the root's `include/` and `src/`, which belongs to
no level; the wrapper is not written yet (`DESIGN.md` §3).

## Where to read

- `OBJECTIVE.md` — what wisr as a whole is for, and the
  decisions that span every level
- `DESIGN.md` — how the levels fit together: the stack, the
  build, and the decisions that cross level boundaries
- `sm1/SM1_OBJECTIVE.md` — what level 1 is for, and the decisions
  that settled its design
- `sm1/SM1_DESIGN.md` — how level 1 works; the design of record
- `sm1/API.md` — sm1's C++ API, call by call, in
  `namespace sm1`
- `sm1/SIREAD.md` — SSI's SIREAD locks and rw-edges, explained
- `sm1/RCOMPACT.md` — whether compaction can run on another
  machine; research, not built
- `sm1/mvcc/MVCC.md` — the isolation rules, as a table of
  scenarios that are executed on every build
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
- `sm3/SM3_DESIGN.md` — the catalog, index maintenance,
  predicate scans and statistics
- `sm3/PLAN.md` — sm3's order of work
- `FEATURELIST.md` — which PostgreSQL features wisr supports
- `PART4.md` — what it would take to put PostgreSQL on this
  stack, and what that target forces on level 2's design

Paths inside a level's documents are relative to that level, so
`src/db.cc` in `sm1/SM1_DESIGN.md` means `sm1/src/db.cc`.

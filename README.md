# wisr

Wisconsin storage on rocks: a stack of storage levels, each one
built on the level below it.

| Level | Where | What it is |
| --- | --- | --- |
| 0 | — | RocksDB: an ordered, durable, crash-atomic key-value store |
| 1 | `sm1/` | the MVCC storage manager: relations, transactions, isolation, locks, 2PC, recovery |
| 2 | `sm2/` | records and indexes: types, a record format, a catalog, secondary indexes |

Level 0 is used only as a place to put bytes. Every notion of
*row version*, *transaction*, *isolation* and *visibility* is
implemented in level 1, and every notion of *column*, *type*,
*row* and *index* belongs to level 2.

## Building

```
make            build every level
make test       build and test every level
make -C sm1     build level 1 alone
```

Each level has a Makefile of its own with the real targets; the
top-level one only delegates.

## Where to read

- `OBJECTIVE.md` — what wisr as a whole is for, and the
  decisions that span every level
- `DESIGN.md` — how the levels fit together: the stack, the
  build, and the decisions that cross level boundaries
- `sm1/OBJECTIVE.md` — what level 1 is for, and the decisions
  that settled its design
- `sm1/DESIGN.md` — how level 1 works; the design of record
- `sm1/API.md` — the `sm_*` C API, call by call
- `sm1/mvcc/MVCC.md` — the isolation rules, as a table of
  scenarios that are executed on every build
- `sm2/OBJECTIVE.md` — what level 2 is for
- `FEATURELIST.md` — which PostgreSQL features wisr supports
- `PART3.md` — what it would take to put PostgreSQL on this
  stack, and what that target forces on level 2's design

Paths inside a level's documents are relative to that level, so
`src/db.cc` in `sm1/DESIGN.md` means `sm1/src/db.cc`.

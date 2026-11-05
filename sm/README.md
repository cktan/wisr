# Wisr

Wisconsin Storage on the Rocks: a stack of storage levels, each
one built on the level below it.

| Level | Where | What it is |
| --- | --- | --- |
| — | — | RocksDB: an ordered, durable, crash-atomic key-value store, under level 1 |
| 0 | `sm0/` | the error state, the log hook, general-purpose helpers that are about no storage, and the system services sm and dg build on, `sm_sys.h` |
| 1 | `sm1/` | the MVCC storage manager: stores, transactions, isolation, locks, 2PC, recovery |
| 2 | `sm2/` | types and tuples: a type system, a key encoding, a tuple format, row and index relations |
| 3 | `sm3/` | schema and index maintenance: a catalog, index maintenance and builds, scans, statistics |
| top | `include/`, `src/` | the API over the stack, the only one, for PostgreSQL to call: C in style, compiled as C++ |

RocksDB is used only as a place to put bytes, and level 0
holds no notion of storage at all. Every notion of
*row version*, *transaction*, *isolation* and *visibility* is
implemented in level 1, every notion of *column*, *type*,
*row* and *index entry* belongs to level 2, and every notion
of *table*, *index definition* and *predicate* belongs to
level 3.

## Building

```
make -j4            build every level
make -j4 test       build and test every level
make -C sm1 -j4     build level 1 alone, and level 0 under it
```

Each level has a Makefile of its own with the real targets; the
top-level one delegates to them. It also builds the C wrapper,
in the root's `include/` and `src/`, which belongs to no level,
into `build/libsm.a`: one archive of every level's objects and
the wrapper's, so a C++ program includes `include/sm.h` and
links that (`DESIGN.md` §2, §5). `make test` ends by driving
the wrapper through the `rel/` test suite.

## Where to read

- `CODE.md` — the rules and practices for changing `sm/`:
  levels, errors, exceptions, names, tests
- `GLOSSARY.md` — the words this stack uses, the ones that
  mean more than one thing first, and the identifiers side
  by side
- `OBJECTIVE.md` — what wisr as a whole is for, and the
  decisions that span every level
- `DESIGN.md` — how the levels fit together: the stack, the
  build, and the decisions that cross level boundaries
- `NEXT.md` — what to build next and in what order, across
  levels, with the loose ends and the gaps that block nothing
- `sm0/README.md` — level 0: the error state, the log hook,
  the helpers every level uses, and the system services
- `sm0/INTERRUPT.md` — interrupting a thread, interruptible
  waits, and timers that interrupt: `sm_sys.h`
- `sm1/SM1_OBJECTIVE.md` — what level 1 is for, and the decisions
  that settled its design
- `sm1/SM1_DESIGN.md` — how level 1 works; the design of record
- `sm1/API.md` — sm1's C++ API, call by call, in
  `namespace sm1`
- `sm1/mvcc/MVCC.md` — the isolation rules, as a table of
  scenarios that are executed on every build
- `sm1/mvcc/POSTGRES.md` — PostgreSQL's isolation test suite,
  listed, and set against the scenarios here
- `sm1/mvcc/DRIVER.md` — the scenario driver: its command
  and the language of the scripts
- `sm1/mvcc/AUDIT.md` and `sm1/mvcc/REPORT.md` — the
  scenarios' expected transcripts, and their driver, audited
- `sm2/SM2_OBJECTIVE.md` — what level 2 is for
- `sm2/SM2_DESIGN.md` — the type system, the two on-disk
  encodings, and the tuple relation
- `sm2/TYPE.md` — every type, one by one: native
  representation, order, and key bytes
- `sm2/TUPLE.md` — a row's bytes: the tuple format
- `sm2/VARLENA.md` — a variable-width datum's bytes
- `sm3/SM3_OBJECTIVE.md` — what level 3 is for
- `sm3/SM3_DESIGN.md` — the catalog, index maintenance and
  builds, scans and statistics
- `rel/REL_DESIGN.md` and `rel/REL.md` — `rel`, the test
  driver over `include/sm.h`: its design and its verbs
- `sm0/VFD.md` — virtual file descriptors, under one ceiling
  for the process
- `FEATURELIST.md` — the PostgreSQL subset wisr targets,
  decided feature by feature; not all of it is built
- `PART4.md` — what it would take to put PostgreSQL on this
  stack, and what that target forces on level 2's design

Paths inside a level's documents are relative to that level, so
`src/db.cc` in `sm1/SM1_DESIGN.md` means `sm1/src/db.cc`.

### The modules

One document for each module, with its design and implementation.
`GLOSSARY.md` holds the same names, with the code of each.

- `DBMGR.md` — Database Manager: environments, databases, stores,
  the WAL, recovery
- `XCTMGR.md` — Transaction Manager: snapshots, commit,
  savepoints, SSI, 2PC
- `LOCKMGR.md` — Lock Manager: row and object locks, deadlocks,
  write latches
- `BUFMGR.md` — Buffer Manager: sm's buffer pool: frames, pins,
  the clock
- `GC.md` — Garbage Collector: the watermark, the compaction
  filter, the `CF:txn` sweep
- `ROWSTORE.md` — Row Store: row keys, versions, chains, the read
  and write paths
- `TOAST.md` — TOAST: a datum stored out of line
- `LSMINDEX.md` — LSM Index: an index whose entries are sm1 keys
- `BTREEINDEX.md` — B-tree Index: an index of its own pages, its
  in-doubt bit, the cleaner
- `INDEXBUILD.md` — Index Builder: concurrent index builds
- `CATALOG.md` — Catalog: definitions in memory and on disk, DDL
- `NAMESPACE.md` — Namespace: a schema, and name lookup
- `SEQUENCE.md` — Sequence: counters and their leases
- `TEMPTABLE.md` — Temporary Table: a table of one session
- `VIEW.md` — View: a stored query's definition
- `STATISTICS.md` — Statistics: what ANALYZE records
- `BULKLOAD.md` — Bulk Load: loading an empty table from sorted
  files
- `CLUSTER.md` — Clustered Table: rows stored in primary-key
  order
- `FOREIGNKEY.md` — Foreign Key: a table's references to another
- `LOGICALDECODE.md` — Logical Decoding: a design, not built


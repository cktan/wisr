# DESIGN — sm1, the MVCC storage manager

sm1 is level 1 of the stack. It is a multi-version (MVCC)
storage manager for OLTP, built on RocksDB.

RocksDB is used only as an ordered, durable, crash-atomic
key-value store. sm1 owns everything else: row versions,
transactions, isolation and visibility.

This document is an overview. Each module has its own design
document (§3). [`SM1_OBJECTIVE.md`](SM1_OBJECTIVE.md) says what
the level is for.

## 1. What the level provides

- **Stores:** ordered maps from a byte-string key to a
  byte-string value. Each store is one RocksDB column family. A
  store holds two kinds of key
  ([`../ROWSTORE.md`](../ROWSTORE.md#31-two-kinds-of-key-in-one-store)
  §3.1):
  - a *versioned* key keeps each version as a key of its own;
  - a *chained* key keeps a row's newest version under its plain
    key, and the older versions in a chain behind it.
- **Transactions:** MVCC reads and forward scans, nested
  savepoints, and two-phase commit (2PC). The isolation levels
  are READ COMMITTED, REPEATABLE READ and SERIALIZABLE. Every
  level uses snapshot isolation. SERIALIZABLE adds SSI
  (serializable snapshot isolation).
- **Locks:** row locks and object locks in Gray's five modes,
  with deadlock detection.
- **Pages:** a buffer pool of 4 KB frames, for the B-tree Indexes
  of the levels above.
- **Durability:** real 2PC with a durable prepare record, and
  crash recovery with no undo pass.
- **Upkeep:** garbage collection of dead versions, named
  sequences, and the sizes and probes statistics need.

The API is `include/sm1.h`, in `namespace sm1`.
[`API.md`](API.md) documents it call by call.

sm1 keeps no types and no schema. A value is opaque bytes to
sm1, and sm1 has no secondary index
([`../DBMGR.md`](../DBMGR.md) §1.1).

## 2. What the level depends on

| Dependency | What sm1 takes from it |
| --- | --- |
| RocksDB | column families, atomic write batches, the WAL and compaction; sm1 installs its own comparator, prefix extractor, compaction filter, file system and listeners |
| `../sm0` | the error state and the log hook, which `sm1.h` brings into `namespace sm1`; the codec, defer blocks and `format_into`; the vendored doctest; the system services of `sm_sys.h` |
| `../include` | the stack's shared headers: `sm_error.h`, `sm_lock.h`, `sm_options.h` |

sm1 includes nothing from the levels above it. It is usable
alone, because an MVCC key-value store is a complete product.

## 3. Modules

| Module | Document | Summary | Code in `src/` |
| --- | --- | --- | --- |
| Database Manager | [`../DBMGR.md`](../DBMGR.md) | environments, databases, sessions, stores and their column families; the WAL, recovery, the store catalog | `env.cc`, `db.cc`, `session.cc`, `catalog.cc`, `walfs.cc`, `recover.cc` |
| Transaction Manager | [`../XCTMGR.md`](../XCTMGR.md) | timestamps, snapshots, isolation levels, savepoints, SSI, commit, the commit cache, 2PC | `xct.cc`, `ssi.cc` |
| Lock Manager | [`../LOCKMGR.md`](../LOCKMGR.md) | row and object locks, deadlocks, what a write does when it meets another writer | `lockmgr.cc`, `locktab.cc` |
| Buffer Manager | [`../BUFMGR.md`](../BUFMGR.md) | the pool of 4 KB frames that holds B-tree Index pages; a pin is also a latch | `bufpool.cc`, `page.cc` |
| Row Store | [`../ROWSTORE.md`](../ROWSTORE.md) | versions in a store: the write path, the read path, chains and pins; the row key and the tuple relation above sm1 | `access.cc`, `chain.cc`, `version.cc`, `pin.cc` |
| Garbage Collector | [`../GC.md`](../GC.md) | reclaims dead versions, tombstones, dropped stores and status records, on RocksDB compaction | `gc.cc` |
| Sequence | [`../SEQUENCE.md`](../SEQUENCE.md) | sm1's named counters, the way any level gets unique ids | `seq.cc` |

Three designs span levels, and start in sm1:

| Design | What it covers | Code in `src/` |
| --- | --- | --- |
| [`../TEMPTABLE.md`](../TEMPTABLE.md) | the temporary instance and its stores | |
| [`../BULKLOAD.md`](../BULKLOAD.md) | bulk load | `load.cc` |
| [`../STATISTICS.md`](../STATISTICS.md) | statistics | `stats.cc`, `memcount.cc` |

Three documents in `mvcc/` cover isolation:

- [`mvcc/MVCC.md`](mvcc/MVCC.md) is the specification of
  isolation, as a table of scenarios.
- [`mvcc/POSTGRES.md`](mvcc/POSTGRES.md) compares it with
  PostgreSQL's isolation suite.
- [`mvcc/DRIVER.md`](mvcc/DRIVER.md) describes the scenario
  driver and its language.

## 4. Structure

### 4.1 Source layout

```
include/sm1.h    the public C++ API
src/             the engine, and nothing else
test/            the unit tests, doctest
mvcc/            isolation: MVCC.md and one file a scenario
mvcc/driver/     the scenario driver
bench/           the lock table's benchmark
example/         small programs against the public API
```

[`../DBMGR.md`](../DBMGR.md#131-source-layout-of-sm1) §13.1 lists
`src/` file by file. A utility that is not about the engine
belongs in `../sm0`.

### 4.2 Build

```
make -j4              libsm1.a, the tests, the examples, the benchmark
DEBUG=1 make -j4      the same, -O0 -g, with assert()
make format           clang-format src/, test/, example/, bench/
```

`make -j4` builds with `-O3 -DNDEBUG`. `build/flags` records
the flags, so a change of `DEBUG` rebuilds everything. The build
makes `sm0` first.

### 4.3 Tests

**Warning: the scenarios in `mvcc/` are the specification.** If
a change to sm1 makes a scenario fail, the change is presumed
wrong, not the scenario. Do not run `make accept`, which takes
the new transcripts as expected, without the user's approval
([`../../CLAUDE.md`](../../CLAUDE.md)).

```
DEBUG=1 make -j4 test    unit tests, scenarios, examples
```

`make test` runs:

1. Each scenario script of `mvcc/in`, twice: once on versioned
   stores and once on chained stores. Both runs must match the
   one expected transcript in `mvcc/good`.
2. The unit tests in `test/`.
3. The stress programs, `mvcc/e*.cc`.
4. The examples.

[`../DBMGR.md`](../DBMGR.md#17-tests) §17 says what the unit
tests prove, and each module document lists its own tests.
[`../DESIGN.md`](../DESIGN.md#4-testing-across-levels) §4 gives
the two-layer test scheme that every level follows.

Set `TMPDIR` to a directory under `bench/build` at the root of
the repository. Test homes do not go in `/tmp`.

## 5. Cross-level decisions

sm1 owns no cross-level decision alone.
[`../DESIGN.md`](../DESIGN.md#5-cross-level-decisions) §5 holds
them. Two of them concern sm1:

- sm1's interface is the only way into sm1.
- Every level reports failure the way sm1 does.

Two decisions are sm1's own, and bind the levels above it:

- **Backward scans are out of scope, permanently**
  ([`../DBMGR.md`](../DBMGR.md) §1.2). Level 2 builds descending
  order into the key instead.
- **One process, many threads, and one shared RocksDB instance
  for each database**
  ([`../DBMGR.md`](../DBMGR.md#11-the-decisions-of-level-1)
  §1.1). Access from another process needs a server front end,
  not a change to this design.

The modules own their trade-offs:
[`../DBMGR.md`](../DBMGR.md#14-trade-offs) §14,
[`../XCTMGR.md`](../XCTMGR.md) and [`../GC.md`](../GC.md).

# bench — design

What is measured, on what, and how, so that a number from
digres and one from PostgreSQL 18 can be set side by side.
`README.md` says why.

## 1. The two systems

| | PostgreSQL 18 | digres |
| --- | --- | --- |
| Source | `~/p/postgres18`, at digres's fork point, 18.6, `724edf9` (`../dg/README.md`) | this repository |
| Build | `./configure` (`-O2`, no asserts), installed under `bench/build/pg18` | `-O2`, no asserts, objects of their own (§2) |
| Storage | heap pages, shared buffers, the OS page cache | sm on RocksDB: block cache, memtables |
| Server | `postgres -D` on a data directory `initdb` makes | `digres -D` on one digres's `initdb` makes |

Both are run on the same machine, one at a time, each on its
own port, with nothing else of note running.

## 2. Optimized builds

- **PostgreSQL**: a worktree of `~/p/postgres18` checked out
  at `724edf9`, `./configure --prefix=$PWD/bench/build/pg18`
  with no `--enable-cassert` or `--enable-debug`, `make` and
  `make install`. The tree there now is 187 commits past it,
  on REL_18_STABLE; the fork point keeps the executor and
  planner the same code on both sides.
- **digres**: `make -C dg release` builds
  `build/digres-release` from objects of its own, `-O2
  -DNDEBUG` without `USE_ASSERT_CHECKING`, as its TSan build
  has its own (`OBJ=tsan.o`), so the debug build the tests use
  stays as it is. It links sm built into `sm/build/release` at
  `-O3 -DNDEBUG`, sq as `libsq-release.a` at `-O2 -DNDEBUG`,
  and RocksDB 8.9.1 built by `test/rocksdb.sh release` at
  `-O3 -DNDEBUG`, statically: the system's `librocksdb.so`
  keeps its `assert()`s (5,858 calls of `__assert_fail`).
  `CODE.md` makes this a rule, and `run.sh` checks it.
- Each report records both commits and the flags.

## 3. Settings

Chosen so neither side does work the other cannot:

- **No parallel query**: `max_parallel_workers_per_gather =
  0` on PostgreSQL. digres has none: its parallel backend is
  gone (`../dg/WORKNOTE.md` step 10).
- **No JIT**: `jit = off`. digres was built without it.
- **Memory**: PostgreSQL's `shared_buffers` 2 GB, large
  enough to hold the data. sm's block cache cannot be set: sm
  leaves each store RocksDB's default, a cache of 32 MB of its
  own (`store_cf_options()`, `../sm/sm1/src/db.cc`), so a scan
  of a table larger than that reads its blocks from the OS's
  page cache, and decompresses them, each time. That is how
  digres runs today, and is measured as it is; making the
  cache settable is a change to sm of its own.
- **PostgreSQL's heap settled**: `VACUUM (FREEZE, ANALYZE)`
  after the load, so no scan pays for setting hint bits or
  pruning, and autovacuum off, so none starts during a run.
  digres: `ANALYZE` after the load.
- Everything else as each ships.

## 4. The data

Sizes fit a machine of 7 GB with room to spare: about 1 GB a
table at most.

- **`narrow`**: `id int8` primary key, and 4 more columns, a
  million rows. The baseline: a row's own cost, small.
- **`wide_fixed`**: `id int8` primary key and 49 more `int8`
  columns, a million rows: deforming the last column walks
  49 fixed-width ones before it.
- **`wide_mixed`**: as `wide_fixed`, but every fifth column
  `text` of 10 to 30 bytes: a variable-width column before the
  last makes PostgreSQL walk each one (§6).
- **`toasted`**: `id int8` primary key and `body text` of
  8 KB, 100,000 rows. The text is random, so neither side can
  compress it; on PostgreSQL the column is `STORAGE EXTERNAL`
  besides, so its values go out of line uncompressed, as
  sm's do.

Every value is made by `generate_series` and functions of the
row number, so both sides hold the same rows, and a run can be
repeated.

## 5. The queries

Each is an aggregate, so a single row comes back and the
client's cost is the same on both sides.

| # | Query | What it isolates |
| --- | --- | --- |
| Q1 | `select count(*) from T` | the scan, deforming nothing |
| Q2 | `select sum(c1) from T` | the scan, and the first column |
| Q3 | `select sum(c49) from T` | the scan, and the last column |
| Q4 | `select sum(length(body)) from toasted` | a value out of line, read whole |
| Q5 | `select count(*) from toasted` | the same scan, not reading the values |
| Q6 | `select sum(c1) from T where id between :a and :a + 9999`, index scan only | an index range of 10,000 rows |
| Q7 | `select c1 from T where id = :k`, `:k` random | one row by key, a query at a time |

Q1 to Q3 run on `narrow`, `wide_fixed` and `wide_mixed`. Q6
and Q7 run with `enable_seqscan`, `enable_bitmapscan` off, so
the plan is the index scan each names; each run checks the
plan with `EXPLAIN` first.

### 5.1 How a query is timed

- **Q1 to Q6**: `EXPLAIN (ANALYZE, TIMING OFF)`, whose
  `Execution Time` is the executor's alone, with no parse,
  plan or network in it; each query run 10 times after 2
  warm-up runs, and the median reported, with the spread.
- **Q7**: `pgbench -n -f` with the query, one client, 30
  seconds, its transactions a second: a point read is small
  enough that the round trip is part of what it costs, on
  both sides alike.
- Each report gives each number for both systems and their
  ratio, digres over PostgreSQL.

## 6. The cost of converting sm's tuple format

digres reads a row as sm2 laid it out, and deforms the
columns a scan needs into the slot's `Datum`s and nulls
(`tuple_into_slot()`, `../dg/src/backend/access/sm/
smstore.c`; `sm2::tup_deform`, `../sm/sm2/TUPLE.md` §8).
PostgreSQL deforms its heap tuple the same way, into the same
slot (`slot_deform_heap_tuple()`). The two layouts differ in
what reaching column n costs:

- **PostgreSQL** walks every column before n: the heap tuple
  keeps no offsets, so a variable-width column on the way, or
  a null, makes it compute each one (its `attcacheoff` helps
  only up to the first of them).
- **sm2** keeps its widths and entry offsets in the table's
  `tup_desc_t` (`../sm/sm2/TUPLE.md` §6): a column's place is
  found in a step per earlier null, not a walk over the row.

So Q2 against Q3 on `wide_fixed` and `wide_mixed` asks, on
each side, what the last column costs over the first; the
difference of the two differences is what the formats do.
Two ways to see the cost directly, besides:

- **A profile**: `perf record` on each server while Q3 runs
  on `wide_mixed`, and the share of time in deforming —
  `tuple_into_slot()`, `sm_tbl_deform()` and `tup_deform` on
  digres, `slot_deform_heap_tuple()` on PostgreSQL — against
  the scan's own (RocksDB's iterator, the heap's page walk).
- **A microbenchmark, sm alone**: a program against the C
  wrapper (`../sm/include/sm.h`) that scans `wide_mixed`'s
  store and only reads each row, then reads and deforms its
  first column, then its last, then all of them: sm's cost
  with no executor in it at all.

## 7. A report

`results/YYYYMMDD-HHMM.md`, one a run: the machine (CPU,
cores, memory, kernel), both commits and builds, the settings
of §3, each table's size on disk on both sides, and a table of
§5's numbers with their ratios. A run is the unit: numbers
from two runs are not mixed in one table.

## 8. What can mislead

- **WSL2's clock and scheduler**: a run is repeated, and the
  spread reported, before a difference is believed.
- **The cache**: a first run reads from disk on one side and
  memory on the other; the warm-up runs and §3's sizes are to
  keep every measured run in memory.
- **Plans**: the two planners have different statistics, so a
  plan may differ; each run checks it (§5).
- **Sizes**: sm's store and PostgreSQL's heap hold the same
  rows in different bytes; each report gives both, as a cost
  of its own.

## 9. Decided

1. **PostgreSQL at the fork point**, `724edf9`.
2. **digres's `release` build** as its own objects.
3. **The sizes** of §4.
4. **The microbenchmark** of §6 waits for the profile.

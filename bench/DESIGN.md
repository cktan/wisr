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
- **Memory**: `shared_buffers` 2 GB on both, large enough to
  hold the data. On digres it sizes sm's block cache, the one
  every database's RocksDB reads through
  (`../sm/sm1/SM1_DESIGN.md` §12.2), whose blocks sm stores
  compressed with LZ4 and the cache holds decompressed.
- **Each side settled after the load**: `VACUUM (FREEZE,
  ANALYZE)` on both. On PostgreSQL no scan then pays for
  setting hint bits or pruning, and autovacuum is off, so none
  starts during a run. On digres it compacts each table's
  store into one sorted run, so a lookup visits one, and FREEZE
  has nothing to do (`../dg/WORKNOTE.md` step 284).
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
  last makes PostgreSQL walk each one (§6). It has a secondary
  index, `wide_mixed_c2` on `c2`, which is `id + 2`, one row a
  value, made before the load, for Q9 to Q11.
- **`toasted`**: `id int8` primary key and `body text` of
  8 KB, 100,000 rows. The text is random, so neither side can
  compress it; on PostgreSQL the column is `STORAGE EXTERNAL`
  besides, so its values go out of line uncompressed, as
  sm's do.

Every value is made by `generate_series` and functions of the
row number, so both sides hold the same rows, and a run can be
repeated.

## 5. The queries

Q1 to Q6 and Q9 are aggregates, so a single row comes back
and the client's cost is the same on both sides. Q6 to Q8 are
what a database serving web pages does: a page of rows by a
range of their key, a batch of rows by their keys, and those
reads under many clients with writes among them. Q9 to Q11
are Q6 to Q8 by a secondary index, the same rows: an entry
found, then its row read, where the primary key's entry is
the row.

| # | Query | What it isolates |
| --- | --- | --- |
| Q1 | `select count(*) from T` | the scan, deforming nothing |
| Q2 | `select sum(c1) from T` | the scan, and the first column |
| Q3 | `select sum(c49) from T`, `sum(c4)` on `narrow` | the scan, and the last column |
| Q4 | `select sum(length(body)) from toasted` | a value out of line, read whole |
| Q5 | `select count(*) from toasted` | the same scan, not reading the values |
| Q6 | `select sum(c1) from T where id between :a and :a + 99`, index scan only | an index range of 100 rows: a page of them, its start costing as much as its rows |
| Q7 | `select c1 from T where id = any(array[:k1, ..., :k100])`, each `:k` Zipfian | 100 rows by key in one statement, a batch loader's fetch, hot keys drawn often |
| Q8 | Q7, 9 times in 10, and `update T set c1 = c1 + 1 where id = :k`, `:k` Zipfian, 1 in 10; 16 clients | reads and writes at once, the hot rows contended |
| Q9 | `select sum(c1) from T where c2 between :a and :a + 99`, index scan only | Q6 by the secondary index |
| Q10 | `select c1 from T where c2 = any(array[:k1, ..., :k100])`, each `:k` Zipfian | Q7 by the secondary index |
| Q11 | Q10, 9 times in 10, and `update T set c1 = c1 + 1 where c2 = :k`, 1 in 10; 16 clients | Q8 by the secondary index |

Q1 to Q3 run on `narrow`, `wide_fixed` and `wide_mixed`. Q1
to Q5 run with index, index-only and bitmap scans off, and Q6
and Q9 with `enable_seqscan`, `enable_bitmapscan` off, so the
plan is the scan each names; each run records the plan's scan
node from `EXPLAIN` in the report, and does not enforce it.
Q7, Q8, Q10 and Q11 are plain pgbench, with no setting; Q7's
and Q10's plans are recorded as Q6's is, from each statement
with keys 1 to 100.

The keys of Q7 and Q8 are pgbench's `random_zipfian(1,
1000000, 1.1)`: the most drawn key comes up about once in 8
draws, the 10 most drawn a third of the time, and the 1,000
most drawn 69%, as popular items and active users do in a
site's traffic. The 100 keys of Q7 are drawn apart, so some
repeat; the statement returns each row once. `sql/q7.sql`
has the 100 `\set` lines written out, as pgbench has no
loop. Q10 and Q11 draw theirs from `random_zipfian(3,
1000002, 1.1)`, `c2`'s values, so each key names Q7's row.

### 5.1 How a query is timed

- **Q1 to Q6, Q9**: `EXPLAIN (ANALYZE, TIMING OFF)`, whose
  `Execution Time` is the executor's alone, with no parse,
  plan or network in it; each query run 10 times after 2
  warm-up runs, and the median reported, with the spread,
  to a tenth of a millisecond, or a thousandth below 10 ms.
- **Q7, Q10**: `pgbench -n -M prepared -f` with the query, one
  client, 30 seconds, its transactions a second: a fetch of
  100 rows is small enough that the round trip is part of
  what it costs, on both sides alike. Prepared, as an
  application's statements are, so the plan is made once.
- **Q8, Q11**: `pgbench -n -M prepared -c 16 -j 4`, Q7's
  script at weight 9 and `sql/q8.sql`'s update at weight 1,
  or Q10's and `sql/q11.sql`'s, 30 seconds, its transactions
  a second. They run last, as their updates change `c1`.
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
cores, memory, kernel), both commits and builds, the runs a
query, a table of §5's numbers with their ratios and each
side's scan node, and each table's size as each system
reports it, with each data directory's. The settings of §3
are not in it. A run is the unit: numbers from two runs are
not mixed in one table.

Each report also gets a row in `results/LOG.md`, the log of
every run: its date, commit, machine and file (`CODE.md`).

## 8. What can mislead

- **WSL2's clock and scheduler**: a run is repeated, and the
  spread reported, before a difference is believed.
- **The cache**: a first run reads from disk on one side and
  memory on the other; the warm-up runs and §3's sizes are to
  keep every measured run in memory.
- **Plans**: the two planners have different statistics, so a
  plan may differ; each report shows each side's scan node
  (§5), which is read, not checked.
- **Sizes**: sm's store and PostgreSQL's heap hold the same
  rows in different bytes; each report gives both, as a cost
  of its own.

## 9. Decided

1. **PostgreSQL at the fork point**, `724edf9`.
2. **digres's `release` build** as its own objects.
3. **The sizes** of §4.
4. **The microbenchmark** of §6 waits for the profile. It is
   built since, as `smbench/`, on rows of its own, layer by
   layer down to RocksDB.

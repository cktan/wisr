# STATISTICS — what the planner needs, and how wisr gives it

The fork keeps PostgreSQL's planner (`PART4.md` §4), and the
planner costs every plan from statistics that PostgreSQL's
storage has always supplied. This file is the strategy for
supplying the same, or near enough, from a stack that has no
pages, no heap files and no vacuum.

It is a strategy and not yet a design. `sm3/SM3_DESIGN.md` §6
is where the design goes once §6 below has been measured.

What is said here of PostgreSQL is from memory of its source
and its documentation. Each claim the strategy leans on is
listed in §7 to be checked against the source before the fork
relies on it.

---

## 0. The proposal in brief

**Who does what**

- The stack analyzes. `sm_analyze(table)` draws a sample,
  computes the statistics, and writes them to sm3's catalog.
- What it writes describes an LSM keyed by `rowno`: bytes,
  rows, rownos handed out, levels. It has no pages, no tree
  height and no all-visible fraction in it.
- The host reads them and derives what PostgreSQL's planner
  wants: pages, `reltuples`, `pg_statistic`'s slots. That
  conversion is the fork's code.
- The fork has no `pg_class` and no `pg_statistic`. Its
  `ANALYZE` is a call to `sm_analyze`.

**What `sm_analyze` records**

- **A table:** bytes on disk, live rows, the rowno limit, and
  the average bytes of a row.
- **An index:** bytes on disk and entries.
- **A column, and an expression of an index:** null fraction,
  average bytes, distinct count, most common values, histogram
  bounds, and correlation with `rowno`.

**What the stack answers live, at plan time**

- The bytes of a heap or an index now, the rowno limit now,
  and the LSM's level count now. None is stored, and none
  needs an `ANALYZE`.

**Why the stack can compute them**

- A distribution needs only an order and an equality on a
  type. sm2 has both for every type it stores, since index
  keys need them, and they already have to agree with
  PostgreSQL's.
- The one thing it cannot do is evaluate an expression, and
  for that it calls the host's index evaluator, as index
  maintenance does.

**What stays the host's**

- Turning the stack's numbers into the planner's (§3.7).
- The cost of an index lookup, from the level count.
- Deciding when to analyze again.
- Extended statistics over groups of columns, if ever wanted.

**What has to be built in the stack**

- sm1: the bytes of a key range, a sequence's last value
  without drawing one, and the LSM's level count.
- sm2 and sm3: the same for a heap and for an index, and the
  rowno limit of a table.
- sm3: the sampler, the estimators, statistics in the catalog,
  and `analyze` over them.
- `include/sm.h`: `sm_analyze` and the calls that read.
- `rel`: an `analyze` verb, so that all of it is tested here.

**What is not known**

- Whether RocksDB's size estimates are good enough. Nothing
  has measured it, and four experiments come before any
  design (§6).
- Whether what is said here of PostgreSQL is right. It is
  from memory, and the claims are listed to check (§7).
- The weights in the index lookup cost, to be calibrated
  against real queries.

---

## 1. What PostgreSQL expects

Four kinds of thing, from three different places. The middle
column is stock PostgreSQL, to say what the planner is used
to being given.

**None of those catalogs exists in the fork.** `pg_class`,
`pg_statistic` and the rest are heap tables in PostgreSQL, and
the fork has no heap: its catalog is sm3's (`PART4.md` §4).
There is no `pg_class` row to update and no `pg_statistic`
row to insert. The numbers below keep PostgreSQL's names in
this file, `relpages` and `reltuples`, only because those are
the names the planner's code knows them by. Where they are
kept is §3.6, and how the fork's planner gets them is §3.7.

| What | Where stock PostgreSQL keeps it | Who produces it there |
| --- | --- | --- |
| a relation's size in pages, its row count, and how much of it is all-visible | `pg_class`: `relpages`, `reltuples`, `relallvisible` | `VACUUM`, `ANALYZE`, `CREATE INDEX`; and the storage, asked again at plan time |
| each column's distribution | `pg_statistic`: null fraction, average width, distinct count, most common values, histogram, correlation | `ANALYZE`, from a sample |
| a sample of the table's rows | nowhere; it is taken when wanted | the storage, a block and then a row at a time |
| counts of rows inserted, updated and deleted | the cumulative statistics system | the storage's write path, as it goes |

Three properties of how they are used shape everything below.

- **The planner does not trust `relpages`.** At plan time it
  asks the storage how big the relation is now, and scales
  `reltuples` by the density it stored: rows a page, times
  pages today. So the stored numbers are a density, and the
  live number that matters is the size.
- **`ANALYZE` computes the distributions itself.** In stock
  PostgreSQL, histograms, most common values, distinct counts
  and correlation are PostgreSQL's code working on sampled
  rows, and the storage does not know what a histogram is.
  The fork moves that work into the stack (§2).
- **Everything is costed in pages.** `seq_page_cost` and
  `random_page_cost` multiply a page count. A storage with no
  pages still has to say how many it has.

---

## 2. Principles

1. **The stack analyzes; the host translates.** A statistic
   needs an order and an equality on a type, and sm2 has
   them. Evaluating an expression is still the host's, by the
   evaluator callback (`DESIGN.md` §5).
2. **The stack speaks LSM.** What `sm_analyze` records is what
   is true of this storage: bytes, rows, rownos, levels. It
   does not pretend to have pages or a B-tree. Fitting those
   numbers to a planner written for a paged heap is the
   host's, so that a planner written for an LSM later loses
   nothing.
3. **Testable here.** sm3 is built and tested without a
   PostgreSQL tree. Every estimate is checked in this
   repository against a full scan of generated data.
4. **Approximate is the contract.** PostgreSQL's own numbers
   are estimates, a sample of 30,000 rows scaled up. The aim is
   an error the planner already tolerates, not exactness.
5. **No new hot key.** Nothing here may make every writer
   update one shared row. The schema lock is already the
   contended key of a table (`sm3/SM3_DESIGN.md` §3).
6. **Forward only.** A sample is taken with forward seeks, as
   every scan in the stack is (`sm1/SM1_OBJECTIVE.md`).

---

## 3. The strategy, item by item

### 3.1 Size

**Give:** for a table's heap, and for each of its indexes, an
approximate size in bytes, cheap enough to ask for at every
plan.

**How:** a table is one sm1 store, a RocksDB column family,
and its heap and each index sit under a 2-byte prefix of their
own (`sm3/SM3_DESIGN.md` §4). RocksDB estimates the bytes in a
key range without reading it. Asked of a prefix's range, with
the memtables counted, that is the object's size.

**What it measures:** bytes on disk, after compression, with
every version sm1 has not yet collected and every tombstone
not yet compacted away. That is the right thing to charge a
scan for, since a scan reads them.

`sm_analyze` records the size it saw, beside the row count it
estimated at the same moment. The size is also answered live,
so a caller can tell how far a table has moved since.

### 3.2 Row count

**Give:** an estimate of a table's live rows.

**How:** by sampling, from two facts the stack has exactly.

- A row's key is its `rowno`, drawn from a per-table sequence
  and never reused (`sm2/SM2_DESIGN.md` §5). So the sequence's
  last value, `N`, is how many rownos were ever handed out,
  and the live rows are some fraction of them.
- A sample of rownos, each looked for, says what fraction.

The row count is that fraction times `N`. With the sample of
§3.3, the error is a fraction of a percent for a table that
is not nearly empty.

**Between two runs** nothing is updated, and two live numbers
say how the table has moved.

- `N` now, less `N` as recorded, is the rownos handed out
  since: an upper bound on the rows inserted, exact but for
  the rownos of transactions that rolled back.
- Bytes now over bytes as recorded is growth of every kind,
  old versions not yet collected among it.

**Why not a counter.** An exact live count kept in the catalog
would be a row every insert and delete updates: the hot key
principle 5 forbids. PostgreSQL does not keep one either.

### 3.3 The sample

**Give:** a random sample of the table's live rows, of one
moment, of about 300 rows for each unit of a statistics
target that the caller passes. PostgreSQL's default target of
100 makes that 30,000 rows.

**How:** the sampling unit is a run of consecutive rownos, of
a fixed length `B`.

- Runs are chosen at random over `0` to `N`, and visited in
  ascending order: one forward cursor over the heap, moved
  with `scan_seek` to each run's first rowno, reads the live
  rows of the run and moves on.
- The cursor's snapshot is fixed when it is opened, so the
  whole sample is of one moment.
- The live rows found, over the rownos covered, is the live
  fraction of §3.2.
- If the runs yield more rows than wanted, a reservoir keeps
  a uniform subset.

**The length of a run is a trade.** A short run is nearly a
uniform sample of rows and costs a seek for each row or two.
A long run costs few seeks, and its rows are neighbours, which
were inserted together and are alike, so distinct counts come
out low. PostgreSQL's sampler, which takes about one row from
each of 30,000 heap pages, sits at the short end. Where wisr
should sit is measured (§6), not assumed.

No part of this is PostgreSQL's sampler, and no sampled row
crosses the API.

### 3.4 Each column's statistics

**Give:** for each column, from the sample:

| Statistic | Needs of the type |
| --- | --- |
| null fraction | nothing |
| average bytes of a value, as stored | nothing |
| distinct count, and whether it grows with the table | equality |
| most common values and their frequencies | equality |
| histogram bounds over the rest | order |
| correlation of the column's order with `rowno` order | order |

Equality and order are sm2's `cmp` for the column's type,
under the column's collation: the same one index keys are
ordered by (`sm2/TYPE.md`).

**The estimators are PostgreSQL's, ported.** How many distinct
values a sample implies, which values are common enough to
list, and where histogram bounds fall are judgements the
planner's selectivity code was written against. They are
copied and adapted into sm3, not reinvented, so that the
numbers mean what the planner assumes.

**Correlation is a fact about this storage**, not a borrowed
one. The heap is stored in `rowno` order, so a column whose
order follows `rowno` is clustered on disk, and a range scan
of an index on it fetches neighbouring rows.

**Expressions and predicates.** For an expression index,
`sm_analyze` calls the host's evaluator on each sampled row
(`sm3/SM3_DESIGN.md` §3). The evaluator returns the
expression's value and whether the row belongs to the index,
so the same statistics are computed for each expression, and
the fraction of rows a partial index takes comes with them.

**Not covered.**

- Extended statistics over groups of columns. PostgreSQL has
  none either unless a user creates them. When wanted, the
  host computes them over a scan and sm3 keeps them as bytes
  it does not read.
- A type sm2 does not store. Each new type brings an order and
  an equality with it, and that is all this needs.
- An inheritance or partition tree. The host combines the
  statistics of its tables.

### 3.5 Indexes

**Size:** as §3.1, over the index's prefix.

**Entries:** the table's row count, times the fraction of
sampled rows that belong to the index, which is 1 unless it
is partial.

**Levels:** how many levels the store's LSM has now, answered
live. An LSM has no tree height. A lookup costs a probe of the
memtables and of some number of sorted files, cut down by
bloom filters, and the level count is what the stack knows of
that.

### 3.6 What is recorded, and where

`sm_analyze(table)` runs in the caller's transaction, reads
at its snapshot, and writes these to sm3's catalog.

| Of | Recorded |
| --- | --- |
| the table | rowno limit `N`; live rows; heap bytes; average bytes of a row; rows sampled |
| each index | bytes; entries |
| each column | the six statistics of §3.4 |
| each expression of an index | the same six |

Values among them, the common ones and the histogram bounds,
are kept in sm2's own encoding of the column's type.

Unlike an index's evaluator blob, sm3 reads and writes this
format, and it is declared in `sm3/include/sm3.h`.

- **Under keys of their own**, not in the table's definition.
  `sm_analyze` must not take the schema lock X, which every
  reader and writer of the table waits for. Statistics change
  no definition, and a reader that sees last week's histogram
  is not wrong.
- **In memory with the catalog**, read at every plan, and
  written through in the caller's transaction.
- **Gone with what they describe.** Dropping a table drops
  its statistics; dropping a column, or an index, drops that
  one's.

Heap bytes on disk beside rows times average row bytes says
how much of the table is compression, old versions and
tombstones. That ratio is an LSM's bloat, and it falls out of
what is recorded.

### 3.7 What the host derives

The fork's planner is PostgreSQL's, written for a paged heap
and a B-tree. The fork converts, in its own code, where it
used to read `pg_class` and `pg_statistic`.

| The planner wants | The host makes it from |
| --- | --- |
| `relpages` | bytes now, over 8 KB, and never less than 1 for an object with a row, so that the page cost constants keep their meaning |
| `reltuples` | recorded live rows, scaled by bytes now over bytes recorded, or moved by the rownos handed out since |
| `relallvisible` | `relpages`. An index entry carries versions of its own (`sm3/SM3_DESIGN.md` §3), so an index-only scan never visits the heap |
| null fraction, most common values, histogram | the same, values decoded to PostgreSQL's datums |
| average width | average bytes as stored |
| distinct count | the count, negated as a fraction of rows when it grows with the table, which is PostgreSQL's convention |
| correlation | correlation with `rowno`, which is the fork's `ctid` |
| a B-tree's height | nothing. The fork's index access method costs a lookup from the level count, the index's bytes and its entries, with weights to be calibrated (§6) |
| dead rows | none. Vacuum is gone (`PART4.md` §4) |

Each row of that table is a choice the host can change
without the stack changing.

### 3.8 When to analyze again

`sm_analyze` is a call, and when to make it is the host's
decision. In PostgreSQL the decision is autovacuum's, from
counts of rows changed, and autovacuum goes with vacuum
(`PART4.md` §4).

The fork's glue calls the table writes one row at a time, and
can count them where PostgreSQL's heap code did. The stack
also offers, without a counter, the two live numbers of §3.2.

Whether the stack should one day launch `sm_analyze` itself,
on the worker threads that build indexes, is left open. It
would need the stack to decide when, and nothing here does.

---

## 4. What has to be built

| Level | What | For |
| --- | --- | --- |
| sm1 | the approximate bytes of a key range in a store | §3.1 |
| sm1 | a sequence's last value, read without drawing one | §3.2, §3.3 |
| sm1 | the number of LSM levels a store has now | §3.5 |
| sm2 | the same sizes for a heap or an index, by its prefix | §3.1 |
| sm3 | on an open table: the heap's size, an index's size, and the rowno limit `N` | §3.1, §3.2 |
| sm3 | the sampler over runs of rownos | §3.3 |
| sm3 | the estimators, ported from PostgreSQL | §3.4 |
| sm3 | a catalog entry kind for statistics, outside the schema lock | §3.6 |
| sm3 | `analyze`, over those four | §3.6 |
| top | `sm_analyze`, and the calls that read, in `include/sm.h` | all |
| `rel` | an `analyze` verb and one that prints statistics | testing |
| fork | `ANALYZE` as a call to `sm_analyze`; the planner's numbers derived from sm3's; an index cost estimate; something to launch `ANALYZE` | §3.7, §3.8 |

---

## 5. What is approximate, and how far off

| Number | Source of error | Expected size |
| --- | --- | --- |
| bytes | RocksDB's estimate; compression; versions and tombstones not yet collected | unknown; §6 |
| bytes, small table | data still in the memtable, where the estimate is coarse | unknown; likely the worst case |
| row count at `sm_analyze` | sampling | well under 1% for a table that is not nearly empty |
| row count between runs | whatever the host scales it by: bytes inherit their error and count a burst of updates as growth; rownos see no delete | unknown; §6 |
| distinct count | sampling, and the likeness of neighbouring rows in a run | depends on the run length; §6 |
| other column statistics | sampling | as PostgreSQL's on a heap |
| index lookup cost | weights not yet calibrated | unknown; §6 |

A wrong size for a small table matters less than it looks.
PostgreSQL itself assumes a never-analyzed table is ten pages,
and small tables are cheap whichever plan is chosen.

---

## 6. What to measure before designing

`PART4.md` §8 and `sm3/SM3_OBJECTIVE.md` both list the accuracy
of RocksDB's approximate statistics as unverified. Four
experiments settle the strategy. Each is a small program over
sm1, with no PostgreSQL in it.

1. **Size, at rest.** Load tables of known row count and row
   width, from a thousand rows to tens of millions, compact,
   and compare the estimated bytes of the heap's range with
   the bytes written.
   → accept: within a factor of 1.5 above ten thousand rows.
2. **Size, in motion.** The same tables while rows are being
   inserted, with some data only in the memtable, and after a
   burst of updates before sm1 has collected the old versions.
   → report: how far the estimate lags and overshoots, since
   this is what the planner sees between runs.
3. **Size, by prefix.** One store holding a heap and several
   indexes of very different sizes. Compare each range's
   estimate with the truth.
   → accept: a small index beside a large heap is not charged
   the heap's bytes.
4. **Sampling cost, by run length.** Draw 30,000 rows from a
   table of a hundred million rownos by forward seeks, with
   runs of 1, 10, 100 and 1,000 rownos, with the table dense
   and with nine rows in ten deleted.
   → report: wall time and reads against a full scan, and the
   distinct count each run length gives for a column whose
   neighbours are alike, against the truth.

If experiment 1 or 3 fails, §3.1 needs a fallback: sizes that
`sm_analyze` measures itself, from its sample, and records.
That is worse, because it goes stale between runs, which is
exactly what the planner asks the storage to avoid.

The index lookup weights of §3.7 are calibrated last, against
real queries through the fork, and are not part of this.

---

## 7. To check against PostgreSQL's source

The strategy leans on these, recalled and not verified:

- the planner re-reads a relation's size at plan time and
  scales `reltuples` by the stored density;
- `ANALYZE`'s standard routine for a column uses nothing of
  the type but its equality and its ordering operator;
- the planner's selectivity code reads `pg_statistic` only
  through a few functions, so that statistics from elsewhere
  can be handed to it in the same shape;
- an index-only scan is costed from the all-visible fraction
  and nothing else of visibility;
- the B-tree cost estimate reads the tree's height from the
  index's metapage;
- the heap's write path is where the insert, update and delete
  counts are reported, and autovacuum is what launches
  `ANALYZE`.

---

## 8. Order of work

1. The four measurements of §6.
   → verify: the acceptance lines there, written up here.
2. `sm3/SM3_DESIGN.md` §6, from this file and what was
   measured, with the format of §3.6 written out.
3. The sm1 primitives of §4, with sm2 and sm3 over them.
   → verify: a table's and an index's size follow the rows
   loaded; `N` is the sequence's last value.
4. Statistics in the catalog.
   → verify: they survive a restart, and writing them blocks
   no reader or writer of the table.
5. The sampler, and the row count from it.
   → verify: against a full count, on dense and sparse tables.
6. The estimators, one statistic at a time, tests first.
   → verify: each against a full scan of generated data:
   uniform, skewed, clustered, and mostly NULL.
7. `sm_analyze` in `include/sm.h`, and the `rel` verbs.
   → verify: a `rel` script analyzes a table with a partial
   and an expression index and prints what it expects.

The fork's own work in §4 is outside this repository.

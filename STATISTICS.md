# STATISTICS — what the planner needs, and how wisr gives it

The fork keeps PostgreSQL's planner (`PART4.md` §4), and the
planner costs every plan from statistics that PostgreSQL's
storage has always supplied. This file is the strategy for
supplying the same, or near enough, from a stack that has no
pages, no heap files and no vacuum.

It is a strategy and not yet a design. `sm3/SM3_DESIGN.md` §6
is where the design goes once §6 below has been measured.

**§6 has now been measured and §7 checked**, against RocksDB
9.11.2 through sm1 and against PostgreSQL 17.11's source and a
server built from it. §6 carries the numbers, §7 the verdicts.
Two things the strategy said of PostgreSQL were wrong, and both
are corrected where they were said: how its sampler works
(§3.3) and where the planner's size conversion lives (§3.7).

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

**What is now known, and what is not**

- RocksDB's size estimates are good enough **at rest**: exact
  over a whole store, and within 8% over one rel's relid
  beside a much larger one (§6).
- They are **not** good in motion. Unflushed rows are
  estimated at their uncompressed size, which overstated a
  table by 7x until it was flushed (§6, experiment 2). That is
  the one measured result the design has to answer.
- Sampling by runs of rownos gives an exact row count on a
  dense table and one within 0.26% on a sparse one, at every
  run length tried, and costs a fraction of a scan. The run
  length decides the distinct count, over a 30x range (§6).
- What is said here of PostgreSQL is now checked against
  17.11 (§7).
- Still unknown: the weights in the index lookup cost, to be
  calibrated against real queries.

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
   update one shared row. The table-oid lock is already the
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
and its heap and each index sit under a 2-byte relid of their
own (`sm3/SM3_DESIGN.md` §4). RocksDB estimates the bytes in a
key range without reading it. Asked of a relid's range, with
the memtables counted, that is the object's size.

**What it measures:** bytes on disk, after compression, with
every version sm1 has not yet collected and every tombstone
not yet compacted away. That is the right thing to charge a
scan for, since a scan reads them.

**Except for what is still in a memtable.** A memtable holds
rows uncompressed, and the estimate adds them at that size, so
a table whose rows have not been flushed is charged what they
would occupy with no compression at all — 7x its flushed size,
in the measurement of §6. Asking for the estimate without the
memtables is worse: it answers zero for a table that is
entirely unflushed. So the number is right at rest, overstated
in proportion to the compression ratio while a load or a burst
of updates is in flight, and the design has to say which it
reports (§6).

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
out low. §6's experiment 4 measures both ends: a run of 1
costs twenty times a run of 1,000 and sees thirty times the
distinct values.

**PostgreSQL's sampler is not at the short end**, which an
earlier draft of this file had wrong. `acquire_sample_rows`
picks up to `targrows` *blocks* at random and reads **every**
tuple of each, then thins them to `targrows` rows with Vitter's
algorithm. Its own comment grants the consequence: "for large
relations the number of different blocks represented by the
sample tends to be too small". A block is 8 KB, so at a
hundred-byte row PostgreSQL's unit is some eighty neighbouring
rows — wisr's run of about that length, not a run of one. And
for a table of fewer than `targrows` blocks, 240 MB at the
default target, it reads every block there is.

So the clustering a long run brings is not a cost wisr would
be paying and PostgreSQL would not. Matching PostgreSQL means
a run of roughly the rows in 8 KB, and §6 measures what that
costs and what it does to a distinct count.

**A sparse table needs more runs than rows wanted.** The
reservoir handles a sample that comes out too big; too small
is the case a purged table gives, and §3.3 as first written
had no answer. With nine rownos in ten dead, runs of one
rowno returned 2,950 rows where 30,000 were asked for (§6).
The count of runs has to be scaled by the live fraction, which
is not known until something has been sampled: either a first
pass to find it, or runs drawn until the sample fills.

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

The routine to port is `std_typanalyze` and the
`compute_scalar_stats` it chooses, and §7 confirms it asks the
type for nothing but its equality and its ordering operator.
It chooses among three by what the type has: both operators
gives the full six statistics, equality alone drops the
histogram and the correlation, and neither leaves the null
fraction and the width. A measured shape to match: at the
default target the histogram comes back as **target + 1**
bounds, and a column whose every value is common gets a most-
common-values list and no histogram at all, while a unique one
gets a histogram and no list.

**There is more than one analyze routine.** PostgreSQL also
has `array_typanalyze`, `range_typanalyze`,
`multirange_typanalyze` and `ts_typanalyze`, which fill slots
`std_typanalyze` never does — most common elements, a
distinct-element-count histogram, and range bounds and length
histograms. A fork that ports only the standard routine costs
the planner its selectivity for the containment and overlap
operators those types are queried by, which is what a GIN or
a GiST index is usually for. Porting them is not in this
strategy, and the cost of leaving them out should be stated
rather than discovered.

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

**Size:** as §3.1, over the index's relid.

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

Heap bytes on disk beside rows times average row bytes says
how much of the table is compression, old versions and
tombstones. That ratio is an LSM's bloat, and it falls out of
what is recorded.

**Everything below follows from one asymmetry.** A stale
definition is corruption: read last week's column list after a
`DROP COLUMN` committed and a row is decoded wrong. A stale
histogram is a worse plan and nothing else. The catalog pays
for correctness with the table-oid lock and a session overlay;
statistics need neither, and most of what follows is the
saving.

**On disk: store 0, under a key kind of their own.** The
catalog is a key-value map in store 0 under the prefix `"U"`,
reached through sm2's `catkv_*` (`CATALOG.md` §1), and
statistics go there like everything else — written in the
caller's transaction, so they commit, roll back and survive a
crash with it, and sm3 writes no undo of its own.

- **A kind of their own**, not the `TABLE` blob. A table is
  one catalog entry and every DDL rewrites it, so writing
  statistics into it would make `sm_analyze` contend with DDL
  for the same key under first-committer-wins, and mutate the
  definition its readers hold. `CATALOG.md` §2's kinds run to
  7 and are frozen because they are on disk; statistics are
  kind 8, and that document has to say so.
- **One entry a table**, not one a column. `sm_analyze` writes
  every column at once and a plan reads every column at once.
  PostgreSQL's per-column granularity suits a `pg_statistic`
  it reaches through an index; this is a blob.
- **Gone with what they describe.** Dropping a table drops its
  statistics; dropping a column, or an index, drops that one's.

**In memory: beside the catalog, not in it.** A map from table
oid to an immutable `stats_t`, held by
`std::shared_ptr<const stats_t>`, loaded **lazily** on the
first plan that wants a table and never edited after it is
built. A new `sm_analyze` publishes by swapping the pointer.

This is PostgreSQL's own shape, and for its reasons. Relation-
level numbers ride in the relcache with the `pg_class` row;
column distributions sit in a *different* cache, the syscache
`STATRELATTINH`, populated per column on first use and not
loaded with the relation. The fork sets
`get_relation_stats_hook` anyway, so its statistics lookup is
already a separate path from its definition lookup.

Lazily, because the sizes differ by two orders of magnitude.
At the default target a column's statistics run about 1.2 KB —
101 histogram bounds, up to 100 common values, their
frequencies — so a twenty-column table is some 24 KB and a
thousand tables some 24 MB, against definitions that are
bytes. `CATALOG.md` §7 lists startup cost for thousands of
tables as unmeasured and open; loading statistics eagerly
would be most of it, and none of it is wanted until something
plans against that table. The definition must be there
eagerly, because the table-oid lock is taken before it is read.
Statistics are under no such rule.

**Not in the session overlay.** The overlay exists to give a
transaction its own correct view of a definition, at the cost
of a publish that cannot be made atomic with sm1's commit. A
transaction's own statistics are not worth that: see the
truncate below, where the one case that looked as though it
needed two versions turns out not to.

**Written in the transaction, published after it commits.**
The in-memory publish and the sm1 commit are two steps and
cannot be one. For a definition the table-oid lock covers the
gap, holding readers out until the publish is done. For
statistics there is nothing to cover: publish after the commit
returns, and the window where the store is new and the cache
is old is a stale read, which is allowed. If the process dies
in that window the store is right and the cache reloads.

**Never read at the planner's snapshot.** It is tempting —
sm1 would then give the two versions, the abort and the
savepoint for nothing, which is exactly how PostgreSQL's MVCC
catalog behaves. Two reasons not to.

- **Cost.** An sm1 read and a blob decode for every table of
  every plan. Avoiding that is why the catalog is in memory
  at all.
- **It would enrol statistics in SSI**, which is the serious
  one. A SERIALIZABLE transaction's read leaves a SIREAD on
  the row (§1), and `catkv_get` is `sm1::read`. Every planner
  reading a table's statistics would take one, and
  `sm_analyze` writing them would make an rw-edge to every
  planner concurrent with it: metadata reads manufacturing
  dangerous structures, and `ANALYZE` a serialization failure
  waiting to happen. `catkv_get_latest` is worse, being
  `sm1::read_latest`, which holds a lock — one lock on one key
  per plan, which is principle 5's hot key. The catalog
  already avoids this class of thing on purpose: the schema
  lock is an `sm1::LOCKTYPE_OID` lock, which writes
  nothing, so that taking it "never fails on a newer commit,
  at any level"
  (`CATALOG.md` §2). Reading statistics per plan through sm1
  would put the snapshot sensitivity back.

**What a `TRUNCATE` does, and why it needs no versioning.**
`truncate_table` makes a new empty store, writes the new
storeid into the table's record, publishes the rebuilt
definition to the session's overlay, and drops the old store
at commit (`src/catalog/ddl.cc`, `SM3_DESIGN.md` §4). A
table's oid is stable but the store holding its rows is a
field of its definition, and the definition is already
overlay-versioned. So after an uncommitted truncate the
truncating transaction resolves the table to the new, empty
store and every other to the old one, and both ask sm1 the
same question of different stores:

| asks for the heap's bytes | resolves through | gets |
| --- | --- | --- |
| the truncating transaction | its overlay | the new store, 0 |
| any other | the shared catalog | the old store, its bytes |

Nothing in the statistics path learns that a truncate
happened, and no row count is held twice, because §3.1 and
§3.2 do not store one: it is derived from whichever store the
asker's definition points at. An abort is free the same way —
`mkstore` and `rmstore` are transactional, so the old store
stays and the overlay is discarded.

The recorded distributions are keyed by oid, which a truncate
does not change, so they survive it and describe rows that are
gone. PostgreSQL does the same — measured, `pg_stats` came
through a `TRUNCATE` intact (§7) — and it is harmless there
because a live row count of nearly nothing multiplies every
selectivity into nearly nothing. The one number that does not
get that protection is **average row width**, which §3.7 hands
to the planner unscaled; after a truncate it is the old
table's. Small, and the only place the free ride stops.

**A truncated table is not an empty one.** PostgreSQL sets
`reltuples` to -1 on truncate, which is its never-analyzed
marker, and the truncating transaction then plans 2,260 rows
rather than 0 (§7). That is deliberate: a truncated table is
usually about to be refilled, and planning it empty invites a
nested loop that turns bad the moment it is. An AM callback
returning 0 tuples from a live size of 0 would be more
aggressive than PostgreSQL, so §3.7's floor on `pages` wants a
companion on `tuples`, and whoever writes
`sm3/SM3_DESIGN.md` §6 should choose it rather than inherit it.

**The cost of keeping them separate.** Statistics inside the
catalog entry would go when it went. A side structure has to
be swept on `DROP`, which is code. sm3's oids come from a
sequence and are not reused (`CATALOG.md` §1), so an entry
left behind is never mistaken for another table's — it is
wasted memory until the sweep, not a wrong answer.

### 3.7 What the host derives

The fork's planner is PostgreSQL's, written for a paged heap
and a B-tree. The fork converts, in its own code, where it
used to read `pg_class` and `pg_statistic`.

**Where the conversion goes, corrected.** This file said the
host converts "where it used to read `pg_class` and
`pg_statistic`". For a relation's size that is the wrong
place, and §7 found it: `estimate_rel_size` hands a table
straight to the table access method's `relation_estimate_size`
callback, and only an *index* is sized by the planner itself.
The fork has its own table AM, so it implements that callback
and answers `pages`, `tuples` and `allvisfrac` from sm3's
numbers directly. There is no `pg_class` row to fake and no
density to store: the callback is handed the relation and
returns the three, so the "scaled by bytes now over bytes
recorded" of the row below happens inside it, at every plan.

For column statistics the shape is firmer than "the same
shape". `examine_variable` reaches them through
`get_relation_stats_hook`, which the fork sets, but what it
must hand back is a **`pg_statistic`-shaped `HeapTuple`**: the
planner does `GETSTRUCT` on it and reads the slots with
`get_attstatsslot`. So the fork builds that tuple in memory
from sm3's statistics; it does not hand over loose numbers.

| The planner wants | The host makes it from |
| --- | --- |
| `pages` | bytes now, over 8 KB, and never less than 1 for an object with a row, so that the page cost constants keep their meaning. Returned from the AM callback, not stored |
| `tuples` | recorded live rows, scaled by bytes now over bytes recorded, or moved by the rownos handed out since. Also from the callback |
| `allvisfrac` | 1. An index entry carries versions of its own (`sm3/SM3_DESIGN.md` §3), so an index-only scan never visits the heap, and §7 measured that this is exactly what removes the heap-fetch charge |
| null fraction, most common values, histogram | the same, values decoded to PostgreSQL's datums |
| average width | average bytes as stored |
| distinct count | the count, negated as a multiplier of **total** rows, nulls included, when it grows with the table. That is PostgreSQL's convention, and §7 found the reason it exists: the row count is refreshed more often than the statistics, so a multiplier stays right as the count drifts — which is exactly what §3.2 does between runs |
| correlation | correlation with `rowno`, which is the fork's `ctid`. PostgreSQL computes its own from the sample in physical order, and relies on the sample being returned that way |
| a B-tree's height | nothing. The fork's index access method costs a lookup from the level count, the index's bytes and its entries, with weights to be calibrated (§6). What the height buys in PostgreSQL is a CPU charge, `(height + 1)` times 50 `cpu_operator_cost`, not page I/O, so the level count should buy the same kind of thing. An AM that leaves the height unknown has it guessed as `log(pages)/log(100)` |
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
| sm2 | the same sizes for a heap or an index, by its relid | §3.1 |
| sm3 | on an open table: the heap's size, an index's size, and the rowno limit `N` | §3.1, §3.2 |
| sm3 | the sampler over runs of rownos | §3.3 |
| sm3 | the estimators, ported from PostgreSQL | §3.4 |
| sm3 | catalog kind 8 for statistics, one entry a table, outside the `TABLE` blob and the table-oid lock | §3.6 |
| sm3 | the in-memory statistics map, beside the catalog: immutable, lazily loaded, published after commit | §3.6 |
| sm3 | `analyze`, over those four | §3.6 |
| top | `sm_analyze`, and the calls that read, in `include/sm.h` | all |
| `rel` | an `analyze` verb and one that prints statistics | testing |
| fork | `ANALYZE` as a call to `sm_analyze`; the planner's numbers derived from sm3's; an index cost estimate; something to launch `ANALYZE` | §3.7, §3.8 |

---

## 5. What is approximate, and how far off

| Number | Source of error | Measured |
| --- | --- | --- |
| bytes, at rest | RocksDB's estimate over a relid | none to 8%: exact over a whole store, 1.000 and 1.002 for a heap and a large index, 0.923 for a thousand-row index beside them (§6) |
| bytes, unflushed | a memtable is uncompressed, and the estimate adds it at that size | 7x over, until the rows are flushed (§6). The worst number here, and the one §3.1 now warns about |
| bytes, mid-update | new versions in the memtable, old ones not yet collected | 6.6x over at the peak of a burst that changed no row count, back to exact after a compaction (§6) |
| row count at `sm_analyze` | sampling | exact on a dense table; 0.26% low on one with nine rownos in ten dead, at every run length (§6) |
| row count between runs | whatever the host scales it by: bytes inherit their error and count a burst of updates as growth; rownos see no delete | not measured. PostgreSQL's own version of this was 3.9% low over a tenfold growth (§7) |
| distinct count | sampling, and the likeness of neighbouring rows in a run | the run length decides it, over a 30x range for a column whose neighbours are alike (§6) |
| other column statistics | sampling | as PostgreSQL's on a heap, whose sampler clusters the same way (§3.3) |
| index lookup cost | weights not yet calibrated | still unknown |

A wrong size for a small table matters less than it looks.
PostgreSQL itself assumes a never-analyzed table is ten pages,
and small tables are cheap whichever plan is chosen. Measured,
that assumption is cruder than this file implied: a
never-analyzed table of five rows is planned as **2,550**
rows, five hundred times the truth, and one `ANALYZE` brings
it to exactly five (§7). The condition is `reltuples < 0`,
which is how "never analyzed" is written down, with 0 meaning
an analyzed empty table.

---

## 6. What was measured

`PART4.md` §8 and `sm3/SM3_OBJECTIVE.md` both listed the
accuracy of RocksDB's approximate statistics as unverified.
Four experiments were run against RocksDB 9.11.2 through sm1,
with no PostgreSQL in them, as one program over sm1's own
store options: the same comparator, the same userkey bloom
filter, keys of a 2-byte relid and an 8-byte rowno.

**1. Size, at rest. Accepted.** Tables of 1,000 to 1,000,000
rows at 100 and at 1,000 bytes a row, flushed and compacted,
then the estimate over the heap's relid against the store's
bytes on disk.

| rows | width | disk | estimate | est/disk |
| --- | --- | --- | --- | --- |
| 1,000 | 100 | 27,865 | 27,865 | 1.000 |
| 1,000,000 | 100 | 26,904,808 | 26,904,808 | 1.000 |
| 1,000 | 1,000 | 87,060 | 87,060 | 1.000 |
| 1,000,000 | 1,000 | 88,558,446 | 88,558,446 | 1.000 |

Exact, at every size and both widths, against an acceptance
line of 1.5. Worth knowing why it is *so* exact and not merely
good: the range covers the whole store, so RocksDB has only to
total whole files, and none has to be interpolated. The
acceptance line was really a test of experiment 3.

**2. Size, in motion. The one bad result.** 200,000 rows of
200 bytes, and the same after updating every row three times.

| state | logical | on disk | estimate |
| --- | --- | --- | --- |
| unflushed | 44,400,000 | 0 | 48,400,000 |
| flushed and compacted | 44,400,000 | 6,756,562 | 6,756,562 |
| mid-burst, 3 updates a row | — | — | 44,664,992 |
| burst flushed and compacted | — | 6,767,319 | 6,767,319 |

Unflushed rows are counted at their uncompressed size, so the
estimate was **7.2x** the bytes those same rows occupied once
flushed. Asked without the memtables it answers 0 instead,
which is no better. A burst of updates that changed no row
count took the estimate to 6.6x and a compaction brought it
back. Once anything is flushed and compacted the estimate is
exact again. §3.1 and §5 now say so.

**3. Size, by relid. Accepted.** One store holding a heap of
500,000 rows at 400 bytes and three indexes at 16, of 500,000,
1,000 and 10 rows. The truth is each object loaded alone into
a store of its own, since logical bytes are not the truth when
each object compresses differently — the heap's payload
compressed 8.8x against the index's 1.8x, which is why a share
of logical bytes looked wrong and was not.

| object | rows | alone on disk | estimate in the store | est/alone |
| --- | --- | --- | --- | --- |
| heap | 500,000 | 23,901,543 | 23,890,172 | 1.000 |
| index, big | 500,000 | 10,353,564 | 10,370,944 | 1.002 |
| index, small | 1,000 | 21,827 | 20,153 | 0.923 |
| index, tiny | 10 | 1,462 | 1,443 | 0.987 |

A thousand-row index beside a half-million-row heap is charged
20 KB, not the heap's 24 MB, and the four estimates sum to the
store's bytes exactly. So §3.1 stands and the fallback below
is not needed.

**4. Sampling, by run length. Reported.** 30,000 rows wanted
from 10,000,000 rownos by forward seeks, dense and with nine
rownos in ten never inserted. The column whose neighbours are
alike is `rowno / 32`, so its truth is known. Ten million
rather than the hundred million first written, which changes
none of the ratios below.

Dense, 10,000,000 live, a full scan of it 6,659 ms:

| run | seeks | rows | row count | distinct seen | time |
| --- | --- | --- | --- | --- | --- |
| 1 | 29,959 | 29,959 | 10,000,000 | 28,617 | 395 ms |
| 10 | 2,999 | 29,963 | 10,000,000 | 3,837 | 53 ms |
| 100 | 300 | 29,948 | 10,000,000 | 1,223 | 24 ms |
| 1,000 | 30 | 30,000 | 10,000,000 | 966 | 19 ms |

Sparse, 1,000,000 live of 10,000,000 rownos, a full scan 674 ms:

| run | seeks | rows | row count | truth | time |
| --- | --- | --- | --- | --- | --- |
| 1 | 29,576 | 2,950 | 997,430 | 1,000,000 | 230 ms |
| 10 | 2,996 | 2,996 | 1,000,601 | 1,000,000 | 30 ms |
| 100 | 300 | 2,995 | 1,000,167 | 1,000,000 | 5 ms |
| 1,000 | 30 | 3,000 | 1,000,000 | 1,000,000 | 2 ms |

Three things come out of it.

- **The row count is the easy part.** Exact when dense, and
  0.26% low at worst when sparse, at every run length. §3.2
  needs no more than this.
- **The run length decides the distinct count**, over a 30x
  range, and nothing else here moves as much. A run of 1 is
  what a uniform sample would give; a run of 1,000 sees 966
  distinct groups where the sample holds 30,000 rows.
- **Cost is not the argument for a long run.** Even a run of
  1 cost 395 ms against a 6,659 ms full scan, seventeen times
  less. The spread from 395 ms to 19 ms is real but it is a
  fifth of a second on ten million rows. So the run length
  should be chosen for what it does to the statistics, and
  §3.3 now argues for the length that matches PostgreSQL's
  own clustering rather than the cheapest one.

And the result §3.3 did not expect: sparse with a run of 1
returned 2,950 rows where 30,000 were asked for, because nine
seeks in ten land on a dead rowno. The sampler has to draw
until the sample fills, not draw a fixed count of runs.

The index lookup weights of §3.7 are calibrated last, against
real queries through the fork, and are not part of this.

---

## 7. Checked against PostgreSQL

All six claims were checked against 17.11's source, and the
four that can be observed were also run against a server built
from it. All six hold. Two were imprecise in a way that
changes what the fork builds, and §3.3 and §3.7 are corrected.

| Recalled | Verdict |
| --- | --- |
| the planner re-reads a relation's size at plan time and scales `reltuples` by the stored density | **true, wrong place.** For a *table* `estimate_rel_size` calls the table AM's `relation_estimate_size`; the planner sizes only indexes itself. Measured: a table analyzed at 1,000 rows and grown tenfold with no `ANALYZE` kept `relpages`/`reltuples` at 18/1,000 and planned 9,611 rows against a true 10,000 — the size re-read, the density stored, 3.9% low. §3.7 corrected |
| `ANALYZE`'s standard routine for a column uses nothing of the type but its equality and its ordering operator | **true.** `std_typanalyze` picks `compute_scalar_stats` with both, `compute_distinct_stats` with equality alone, `compute_trivial_stats` with neither — which is §3.4's table of what each statistic needs. But four other analyze routines exist for arrays, ranges, multiranges and tsvectors; §3.4 now records that |
| the planner's selectivity code reads `pg_statistic` only through a few functions, so statistics from elsewhere can be handed to it in the same shape | **true, and the shape is specific.** `get_relation_stats_hook` and `get_index_stats_hook` in `utils/selfuncs.h` are the way in, and what they must return is a `pg_statistic`-shaped `HeapTuple`, read with `GETSTRUCT` and `get_attstatsslot`. §3.7 says so now |
| an index-only scan is costed from the all-visible fraction and nothing else of visibility | **true.** `cost_index` applies `pages_fetched * (1 - allvisfrac)` only `if (indexonly)`. Measured: 50,000 rows, an index-only scan cost 46.81 with `relallvisible` 0 and 32.81 once a `VACUUM` made it equal `relpages` — the whole heap-fetch charge, gone |
| the B-tree cost estimate reads the tree's height from the index's metapage | **true.** `plancat.c` calls `_bt_getrootheight` for a B-tree and leaves the height -1 otherwise. What it buys is a CPU charge, `(height + 1) * 50 * cpu_operator_cost`, not page I/O; an AM that leaves it unknown gets `log(pages)/log(100)` guessed instead |
| the heap's write path is where the insert, update and delete counts are reported, and autovacuum is what launches `ANALYZE` | **true.** `pgstat_count_heap_insert`, `_update` and `_delete` are all in `heapam.c`, and autovacuum compares `mod_since_analyze` against `analyze_base_threshold + analyze_scale_factor * reltuples` |

Four more things the measurements settled, which this file had
either loosely or not at all.

- **`reltuples` is -1 when never analyzed**, not 0, and 0
  means an analyzed empty table. The ten-page floor applies
  only in the -1 state, and it is coarse: five rows plan as
  2,550 (§5).
- **A negative `stadistinct` is a multiplier of total rows**,
  nulls included. A column 99% null with 100 distinct values
  in 10,000 rows came back as -0.00999999. The header in
  `pg_statistic.h` gives the reason, and it is wisr's reason
  too: the row count is refreshed more often than the
  statistics, so a multiplier survives the drift §3.2 creates.
- **The statistics target sizes the output as well as the
  sample.** 300 rows a unit going in; target + 1 histogram
  bounds coming out, measured at 11, 101 and 301 for targets
  of 10, 100 and 300. Most common values and a histogram are
  complementary: a column of seven values got a list of seven
  and no histogram, a unique one a histogram and no list.
- **PostgreSQL's sampler clusters**, which §3.3 had backwards
  and now states correctly.

### 7.1 An alternative this file does not take

PostgreSQL's table AM has analyze callbacks of its own,
`scan_analyze_next_block` and `scan_analyze_next_tuple`, and
`acquire_sample_rows` drives them: it chooses blocks and asks
the AM for the tuples of each. A fork could implement them
with a block standing for a run of rownos — §3.3's sampling
unit, exactly — and then PostgreSQL's own sampler, reservoir
and estimators would run unchanged, and §3.4 would have
nothing to port.

It is not free. The results would still have nowhere to go:
`update_attstats` writes `pg_statistic`, which the fork does
not have, so that half must be replaced regardless. And it
would put the sample's shape under PostgreSQL's control rather
than the stack's, against principle 2, and leave sm3 unable to
analyze anything without a host.

This file keeps the stack analyzing. The point of recording
the alternative is that the choice is real and was not made
knowingly before: whoever writes `sm3/SM3_DESIGN.md` §6 should
make it on purpose.

---

## 8. Order of work

1. ~~The four measurements of §6.~~ **Done**: both acceptance
   lines pass, and §6 carries the numbers. What they leave for
   the design: what to report for a table whose rows are still
   unflushed (§6, experiment 2), and what run length to sample
   at (§6, experiment 4, and §3.3).
2. `sm3/SM3_DESIGN.md` §6, from this file and what was
   measured, with the format of §3.6 written out, and with
   §7.1's choice made on purpose.
3. The sm1 primitives of §4, with sm2 and sm3 over them.
   → verify: a table's and an index's size follow the rows
   loaded; `N` is the sequence's last value.
4. Statistics in the catalog, and the map beside it.
   → verify: they survive a restart; writing them blocks no
   reader or writer of the table; a table's are loaded on the
   first plan that wants them and not at startup; an
   `sm_analyze` that aborts leaves the old ones in place; and
   a planner holding one is unaffected by another publishing
   the next.
5. The sampler, and the row count from it.
   → verify: against a full count, on dense and sparse tables.
6. The estimators, one statistic at a time, tests first.
   → verify: each against a full scan of generated data:
   uniform, skewed, clustered, and mostly NULL.
7. `sm_analyze` in `include/sm.h`, and the `rel` verbs.
   → verify: a `rel` script analyzes a table with a partial
   and an expression index and prints what it expects.

The fork's own work in §4 is outside this repository.

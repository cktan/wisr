# Summary: digres against PostgreSQL, MyRocks and InnoDB

Written 2026-11-07. Results 1 to 3 report throughput and latency.
[Why q3 is slow](#why-q3-is-slow-a-row-found-by-its-key) profiles
q3's instruction cost; other profiles are in
[`PARITY.md`](PARITY.md).

## Aim

Find how far digres is from PostgreSQL 18 on five simple queries,
and what the gap is made of. Each query isolates one cost:

- q0, `bench/q0-pkget.sql`: the cost of one row read by the
  primary key.
- q1, `bench/q1-sk10.sql`: the cost of a short index range (10
  entries on average).
- q2, `bench/q2-sk180.sql`: the cost of a long index range (183
  entries).
- q3, `bench/q3-skfetchrow.sql`: the cost of reading a table row
  after an index lookup.
- q4, `bench/q4-seqscan.sql`: the cost of reading every row of
  the table, with no index.

MariaDB with MyRocks (an LSM row store, as digres has) and with
InnoDB (a clustered B-tree) are a second reference.

## The queries

The table is `usertable`: 10,000 rows, `id` as primary key, and
text columns `lastname`, `firstname`, `street`, `city`, `state`,
`zip`, `phone`. There are two secondary indexes, on `lastname`
and on `state`.

**q0, `q0-pkget.sql`.** `n` is random from 1 to 10000.

```sql
select zip from usertable where id = n;
```

- Plan: a lookup in the primary key. `zip` is in no index, so the
  one matching row is read. Both digres and PostgreSQL use an
  index scan. MariaDB reads the row by its primary key.

**q1, `q1-sk10.sql`.** `n` is random from 1 to 1000.

```sql
select count(*) from usertable
 where lastname = 'Last' || n;
```

- Plan: an index-only scan of the `lastname` index, then a count.
  There are 10 matching entries on average, ranging from 2 to
  24 across the 1,000 last names. No table row is read.

**q2, `q2-sk180.sql`.**

```sql
select count(*) from usertable where state = 'CA';
```

- Plan: an index-only scan of the `state` index, then a count.
  183 entries match. No table row is read.

**q3, `q3-skfetchrow.sql`.** `n` is random from 1 to 1000.

```sql
select phone from usertable
 where lastname = 'Last' || n;
```

- Plan: `phone` is in no index, so each matching row is read
  from the table: 10 rows on average. digres uses an index
  scan. PostgreSQL uses a bitmap heap scan.

**q4, `q4-seqscan.sql`.**

```sql
select sum(zip::int) from usertable;
```

- Plan: a sequential scan of all 10,000 rows, then a sum. No
  index is used. `zip` is text of digits, so the cast makes it a
  number. PostgreSQL does not use parallel workers.
- MariaDB runs `sum(cast(zip as signed))`. All four systems
  reportedly return the same sum. The dataset gives 501,892,887.

## Method

- One client, prepared statements, 10 seconds a query.
- Server on CPU 2, client on CPU 3. Cache of 2 GB on each system.
- digres and PostgreSQL: `pgbench`, release build of digres from
  `main` 7f381103, run by `bench/run.sh`. The numbers are from
  its last run, `bench/build/mainrun/run5.out`.
- MariaDB 11.8.9: `sysbench`, because `pgbench` cannot connect.
  Its numbers are indicative only.
- Each cell is one run, except the q0 and q4 means below. The
  two q0 runs differ by about 9% of their mean. This does not
  establish a noise bound for these queries.
- digres is tested with two index kinds: regular (an LSM index,
  its default) and btree (a tree). The aim of the work was the
  btree. q0 uses only the primary key (an LSM index) and q4 uses
  no index, so for them the index kind does not matter.

The cited throughput log and raw profiles were not available
locally for the 2026-11-09 review. Arithmetic and the dataset
were checked; the measurements were not reproduced.

## Result 1: digres against PostgreSQL

Latency of digres divided by latency of PostgreSQL. A value of
1.00 is parity. Smaller is better. For one client, the ratios
below use PostgreSQL tps divided by digres tps. This avoids
dividing latencies rounded to 0.001 ms by `pgbench`. For q0 and
q4, these are ratios of mean throughput, not mean latency.

| query | regular index | btree index |
| --- | --: | --: |
| q1 | 1.47 | 1.17 |
| q2 | 2.93 | 1.28 |
| q3 | 2.38 | 1.74 |

Transactions a second:

| query | index | digres | PostgreSQL |
| --- | --- | --: | --: |
| q1 | regular | 41,002 | 60,115 |
| q1 | btree | 52,130 | 61,026 |
| q2 | regular | 14,758 | 43,202 |
| q2 | btree | 32,207 | 41,100 |
| q3 | regular | 20,676 | 49,171 |
| q3 | btree | 24,856 | 43,271 |

Queries that use no secondary index (the index kind is
irrelevant). Each is the mean of two runs of the same code, with
one run on a regular and one on a btree database:

| query | digres tps | PostgreSQL tps | latency ratio |
| --- | --: | --: | --: |
| q0 | 49,983 | 66,502 | 1.33 |
| q4 | 529 | 1,419 | 2.68 |

- The two q0 runs give 52,206 and 47,759 digres tps. Their
  difference is 8.9% of their mean, 49,982.5 tps.
- digres reaches about 50,000 primary key lookups a second,
  at 1.33 times PostgreSQL's latency.
- `bench/run.sh` now runs q0 and q4 once, not once for each index
  kind.
- **q4 is slower on digres in these measurements.**
  PostgreSQL's heap scan reads 8 KB pages in order, and steps
  from one tuple to the next inside a page. An LSM scan merges
  several sorted runs, the memtable and the files of each level,
  and compares keys to find the next one. It also decodes keys
  from blocks. These costs are plausible causes, but q4 needs
  a profile to establish their shares. Result 3 shows MyRocks,
  also an LSM row store, is 0.27 of PostgreSQL's throughput on
  q4, and digres is 0.37. InnoDB, a clustered B-tree, is 0.51.
  Parity on q4 is not a target: an OLTP workload runs few
  sequential scans
  ([`sm/NEXT.md`](sm/NEXT.md#1-next) §1, item 3).

## Result 2: effect of the work on the btree index

Compared with `main` at 20f43e31, the start of the session.
q0 and q4 were not measured then.

| query | tps before | tps now | throughput gain | latency ratio now |
| --- | --: | --: | --: | --: |
| q1 | 47,924 | 52,130 | 8.8% | 1.17 |
| q2 | 24,267 | 32,207 | 32.7% | 1.28 |
| q3 | 24,347 | 24,856 | 2.1% | 1.74 |

- q2 shows the largest throughput gain. The instruction counts
  in `PARITY.md` also show a reduction in its cost.
- q1 and q3 need repeated runs to distinguish an improvement
  from variation between runs.
- The earlier latency ratios were 1.24, 1.78 and 1.95. They
  used rounded latencies. The earlier PostgreSQL tps are not
  recorded here, so those ratios cannot be corrected here.
- Parked table handles, done earlier, made the fixed cost of a
  statement equal to PostgreSQL's. A query that reads no row, on
  an index that matches nothing, is at parity.

## Result 3: MariaDB engines

Transactions a second, with the btree index of digres and
PostgreSQL from Result 1. MariaDB ran at another time and with
another client. Both the absolute numbers and the ratios are
indicative: dividing by PostgreSQL tps does not remove client
or run differences.

| system | q0 | q1 | q2 | q3 | q4 |
| --- | --: | --: | --: | --: | --: |
| PostgreSQL 18 | 66,502 | 61,026 | 41,100 | 43,271 | 1,419 |
| digres | 49,983 | 52,130 | 32,207 | 24,856 | 529 |
| InnoDB | 52,468 | 35,227 | 21,191 | 30,899 | 730 |
| MyRocks | 38,026 | 19,550 | 5,545 | 18,738 | 377 |

Throughput of each system relative to PostgreSQL:

| system | q0 | q1 | q2 | q3 | q4 |
| --- | --: | --: | --: | --: | --: |
| digres | 0.75 | 0.85 | 0.78 | 0.57 | 0.37 |
| InnoDB | 0.79 | 0.58 | 0.52 | 0.71 | 0.51 |
| MyRocks | 0.57 | 0.32 | 0.13 | 0.43 | 0.27 |

- In these runs, digres is faster than both MariaDB engines on
  q1 and q2.
- InnoDB is faster than digres on q0, q3 and q4. Its rows sit in
  the leaves of a clustered B-tree.
- MyRocks is the slowest on every query. It is also an LSM row
  store, and digres is 1.4 times faster than it on q4.

## Why q3 is slow: a row found by its key

The reported q3 instruction ratio is 178,000 / 89,100, or
2.00. Instruction counts and latency ratios measure different
costs; they need not agree.

- **Instructions a statement**: `perf stat` on the server, the
  slope between pgbench runs of 5,000 and 25,000 prepared
  statements, server and client pinned. This method aims to
  cancel connection and initial plan costs.
- **Instructions a function**: callgrind self costs. The
  original account says dumps followed 300 statements and
  "1,300 more," but divides their difference by 1,000. That
  divisor requires cumulative counts of 300 and 1,300. Raw
  dumps are needed to resolve this ambiguity and verify the
  reported agreement with `perf stat` within 5%.

digres is `digres-release` at `81aaf873`, PostgreSQL is 18.6,
both on the data of `bench/load.sql` with btree indexes, on
aarch64 under WSL2, 2026-11-07.

### 1. Getting the keys, or reading the rows?

q1 counts matching index entries; q3 returns their rows' phone
values. The reported instruction counts are:

| | digres | PostgreSQL |
| --- | --: | --: |
| q1: count matching entries | 66,100 | 62,100 |
| q3: read and return matching rows | 178,000 | 89,100 |
| q3 less q1 | 111,900 | 27,000 |

q1 is near parity in instructions, at 1.06 times PostgreSQL.
The q3 increment is 4.14 times PostgreSQL's. It includes row
reads, but also changes in aggregation, output and scan plans.
It is not an isolated measurement of row fetching.

The original profile assumed eight matching rows. The supplied
random queries instead match ten rows on average. If profiling
used those queries, the increments per expected row are 11,190
and 2,700 instructions. A fixed key could match eight rows, but
no such key is documented. Actual row counts are needed before
using these estimates as measured costs per row.

### 2. A row read, part by part

The following historical breakdown divides callgrind's q3 less
q1 counts by eight. It is retained for traceability, not as a
verified cost per row. If the profile averaged ten rows, each
instruction count would be 20% lower; the shares would stay
the same.

| Part | Instructions | Share |
| --- | --: | --: |
| RocksDB's `MultiGet`: the filter, the index block's and the data block's searches | 7,320 | 52% |
| sm1's key comparator, which RocksDB calls at each comparison | 1,100 | 8% |
| the row's decode (sm2 `tup_read`, sm3 `pin_deform`) | 1,240 | 9% |
| `malloc`, `free` and `new` | 1,080 | 8% |
| `memcpy` and `memcmp` | 910 | 6% |
| sm1's and sm2's read wrappers (`read_chained_batch`, pins) | 675 | 5% |
| the recheck: the entry's key made again from the row (`still_has`) | 675 | 5% |
| other sm and `smstore.c` code | 550 | 4% |
| the row sent to the client | 340 | 2% |
| PostgreSQL's executor | 240 | 2% |

The entries sum to 14,130 instructions. Rounded shares sum to
101%. RocksDB and the comparator total 8,420, or 59.6% of this
breakdown. The total is about 1% above 111,900 / 8; these are
separate reported measurements, not an exact reconciliation.

### 3. One row by its key: q0

q0 reads one row by its primary key. The lookup paths differ:

1. PostgreSQL walks its primary key's btree to the leaf entry,
   and takes the entry's TID: the row's address, a heap page and
   a slot in it.
2. It then follows the TID to the heap page, and reads the row
   there: the **dereference**.
3. digres searches RocksDB once, by the key. The row is what the
   search finds: there is no second step.

Yet digres's one search costs more than PostgreSQL's two steps.
From callgrind, PostgreSQL's by inclusive costs, which tail calls
on aarch64 make approximate:

| One row by its key | Instructions |
| --- | --: |
| PostgreSQL: the walk of the btree (`_bt_first` and its glue) | about 6,200 |
| PostgreSQL: the dereference (`index_fetch_heap`) | about 950 |
| PostgreSQL: the whole read (`index_getnext_slot`) | about 7,200 |
| digres: RocksDB's `Get` | 6,160 |
| digres: sm1's comparator, in that `Get` | 760 |
| digres: the decode, the wrappers, their copies and allocations | about 2,000 |
| digres: the whole read | about 9,000 |

q0 still ends near parity, 42,700 against 40,800 instructions a
statement, because the read is under a quarter of either system's
statement. Other statement costs dominate both totals.

A search of RocksDB, for one key that is in the cache, does these
steps:

1. Check the prefix bloom filter.
2. Look the index block up in the block cache: a hash, and a
   mutex of the cache's shard.
3. Search the index block for the data block: a binary search
   over restart points, then a walk.
4. Look the data block up in the block cache, as in the
   index block lookup.
5. Search the data block for the key, using restart points.

Key comparisons in the block searches call sm1's comparator, a
virtual call that decodes a versioned key, not a `memcmp`.
RocksDB's settings for this are already chosen for reads: blocks
of 16 KB, a restart interval of 4, a hash index inside each data
block, and a prefix bloom filter
([`PARITY.md`](PARITY.md#4-prognosis) §4).

### 4. Why q3 multiplies the difference

In q0 both systems locate a row by its primary key. In q3:

- **A PostgreSQL index entry holds a TID**, the heap address.
  The bitmap heap scan collects TIDs and visits heap pages.
  It does not search the primary key index for each row.
- **A digres index entry holds the row's primary key**. The
  row is found by that key in RocksDB
  ([`sm/CLUSTER.md`](sm/CLUSTER.md#6-digres) §6).

This difference is a plausible source of the gap. However,
q0's 950-instruction `index_fetch_heap` cost is not a measured
cost for q3's bitmap heap scan. Multiplying it by q3's row
count does not establish PostgreSQL's q3 fetch cost.

InnoDB's secondary indexes also hold primary keys. Its q3
throughput is 0.71 of PostgreSQL's, against digres's 0.57.
These results are consistent with a cost for row lookup, but
they do not isolate that cost or confirm the cause of the gap.

### 5. What parity requires

The reported q3 totals differ by 88,900 instructions. Reaching
1.5 times PostgreSQL's instruction count would require saving
44,350 instructions from digres's 178,000.

The historical breakdown assigns 8 × 8,420 = 67,360 instructions
per statement to RocksDB and the comparator. Even removing
that cost entirely leaves 110,640 instructions, or 1.24 times
PostgreSQL. Search improvements alone therefore cannot reach
instruction parity under this model. A reduction of about 66%
in that search cost would reach 1.5 times, with other costs
unchanged. These are arithmetic scenarios, not measured gains.

The 9,000-instruction whole read belongs to q0, not q3. q3's
historical breakdown totals 14,130 per assumed row. Neither
supports a claim that a ninefold search improvement is required
or sufficient for parity. The profiles also do not prove that
faster searches are impossible. Confirm the profile interval,
row counts and bitmap heap scan costs before choosing a design.

## Conclusion

- On index-only queries with a btree index, digres is about
  17% (q1) and 28% (q2) above PostgreSQL's latency, using the
  throughput ratios. These queries were the target of the work.
- A primary key lookup (q0) has a ratio of 1.33. A secondary
  index lookup returning ten rows on average (q3) has a ratio
  of 1.74. A scan of all rows (q4) has a ratio of 2.68.
- q3's profile suggests a substantial cost for finding rows by
  key in RocksDB. Exact costs per row and comparisons with
  PostgreSQL's bitmap heap scan remain unverified.
- Regular (LSM) indexes have latency ratios of 1.47 to 2.93.
  The work did not change them.
- Repeat each measurement at least 3 to 5 times and report the
  median and spread before relying on small differences. The
  current runs do not establish a statistical noise bound.

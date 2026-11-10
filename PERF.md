# Summary: digres against PostgreSQL, MyRocks and InnoDB

Written 2026-11-07. Results 1 to 3 report throughput and latency.
[Why q3 is slow](#why-q3-is-slow-a-row-found-by-its-key) profiles
the query furthest from PostgreSQL; other profiles are in
[`PARITY.md`](PARITY.md).

## Aim

Find how far digres is from PostgreSQL 18 on five simple queries,
and what the gap is made of. Each query isolates one cost:

- q0, `bench/q0-pkget.sql`: the cost of one row read by the
  primary key.
- q1, `bench/q1-sk10.sql`: the cost of a short index range (8
  entries).
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
  About 8 entries match. No table row is read.

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

- Plan: `phone` is in no index, so each of the 8 matching rows is
  read from the table. digres uses an index scan. PostgreSQL uses
  a bitmap heap scan.

**q4, `q4-seqscan.sql`.**

```sql
select sum(zip::int) from usertable;
```

- Plan: a sequential scan of all 10,000 rows, then a sum. No
  index is used. `zip` is text of digits, so the cast makes it a
  number. PostgreSQL does not use parallel workers.
- MariaDB runs `sum(cast(zip as signed))`. All four systems
  return the same sum.

## Method

- One client, prepared statements, 10 seconds a query.
- Server on CPU 2, client on CPU 3. Cache of 2 GB on each system.
- digres and PostgreSQL: `pgbench`, release build of digres from
  `main` 7f381103, run by `bench/run.sh`. The numbers are from
  its last run, `bench/build/mainrun/run5.out`.
- MariaDB 11.8.9: `sysbench`, because `pgbench` cannot connect.
  Its numbers are indicative only.
- Each cell is one run. Noise is about 5% for one run, and up to
  9% between two runs of the same code (see q0 below).
- digres is tested with two index kinds: regular (an LSM index,
  its default) and btree (a tree). The aim of the work was the
  btree. q0 uses only the primary key (an LSM index) and q4 uses
  no index, so for them the index kind does not matter.

## Result 1: digres against PostgreSQL

Latency of digres divided by latency of PostgreSQL. A value of
1.00 is parity. Smaller is better.

| query | regular index | btree index |
| --- | --: | --: |
| q1 | 1.41 | 1.19 |
| q2 | 2.96 | 1.29 |
| q3 | 2.40 | 1.74 |

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

- The two runs of q0 differ by 9% (52,206 and 47,759 digres, one
  run each). Read 9% as the noise of a single run.
- A primary key lookup is the cheapest query: digres reaches
  about 50,000 a second, 1.33 times PostgreSQL's latency.
- `bench/run.sh` now runs q0 and q4 once, not once for each index
  kind.
- **q4 is a sequential scan, and an LSM store is known to be
  slower than a heap file of pages at a sequential scan.**
  PostgreSQL's heap scan reads 8 KB pages in order, and steps
  from one tuple to the next inside a page. An LSM scan merges
  several sorted runs, the memtable and the files of each level,
  and compares keys to find the next one. It also decodes each
  key from its block. Result 3 shows the same order: MyRocks,
  also an LSM row store, is 0.27 of PostgreSQL's throughput on
  q4, and digres is 0.37. InnoDB, a clustered B-tree, is 0.51.
  Parity on q4 is not a target: an OLTP workload runs few
  sequential scans
  ([`sm/NEXT.md`](sm/NEXT.md#1-next) §1, item 3).

## Result 2: effect of the work on the btree index

Compared with `main` at 20f43e31, the start of the session.
q0 and q4 were not measured then.

| query | tps before | tps now | latency ratio before | latency ratio now |
| --- | --: | --: | --: | --: |
| q1 | 47,924 | 52,130 | 1.24 | 1.19 |
| q2 | 24,267 | 32,207 | 1.78 | 1.29 |
| q3 | 24,347 | 24,856 | 1.95 | 1.74 |

- q2 is 33% faster, and its ratio fell from 1.78 to 1.29. The
  change is above the noise.
- q1 is 9% faster, but PostgreSQL also moved by 6% between the
  runs. The ratio change, 1.24 to 1.19, is within the noise.
- q3 did not change in tps. Its ratio moved because PostgreSQL
  moved by up to 10% between runs.
- Parked table handles, done earlier, made the fixed cost of a
  statement equal to PostgreSQL's. A query that reads no row, on
  an index that matches nothing, is at parity.

## Result 3: MariaDB engines

Transactions a second, with the btree index of digres and
PostgreSQL from Result 1. MariaDB ran at another time and with
another client, so compare the ratios, not single numbers.

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

- digres is faster than both MariaDB engines on q1 and q2.
- InnoDB is faster than digres on q0, q3 and q4. Its rows sit in
  the leaves of a clustered B-tree.
- MyRocks is the slowest on every query. It is also an LSM row
  store, and digres is 1.4 times faster than it on q4.

## Why q3 is slow: a row found by its key

q3 reads 8 rows through the `lastname` index, and it is the query
furthest from PostgreSQL: 2.00 times its instructions a
statement. This section asks where that cost is. It counts
instructions, not time, as a count does not move with the
machine's clock:

- **Instructions a statement**: `perf stat` on the server, the
  slope between pgbench runs of 5,000 and 25,000 prepared
  statements, server and client pinned. The connection and the
  first plan cancel.
- **Instructions a function**: callgrind on the same servers,
  counts dumped after 300 statements and after 1,300 more, self
  costs per function, the difference over 1,000. The totals
  match `perf stat`'s within 5%.

digres is `digres-release` at `81aaf873`, PostgreSQL is 18.6,
both on the data of `bench/load.sql` with btree indexes, on
aarch64 under WSL2, 2026-11-07.

### 1. Getting the keys, or reading the rows?

q3 does what q1 does, then more. q1 finds the 8 entries of one
last name and counts them; q3 finds the same 8 entries, reads the
row each names, and returns its `phone`. So q3 less q1 is the
cost of the 8 rows:

| | digres | PostgreSQL |
| --- | --: | --: |
| q1: find the 8 entries, count them | 66,100 | 62,100 |
| q3: find the 8 entries, read and return their rows | 178,000 | 89,100 |
| q3 less q1: the 8 rows | 111,900 | 27,000 |
| one row | 14,000 | 3,400 |

Finding the entries is near parity, 1.06 times. Reading a row
costs digres about 4 times what it costs PostgreSQL. The gap is
in the rows.

### 2. A row read, part by part

What one of q3's rows costs digres, from callgrind, q3 less q1
over 8:

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

Six tenths of a row is the search for its key in RocksDB.

### 3. One row by its key: q0

q0 reads one row by its primary key. Here PostgreSQL does more
work than digres:

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
statement. The rest is the executor around it, which is
PostgreSQL's in both.

A search of RocksDB, for one key that is in the cache, does these
steps:

1. Check the prefix bloom filter.
2. Look the index block up in the block cache: a hash, and a
   mutex of the cache's shard.
3. Search the index block for the data block: a binary search
   over restart points, then a walk.
4. Look the data block up in the block cache, as in step 2.
5. Search the data block for the key, as in step 3.

Every comparison in steps 3 and 5 calls sm1's comparator, a
virtual call that decodes a versioned key, not a `memcmp`.
RocksDB's settings for this are already chosen for reads: blocks
of 16 KB, a restart interval of 4, a hash index inside each data
block, and a prefix bloom filter
([`PARITY.md`](PARITY.md#4-prognosis) §4).

### 4. Why q3 multiplies the difference

In q0 both systems pay one full read by key. In q3 they do not:

- **A PostgreSQL index entry holds a TID.** Each of q3's rows is
  a dereference alone, about 950 instructions. PostgreSQL never
  walks its primary key's btree in q3.
- **A digres index entry holds the row's primary key**, as a row
  in sm has no address: it is found by its key
  ([`sm/CLUSTER.md`](sm/CLUSTER.md#6-digres) §6). Each of q3's
  rows is a full search of RocksDB, about 8,400 instructions
  under `MultiGet` with the comparator.

So q3 pays digres's most expensive operation 8 times, where
PostgreSQL pays its cheapest. A worked count of the 8 rows' reads
alone: 8 × 8,400 = 67,200 instructions for digres, 8 × 950 =
7,600 for PostgreSQL.

InnoDB confirms the cause. Its secondary indexes also hold the
primary key, and each row is a walk of its clustered btree, the
structure of digres. On q3 it runs 0.71 of PostgreSQL's
throughput, and digres 0.57 ([Result
3](#result-3-mariadb-engines)): the same structure, with a
cheaper search for the key.

### 5. What parity requires

For q3 to cost what PostgreSQL's does, a row read from an index
entry must cost about what PostgreSQL's dereference costs: about
1,000 instructions. digres's search for a key costs about 8,400
under `MultiGet`, and 9,000 with sm's work around it. So the read
must become about 9 times cheaper than digres's search today, and
about 7 times cheaper than PostgreSQL's own read by key in q0.

The rest of a row's cost does not close the gap alone. Outside
RocksDB, a row costs digres about 5,700 instructions, and
PostgreSQL about 2,450: its row's 3,400 less its dereference. Cut
to PostgreSQL's, digres's rows would save about 26,000
instructions, and q3 would be about 1.7 times PostgreSQL: still
over the goal of 1.5. The search for the key is the gap.

A faster search does not reach it either. RocksDB's steps of §3
are what any search of a sorted, compressed store does, and its
settings are tuned. What PostgreSQL has, and digres does not, is
an index entry that leads to its row with no search at all.

## Conclusion

- On index-only queries with a btree index, digres is 19% (q1)
  and 29% (q2) above PostgreSQL's latency. These queries were
  the target of the work.
- A single row by primary key (q0) is 1.33 times PostgreSQL's
  latency. Reading 8 rows through an index (q3) is 1.74. Reading
  all rows (q4) is 2.7. The cost of a row is the main gap, and it
  grows with the number of rows read.
- q3's gap is the read of each row. A digres index entry holds
  the row's primary key, so each row is a full search of RocksDB,
  about 8,400 instructions; a PostgreSQL entry holds the row's
  address, and each row is a dereference, about 950
  ([Why q3 is slow](#why-q3-is-slow-a-row-found-by-its-key)).
- Regular (LSM) indexes are 1.4 to 2.9 times PostgreSQL's
  latency, and the work did not touch them.
- The ratios in Result 1 come from one run each. Repeat each
  cell 3 to 5 times and take the median before a decision that
  depends on a difference under 5%.

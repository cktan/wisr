# Summary: digres against PostgreSQL, MyRocks and InnoDB

Written 2026-11-07. This file reports throughput and latency
only. Profiles are in `PARITY.md`.

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

## Conclusion

- On index-only queries with a btree index, digres is 19% (q1)
  and 29% (q2) above PostgreSQL's latency. These queries were
  the target of the work.
- A single row by primary key (q0) is 1.33 times PostgreSQL's
  latency. Reading 8 rows through an index (q3) is 1.74. Reading
  all rows (q4) is 2.7. The cost of a row is the main gap, and it
  grows with the number of rows read.
- Regular (LSM) indexes are 1.4 to 2.9 times PostgreSQL's
  latency, and the work did not touch them.
- The ratios in Result 1 come from one run each. Repeat each
  cell 3 to 5 times and take the median before a decision that
  depends on a difference under 5%.

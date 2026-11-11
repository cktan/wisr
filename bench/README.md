# bench: the benchmarks of digres

This directory holds the benchmarks of digres. Most compare
digres with PostgreSQL 18 on the same machine. Each script
starts with a comment that tells how to run it and what its
environment variables are.

## 1. Rules for every run

- Use the optimized build, `dg/build/digres-release`
  (`make -C dg release -j4`). `run.sh` refuses a debug build.
- Use the optimized PostgreSQL 18.6 in `bench/build/pg18`, and
  PostgreSQL 18's `pgbench` and `psql` (the top
  [`README.md`](../README.md) tells how to build them).
- Data directories, instances and logs go under `bench/build/`,
  not `/tmp`. The scripts do this.

## 2. Query throughput: run.sh

`run.sh` is the main benchmark. It times five queries with
`pgbench`: prepared statements, one client, `Q_SECONDS` (10)
for each. It prints a table of digres against PostgreSQL.

```
DG_PORT=5432 PG_PORT=5433 bench/run.sh
```

Both servers must run before you start it. The run does these
steps on each server:

1. Load the data again: `load.sql`.
2. Run q0 and q4, which use no secondary index.
3. Make the regular indexes: `mkregidx.sql`.
4. Run q1 to q3.
5. Make the B-tree indexes: `mkbtidx.sql`.
6. Run q1 to q3 again.

| File | What it measures |
|---|---|
| `q0-pkget.sql` | one row by the primary key |
| `q1-sk10.sql` | a count by `lastname`: about 10 rows |
| `q2-sk180.sql` | a count by `state = 'CA'`: about 200 rows |
| `q3-skfetchrow.sql` | `phone` by `lastname`: an index, then each row |
| `q4-seqscan.sql` | a sum over the whole table: a sequential scan |

The data is `usertable.csv.gz`, 10,000 users. `gen.py` writes
it, the same each time. The results of past runs on GCP are in
`results-gcp/`.

## 3. Cost of one code path

These scripts start their own servers. Most count
instructions, so the result does not change with the load on
the machine.

| Script | What it measures |
|---|---|
| `sqbench.sh` | the parse of a large `SELECT ... AND false`: parser and planner only |
| `sortload.sh` | a `COPY` of 5,000,000 rows into a table with three indexes; the sort spills (`KIND=lsm` or `btree`) |
| `idxopen.sh` | what 0, 1, 4 or 16 secondary indexes cost a statement that uses none of them |
| `privcheck.sh` | what privilege checks cost a role that is not a superuser |

## 4. Long runs and memory leaks

| File | What it does |
|---|---|
| `soak.sh dg\|pg` | the school workload for 4 hours; samples memory and checks the data ([`SOAK.md`](SOAK.md), scripts in `soak/`) |
| `leakupd.sh` | asks if `UPDATE` grows the memory of digres, on a table of 1000 rows |
| `leakrel.rel` | the same question through `sm.h` alone, with `rel` |
| `leakrel2.rel` | the soak's updates through `sm.h` alone, with two tree indexes |

[`LEAK.md`](LEAK.md) tells how to find a leak: reduce the
workload to `rel`, then run it under valgrind.

## 5. Benchmarks in other directories

- `sm/sm1/bench/locktab_bench.cc`: the lock table of sm1 alone,
  with threads, and no session or wait. `make -C sm/sm1 -j4`
  builds it as `sm/sm1/build/bench/locktab_bench`.
- `bench.old/`: an earlier set of benchmarks, with its own
  [`README.md`](../bench.old/README.md).

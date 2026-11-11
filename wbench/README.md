# wbench: write performance of digres against PostgreSQL 18

This directory holds a write benchmark. It compares digres with
PostgreSQL 18 on inserts and updates, with each commit synced
to disk on both. The read benchmarks are in
[`../bench/`](../bench/README.md).

## 1. How to run it

```
make -C dg release -j4
PGBIN=$HOME/pg18/bin wbench/run.sh
```

`run.sh` starts both servers itself, with their data under
`wbench/build/run`. It refuses a build with assertions. The
script's header comment lists its environment variables.

## 2. The workloads

The table `t` has `id`, an identity primary key, and `c1` to
`c16`, of type `int8`.

| Workload | Script | What it measures |
|---|---|---|
| `ins1` | `ins1.sql` | one row a transaction, with 0, 1, 4 or 16 secondary indexes |
| `ins1k` | `ins1k.sql` | 1000 rows a transaction: the cost of a row, not of a commit |
| `upd` | `upd.sql` | an update of a column with no index, by primary key, in 100,000 rows |
| `uniq` | `ins1.sql` | one row a transaction, with one unique secondary index |

- digres runs each workload with secondary indexes twice: with
  LSM indexes, its default, and with B-tree indexes.
  PostgreSQL has B-tree indexes only.
- Each run uses 1 client and then 16 clients.
- The table is made again before each run.

## 3. What it reports

- **tps**: transactions a second, from pgbench.
- **p50 ms, p99 ms**: the latency of a transaction, from the
  log of each transaction.
- **KB/txn**: the bytes written to the device of the data,
  divided by the transactions. It includes the WAL, page
  writes, flushes and compactions in the run.

## 4. Durability

Each commit is synced to disk on both systems:

- PostgreSQL: `fsync`, `synchronous_commit` and
  `full_page_writes` are on.
- digres: sm syncs its RocksDB WAL at each commit
  (`durability_sync`, the default in
  `sm/include/sm_options.h`). digres has no setting to turn it
  off.

## 5. Limits

- A run is short (30 seconds by default). PostgreSQL writes
  most heap pages at a checkpoint, every 5 minutes by default,
  so a short run counts its WAL but few of its page writes.
  digres flushes and compacts when its memtables fill, so a
  short run can count those. Set `W_SECONDS` higher to compare
  bytes.
- `CHECKPOINT` before a run writes PostgreSQL's dirty pages. In
  digres it does not flush the memtables, so the setup's writes
  can be counted in the run.
- Another process that writes to the same device adds to the
  count of bytes.
- Locally, the disk is a WSL virtual disk. Its sync cost is not
  that of a real server disk.

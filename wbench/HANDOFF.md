# wbench handoff: run the write benchmark on the GCP machine

This file is for an AI agent that continues the work on the
GCP machine. Read the repository's `CLAUDE.md` first, then
[`README.md`](README.md) in this directory, then `run.sh`.

## 1. Context

- `wbench/` compares digres with PostgreSQL 18 on writes, with
  each commit synced to disk on both. It is on branch
  `wip-wbench`, not merged into `main`.
- It was tested only on a WSL laptop, with 3-second runs. Those
  numbers show that the harness works. They are not results.
- The goal now: a full run on the GCP machine, and a results
  file in `wbench/results-gcp/`, in the form of
  `bench/results-gcp/20261109-1920.md`.
- The GCP machine of earlier runs was a Neoverse-V2 (ARM),
  4 cores, 15 GB. Verify what it is now.

## 2. Steps

1. Get the branch: `git fetch origin && git checkout
   wip-wbench && git pull --ff-only`.
2. Build the release digres: `make -C dg release -j4`. Then run
   `dg/build/digres-release --build`. Every line must say
   `-DNDEBUG`.
3. Find PostgreSQL 18's bin: it must have `postgres`, `initdb`,
   `psql` and `pgbench`. `run.sh` uses
   `~/p/postgres18/install/bin` by default; set `PGBIN` if it is
   elsewhere. Prefer the optimized build of the top `README.md`
   (`bench/build/pg18`, 18.6, no `--enable-cassert`). Record
   which one you use.
4. Measure the disk's sync cost:
   `$PGBIN/pg_test_fsync -f wbench/build/fsync.test`. The
   1-client `ins1` tps of both systems should be near its
   fdatasync rate. If digres is much faster, it does not sync:
   stop and report.
5. Run a smoke test, to find setup problems in about 3 minutes:
   `W_SECONDS=3 CLIENTS=1 COUNTS="0 1" wbench/run.sh`.
6. Run the full benchmark (about 20 minutes): `wbench/run.sh >
   wbench/build/full.log 2>&1`. Read the table at the end of
   the log.
7. Run the long one, for bytes written (about 3 hours):
   `W_SECONDS=300 wbench/run.sh > wbench/build/long.log 2>&1`.
   A run of 300 seconds includes PostgreSQL's checkpoint, every
   5 minutes, so KB/txn compares writes fairly.
   - Copy `wbench/build/run/results` after each run: `run.sh`
     deletes `wbench/build/run` when it starts.
8. Write the results file
   `wbench/results-gcp/<yyyymmdd-hhmm>.md`, with the content of
   "What the results file says" below.
9. Commit it on `wip-wbench` and push. Do not merge into `main`
   until the user asks.

## 3. What the results file says

- The machine: CPU, cores, memory, kernel, and disk type (for
  example pd-ssd or a local NVMe).
- The digres commit and its `--build` output; the PostgreSQL
  version and where its build came from.
- The `pg_test_fsync` rate for fdatasync.
- The table of each run: the full run and the long run.
- A short analysis, one point a workload:
  - `ins1`, 0 indexes: commit cost. Both should be near the
    sync rate at 1 client. At 16 clients, the group commit
    of each system shows.
  - `ins1`, 1 to 16 indexes: digres's LSM indexes against
    B-tree indexes on both systems. This is where an LSM should
    gain.
  - `ins1k`: the cost of a row, with no commit in it.
  - `upd`: PostgreSQL's HOT update against an sm put.
  - `uniq`: an LSM must read to check uniqueness, so its gain
    should be less than in `ins1` with 1 index.
- Each finding that looks wrong, with the log that shows it.

## 4. Things to watch

- **16 clients on 4 cores:** pgbench uses at most 4 threads,
  and shares the CPUs with the server. Say so in the results.
- **Device count:** KB/txn is read from
  `/sys/class/block/<dev>/stat` of the device under
  `wbench/build`. Another process that writes to that device
  adds to it. Do not run other work during a run.
- **CHECKPOINT is a no-op in digres**
  (`dg/src/backend/tcop/utility.c`, `T_CheckPointStmt`). The
  setup's writes, such as the 100,000 rows of `upd`, can be
  counted in digres's run.
- **A failed run:** `run.sh` stops at the first error and names
  the log. A pgbench line `number of failed transactions` that
  is not 0 is an error to report, not to hide.
- **Data on disk:** keep data and logs under `wbench/build/`,
  never in `/tmp` (`CLAUDE.md`). Only the socket goes in
  `/tmp`.

## 5. Next work, if the user asks

- Hot-row contention: 16 clients that update the same 10 rows,
  at READ COMMITTED and SERIALIZABLE.
- A data size larger than memory, 3 to 5 times
  `shared_buffers`.
- Space amplification: the size of `t` after the load and after
  compaction or `VACUUM`.
- Links to `wbench/` from the top `README.md` and from
  `bench/README.md`.

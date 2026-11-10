# bench — digres against PostgreSQL 18

## Objective

Measure what running on sm costs digres, against the
PostgreSQL it was forked from, on the work where the two
differ. digres keeps PostgreSQL's parser, planner and
executor; what it replaced is the storage under them: a
table is a RocksDB store in sm, a row an sm2 tuple, and a
scan an sm cursor whose rows are deformed into a slot
([`../sm/PGFORK.md`](../sm/PGFORK.md#10-digres-how-the-fork-is-to-be-made)
§10). So the benchmark measures scans, and
the queries are chosen so the scan is nearly all of the time:

- a sequential scan, reading only a table's first column, and
  only its last;
- a sequential scan over values stored out of line (TOAST);
- an index scan, a range of rows and a batch of rows by key,
  by the primary key and by a secondary index;
- those reads under 16 clients, with updates among them.

And one question of its own: **what converting sm's tuple
format costs** — sm2's tuple deformed into PostgreSQL's
`Datum`s and nulls, against PostgreSQL deforming its heap
tuple
([`DESIGN.md`](DESIGN.md#6-the-cost-of-converting-sms-tuple-format)
§6).

## What it is not

Not a test of the executor, the planner, joins or sorts,
which digres has not changed; of writes and concurrency only
as Q8's and Q11's updates under 16 clients; and not a tuning
exercise. Both servers run as built, with the
few settings [`DESIGN.md`](DESIGN.md#3-settings) §3 lists to make
the comparison
fair.

## Layout

| Path | What it is |
| --- | --- |
| [`README.md`](README.md) | this file |
| [`DESIGN.md`](DESIGN.md) | the builds, the data, the queries, the method |
| [`CODE.md`](CODE.md) | rules for running it: optimized builds only |
| `run.sh` | builds, loads, runs, and writes a report |
| `sql/` | the schema, the load, PostgreSQL's one setting of storage (`pg_only.sql`), Q1-Q6 and Q9 one a line in `queries.txt`, and pgbench's scripts for Q7, Q8, Q10 and Q11 (`q7.sql`, `q8.sql`, `q10.sql`, `q11.sql`) |
| `roles.sh` | what checking privileges costs a role that is not a superuser ([`../sm/NEXT.md`](../sm/NEXT.md#1-next) §1) |
| `btree.sh` | a secondary index as a tree against today's entries ([`../sm/BTREEINDEX.md`](../sm/BTREEINDEX.md)): wide_mixed loaded with its c2 index of one kind, Q9's and Q10's instructions a statement, and Q11 and a key-moving write (`sql/q12.sql`) under 16 clients; `btree.py` writes its queries |
| `rowlocks.sh` | what a row lock costs digres: rows read and locked FOR UPDATE, 1, 10 and 100 a statement, and inserts under a foreign key, with 1 and 16 clients ([`../sm/LOCKMGR.md`](../sm/LOCKMGR.md#32-the-lock-table) §3.2) |
| `smbench/` | the floor of a scan and a point read in sm, layer by layer down to RocksDB, on the same rows; `sqlbench.sh` times those rows through SQL in digres and PostgreSQL |
| `getseek/` | one row read by key in RocksDB, a seek to its newest version against a Get of the plain key |
| `relq6/` | a range of 10,000 rows by primary key, the Q6 of before 2026-10-28 (Q6 is 100 rows now), as the calls it makes through sm to RocksDB, run by rel on a wide_mixed of its own, timed per row with no SQL above them |
| `results/` | one report per run, dated, with the machine and commits |
| [`results/LOG.md`](results/LOG.md) | the log of every report: date, commit, machine |
| `results-gcp/` | reports from a Google Cloud VM, with their own log: not comparable with `results/` |

## Status

Designed, and its open questions decided
([`DESIGN.md`](DESIGN.md#9-decided) §9);
`run.sh` is built, and its reports are in `results/`.

# bench — digres against PostgreSQL 18

## Objective

Measure what running on sm costs digres, against the
PostgreSQL it was forked from, on the work where the two
differ. digres keeps PostgreSQL's parser, planner and
executor; what it replaced is the storage under them: a
table is a RocksDB store in sm, a row an sm2 tuple, and a
scan an sm cursor whose rows are deformed into a slot
(`../sm/PART4.md` §10). So the benchmark measures scans, and
the queries are chosen so the scan is nearly all of the time:

- a sequential scan, reading only a table's first column, and
  only its last;
- a sequential scan over values stored out of line (TOAST);
- an index scan, a range of rows and single rows by key.

And one question of its own: **what converting sm's tuple
format costs** — sm2's tuple deformed into PostgreSQL's
`Datum`s and nulls, against PostgreSQL deforming its heap
tuple (`DESIGN.md` §6).

## What it is not

Not a test of the executor, the planner, joins, sorts or
writes, which digres has not changed; not of concurrency; and
not a tuning exercise. Both servers run as built, with the
few settings `DESIGN.md` §3 lists to make the comparison
fair.

## Layout (planned)

| Path | What it is |
| --- | --- |
| `README.md` | this file |
| `DESIGN.md` | the builds, the data, the queries, the method |
| `CODE.md` | rules for running it: optimized builds only |
| `run.sh` | builds, loads, runs, and writes a report |
| `sql/` | the schema, the load, and one file per query |
| `results/` | one report per run, dated, with the machine and commits |

## Status

Designed, and its open questions decided (`DESIGN.md` §9);
`run.sh` is being written.

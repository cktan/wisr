# DUCKPLAN: run DuckDB's SQL tests against digres

This is a plan for a coding agent. Nothing here is built. Read
`CLAUDE.md`, `dg/CODE.md` and `dg/test/slt/README.md` first.

**The goal.** DuckDB's repository holds 6,156 SQL test files
under `test/sql`, in the sqllogictest format, under the MIT
license. Run their statements on PostgreSQL 18 and on digres,
side by side, and report each place where digres differs.
PostgreSQL 18 is the reference. DuckDB's own expected answers are
not used.

**WARNING: this test is separate from every other test.**

- `make test`, `make slt` and `dg/test/run.sh` must not run it.
- A person starts it by hand. Nothing calls it.
- Change no existing test, script, or Makefile target for it.
- Copy no DuckDB file into this repository.

## 1. Why this design

- **Differential, so no dialect work.** DuckDB's SQL differs from
  PostgreSQL's. A statement that fails on both servers in the
  same way is no finding. So the comparison filters the dialect
  out, and no one has to port a file.
- **Its own runner.** `dg/test/slt/slt.cc` compares a server
  against a file's stored answers. This runner compares two
  servers. It holds two connections for every session name, and
  must keep them in step. Sharing the code would change a runner
  `make test` uses.
- **Fetched, not copied.** The files stay in `dg/build`, at a
  pinned commit, as `dg/test/slt_corpus.sh` does for SQLite's
  corpus. Read that script: it is the model for the fetch.

## 2. What to build

All new files are in `dg/test/duck/`:

| File | What it is |
| --- | --- |
| `duck.sh` | starts both servers, fetches the files, runs them, prints the summary |
| `duckrun.cc` | the differential runner, a libpq client |
| `known.txt` | differences reviewed and accepted: `FILE:LINE reason` |
| `README.md` | how to run it, and how to read its report |
| `selftest/` | small `.test` files that prove the runner works |

`duck.sh` compiles `duckrun.cc` itself, into `dg/build/duckrun`,
with the command the `build/slt` target in `dg/Makefile` uses,
less `-lcrypto`. So `dg/Makefile` does not change.

```
dg/test/duck/duck.sh                    the default directories
dg/test/duck/duck.sh test/sql/upsert    these directories or files
dg/test/duck/duck.sh --tree ...         digres indexes are btree
```

## 3. The fetch

1. Pin commit `b2207b7290887503b6280ee41f3c8c597a884141`
   (2026-11-05) of `https://github.com/duckdb/duckdb.git`.
2. Clone once into `dg/build/duck-corpus`, with
   `--filter=blob:none --no-checkout`, as `slt_corpus.sh` does.
3. Check out only the directories asked for, by `git
   sparse-checkout`.
4. The default directories are `test/sql/transactions`,
   `test/sql/update`, `test/sql/upsert`, `test/sql/constraints`,
   `test/sql/delete`, `test/sql/index`, `test/sql/alter` and
   `test/sql/types/decimal`.

Leave out `storage`, `parallelism`, `attach`, `pragma`, `json`,
`pivot` and `peg_parser`: they test DuckDB's own engine or
syntax.

## 4. The servers

`dg/test/slt.sh` shows how to start each one. Start both, once
for a run:

- **digres:** `dg/build/digres`, the debug build, so `assert()`
  and `Assert()` can fire. Build it with `DEBUG=1 make -C sm -j4`
  and `DEBUG=1 make -C dg -j4`.
- **PostgreSQL 18:** in `$PG18`. On this machine it is
  `bench/build/pg18`, version 18.6, so set `PG18` to that path.
  Fail with a clear message if it is not there.
- **Their data, logs and sockets** go in `dg/build/dk`. Keep the
  path short: a socket's path is at most 107 bytes. Do not use
  `/tmp` for data (`CLAUDE.md`).
- **Each test file** gets a new database on each server, dropped
  after, as `slt.cc` does.
- **Every connection** on both servers runs these first:

```sql
set lock_timeout = '500ms';
set statement_timeout = '10s';
set default_transaction_isolation = 'repeatable read';
```

**WARNING: the runner is one thread, and PostgreSQL blocks where
DuckDB fails at once.** A second session's write to a row the
first has changed waits in PostgreSQL. The commit that frees it
is in the next record, which the runner would never reach.
`lock_timeout` turns that wait into error `55P03` on both
servers. It was checked on digres: it works, as does
`statement_timeout`.

`repeatable read` is the level closest to DuckDB's snapshot
isolation. Add `--isolation LEVEL` to run another.

## 5. The runner

### 5.1 The format

DuckDB's records are SQLite's, with additions. A directive is a
line at the start of a record, after a blank line. A line in a
statement's SQL is never a directive, even one that begins with
`set`. These counts come from the eight default directories:
7,359 `statement`, 2,827 `query`, 129 `endloop`, 108 `load`, 101
`loop`, 90 `restart`, 52 `require`, 38 `onlyif`, 19 `foreach`, 12
`set`, 8 `concurrentloop`, 2 `unzip`, 1 `concurrentforeach`.

| Line | What to do |
| --- | --- |
| `statement ok`, `statement error`, `statement maybe` | run the SQL; the word after `statement` does not matter, as the servers are compared |
| `statement ok con1` | run it on session `con1`, on both servers |
| `query III [SORT] [conN]` | run the SQL, and skip DuckDB's answer: lines after `----` up to a blank line. A third word that is not `rowsort`, `nosort` or `valuesort` is the session (seen: `con1` to `con4`, `old`, `updater`) |
| `statement error` with `----` | skip the message lines up to a blank line |
| `# ...` | a comment |
| `load ...` | ignore it: a database file of DuckDB's |
| `require X` | skip the file, unless X is on the list below |
| `loop i 0 N` ... `endloop` | run the records N times, `${i}` replaced |
| `foreach x a b c` ... `endloop` | the same, over the words |
| `mode skip`, `mode unskip` | skip the records between them |
| `onlyif VAR=VALUE` | a test on a loop variable: `=`, `<`, `<>`; skip the record if false. `skipif` is its opposite. In the sample all but one file with `onlyif` have `concurrentloop`, so they are skipped |
| `set ...`, `reset ...` | ignore it: a runner option of DuckDB's, such as `ignore_error_messages` |
| `restart`, `concurrentloop`, `concurrentforeach`, `unzip` | skip the file, and count it |
| anything else | skip the file, count it, and name the word in the report |

`require` words to ignore, as they concern DuckDB's build alone:
`skip_reload`, `no_alternative_verify`, `noforcestorage`,
`no_force_storage`, `vector_size`, `exact_vector_size`, `64bit`.
Add others only after reading what they mean. A session is opened
at its first use, on both servers. A statement with no session
runs on `default`.

**Replace these in the SQL:** `__TEST_DIR__`, `{TEST_DIR}` and
`{TEMP_DIR}` by a directory under `dg/build/dk`. Skip a file that
has `{DATA_DIR}` or `{UNITTEST_OBJECT_FILES}`: its data is not
fetched.

**Rewrite the index kind.** PostgreSQL has neither `art` nor
`lsm`, and digres's default kind is `lsm`:

| `CREATE INDEX` in the file | PostgreSQL 18 | digres | digres with `--tree` |
| --- | --- | --- | --- |
| `USING art` | `USING btree` | `USING lsm` | `USING btree` |
| no `USING` | as written | as written | `USING btree` |

So a default run and a `--tree` run cover both kinds of index.
Keep every other statement as it is. The step that sorts the
findings decides whether any other rewrite pays.

### 5.2 Comparing a statement

Run each statement on PostgreSQL 18, then on digres. Compare:

1. **Both failed.** Compare the SQLSTATE. Equal is no finding.
   Different is `sqlstate-differs`. Do not compare the message
   text. `55P03` on one server and `40001` on the other is
   expected to be common: digres may refuse at once where
   PostgreSQL waits. Decide that case when the first run shows
   it.
2. **One failed.** A finding: `digres-only-error` or
   `pg-only-error`.
3. **Both gave rows.** Compare the column count and the rows, as
   text from `PQgetvalue`. A difference is `result-differs`.
   - With an `ORDER BY` in the SQL, compare in order. If only the
     order differs, it is `order-only`, a lesser finding: ties
     may fall either way.
   - With none, compare as sorted lists: digres scans in key
     order.
4. **The connection to digres dropped.** This is `crash`, the
   highest finding. Note the file and line, keep the server's
   log, restart digres, and go on with the next file.
5. **A timeout on one server alone** (`57014`, `55P03`), where
   the other succeeded: `hang`. Check for this before the rule
   for one failure.

The kinds, worst first: `crash`, `hang`, `digres-only-error`,
`pg-only-error`, `sqlstate-differs`, `result-differs`,
`order-only`.

### 5.3 After a finding

The two servers' data may differ after a finding, so later
results mean nothing. Report the first finding of a file, then
stop that file, and count how many records it did not run. A run
goes on past a statement that failed the same way on both, as the
servers stay in step. The price is that later statements may also
fail: a failed `CREATE TABLE` fails all that use the table, and a
failure in a transaction gives `25P02` up to its `ROLLBACK`. The
`dead` report shows those files.

### 5.4 The report

Write these to `dg/build/dk/report/`:

- `summary`: for each directory, the files run, skipped and
  clean; the records run; the findings by kind.
- `findings`: each one: kind, `FILE:LINE`, session, the SQL, and
  what each server gave. Leave out those in `known.txt`.
- `dialect`: the 30 most common SQLSTATE and first words of a
  message among statements that failed on both. This shows which
  rewrite would pay.
- `skipped`: each file skipped, and why.
- `dead`: files where more than half the statements failed on
  both. Such a file tests little.

The last line of `duck.sh` says `PASSED` or `FAILED (N
findings)`, and its exit status is 0 or 1, 2 if the run could not
start.

## 6. Prove the runner works

**WARNING: a differential test that finds nothing may be
broken.** `duck.sh --selftest` proves each path, with the files
in `selftest/`:

1. A file of plain statements on both servers: no finding.
2. A statement that fails on both: no finding, and the file goes
   on.
3. A statement whose answer differs by server, such as `select
   version()`: one finding, and the file stops.
4. Two sessions on one row: the second gets `55P03` on both, and
   the run does not hang.
5. A `loop`, a `foreach`, a `query` with DuckDB's answer block, a
   `statement error` with its `----` block, `mode skip`, and a
   `require` that skips the file: the record counts are as
   written in the file's own comments.
6. A query with no `ORDER BY`, on a table where the two servers
   scan in a different order: no finding.

## 7. Steps

Do them in order. Finish each before the next. Each ends with the
check named.

1. **Build the runner and its self-test.** Write `duckrun.cc`,
   `selftest/` and enough of `duck.sh` to run them. Check:
   `duck.sh --selftest` passes every case listed under "Prove the
   runner works".
2. **Fetch the corpus and run one directory.** Finish `duck.sh`,
   and run `test/sql/upsert`, a directory with no sessions.
   Check: the report exists, and 47 of its 50 files were not
   skipped.
3. **Run the default directories.** Check: the run ends without
   hanging, and the files not skipped match the table under "What
   to expect".
4. **Sort the findings.** For each one, decide whether it is a
   bug or a known difference. Reduce a bug to its smallest SQL.
   - A bug: add it to `dg/ISSUE.md`, with the smallest SQL. Do
     not fix it in this work.
   - A known difference: add it to `known.txt`, with its reason.
     `dg/UNSUPPORTED.md` lists what digres refuses on purpose, so
     a refusal it lists is a known difference.
   - Read `dialect`: add a rewrite under "The format" only if it
     opens many records.
5. **Run with `--tree`.** Run the default directories again with
   btree indexes, and sort the new findings the same way.
6. **Write `dg/test/duck/README.md`**, and add one line about it
   to `dg/test/slt/README.md`. Check: another person could run it
   from the README alone.

Later, and not in this work: `restart` could restart both
servers, which would test recovery, and `concurrentloop` needs
threads.

## 8. What to expect

These numbers come from reading the files, not from running them.
A file is skipped by the rules under "The format".

| Directory | Files | Not skipped |
| --- | --: | --: |
| `transactions` | 83 | 77 |
| `constraints` | 72 | 67 |
| `upsert` | 50 | 47 |
| `update` | 46 | 39 |
| `index` | 175 | 133 |
| `delete` | 25 | 22 |
| `alter` | 135 | 125 |
| `types/decimal` | 27 | 27 |
| all | 613 | 537 |

`restart` causes 59 of the 76 skipped files, 35 of them in
`index`.

- **`types/decimal` may differ on purpose.** sm's decimals are
  DuckDB's, not PostgreSQL's (`dg/DECIMAL.md`). Read that
  document before calling a decimal difference a bug.
- **Row order and `pg_` catalogs differ on purpose.** See
  `dg/UNSUPPORTED.md` and `DG-vs-PG.md`.
- **Many statements will fail on both.** DuckDB's `ORDER BY ALL`,
  `CREATE MACRO`, `PRAGMA`, list types and `HUGEINT` are not
  PostgreSQL's. This is expected, and costs nothing.

## 9. Rules for this work

- Work on a branch `wip-duck`. Commit when the user asks, as `git
  -c user.name="CK Tan" -c user.email="cktanx@gmail.com"`.
- Run `duck.sh --selftest` before each commit. Run the full
  suites (sm, sq, dg, with `DEBUG=1`) once before the last
  commit, as `CLAUDE.md` requires.
- This work changes only `dg/test/duck/`,
  `dg/test/slt/README.md`'s one line, and `dg/ISSUE.md`. No file
  in `sm/`, `dg/src/` or any existing test changes. If you think
  one must, stop and ask.
- Do not fix a bug it finds: report it.
- Follow `CLAUDE.md`'s code style: `snake_case`, a doc comment on
  every function, `_t` on types, and no `strncpy()`. Open each
  test file with a one-line synopsis and numbered steps.
- Prose in `.md` files wraps at 65 columns.

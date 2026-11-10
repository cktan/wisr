# DuckDB's SQL tests, on PostgreSQL 18 and digres

`duck.sh` runs the statements of DuckDB's SQL test files on
PostgreSQL 18 and on digres, side by side, and reports each
place where digres differs. PostgreSQL 18 is the reference.
DuckDB's own expected answers are not used. The plan behind
it is [`../../DUCKPLAN.md`](../../DUCKPLAN.md).

**WARNING: this test is separate from every other test.** No
make target, `run.sh` or `slt.sh` runs it. A person starts it
by hand. It needs the network once, to fetch DuckDB's files.

## Running it

Build the debug server first, so `assert()` and `Assert()` can
fire:

```
DEBUG=1 make -C sm -j4
DEBUG=1 make -C dg -j4
```

Then, from `dg/`:

```
test/duck/duck.sh                       the default directories
test/duck/duck.sh test/sql/upsert ...   these directories or files
test/duck/duck.sh --tree ...            digres indexes are btree
test/duck/duck.sh --isolation LEVEL ... sessions run at LEVEL
test/duck/duck.sh --selftest            prove the runner works
```

- **PostgreSQL 18** is the one in `$PG18`. When `PG18` is not
  set, it is `../bench/build/pg18`.
- **DuckDB's files** are fetched once, at a pinned commit, into
  `build/duck-corpus`. A run checks out only the directories it
  names. No DuckDB file is copied into this repository.
- **The default directories** are `transactions`, `update`,
  `upsert`, `constraints`, `delete`, `index`, `alter` and
  `types/decimal`, all under `test/sql`.
- **Everything a run makes** is in `build/dk`: the two servers'
  data and logs, and the report. Each run starts it again.
- **The time:** the default directories take about 18 minutes,
  most of it the files of a million rows on the debug build.

The last line says `PASSED` or `FAILED (N findings ...)`. The
exit status is 0 with no finding, 1 with one, and 2 when the
run could not start.

## How it works

`duck.sh` builds the runner, `build/duckrun`, from
`duckrun.cc`. It starts both servers and gives the runner
every file.

1. Each file gets a new database on each server, dropped
   after.
2. Each session in the file is two connections, one to each
   server. A session is opened at its first use.
3. Every connection first sets `lock_timeout` to 500 ms,
   `statement_timeout` to 10 s and the isolation level to
   `repeatable read`. The runner is one thread: a wait for
   another session's lock must end in an error, not a hang.
4. Each statement runs on PostgreSQL 18, then on digres.
5. The two outcomes are compared. A file stops at its first
   finding, as the two servers' data may then differ.
6. After a crash or a hang, `duck.sh` keeps digres's log,
   restarts both servers, and goes on with the next file.

**Comparing a statement:**

- **Both failed:** equal SQLSTATEs are no finding. The message
  text is not compared.
- **Both succeeded:** the command tags, such as `INSERT 0 3`,
  the column counts and the rows, as text, are compared. An
  `EXPLAIN`'s rows are not: a plan is each engine's own. With
  an `ORDER BY` in the SQL, the order is compared too. With
  none, the rows are compared as sorted lists, as digres scans
  in key order.
- **Otherwise**, a finding of one of these kinds, worst first:

| Kind | What it means |
| --- | --- |
| `crash` | the connection to digres dropped |
| `hang` | a timeout (`57014`, `55P03`) on one server, where the other succeeded; or digres did not answer even a cancel |
| `digres-only-error` | digres alone failed |
| `pg-only-error` | PostgreSQL 18 alone failed |
| `sqlstate-differs` | both failed, with different SQLSTATEs |
| `result-differs` | both succeeded, with different results |
| `order-only` | the same rows, in a different order, under an `ORDER BY` |

**The format** is DuckDB's sqllogictest. The runner reads only
the directives and the SQL:

- **Run:** `statement` and `query`. The word after `statement`
  does not matter. A last word that is not a sort mode names
  the session, such as `con1`.
- **Skipped as text:** DuckDB's answers and error messages,
  the lines after `----`.
- **Expanded:** `loop` and `foreach`, with `{var}` and `${var}`
  replaced. `foreach` expands DuckDB's groups, such as
  `<integral>`. `onlyif` and `skipif` test a loop variable.
- **Ignored:** `load`, `set`, `reset`, `hash-threshold`, and a
  `require` of DuckDB's build only, such as `vector_size`.
  `mode skip` skips the records up to `mode unskip`.
- **The whole file is skipped** for any other `require`, for
  `restart`, `concurrentloop`, `concurrentforeach`, `unzip` and
  an unknown directive, such as `include`, and for `{DATA_DIR}`,
  whose data is not fetched.
- **Rewritten:** `__TEST_DIR__`, `{TEST_DIR}` and `{TEMP_DIR}`
  become a directory of each server's own under `build/dk/tmp`.
  `USING art` becomes `USING btree` on PostgreSQL 18 and `USING
  lsm` on digres. With `--tree`, every digres index is btree.
  DuckDB's table function `range()`, after `FROM` or `JOIN`,
  becomes `generate_series()` with DuckDB's meaning: the end left
  out, and a column named `range` when no alias is given. It is
  in about 400 statements, and the tables made from it in many
  more. DuckDB's integer types become PostgreSQL's wide enough
  for their values: `TINYINT` and `UTINYINT` `int2`, `USMALLINT`
  `int4`, `UINTEGER` `int8`, `UBIGINT` and `HUGEINT`
  `numeric(36, 0)`; and `DOUBLE` `float8`. Both rewrites are made
  on both servers alike.

## Reading the report

The files are in `build/dk/report/`:

| File | What it holds |
| --- | --- |
| `summary` | for each directory: files, skipped, clean (a file whose one finding `known.txt` lists among them), records run, findings by kind |
| `findings` | each finding: kind, `FILE:LINE`, session, the SQL, what each server gave |
| `known` | the findings that `known.txt` lists |
| `dialect` | the 30 most common SQLSTATEs and message starts among statements that failed on both |
| `skipped` | each file skipped, and why |
| `dead` | the files where more than half the records failed on both: they test little |
| `files.tsv` | each file: status, detail, records run, records not run, records failed on both |
| `digres-N.log` | digres's log, kept at each restart |

**To sort a finding:**

1. Read its file in `build/duck-corpus` at the line named.
2. Decide if digres is wrong, or differs on purpose.
   [`../../UNSUPPORTED.md`](../../UNSUPPORTED.md),
   [`../../DECIMAL.md`](../../DECIMAL.md) and
   [`../../../DG-vs-PG.md`](../../../DG-vs-PG.md) list what
   differs on purpose.
3. A bug: reduce it to its smallest SQL, and add it to
   [`../../../ISSUE.md`](../../../ISSUE.md).
4. A difference on purpose: add `FILE:LINE reason` to
   `known.txt`.

A clean file can still test little: a failed `CREATE TABLE`
fails every statement on the table, on both servers. `dead`
shows those files, and `dialect` shows which DuckDB syntax
fails most.

## What it found

The first runs, on 2026-11-05, found 13 bugs, in
[`../../../ISSUE.md`](../../../ISSUE.md#3-digres-known-issues)
dg issues 14 to 26. All but one are fixed,
[`../../WORKNOTE.md`](../../WORKNOTE.md) steps
384 to 394; issue 22, the purge of a mass delete stalling every
writer, is open. What is left is in `known.txt`: refusals
[`../../UNSUPPORTED.md`](../../UNSUPPORTED.md) lists, numeric's
36 digits, files whose
statements use `random()`, and statements of a million rows that
the debug build does not finish in 10 s.

## The self-test

**WARNING: a differential test that finds nothing may be
broken.** `duck.sh --selftest` runs the files in `selftest/`.
Each says on its `# expect:` line what `files.tsv` must say of
it: status, detail, records run, records not run, and records
failed on both. Run it after each change to the runner.

# sqllogictest for digres

SQL tests that say what each statement must do, run by
`build/slt` over libpq. The format is SQLite's sqllogictest,
the one DuckDB and CockroachDB extended. The answers come from
PostgreSQL 18: a file is filled by running it there, and
digres must then give the same.

## Running

```
make -j4 slt                     every file, on digres
test/slt.sh FILE...              these files, on digres
test/slt.sh --pg18 [FILE...]     on PostgreSQL 18 instead
test/slt.sh --rewrite FILE...    fill FILE's results from PostgreSQL 18
```

PostgreSQL 18 is the one installed in `$PG18`, `~/pg18` if
that is not set. Each file runs in a database of its own,
made for it and dropped after. What failed, with its line,
its SQL and the expected and actual values, is in
`build/test/slt/failures`; each file's counts and time are in
`build/test/slt/tally`, and every record's time, the slowest
first, in `build/test/slt/times`. A time much worse than
PostgreSQL 18's for the same record is a finding too.

## The format

A file is records separated by blank lines. A line starting
with `#` is a comment.

```
statement ok
create table t (a int4, b text)

statement error division by zero
select 1 / 0

query IT rowsort
select a, b from t
----
1	x
2	NULL
```

- **`statement ok`**: the SQL must succeed.
- **`statement error [PATTERN]`**: it must fail, and its
  message, the primary one without `ERROR:`, must match the
  ECMAScript regular expression PATTERN, if one is given.
- **`query TYPES [SORT [LABEL]]`**: one letter a column. `I`
  prints an integer, truncating a fraction as SQLite's driver
  does; `R` three decimals; `T` the text. NULL prints `NULL`,
  an empty string `(empty)`, a control character `@`. A
  column count other than TYPES' fails.
- **SORT**: `nosort`, the default, compares rows in the order
  they came; `rowsort` sorts the rows, `valuesort` every value,
  as strings, before comparing.
- **Results**, under `----`: a row a line with its values
  separated by tabs, as `--rewrite` writes them, or a value a
  line, as SQLite's files are. `N values hashing to MD5` is
  the MD5 of each value followed by a newline.
- **LABEL**: every query with the same label must give the
  same values.
- **`hash-threshold N`**: `--rewrite` writes the hash for a
  result of more than N values.
- **`skipif NAME`, `onlyif NAME`**, on the lines before a
  record: skip it on, or run it only on, an engine of that
  name. digres is `digres` and `postgresql`; PostgreSQL 18 is
  `postgresql` alone. A divergence digres keeps on purpose is
  a `skipif digres` record with an `onlyif digres` one beside
  it, saying what digres does instead.
- **`connection NAME`**, on the line before a record: run it
  in the session NAME, made at its first use, in the file's
  database. A record with none runs in the session
  `default`. Sessions run one record at a time, so a record
  that would wait for another session's lock waits forever:
  such a test is not one for this format.
- **`halt`**: stop the file here.

## Writing a test

1. Write the statements and queries, with `----` and no
   results under each query.
2. `test/slt.sh --rewrite FILE` fills the results from
   PostgreSQL 18. Read them: a result written is a result
   claimed.
3. `test/slt.sh FILE` runs it on digres.

A record PostgreSQL 18 cannot run, because it tests something
of digres's own, gets `onlyif digres` and results written by
hand.

## The SQLite corpus

`sqlite/select1.test` to `select5.test` are SQLite's
sqllogictest files, from
`https://www.sqlite.org/sqllogictest/raw?ci=trunk&filename=test/selectN.test`,
taken on 2026-10-15 and unchanged. Their copyright notice,
`sqlite/COPYRIGHT.md`, copied from the same repository, lets
them be used as one sees fit, with no attribution required. They are about 10,700 records of joins, subqueries,
aggregates, `CASE` and compound selects over integer and
text columns, whose results every engine sqllogictest was
run on agreed on. PostgreSQL 18 passes every record.

## The whole corpus

The rest of SQLite's corpus, `index/`, `random/` and
`evidence/`, is about 1.2 GB in some 650 files, too much to
keep here. `test/slt_corpus.sh` (`make -j4 slt-corpus`)
fetches it once, from a mirror on GitHub at a pinned commit,
into `build/slt-corpus`, and only the files it is to run:

```
test/slt_corpus.sh                      the sample, below
test/slt_corpus.sh '/test/random/expr/' these files: sparse-checkout patterns
```

It runs them on PostgreSQL 18 and on digres, and reports the
records digres fails that PostgreSQL 18 passes, file:line, in
`build/test/slt-corpus/digres-only`. A record PostgreSQL 18
fails is written for SQLite's or MySQL's dialect, and is not
held against digres. The sample, the first set of files in
each directory and `evidence/`, is about 1.5 million records;
it takes 5 minutes on PostgreSQL 18 and 17 on digres. The
whole corpus, `test/slt_corpus.sh '/test/'`, is 622 files and
5.7 million records, about 45 minutes on PostgreSQL 18. It is
in no `make test`: it needs the network once, and takes long.

DuckDB's SQL tests are run another way: on PostgreSQL 18 and on
digres side by side, by `test/duck/duck.sh`
(`test/duck/README.md`).

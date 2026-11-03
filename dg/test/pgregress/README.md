# PostgreSQL 18's regression files, ported to digres

`../regress/` runs PostgreSQL 18's regression suite as it is
and only measures: most files fail, as digres leaves much of
PostgreSQL out by design. This directory is the suite ported:
each file of PostgreSQL 18.6's `parallel_schedule`
(`724edf9`) with every statement digres does not run as
PostgreSQL does commented out, and expected output that is
PostgreSQL 18's for the file so trimmed. What is left must
pass, so the suite is a test: `make -j4 pgregress` fails on a
difference.

## Layout

| Path | What it is |
| --- | --- |
| `schedule` | the files, `test_setup` first, in `parallel_schedule`'s order |
| `sql/` | the files, trimmed |
| `expected/` | PostgreSQL 18's output for each trimmed file |
| `data/` | PostgreSQL's `data/`, which the files `COPY` from |
| `pgregress.py` | runs the files, trims them, writes `expected/` |
| `split.py` | splits a file into the statements psql runs |

## Running

```
make -j4 pgregress              every file, on build/digres
test/pgregress/pgregress.py -j 4 test FILE...
```

Each file runs alone, in a fresh database `regression`, after
`test_setup` (as trimmed), through PostgreSQL 18's
`pg_regress --use-existing` and `psql`, from the tree in
`$PG18_SRC` (`~/p/postgres18`, built and installed into its
`install/`); with no such tree the run says SKIPPED. Output
is compared with `expected/`, blind to white space as
`pg_regress` is, and to the order of the rows of a result whose
statement has no ORDER BY, which are compared as a multiset
(`../rowset.py`): digres reads a table with a primary key in the
key's order, PostgreSQL in the order its rows went in
(`../../../sm/CLUSTER.md`). A plan where PostgreSQL's has a
bitmap scan is not compared, the statement still run: digres
plans none (`../../../sm/CLUSTER.md` §6), and the results that
follow are compared. An index's kind `lsm`, as
`pg_get_indexdef()` and psql's `\d` print it, is read as
PostgreSQL's `btree`: in digres both are `pg_am`'s `btree`
(`../../../sm/BTREE.md` §12). A result's rule line is compared
without its width, which the shorter name changes. This applies
to `test` only: `trim` and `differs` still see `lsm`. A file's
output and diff go in
`build/test/pgregress/test/`. Four servers run at once, `-j`.

## The markers

A statement taken out has each line prefixed `--dg ` under a
line that says why:

| Marker | Why |
| --- | --- |
| `--dg-skip: CAUSE` | digres refuses it where PostgreSQL does not; CAUSE is the error, with names, numbers and quoted strings taken out, as `../regress/report.py` does |
| `--dg-cascade` | on PostgreSQL it does what it did not before a statement above it was taken out: it needs what that one made |
| `--dg-differs: WHAT` | digres runs it but answers differently: `another error`, `no error where PostgreSQL has`, `the plan` or `the result` |
| `--dg-crash: WHAT` | digres crashed or hung on it |
| `--dg-unstable` | PostgreSQL itself answers it differently each run |

So the backlog is a grep away:

```
grep -h '^--dg-skip' sql/*.sql | sort | uniq -c | sort -rn
grep -c '^--dg-differs' sql/*.sql | grep -v ':0$'
```

A `skip` is a missing feature; a `cascade` is counted under
the statement it follows from, and is not work of its own. A
`differs` is a suspected bug, or a difference to explain: a
query with no `ORDER BY` that reads rows in another order, a
plan digres's statistics choose, an error digres words its
own way. Each is to be looked at; none is to be taken as
right because it is marked.

## How a file is trimmed

`pgregress.py trim FILE` runs the file on PostgreSQL 18 and on
digres, with a `\echo` marker before each statement so each
statement's output can be cut out of psql's, and compares the
two a statement at a time, as the test does, an unordered
result's rows as a multiset:

1. PostgreSQL runs the untrimmed file twice; a statement whose
   output differs between the two is `unstable`.
2. Each pass runs the file as trimmed so far on both servers.
   A statement whose output on PostgreSQL is no longer what it
   was untrimmed is a `cascade`.
3. Of the statements where digres differs from PostgreSQL, the
   first is taken out, and with it every one that meets an
   error a statement meets on its own (`not supported`, no
   catalog tables, an unknown setting or language), as those
   cannot follow from the first.
4. Passes go on until one takes nothing out.

`test_setup` is trimmed first, as every other file runs after
it. The branches of a `\if` are never taken out.
`pgregress.py check` checks `split.py` itself: PostgreSQL's
output with the markers must be its output without them.

## When digres gains a feature

Copy the files it concerns back from PostgreSQL's tree,
`src/test/regress/sql/`, and trim them again, then write their
expected output:

```
cp ~/p/postgres18/src/test/regress/sql/FILE.sql sql/
./pgregress.py trim FILE && ./pgregress.py expected FILE
```

A trim keeps what a file already has taken out, so a file not
copied back first is only trimmed further. Changing `test_setup`
changes every file: trim and write expected for all of them.

## Results, 2026-10-21

At `ed3bc0f5`, digres's `-O0` assertion build on x86_64, 231
files: every one passes. Of their 51,711 statements, 22,076
run and 29,635 are taken out:

| Marker | Statements |
| --- | --- |
| `skip` | 7,581 |
| `cascade` | 20,574 |
| `differs` | 1,479 |
| `unstable` | 1 |
| `crash` | 0 |

27 files run whole. The causes that take out the most
statements:

| Statements | Cause |
| --- | --- |
| 1,366 | `relation pg_authid cannot be opened`: roles |
| 961 | `function ?(...) does not exist` |
| 820 | `language "?" does not exist`: `plpgsql` and `sql` |
| 604 | `CREATE TABLE supports only columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY yet` |
| 277 | `relation "?" does not exist` |
| 264 | `ALTER TABLE supports only ADD COLUMN, ...` |
| 259 | `unrecognized configuration parameter "?"` |
| 245 | `publications are not supported` |
| 217 | `type ? is not supported yet` |
| 211 | `type "?" does not exist` |

The `differs`: 934 another error, 272 the result, 204 the
plan, 69 no error where PostgreSQL has one.

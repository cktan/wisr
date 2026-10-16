# PostgreSQL 18's regression suite on digres

`test/regress.sh` (`make -j4 regress`) runs every file of
PostgreSQL 18's `src/test/regress/parallel_schedule` on digres
and reports which missing features cost the most files. It
only measures: it exits 0 whatever it finds, and is in no
`make test`. Most files fail, by design (`PLAN.md`,
`../sm/NEXT.md`); the point is to know why.

## Running

```
make -j4 regress           every file, about 4 minutes
test/regress.sh FILE...    these files
```

The files, their expected output and `pg_regress` are those of
the PostgreSQL 18 tree built in `$PG18_SRC` (`~/p/postgres18`);
`pg_regress` and the harness run that tree's `psql`.

- One server, on a data directory `initdb` makes in
  `build/test/regress`, its socket in a short directory under
  `/tmp`, as a socket path over 107 bytes cannot be bound.
- Each file runs alone, in a database made for it: the last
  file's database, and the `regression*` databases and
  `regress*` roles it made, are dropped first.
- `test_setup` runs first in each database, as it makes the
  tables most files read (`tenk1`, `onek`, ...), with `COPY`
  from `src/test/regress/data`. Then the file, through
  `pg_regress --use-existing`, stopped after 180 seconds.
- A crash stops the server, as a thread that ends abnormally
  does; it is started again for the next file.

## What it writes

In `build/test/regress/`, by `regress/report.py`:

- **`summary`**: a line a file: `pass`, `fail`, `timeout` or
  `crash`; the diff's hunks; the first error the diff adds
  that PostgreSQL's expected output does not have (a warning
  if there is none); and, after `<-`, the statement that
  raised it.
- **`causes`**: the files grouped by that error, with names,
  numbers and quoted strings taken out, so `type name is not
  supported yet` and `type point is not supported yet` are one
  cause; the most files first, and under each its files and
  its messages as they were. A crash comes first: it is a bug,
  not a missing feature. A file stopped by a table
  `test_setup` could not make is counted under the error that
  stopped `test_setup`, as `test_setup could not make a table:`.
  `relation 1247 cannot be written` names the catalog,
  `relation pg_type ...`, from `src/include/catalog`.
- **`crashes`**: each crash, the statement running, and the
  server log's `TRAP:` line.
- **`setup`**: each statement of `test_setup` that failed.
- **`FILE/`**: what `pg_regress` printed, `regression.diffs`,
  `results/`, and the server's log while the file ran.

A file's cause is its first error only: making that work
shows the next. "Unblocks N files" below means N files get
past their first error, not that N files pass.

## Results, 2026-10-16

At `0d2bed5` with `WORKNOTE.md` step 188, an assertion build
at `-O0` on x86_64; 230 files in 3.5 minutes. Two runs gave
the same summary, causes, crashes and setup.

- **16 pass.** 13 run in full: `boolean`, `char`, `varchar`,
  `text`, `int2`, `int4`, `int8`, `md5`, `comments`, `delete`,
  `json_encoding`, `jsonpath`, `jsonpath_encoding`. Three
  skip themselves on this database's encoding and locale, as
  they do on PostgreSQL: `collate.icu.utf8`,
  `collate.linux.utf8`, `collate.windows.win1252`.
- **211 fail, 3 crash, no timeouts.**

### Crashes

Two assertions, each reproducible by running the file
alone:

| assertion | files | statement |
|---|---|---|
| `DatumGetObjectId(ldatum) == table_oid`, execCurrent.c:212 | portals, rowsecurity | `DELETE` / `UPDATE ... WHERE CURRENT OF` |
| `!TTS_EMPTY(slot)`, execTuples.c:1267 | foreign_key | `UPDATE` of a self-referencing key `ON UPDATE CASCADE`, in a transaction |

A third, `relation_open()`'s `NoLock` assertion, stopped five
more files, on DDL that read a descriptor cached in an earlier
transaction before sm's lock; step 188 fixed it.

One more bug, not a crash: in `strings`,
`SUBSTRING('abcdefg' SIMILAR '#"(b_d)#"%' ESCAPE '#')`, which
is NULL on PostgreSQL, fails with `function 0x... returned
NULL`.

### What test_setup cannot do

Every file starts with these failures, so 12 of `test_setup`'s
tables are missing in every file:

- `onek` and `tenk1` (`name`),
  `POINT_TBL` and `person` (`point`), `road` (`path`):
  types not supported yet; `onek2` and `tenk2` are made
  from them.
- `emp`, `student`, `stud_emp`, `ihighway`, `shighway`:
  `INHERITS`.
- `GRANT ALL ON SCHEMA public` (`pg_namespace`), the
  tablespace, `synchronous_commit` and
  `allow_in_place_tablespaces`, the `sql` functions, the enum
  and range types and the operator classes.

### The causes that stop the most files

| files | cause | what it is |
|---|---|---|
| 29 | `type ? is not supported yet` | `name` 3, `box` 3, `oid` 2, `point` 2, and 19 more one file each: the other geometric types, `bit`, `money`, `pg_lsn`, `timetz`, `cidr`, `macaddr`, `xid8`, `jsonb`, `xml`, arrays, ranges |
| 19 | `a relation's storage cannot be made` | views 11 (one inside `CREATE SCHEMA`), `CREATE TYPE ... AS (...)` 4, `CREATE TABLE AS` 4 |
| 19 | `CREATE TABLE supports only columns, ...` | `WITH (fillfactor, autovacuum_enabled)` 5, `INHERITS` 6, partitions 3, `CHECK` 1, others |
| 16 | `language "?" does not exist` | `plpgsql` 10, `sql` 6 |
| 16 | `relation pg_authid cannot be opened` | `CREATE`/`DROP ROLE`, `CREATE USER` |
| 15 | `test_setup could not make a table: type ? ...` | `onek` 8 and `tenk1` 5 (`name`), `point_tbl` 1, `road` 1 |
| 13 | `relation "?" does not exist` | catalogs and views digres lacks (`pg_prepared_statements`, `pg_locks`, `pg_range`, ...), and tables a file made with what failed |
| 9 | `relation pg_default_acl cannot be opened` | `CREATE FUNCTION`, `CREATE AGGREGATE` |
| 9 | `relation pg_type cannot be written` | `CREATE TYPE ... AS ENUM`, `CREATE DOMAIN`, shell types |
| 7 | `invalid input syntax for type ?` | numeric `nan`/`Infinity` 3 (no NaN, by design); time zone abbreviations `EDT`, `zulu`, `MET DST` 4 |
| 7 | `unrecognized configuration parameter "?"` | parallel query settings 5, `max_prepared_transactions`, `track_counts` |
| 5 | `function ?(...) does not exist` | `normalize`, `gisthandler`, `pg_notify`, `pg_stat_force_next_flush`, `jsonb_path_query` |
| 5 | `relation pg_attribute cannot be opened` | `ctid`, `cmin`: system columns |
| 3 | `relation pg_class cannot be opened` | `pg_partition_tree`, `\d+`, `GRANT` on a table |
| 3 | `relation pg_constraint cannot be opened` | `GROUP BY` a primary key, other columns read through it: `select_implicit`, `select_having`, `functional_deps` |
| 3 | `type "?" does not exist` | `textrange`, `textmultirange` (test_setup's), `tsvector` |

### What to do first

By files unblocked for the work, the cheapest first:

1. **The crashes**, before any feature: two bugs, three
   files.
2. **Load the `Default` time zone abbreviations.**
   `timezone_abbreviations` is empty, so `EDT` and `zulu` are
   unknown: 4 files (`time`, `timestamp`, `timestamptz`,
   `horology`), likely a setting and an installed file.
3. **`name`.** 3 files directly, and through `onek` and
   `tenk1`, 13 more: 16 files, the most of any one change.
   It is what `test_setup`'s busiest tables need. (`char(n)`,
   which they needed too, is done: `char` passes, and 8 more
   files got past it.)
4. **Accept and ignore `WITH (fillfactor, autovacuum_enabled)`**
   on `CREATE TABLE`: sm has no pages to fill. 5 files.
5. **`CREATE TABLE AS`**, as a `CREATE TABLE` and an
   `INSERT ... SELECT`: 4 files, and `onek2` and `tenk2` in
   `test_setup`.
6. **The geometric types**, `point`, `box`, `path` and the
   rest: some 12 files, and `POINT_TBL`, `person`, `road`.

Bigger, and each needs a decision of record first:

- **Roles** (16 files): `pg_authid` is compiled in, and roles
  are the instance's, which a database must not depend on.
- **Views** (10 files, and many more past their first error):
  rules have nowhere to be kept.
- **SQL functions** (`sql` 6 files, `CREATE FUNCTION` 8 more)
  and **`CREATE TYPE`/`DOMAIN`** (9): refused by design now.

Not worth doing for the suite: the parallel query settings
(7 files) were removed with their module, and the plans those
files print would differ anyway; `INHERITS`, partitions and
`ctid` are refused by design.

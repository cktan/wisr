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

## Results, 2026-10-17

At `f8de5b6`, an assertion build at `-O0` on x86_64; 230 files
in 3.5 minutes. Two runs gave the same summary, causes,
crashes and setup.

- **17 pass.** 14 run in full: `boolean`, `char`, `varchar`,
  `text`, `int2`, `int4`, `int8`, `md5`, `time`, `comments`,
  `delete`, `json_encoding`, `jsonpath`, `jsonpath_encoding`.
  Three skip themselves on this database's encoding and
  locale, as they do on PostgreSQL: `collate.icu.utf8`,
  `collate.linux.utf8`, `collate.windows.win1252`.
- **212 fail, 1 crash, no timeouts.**

### Crashes

One, a segmentation fault, so the log has no `TRAP:` line:

| files | statement |
|---|---|
| transactions | a cursor that read a table of sm's inside a savepoint, used after `ROLLBACK TO SAVEPOINT`: its next `FETCH`, its `CLOSE`, or the commit that closes it |

It is new since `name` let `tenk1` be made. Not after
`RELEASE SAVEPOINT`, not when the cursor read nothing inside
the savepoint, and not for a cursor over `generate_series()`:

```
create table k (a int4);
insert into k select generate_series(1, 100);
begin;
declare c cursor for select a from k;
savepoint one;
fetch 2 from c;
rollback to savepoint one;
fetch 2 from c;                  -- the server stops
```

The suite found three assertions before, all fixed:

| assertion | files | statement | fixed by |
|---|---|---|---|
| `lockmode != NoLock \|\| CheckRelationLockedByMe(...)`, relation.c:65 | insert_conflict, create_index, guc, temp, truncate | DDL on a descriptor cached in an earlier transaction | `WORKNOTE.md` step 188 |
| `DatumGetObjectId(ldatum) == table_oid`, execCurrent.c:212 | portals, rowsecurity | `DELETE` / `UPDATE ... WHERE CURRENT OF` | `cb2cbac` |
| `!TTS_EMPTY(slot)`, execTuples.c:1267 | foreign_key | `UPDATE` of a self-referencing key `ON UPDATE CASCADE`, in a transaction | `36ed57b` |

Bugs that are not crashes:

- Fixed: in `strings`, `SUBSTRING ... SIMILAR` with no match
  failed with `function 0x... returned NULL` (step 196); psql
  18's `\d` failed with `relation 2662 cannot be opened` when
  its plan called `pg_partition_ancestors()` (step 198); `time`
  passes since the time zone abbreviations are compiled in
  (step 199).
- Fixed: in `temp`, once a temporary table that hid a table of
  the same name was dropped, `DROP INDEX i_temptest` did not
  find the other table's index of that name: the first `DROP
  INDEX` had dropped it, not the temporary one (step 202).
  `temp` stops next at an expression index.

A file that reads what an earlier file of the schedule made
fails here, as each runs in a database of its own:
`horology` reads `timestamp`'s `TIMESTAMP_TBL`.

### What test_setup cannot do

Every file starts with these failures, so 10 of `test_setup`'s
tables are missing in every file:

- `POINT_TBL` and `person` (`point`), `road` (`path`): types
  not supported yet.
- `onek2` and `tenk2`: `CREATE TABLE AS`.
- `emp`, `student`, `stud_emp`, `ihighway`, `shighway`:
  `INHERITS`.
- `GRANT ALL ON SCHEMA public` (`pg_namespace`), the
  tablespace, `synchronous_commit` and
  `allow_in_place_tablespaces`, the `sql` functions, the enum
  and range types and the operator classes.

`onek` and `tenk1` are made since `name` (step 197).

### The causes that stop the most files

| files | cause | what it is |
|---|---|---|
| 26 | `type ? is not supported yet` | `box` 3, `oid` 2, `point` 2, and 19 more one file each: the other geometric types, `bit`, `money`, `pg_lsn`, `timetz`, `cidr`, `macaddr`, `xid8`, `jsonb`, `xml`, arrays, ranges |
| 24 | `a relation's storage cannot be made` | views 12 (one inside `CREATE SCHEMA`), `CREATE TABLE AS` and `SELECT INTO` 7, `CREATE TYPE ... AS (...)` 5 |
| 20 | `CREATE TABLE supports only columns, ...` | `INHERITS` 6, `WITH (fillfactor, autovacuum_enabled)` 5, partitions 4, `CHECK` 1, others 4 |
| 17 | `language "?" does not exist` | `plpgsql` 12, `sql` 5 |
| 17 | `relation pg_authid cannot be opened` | `CREATE`/`DROP ROLE`, `CREATE USER` |
| 15 | `relation "?" does not exist` | catalogs and views digres lacks (`pg_prepared_statements`, `pg_locks`, `pg_range`, ...); tables a file made with what failed; and `horology`'s `timestamp_tbl`, which `timestamp` makes (below) |
| 9 | `relation pg_default_acl cannot be opened` | `CREATE FUNCTION`, `CREATE AGGREGATE` |
| 9 | `relation pg_type cannot be written` | `CREATE TYPE ... AS ENUM`, `CREATE DOMAIN`, shell types |
| 7 | `unrecognized configuration parameter "?"` | parallel query settings 5, `max_prepared_transactions`, `track_counts` |
| 5 | `column "?" does not exist` | `ctid` 4, `xmin` 1: gone since `a7cd3ab`, by design (a row is named by its rowno) |
| 5 | `function ?(...) does not exist` | `normalize`, `gisthandler`, `pg_notify`, `pg_stat_force_next_flush`, `jsonb_path_query` |
| 5 | `invalid input syntax for type ?` | numeric `nan`, `Infinity`, `-Infinity`: no NaN or infinity, by design (`DECIMAL.md`) |
| 3 | `test_setup could not make a table: a relation's storage ...` | `onek2` 2, `tenk2` 1: `CREATE TABLE AS` |
| 3 | `test_setup could not make a table: type ? ...` | `point_tbl`, `person` (`point`), `road` (`path`) |
| 3 | `an index key is a column, with no collation or operator class, yet` | `create_index`, `btree_index`, `plpgsql` |
| 3 | `ALTER TABLE supports only ADD COLUMN, ...` | `vacuum`, `identity`, `strings` |
| 3 | `relation pg_constraint cannot be opened` | `GROUP BY` a primary key, other columns read through it: `select_implicit`, `select_having`, `functional_deps` |
| 3 | `(results differ; no new error)` | `regex`, `sanity_check`, `select_distinct_on` |
| 3 | `type "?" does not exist` | `textrange`, `textmultirange` (test_setup's), `tsvector` |

### What to do first

By files unblocked for the work, the cheapest first:

1. **The crash**, before any feature: one bug, one file.
2. **`CREATE TABLE AS` and `SELECT INTO`**, as a `CREATE
   TABLE` and an `INSERT ... SELECT`: 7 files directly, and
   through `onek2` and `tenk2`, 3 more: 10 files, the most of
   any one change now.
3. **Accept and ignore `WITH (fillfactor, autovacuum_enabled)`**
   on `CREATE TABLE`: sm has no pages to fill. 5 files.
4. **The geometric types**, `point`, `box`, `path` and the
   rest: 10 files directly, and 3 more through `POINT_TBL`,
   `person` and `road`.

Bigger, and each needs a decision of record first:

- **Roles** (17 files): `pg_authid` is compiled in, and roles
  are the instance's, which a database must not depend on.
- **Views** (12 files, and many more past their first error):
  rules have nowhere to be kept.
- **SQL functions** (`sql` 5 files, `CREATE FUNCTION` 8 more)
  and **`CREATE TYPE`/`DOMAIN`** (9): refused by design now.

Not worth doing for the suite: the parallel query settings
(7 files) were removed with their module, and the plans those
files print would differ anyway; `INHERITS` and partitions are
refused by design.

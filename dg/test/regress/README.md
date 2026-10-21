# PostgreSQL 18's regression suite on digres

`test/regress.sh` (`make -j4 regress`) runs every file of
PostgreSQL 18's `src/test/regress/parallel_schedule` on digres
and reports which missing features cost the most files. It
only measures: it exits 0 whatever it finds, and is in no
`make test`. Most files fail, by design (`../../PLAN.md`,
`../../../sm/NEXT.md`); the point is to know why.

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

## Results, 2026-10-18

At `9b60be7`, an assertion build at `-O0` on aarch64, against
PostgreSQL 18.6's tree as REL_18_STABLE has it (`4732a5c53bd`);
230 files in 4 minutes.

- **19 pass.** 16 run in full: `boolean`, `char`, `varchar`,
  `md5`, `time`, `comments`, `copyselect`, `select_implicit`,
  `select_having`, `delete`, `psql_crosstab`, `portals_p2`,
  `bitmapops`, `json_encoding`, `jsonpath`,
  `jsonpath_encoding`. Three skip themselves on this
  database's encoding and locale, as they do on PostgreSQL:
  `collate.icu.utf8`, `collate.linux.utf8`,
  `collate.windows.win1252`.
- **211 fail, no crash, no timeouts.**

Against 2026-10-17's run: six files pass that did not
(`copyselect`, `select_implicit`, `select_having`,
`psql_crosstab`, `portals_p2`, `bitmapops`), and four that
passed fail (`text`, `int2`, `int4`, `int8`, below).

### Crashes

None. The suite has found these, all fixed:

| assertion | files | statement | fixed by |
|---|---|---|---|
| `lockmode != NoLock \|\| CheckRelationLockedByMe(...)`, relation.c:65 | insert_conflict, create_index, guc, temp, truncate | DDL on a descriptor cached in an earlier transaction | `WORKNOTE.md` step 188 |
| `DatumGetObjectId(ldatum) == table_oid`, execCurrent.c:212 | portals, rowsecurity | `DELETE` / `UPDATE ... WHERE CURRENT OF` | `cb2cbac` |
| `!TTS_EMPTY(slot)`, execTuples.c:1267 | foreign_key | `UPDATE` of a self-referencing key `ON UPDATE CASCADE`, in a transaction | `36ed57b` |
| a segmentation fault | transactions | a cursor that read a table of sm's inside a savepoint, used after `ROLLBACK TO SAVEPOINT` | `WORKNOTE.md` step 204 |
| `!IsA(node, SubLink)`, prepagg.c:331 | subselect | a whole-row reference to a join over a flattened subquery, in a sub-select | PostgreSQL's `7a776fc9d28`, `WORKNOTE.md` step 254 |
| `!have_unsafe_outer_join_ref(...)`, joinpath.c:776 | join | a lateral reference to an outer join's output | PostgreSQL's `cbec572918f`, step 254 |
| `list_length(root->join_rel_level[...]) == 1`, allpaths.c:2488 | join | an outer join commuted through a PlaceHolderVar | PostgreSQL's `d7938ff3085`, step 254 |

The last three are PostgreSQL's own bugs, fixed in
REL_18_STABLE after 18.6, which digres forked from; step 254
takes every planner fix since.

### Bugs that are not crashes

- Fixed since this run: `int4` and `text` met two of
  PostgreSQL's bugs, fixed after 18.6, which REL_18_STABLE's
  expected output shows: `to_char(3, '9V999999999')` gave
  `-1294967296`, not `integer out of range`
  (`ec434db1f49`), and `right('ahoj', -2147483648)` gave
  `ahoj`, not the empty string (`bfb08d0946e`). Both are taken
  (`../../WORKNOTE.md` step 255): `text` passes, and `int4`
  fails only as `int2` and `int8` do, below.
- Fixed since this run: `int2`, `int4` and `int8` printed
  `-2.5::numeric` as `-2.500`, a bare numeric being
  `numeric(17, 3)` everywhere since the decimals were
  reworked; it keeps its value's scale now, and only a column
  of it is `numeric(17, 3)` (`../../WORKNOTE.md` step 256).
  The three pass.
- Fixed before: `SUBSTRING ... SIMILAR` with no match
  (step 196), psql 18's `\d` and `pg_partition_ancestors()`
  (step 198), the time zone abbreviations (step 199), and
  `DROP INDEX` after a temporary table that hid another
  (step 202).

A file that reads what an earlier file of the schedule made
fails here, as each runs in a database of its own:
`horology` reads `timestamp`'s `TIMESTAMP_TBL`.

### What test_setup cannot do

Every file starts with these failures, so 8 of
`test_setup`'s tables are missing in every file:

- `POINT_TBL` and `person` (`point`), `road` (`path`):
  geometric types, not supported by decision
  (`../../PLAN.md` §7).
- `emp`, `student`, `stud_emp`, `ihighway`, `shighway`:
  `INHERITS`.
- `GRANT ALL ON SCHEMA public` (`pg_namespace`), the
  tablespace, `synchronous_commit` and
  `allow_in_place_tablespaces`, the `sql` functions, the enum
  and range types and the operator classes.

`onek2` and `tenk2` are made since `CREATE TABLE AS`
(step 203).

### The causes that stop the most files

| files | cause | what it is |
|---|---|---|
| 31 | `type ? is not supported yet` | `oid`, `box`, `point` 3 each, `"char"` 2, and 20 more one file each: the other geometric types, `bit`, `money`, `pg_lsn`, `timetz`, `cidr`, `macaddr`, `xid8`, `jsonb`, `xml`, arrays, ranges |
| 20 | `language "?" does not exist` | `plpgsql` 13, `sql` 7 |
| 19 | `relation pg_authid cannot be opened` | `CREATE`/`DROP ROLE`, `CREATE USER` |
| 14 | `relation "?" does not exist` | catalogs and views digres lacks (`pg_prepared_statements` 2, `pg_range`, `pg_locks`, `pg_shdepend`, ...); tables a file made with what failed (`b_star`, `box_tbl`, `street`); and `horology`'s `timestamp_tbl` |
| 12 | `CREATE TABLE supports only columns, ...` | `INHERITS` 6, partitions 4, `CHECK` 1, one more |
| 9 | `relation pg_default_acl cannot be opened` | `CREATE FUNCTION`, `CREATE AGGREGATE` |
| 8 | `relation pg_type cannot be written` | `CREATE TYPE ... AS ENUM`, `CREATE DOMAIN`, shell types |
| 7 | `(results differ; no new error)` | `text`, `int2`, `int4`, `int8` (above), `regex`, `sanity_check`, `select_distinct_on` |
| 7 | `a relation's storage cannot be made` | `CREATE TYPE ... AS (...)` 6, `CREATE MATERIALIZED VIEW` 1 |
| 7 | `unrecognized configuration parameter "?"` | parallel query settings 5, `max_prepared_transactions`, `track_counts` |
| 6 | `column "?" does not exist` | `ctid` 5, `xmin` 1: gone since `a7cd3ab`, by design (a row is named by its rowno) |
| 6 | `function ?(...) does not exist` | `normalize`, `gisthandler`, `pg_notify`, `pg_stat_force_next_flush`, `jsonb_path_query`, `pg_numa_available` |
| 6 | `invalid input syntax for type ?` | numeric `nan`, `Infinity`, `-Infinity` 5: no NaN or infinity, by design (`../../DECIMAL.md`); a float with `_` 1 |
| 5 | `test_setup could not make a table: type ? ...` | `person` 3, `point_tbl` 1 (`point`), `road` 1 (`path`) |
| 4 | `ALTER TABLE supports only ADD COLUMN, ...` | `strings`, `insert`, `vacuum`, `identity` |

### What to do first

Nothing cheap is left: the four files that passed on
2026-10-17 and failed here pass again (steps 255, 256).

Done since 2026-10-17: the crash in `transactions`;
`CREATE TABLE AS` and `SELECT INTO` (step 203), which make
`onek2` and `tenk2`; `WITH (fillfactor, autovacuum_enabled)`
accepted (step 208); views (`../../../sm/VIEW.md`); an index
key naming its type's default operator class or its column's
collation (step 274), which leaves `insert_conflict` at
`text_pattern_ops` and `temp` at an expression key.

Bigger, and each needs a decision of record first:

- **Roles** (19 files): `pg_authid` is compiled in, and roles
  are the instance's, which a database must not depend on.
- **SQL functions** (`sql` 7 files, `CREATE FUNCTION` 9 more)
  and **`CREATE TYPE`/`DOMAIN`** (8, and 6 composite types):
  refused by design now.

Not worth doing for the suite: the parallel query settings
(7 files) were removed with their module, and the plans those
files print would differ anyway; `INHERITS` is outside the
subset wisr targets (`../../../sm/FEATURELIST.md` §4), and the
geometric types are refused by design (`../../PLAN.md` §7):
10 files directly, and 5 more through `POINT_TBL`, `person`
and `road`. Partitions are in that subset, but not built.

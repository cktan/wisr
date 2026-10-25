# SYSCATALOG — the catalog tables, readable again

psql's `\dt`, `\d`, `\l` and the rest are SQL: queries on
`pg_class`, `pg_namespace` and their kind. digres has no
catalog tables, so each fails. This file is the design for
making the catalog tables readable relations again, with no
storage behind them, so that those queries run as they are
sent.

**It is being built**, a step at a time, and §6 tracks what is
done. It is in the area of the catalog lookups, which are being
moved to the compiled-in rows step by step (`WORKNOTE.md` step 6
and after).

---

## 1. Where digres stands

Where it stood when this was written, on 2026-10-14; §6 is
what runs now.

- **psql connects and runs SQL.** `server.sh` drives the
  server through it.
- **A command that describes objects fails.** Tried with
  psql 16 on a new data directory:

  | Command | What it reads | Today |
  | --- | --- | --- |
  | `\dt`, `\ds` | `pg_class`, `pg_namespace`, `pg_am` | `relation "pg_catalog.pg_class" does not exist` |
  | `\di` | those, and `pg_index` | the same |
  | `\dn` | `pg_namespace` | `relation "pg_catalog.pg_namespace" does not exist` |
  | `\l` | `pg_database` | `relation "pg_catalog.pg_database" does not exist` |
  | `\du` | `pg_roles` | `relation "pg_catalog.pg_roles" does not exist` |
  | `\d name` | `pg_class`, then a dozen more (§4) | fails on the first |

- **They also call functions.** Checked on 2026-10-14:

  | Function | Today | Called by |
  | --- | --- | --- |
  | `pg_table_is_visible`, `format_type`, `pg_encoding_to_char`, `array_to_string`, `current_schemas` | run | `\dt`, `\d`, `\l` |
  | `pg_get_userbyid` | opens `pg_authid` (1260) | almost every command, for the owner |
  | `pg_get_indexdef` | opens `pg_index` (2610) | `\d name`, for its indexes |
  | `obj_description`, `pg_relation_size`, `pg_table_size` | not defined | `\dt+`, `\d+` |
- **A catalog table cannot be opened.** Its name is not found:
  `get_relname_relid()` asks sm, which has no table named
  `pg_class`. Its oid is refused: `ScanPgRelation()` raises
  `relation 1259 cannot be opened: digres has no catalog
  tables` (`relcache.c`).
- **What a catalog table would say is already at hand**, in
  two places:
  - the rows compiled in from PostgreSQL's `.dat` files
    (`catalog/cat_builtin.h`): namespaces, types, functions,
    operators, casts, access methods, operator classes and
    families, aggregates, ranges and collations;
  - sm's catalog, for what a user makes: namespaces, tables
    and their columns, indexes, sequences, and databases
    (`sm_cat_*`, `sm_dbid`).
- **The columns of some are compiled in** as descriptors:
  `schemapg.h` has `Schema_pg_class`, `Schema_pg_attribute`,
  `Schema_pg_type`, `Schema_pg_proc`, `Schema_pg_index`,
  `Schema_pg_database`, `Schema_pg_authid` and a few more.
  `pg_statistic` is already read through one
  (`statistic_tupdesc()`, step 76).

---

## 2. Decisions

- **The catalog tables are relations with no storage.** A
  catalog table has its PostgreSQL name, oid and columns, and
  can be named in a query. A scan of one calls a function
  that makes its rows; nothing is read from disk and nothing
  is stored. Called a **catalog relation** here.
- **The queries are not converted.** psql's SQL differs by
  command and by psql's version, and every other client --
  pgAdmin, DBeaver, JDBC's metadata, an ORM -- sends its own.
  A readable `pg_class` serves them all. Recognising one
  client's text and answering it by hand would serve one, and
  follow its releases for ever.
- **Nothing is stored for them.** `sm_mkdb` makes no
  `pg_class`, and DDL writes nothing but sm's catalog: one
  catalog, and no second copy to keep in step with it. A
  catalog relation is a name, an oid and columns, compiled in.
- **A scan goes through a table access method of its own**,
  as a table in sm goes through `smtable.c`'s: its scan fills
  a tuple store from the catalog's generator and reads it, its
  writes are refused, and it tells the planner its size. So
  the executor's and the planner's paths are PostgreSQL's, and
  no new branch is made in `nodeSeqscan.c`.
- **They are not views.** A system view is SQL over the
  catalogs, which `initdb` no longer runs; a catalog relation
  gives a client the same thing. A user's view is sm's
  (`../sm/VIEW.md`), and is listed in them as §3 says.
- **A row comes from the two sources of §1**, the compiled-in
  rows first and sm's after:
  - a built-in object's row is its compiled-in one;
  - a user's object's row is made from sm's definition of it,
    as the session's transaction sees it, so a table made in
    an open transaction is listed there and nowhere else.
- **The session's temporary tables are listed**, in `pg_temp`
  (`PG_TEMP_NAMESPACE`), and no other session's.
- **Read only.** INSERT, UPDATE, DELETE and TRUNCATE of one are
  refused. DDL goes on changing sm's catalog, and the next
  scan shows it.
- **Sequential scan only.** A catalog relation has no index:
  the planner is offered none, and joins them by hash or
  nested loop. They are small. `pg_proc`, the largest, is some
  three thousand rows.
- **A row is made on each scan**, and nothing is cached. If a
  query on them is ever slow, that is when to cache.
- **A scan for one relation makes that relation's rows alone.**
  A qual of a filter column's oid equal to a constant or a
  parameter -- `pg_class.oid`, `pg_attribute.attrelid`,
  `pg_attrdef.adrelid`, `pg_index.indexrelid` and `indrelid` --
  is handed to the access method as a scan key, and the rows
  are made for that relation, its table and its table's
  indexes. The quals are still checked. A join clause is not:
  the planner hands a sequential scan none.
- **The server's own lookups come this way only for a
  syscache miss in a catalog of compiled-in rows alone**:
  `pg_am`, `pg_cast`, `pg_collation`, `pg_language`,
  `pg_operator`, `pg_proc` and `pg_type`
  (`catrel_is_builtin()`). `systable_beginscan()` opens no index
  on one, and the scan makes only the rows its keys pass. Those
  rows are fixed, so a cached one is never stale, and they are
  PostgreSQL's own, with no fixed value standing in for one
  digres has not. A catalog with sm's rows in it (`pg_class`,
  `pg_attribute`, `pg_index`, `pg_namespace`) is not looked up
  this way: the parser, planner and executor go on reading sm
  directly, and a syscache lookup in one fails, until its caller
  is converted.
- **A column with nothing true to say** has a fixed value,
  listed by catalog in §4: the owner is the superuser, an ACL
  is null, a tablespace is 0. `reltuples` is -1 and
  `relpages` 0, a table never analyzed as PostgreSQL has it
  (`class_row()` in `catrel.c`); the planner takes sm's
  estimates (`smstore_estimate_size()`), not these.
- **A catalog that digres has nothing of is empty**, and still
  readable: `pg_trigger`, `pg_policy`, `pg_inherits`,
  `pg_publication`. `\d` reads them, and an empty answer is
  the right one.

---

## 3. The mechanism

Read against the code on 2026-10-14 (step 1 of §5).

- **The name is found.** `get_relname_relid()` asks sm alone
  (`lsyscache.c`, `smstore_relation_oid()`). It is to answer a
  catalog relation's oid from a compiled-in list first, for
  `PG_CATALOG_NAMESPACE`, and for no namespace when the
  search path reaches `pg_catalog`.
- **The relation is opened.** `RelationBuildDesc()` sends an
  oid from `FirstNormalObjectId` up to sm
  (`RelationBuildSmDesc()`, `RelationBuildSmIndexDesc()`), and
  every other one to `ScanPgRelation()`, which refuses it:
  `relation %u cannot be opened`. A catalog relation's oid is
  to be built from its compiled-in columns before that
  refusal. Its `relkind` is `r`, its namespace `pg_catalog`,
  and its `rd_tableam` the catalog relations' access method.
- **Its columns are compiled in for ten.** `schemapg.h` has
  `pg_attribute`, `pg_auth_members`, `pg_authid`, `pg_class`,
  `pg_database`, `pg_index`, `pg_proc`, `pg_shseclabel`,
  `pg_statistic` and `pg_type` (`pg_subscription` went in
  `WORKNOTE.md` step 214). The rest --
  `pg_namespace`, `pg_am`, `pg_attrdef`, `pg_constraint`,
  `pg_collation`, `pg_inherits`, `pg_trigger` and the others
  of §4 -- need one each, written by `gen_cat.py` from their
  headers.
- **A scan is the access method's.** A catalog relation's oid
  is below `FirstNormalObjectId`, so `RelationIsSm()` is false
  and `nodeSeqscan.c` takes PostgreSQL's path,
  `table_beginscan()` and `table_scan_getnextslot()`. The
  access method, beside `smtable.c`, begins a scan by filling
  a tuple store from the catalog's generator, and reads a
  virtual tuple from it at each call.
- **A generator is one function a catalog**, in a file of its
  own under `catalog/`: it is given a tuple store and puts the
  catalog's rows in it. `pg_stats()` is written this way
  (`smstore.c`, step 78).
- **The planner sizes it** through the access method's
  `relation_estimate_size`: a fixed guess a catalog, with no
  statistics.
- **Locks**: none is taken. Its definition cannot change.

---

## 4. What each command needs

From `psql -E` for the commands that got as far as a query,
and from PostgreSQL's `describe.c` for `\d name`, whose later
queries were not reached. To be confirmed as each is made to
run.

| Command | Catalog relations | Functions |
| --- | --- | --- |
| `\dn` | `pg_namespace` | `pg_get_userbyid` |
| `\dt`, `\ds` | `pg_class`, `pg_namespace`, `pg_am` | `pg_get_userbyid`, `pg_table_is_visible` |
| `\di` | those, `pg_index` | the same |
| `\l` | `pg_database` | `pg_get_userbyid`, `pg_encoding_to_char`, `array_to_string` |
| `\du` | `pg_roles`, a view on `pg_authid` | |
| `\d name` | `pg_class`, `pg_namespace`, `pg_attribute`, `pg_attrdef`, `pg_type`, `pg_collation`, `pg_index`, `pg_constraint`, `pg_am`, `pg_rewrite`, and, empty: `pg_inherits`, `pg_trigger`, `pg_policy`, `pg_statistic_ext`, `pg_publication` and its two relations | `format_type`, `pg_get_indexdef`, `pg_get_constraintdef`, `pg_get_expr`, `pg_table_is_visible`, `pg_get_viewdef` |
| `\dv` | `pg_class`, `pg_namespace` | `pg_get_userbyid`, `pg_table_is_visible` |

What a row says, for the first of them:

- **`pg_namespace`**: the compiled-in ones, then sm's
  (`sm_cat_list_namespaces`). `nspowner` is the superuser;
  `nspacl` is null.
- **`pg_class`**: a row a table, an index, a view and a
  sequence of sm's, and one a catalog relation.
  `relnamespace`, `relname`, `relkind`, `relnatts`,
  `relpersistence` and `relam` are true; `relowner` is the
  superuser; `relfilenode` is the oid, and 0 for a view;
  `reltablespace` is 0; `relhasindex` is whether sm has an
  index of it; `relhasrules` is true of a view alone; `relacl`
  and `reloptions` are null.
- **`pg_attribute`**: a view's columns are its query's select
  list, as the relation cache makes them.
- **`pg_rewrite`**: a row a view of sm's, its `_RETURN` rule,
  whose oid is the view's, and whose `ev_action` is the query
  sm keeps. `pg_get_viewdef()` reads it, over SPI, as it
  always has.
- **`pg_views`**: a view of sm's, a row, its definition
  `pg_get_viewdef()`'s; a view in PostgreSQL, a catalog
  relation here, as `pg_roles` is.
- **`pg_am`**: the compiled-in rows (`cat_am`).
- **`pg_database`**: a row a database of sm's; its oid is
  `FirstNormalObjectId` plus its `dbid` (`CODE.md` §5).
  Encoding is UTF-8, and the locale columns the server's.
- **`pg_roles`**: one row, the superuser, until there are
  roles.

---

## 5. Order of work

Each step lands on its own, with a check in `server.sh` that
runs the psql command and reads its output.

1. **Read the code**: confirm the touch points of §3, and
   which functions of §4 open a catalog table.
2. **The mechanism, with `pg_namespace` alone**: the name
   found, the relation opened, the scan calling a generator,
   writes refused; and `pg_get_userbyid()` naming the
   superuser with no catalog. `\dn` runs.
3. **`pg_class` and `pg_am`**: `\dt` and `\ds` run.
4. **`pg_index`**, and `pg_get_indexdef()` made from sm's
   definition: `\di` runs.
5. **`pg_database`, and `pg_roles`**: `\l` and `\du` run.
6. **`\d name`**: `pg_attribute`, `pg_type`, `pg_attrdef`,
   `pg_collation`, `pg_constraint`, the empty ones, and the
   functions that print a definition.
7. **The rest of the compiled-in catalogs** (`pg_proc`,
   `pg_operator`, `pg_cast` and the others), for `\df`, `\do`
   and a client that reads them.

---

## 6. Status

| Step | State |
| --- | --- |
| 1. Read the code | done, 2026-10-14: §1's functions, §2's access method, §3 |
| 2. Mechanism, `pg_namespace`, `\dn` | done, `WORKNOTE.md` step 112 |
| 3. `pg_class`, `pg_am`, `\dt`, `\ds` | done, `WORKNOTE.md` step 113 |
| 4. `pg_index`, `\di` | done, `WORKNOTE.md` step 114 |
| 5. `pg_database`, `pg_roles`, `\l`, `\du` | done, `WORKNOTE.md` steps 115 and 118; psql 16's `\l` asks for a column PostgreSQL 17 renamed |
| 6. `\d name` | done: a table and an index, `WORKNOTE.md` step 115; a sequence, step 119; `\d+`, step 120; sizes, step 121 |
| 7. The other compiled-in catalogs | `pg_proc`, `pg_operator`, `pg_cast`, `pg_extension`, `pg_description`: `\df`, `\do`, `\dC`, `\dx`, step 123 |
| 8. A syscache miss in a compiled-in catalog | `pg_language` too; `pg_get_functiondef()`, `\sf`, step 124 |
| 9. Views | `pg_rewrite`, `pg_views`, `\dv`, `\d+` of a view, `pg_get_viewdef()`, step 239 |
| 10. Foreign keys | `pg_constraint`'s rows of type `f`, `relhastriggers`, `\d`'s Foreign-key constraints and Referenced by, `pg_get_constraintdef()`, step 246; a primary key's and a unique constraint's, `\d`'s UNIQUE CONSTRAINT, step 248 |

---

## 7. Left out

- **`information_schema`**: views over the catalogs, in SQL.
  Views are sm's now (`../sm/VIEW.md`), so it can be made of
  views in each database, or written as catalog relations of
  its own; neither is done.
- **The statistics views** (`pg_stat_activity`,
  `pg_stat_user_tables`, `pg_locks`): each is a function
  behind a view, and a separate piece.
- **`pg_stats`** stays the function it is (step 78), until
  this gives it a relation's name.
- **An index on a catalog relation**, and a cache of its rows.
- **Roles and privileges**: every owner is the superuser and
  every ACL null until digres has roles (`PLAN.md` §7).
- **Tab completion in psql**, which sends queries of its own
  on the same catalogs: it should come with steps 3 to 6, and
  is not checked here.

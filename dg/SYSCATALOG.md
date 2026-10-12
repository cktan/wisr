# SYSCATALOG — the catalog tables, readable again

psql's `\dt`, `\d`, `\l` and the rest are SQL: queries on
`pg_class`, `pg_namespace` and their kind. digres has no
catalog tables, so each fails. This file is the design for
making the catalog tables readable relations again, with no
storage behind them, so that those queries run as they are
sent.

**It is a proposal, and not built.** It is in the area of the
catalog lookups, which are being moved to the compiled-in rows
step by step (`WORKNOTE.md` step 6 and after), so it is to be
agreed before it is begun. §6 tracks what is done.

---

## 1. Where digres stands

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

- **They also call functions**: `pg_table_is_visible()`,
  `pg_get_userbyid()`, `pg_encoding_to_char()`,
  `array_to_string()`. Each is to be checked: one that looks
  in a catalog table fails as the query would.
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
- **They are not views.** A view is a stored rule
  (`pg_rewrite`), which is among what was cut. A catalog
  relation gives a client the same thing.
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
- **The server's own lookups do not come this way.** The
  parser, planner and executor go on reading the compiled-in
  rows and sm directly (`lsyscache.c`, `cat_builtin.h`). A
  catalog relation is for a query a client sends, and no code
  in the server scans one.
- **A column with nothing true to say** has a fixed value,
  listed by catalog in §4: the owner is the superuser, an ACL
  is null, a tablespace is 0. `reltuples` and `relpages` are
  sm's estimates (`smstore_estimate_size()`), and -1 before
  an ANALYZE, as PostgreSQL has it.
- **A catalog that digres has nothing of is empty**, and still
  readable: `pg_trigger`, `pg_policy`, `pg_inherits`,
  `pg_rewrite`, `pg_publication`. `\d` reads them, and an
  empty answer is the right one.

---

## 3. The mechanism

Not read against the code yet in detail; the touch points are
from a first look, and step 1 of §5 is to confirm them.

- **The name is found.** `get_relname_relid(name,
  PG_CATALOG_NAMESPACE)` answers a catalog relation's oid from
  a compiled-in list, before it asks sm.
- **The relation is opened.** `RelationBuildDesc()` builds the
  descriptor of a catalog relation from its compiled-in
  columns, as `RelationBuildSmDesc()` builds a table's from
  sm's, in place of `ScanPgRelation()`'s refusal. Its
  `relkind` is `r` and its namespace `pg_catalog`.
- **Its columns are compiled in.** `schemapg.h` has a
  descriptor for the catalogs marked `BKI_SCHEMA_MACRO`. The
  others need one each: either the mark on their header, or
  `gen_cat.py` writing every catalog's.
- **A scan calls its generator.** `nodeSeqscan.c` already
  branches for a table in sm (`RelationIsSm()`); a catalog
  relation is a third case, whose scan fills a tuple store
  from the catalog's generator when it starts, and reads rows
  from it.
- **A generator is one function a catalog**, in a file of its
  own under `catalog/`: it is given a tuple store and puts the
  catalog's rows in it. `pg_stats()` is written this way
  (`smstore.c`, step 78).
- **The planner sizes it** by a fixed guess a catalog, with no
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
| `\d name` | `pg_class`, `pg_namespace`, `pg_attribute`, `pg_attrdef`, `pg_type`, `pg_collation`, `pg_index`, `pg_constraint`, `pg_am`, and, empty: `pg_inherits`, `pg_trigger`, `pg_policy`, `pg_rewrite`, `pg_statistic_ext`, `pg_publication` and its two relations | `format_type`, `pg_get_indexdef`, `pg_get_constraintdef`, `pg_get_expr`, `pg_table_is_visible` |

What a row says, for the first of them:

- **`pg_namespace`**: the compiled-in ones, then sm's
  (`sm_cat_list_namespaces`). `nspowner` is the superuser;
  `nspacl` is null.
- **`pg_class`**: a row a table, an index and a sequence of
  sm's, and one a catalog relation. `relnamespace`, `relname`,
  `relkind`, `relnatts`, `relpersistence` and `relam` are
  true; `relowner` is the superuser; `relfilenode` is the oid;
  `reltablespace` is 0; `relhasindex` is whether sm has an
  index of it; `relacl` and `reloptions` are null.
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
   writes refused. `\dn` runs.
3. **`pg_class` and `pg_am`**: `\dt` and `\ds` run.
4. **`pg_index`**: `\di` runs.
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
| 1. Read the code | not begun |
| 2. Mechanism, `pg_namespace`, `\dn` | not begun |
| 3. `pg_class`, `pg_am`, `\dt`, `\ds` | not begun |
| 4. `pg_index`, `\di` | not begun |
| 5. `pg_database`, `pg_roles`, `\l`, `\du` | not begun |
| 6. `\d name` | not begun |
| 7. The other compiled-in catalogs | not begun |

---

## 7. Left out

- **`information_schema`**: views over the catalogs, in SQL.
  It waits for views, or is written as catalog relations of
  its own.
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

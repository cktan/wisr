# Known issues

Problems found and left unfixed, each with what it would take
to fix. The code it concerns points here by number. A fixed
issue is removed, and its number is not used again: issue 2,
ON CONFLICT DO UPDATE affecting a row twice, was fixed in
`WORKNOTE.md` step 237; issue 3, a numeric range in an array
or a multirange losing its bound, in step 300; and issue 6, a
duplicate key in a table made in a savepoint saying the wrong
error, in step 302.

---

## Issue 1: a library a session loads shares its statics

A PostgreSQL extension keeps its state in static variables and
sets it in `_PG_init()`, once per backend: each backend is a
process, with its own copy. digres's sessions are threads of
one process, and `dlopen` loads a library once for all of them,
so its statics are one for the process. A library a session
loads after it starts, by `LOAD` or a C function's first call,
breaks:

- **Each session's load runs `_PG_init()` again** on the same
  statics. The usual hook chain, `prev_planner_hook =
  planner_hook; planner_hook = my_planner;`, keeps one
  `prev_planner_hook`: the last session to load the library
  sets it, from its own hooks (`tcb->hooks`), and every other
  session's `my_planner` then calls that session's previous
  hook, a library it may never have loaded.
- **The write is a data race** with any other session calling
  the hook meanwhile.

`shared_preload_libraries` is safe: `_PG_init()` runs once, in
the postmaster, and each session starts with a copy of what it
set (`session_thread_start()`: `tcb->hooks`, `tcb->dfmgr`,
`tcb->explain`).

**Loading is disabled until it is redesigned.** dfmgr.c's
`internal_load_library()` refuses every library with an error,
`loading library "..." is not supported yet`: `LOAD`, a C
function, `shared_preload_libraries` and the session's preload
settings alike. It asserted that no session thread loaded one,
which took the server down on PostgreSQL's
`create_function_c`. No library is built in the tree, so
nothing digres runs loaded one.

**The fix is to redesign dynamic loading and redo it**, from
one of:

- **Load libraries only through `shared_preload_libraries`**,
  and refuse a session's `LOAD` and later loads with an error,
  not an assert. Simple and safe; a session loses `LOAD`.
- **Require a digres extension to keep its state
  `thread_local`.** `LOAD` stays; every extension must change.
- **Run `_PG_init()` once per process**, with process-wide
  hooks under a lock. Undoes the per-session hooks
  (`WORKNOTE.md` step 222).

---

The issues below were found by triaging pgregress's
`--dg-differs` statements (`test/pgregress/TRIAGE.md`). Each
repro runs in psql on an empty database, the statements on
psql's input (`psql -f -`), not in one `-c`; each says what
PostgreSQL 18 answers and what digres does. None is diagnosed
yet: "where to start" is where the difference shows, not a
cause found.

---

## Issue 4: a cursor sees its own transaction's later DELETE

A cursor reads with the snapshot it was declared with, so a
row the transaction deletes afterwards is still the cursor's,
as `MOVE BACKWARD ALL` and a FETCH show. digres's cursor loses
it. A wrong answer.

```sql
create table uc (f1 int, f2 text);
insert into uc values (1, 'one'), (2, 'two'), (3, 'three');
begin;
declare c1 cursor for select * from uc;
fetch 2 from c1;
delete from uc where current of c1;
move backward all in c1;
fetch all from c1;
-- PostgreSQL: 1 one, 2 two, 3 three
-- digres:     1 one, 3 three
commit;
```

pgregress: `portals`. **Where to start**: what a cursor's scan
reads when it is rewound, and at which statement mark
(`sm1::stmt_advance`): the DECLARE's would keep the row, the
transaction's latest would not.

## Issue 5: a FOR UPDATE cursor returns a row updated after it was declared

In a SERIALIZABLE transaction, a `FOR UPDATE` cursor declared
before an UPDATE of its only row returns no row in
PostgreSQL: the row it would lock was changed by a later
command of its own transaction. digres returns the row as it
was.

```sql
create table cursor (a int);
insert into cursor values (1);
begin;
set transaction isolation level serializable;
declare c1 no scroll cursor for select * from cursor for update;
update cursor set a = 2;
fetch all from c1;
-- PostgreSQL: (0 rows)
-- digres:     1
commit;
```

pgregress: `portals`. **Where to start**: the row lock a FOR
UPDATE fetch takes, and what it does on finding the row
updated by a later command of the same transaction
(PostgreSQL's `TM_SelfModified`).

## Issue 7: ON COMMIT DELETE ROWS ignores a foreign key into the table

PostgreSQL refuses to commit when a table that empties at
commit is referenced by a foreign key of a table that does
not, as emptying it would break the key. digres commits and
empties it, leaving a child row with no parent.

```sql
begin;
create temp table t (a int primary key) on commit delete rows;
create temp table u (a int references t) on commit preserve rows;
insert into t values (1);
insert into u values (1);
commit;
-- PostgreSQL: ERROR:  unsupported ON COMMIT and foreign key
--             combination
-- digres:     commits; u keeps 1, t is empty
```

pgregress: `temp`. **Where to start**:
`PreCommit_on_commit_actions()` in
`src/backend/commands/tablecmds.c` truncates each table by
`smstore_truncate()`; PostgreSQL's calls `heap_truncate()`,
which first calls `heap_truncate_check_FKs(rels, true)`, the
check digres skips.

## Issue 8: a table can be made in pg_catalog

```sql
create table pg_catalog.new_system_table();
-- PostgreSQL: ERROR:  permission denied to create
--             "pg_catalog.new_system_table"
-- digres:     CREATE TABLE
```

pgregress: `alter_table`. **Where to start**: PostgreSQL makes
the check in `heap_create()` (`src/backend/catalog/heap.c`,
`IsCatalogNamespace()` unless `allow_system_table_mods`),
which digres's CREATE TABLE, made in sm, does not reach.

## Issue 9: ON CONFLICT under REPEATABLE READ says sm1's error

```sql
create table sc (f1 int primary key, f2 int);
begin isolation level repeatable read;
insert into sc values (5, 1), (5, 2)
  on conflict (f1) do update set f2 = 0;
-- PostgreSQL: ERROR:  ON CONFLICT DO UPDATE command cannot
--             affect row a second time
-- digres:     ERROR:  could not insert a row: sm1_read_latest
--             requires READ COMMITTED
commit;
```

So does `ON CONFLICT DO NOTHING` under REPEATABLE READ or
SERIALIZABLE. pgregress: `insert_conflict`. **Where to
start**: ON CONFLICT reads the key's latest version
(`sm1_read_latest`, `sm/sm1/src/access.cc`), which sm1 allows
only under READ COMMITTED; digres should refuse ON CONFLICT
at the other levels with an error of its own, or read
otherwise.

## Issue 10: CREATE TABLE (LIKE a, LIKE a) says sm's error

```sql
create table inhx (xx text default 'text');
create table inhf (like inhx, like inhx);
-- PostgreSQL: ERROR:  column "xx" specified more than once
-- digres:     ERROR:  could not create a table: create table: a
--             column without a name, or two with one: xx
```

pgregress: `create_table_like`. **Where to start**: digres
does not check the merged column list for a repeated name
before it hands it to sm, as PostgreSQL's
`MergeAttributes()` does.

## Issue 11: a foreign key keeps a SET DEFAULT column twice

```sql
create table pk (tid int, id int, primary key (tid, id));
create table fk (tid int, d int default 0,
  foreign key (tid, d) references pk on delete set default (d, d));
select pg_get_constraintdef(oid) from pg_constraint
  where conrelid = 'fk'::regclass and contype = 'f';
-- PostgreSQL: ... ON DELETE SET DEFAULT (d)
-- digres:     ... ON DELETE SET DEFAULT (d, d)
```

pgregress: `foreign_key`. **Where to start**: PostgreSQL
removes duplicates from the column list
(`validateFkOnDeleteSetColumns()`); the list digres keeps in
sm3's foreign key is the one given.

## Issue 12: the hash access method says it cannot exclude

```sql
select pg_indexam_has_property(a.oid, 'can_exclude')
  from pg_am a where amname = 'hash';
-- PostgreSQL: t
-- digres:     f
```

pgregress: `amutils`. **Where to start**: the `hash` row of
`pg_am` and the routine its handler returns
(`amcanexclude`).

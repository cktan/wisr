# VIEW — views: sm3 keeps the query, the host expands it

A view is a named query. sm3 keeps the query and what it reads,
and the host expands it. The code is in `sm3/src/ddl/ddl.cc`
(DDL), `sm3/src/catalog/` (the entry, the session's views and
the register of §12.3), `src/sm.cc` (the calls of §6), and
`../dg/src/backend/commands/view.c` and
`../dg/src/backend/access/sm/smstore.c` (digres).

**Status: sm3's part and `sm.h`'s are built** (§3 to §7,
`sm3/test/test_view.cc`), as proposed in §11, and digres's
CREATE, query and DROP (§8,
[`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 238),
the catalog relations (step 239), and ALTER VIEW's RENAME and
a view's options (step 240), and temporary views (§12, step
245). Not yet: the rest of §9 ([`NEXT.md`](NEXT.md#2-loose-ends)
§2).

Before this, `CREATE VIEW` failed in digres: `DefineView()`
made a PostgreSQL relation, which digres has no catalog for.
It was what digres alone failed of SQLite's corpus: 48,608
records in the whole of it, 24,408 in the sample `make
slt-corpus` runs ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
164), every one a
`CREATE VIEW` refused or a query of the view it did not make.
`information_schema` and the views of `system_views.sql`,
`pg_stats` among them, still wait
([`../dg/SYSCATALOG.md`](../dg/SYSCATALOG.md#7-left-out) §7,
[`../dg/STATISTICS.md`](../dg/STATISTICS.md#25-pg_stats-as-a-function-for-now)
§2.5).

This file is the design: a new kind of entry in sm3's catalog,
what DDL does to it and to the relations a view reads, the
calls in `sm.h`, and what digres builds on them.

---

## 1. What is whose

- **sm3 keeps the view**: its name, in a namespace, as a
  relation's; its query, as the host serialized it, bytes sm3
  never reads, as an index's predicate is kept
  (`sm_cat_create_index`'s `eval`); and the relations and
  columns the query reads.
- **sm3 keeps the view true to what it reads**: a table, a
  column or a view a view reads cannot be dropped from under
  it (§5). This is the one dependency check sm3 must make;
  the others wait for a dependency index (§9,
  [`CATALOG.md`](CATALOG.md#11-not-built) §11).
- **The host expands it.** In digres that is PostgreSQL's
  rewriter, unchanged: the view's relation cache entry carries
  its `_RETURN` rule, made from the query sm3 keeps, and
  `fireRIRrules()` puts the query in place of the view.
- **The host derives the view's columns** from its query's
  select list, as PostgreSQL's `DefineVirtualRelation()` does
  before it writes them to `pg_attribute`. sm3 keeps no
  columns of a view, so a view's column may be of any type
  the host has, one sm2 does not know among them.

## 2. What PostgreSQL keeps of one

| Where | What | Here |
| --- | --- | --- |
| `pg_class` | name, namespace, `relkind` `v`, `reloptions` | the entry's name and namespace; its options, a host blob |
| `pg_attribute` | the columns | derived by the host from the query |
| `pg_type` | the view's row type | none: a view's row is a record, as a table's in sm is |
| `pg_rewrite` | the `_RETURN` rule: `ev_action`, the query as `nodeToString()` writes it | the entry's query, a host blob |
| `pg_depend` | what the rule reads: relations, columns, functions, types, collations | the relations and columns alone (§3) |

- **Columns by attnum, relations by oid.** A query tree names
  a table by its oid and a column by its place, so renaming
  either changes no view, in PostgreSQL and here.
- **The oids in the query are the database's or compiled
  in**: its tables', views' and collations', which a copied
  `base/X` keeps, and the functions', operators' and types',
  the same in every instance of one version
  ([`../dg/CODE.md`](../dg/CODE.md)). A view's blob therefore
  moves with its
  database, as an index's `eval` and a column's default do,
  and like them is tied to the version's node format.

## 3. The entry

A new kind in [`CATALOG.md`](CATALOG.md#33-keys) §3.3, and a part
in §3:

| Kind | Value | Key after the kind | Holds |
| --- | --- | --- | --- |
| `VIEW` | 13 | oid `int8` | a view: its namespace, name, query, options, and what it reads |

```
VIEW value = view | read × nread | query × npiece
```

| Part | Fields |
| --- | --- |
| view | namespace `int8`, name `text`, options `bytea` or null, nread `int4`, npiece `int4` |
| view read | relation `int8`, attnum `int4` |
| view query | a piece of the query, `bytea`, at most 60,000 bytes |

- **A read is a relation and a column**: a table's column,
  0-based as sm's attnums are, or -1 for the relation as a
  whole: a view, a table read with no column named
  (`SELECT count(*) FROM t`), or a whole-row reference. The
  host gives every relation the query reads, each table
  column once, as `recordDependencyOnExpr()` finds them.
- **Its name is a relation's**: a `NAME` key in its namespace,
  kind `VIEW`, shared with the namespace's tables, indexes and
  sequences, so a name taken fails with `SM_ERR_DUPKEY`.
- **The query is in pieces**, in order. A part is an sm2
  tuple, at most 65,535 bytes, and a query is not bounded:
  one of SQLite's corpus serializes to 100 KB.
- **The query and the options are opaque**: kept and handed
  back, never run, never parsed. Options are the host's
  `reloptions`: `check_option`, `security_barrier`,
  `security_invoker`.
- **A new kind changes no key's layout**: `META`'s version
  stays 4, as when `SEQUENCE` and `CONSTRAINT` came.

## 4. In memory

- **A view's definition is in memory**, published as a
  table's is: `view_def_t`, its namespace, name, query,
  options, reads and a version. A table's is in memory so DDL
  and a host's lookup need no read of the store; a view's is
  for the same reasons, and for the reverse lookup below.
- **What reads a relation is derived, never stored**: found by
  a pass over the views in memory, as an index's name is found
  by a pass over the tables. A table's own definition does not
  change, so a view made on a table moves no version of the
  table's, and invalidates no host's cache of it (unlike a
  foreign key, [`FOREIGNKEY.md`](FOREIGNKEY.md#4-in-memory) §4).
- **The version moves** with each `replace_view` and
  `rename_view`, so a host's cache of the view is seen to be
  stale ([`DESIGN.md`](DESIGN.md#5-cross-level-decisions) §5, a
  cache is kept fresh by a version).
- **A namespace knows it holds views**, so its drop is
  refused while it does, as with its tables and sequences.

## 5. What DDL does

- **`create_view(session, nspoid, name, def, oid)`** checks
  that each read names a table or a view, and a table's
  column live; takes each such relation's oid lock IS, for
  the transaction, so none is dropped before the view
  commits (§7); and puts the entry and its name.
- **`replace_view(session, oid, def)`**, SQL's `CREATE OR
  REPLACE VIEW`, under the view's lock X: a new query,
  options and reads, checked and locked as `create_view`'s.
  Whether the new columns may replace the old is the host's
  to check (`checkViewColumns()`): sm3 does not know them.
- **`rename_view(session, oid, name)`**, under the view's lock
  X: its `NAME` key moves. Nothing that reads it changes.
- **`drop_view(session, oid)`**, under its lock X, dels the
  entry and its name. One another view reads is
  `SM_ERR_INUSE`: the host drops those first for `CASCADE`.
- **`drop_table`** of a table a view reads is
  `SM_ERR_INUSE`, as of a foreign key's parent.
- **`drop_column`** of a column a view reads is
  `SM_ERR_INUSE`. A read of -1 does not hold every column: a
  whole-row reference reads the row as it is when queried, as
  PostgreSQL's does.
- **`drop_namespace`** of one holding a view is
  `SM_ERR_INUSE`.
- **`alter_column_type`** of a column a view reads is
  `SM_ERR_INUSE`.
- **`add_column`, `rename_table`, `rename_column`,
  `truncate_table`** change nothing of a view.

## 6. The calls in `sm.h`

```c++
/* A relation a view reads: a table's column, or -1 for all of it. */
struct sm_view_read_t {
    uint64_t relation;
    int32_t  attnum;   /* 0-based, or -1 */
};

/* A view: its namespace, name, the host's query and what it reads. */
struct sm_view_def_t {
    uint64_t                    nspoid;
    std::string                 name;
    std::string                 query;    /* the host's, opaque */
    std::string                 options;  /* the host's, or empty */
    std::vector<sm_view_read_t> reads;
    uint64_t                    version = 0;
};
```

- **`sm_cat_create_view(session, nspoid, name, def, &oid)`**,
  **`sm_cat_replace_view(session, oid, def)`**,
  **`sm_cat_rename_view(session, oid, name)`**,
  **`sm_cat_drop_view(session, oid)`**: §5. `def`'s name,
  namespace and version are ignored.
- **`sm_cat_lock_view(session, oid, mode, &version)`**: the
  view's oid lock in `mode`, held until the transaction ends,
  and its version under the lock; 1 if there is no such view.
  This is a query's lock on a view, as `sm_tbl_open` is on a
  table.
- **`sm_cat_view_oid(session, nspoid, name, &oid)`**,
  **`sm_cat_view_def(session, oid, def)`**,
  **`sm_cat_list_views(session, nspoid, names)`**: as the
  session's transaction sees them, with no lock, as
  `sm_cat_table_def_oid` reads; 1 if there is none.
- **`sm_cat_view_readers(session, oid, views)`**: the views
  that read the relation `oid`, a table or a view, directly,
  sorted; for `CASCADE`, and for the message that names them.
- Errors as the rest of `sm_cat_*`: `SM_ERR_INVALID`,
  `SM_ERR_NOTFOUND`, `SM_ERR_DUPKEY`, `SM_ERR_INUSE`.

## 7. Locks

- **A view has an oid lock**, sm1's `LOCKTYPE_OID` on its oid,
  as a table has ([`CATALOG.md`](CATALOG.md#1-design) §1). A
  query takes it IS,
  DDL on the view X: a `DROP VIEW` or `CREATE OR REPLACE`
  waits for the transactions reading it, and they for it, as
  PostgreSQL's `AccessShareLock` and `AccessExclusiveLock` do.
- **A view's DDL takes what it reads IS**, as a reader does,
  so a writer of those tables is not kept out, and a
  `DROP TABLE` (X) waits for the view's transaction to end,
  then finds the view and is refused. The other order, the
  drop first, makes the view's DDL wait, then fail on a table
  gone. No dependency is missed in either.
- **The tables a view reads are locked by the query**, not
  by the view: the rewriter's `AcquireRewriteLocks()` locks
  each by its oid as it expands it, IS through
  `smstore_lock_relation()`.

## 8. What digres builds on it

- **`CREATE [OR REPLACE] VIEW`**: `DefineView()` keeps
  PostgreSQL's parse analysis and checks, aliases and
  `WITH CHECK OPTION` among them; `DefineVirtualRelation()`
  calls `smstore_create_view()` or `smstore_replace_view()`
  in place of `DefineRelation()` and `StoreViewQuery()`. The
  query is `nodeToString()` of a list of the analyzed `Query`,
  as `ev_action` holds it; the reads are found by the walker
  `recordDependencyOnExpr()` uses, through
  `find_expr_references()`, its relations and columns kept and
  the rest dropped.
- **Lookup**: `smstore_relation_oid()` finds a view after a
  table, an index and a sequence.
- **The relation cache**: `RelationBuildSmDesc()` builds a
  view's entry from `sm_cat_view_def`: `relkind` `v`, no
  storage and no access method; its columns from the query's
  select list, its non-junk entries' names, types, typmods and
  collations; `rd_rules`, one rule, `_RETURN`, `ON SELECT DO
  INSTEAD` the query, read by `stringToNode()`; `rd_options`
  from the options. `rd_smversion` is the view's version.
- **The lock**: `smstore_lock_relation()` of a view calls
  `sm_cat_lock_view` and `RelationCheckSmVersion()`, so an
  entry built before another session's `CREATE OR REPLACE` is
  rebuilt, and a cached plan over it, which lists the view
  among its relations, is planned again.
- **The rewriter, the planner and the executor** are
  PostgreSQL's, unchanged: an automatically updatable view's
  INSERT, UPDATE and DELETE are rewritten onto its table in
  sm, `WITH CHECK OPTION` checked by the executor.
- **`DROP VIEW [IF EXISTS] ... [CASCADE]`**, and **`DROP TABLE
  ... CASCADE`**, `ALTER TABLE ... DROP COLUMN ... CASCADE`
  and `DROP SCHEMA ... CASCADE`: the views reading what is
  dropped are found by `sm_cat_view_readers`, transitively,
  and dropped first, readers before what they read. Without
  `CASCADE`, PostgreSQL's error: `cannot drop table t because
  other objects depend on it`, a `DETAIL` line naming each
  view, and its `HINT`.
- **`ALTER VIEW ... RENAME TO`** is `sm_cat_rename_view`, and
  `ALTER TABLE ... RENAME TO` of a view the same;
  `ALTER VIEW ... RENAME COLUMN` a replace with the select
  list's names changed, as PostgreSQL renames a view's column
  in `pg_attribute` alone.
- **A view's options** are its `reloptions`, kept in sm as
  `untransformRelOptions()`'s list of them; the relation cache
  makes `rd_options` of them, so the rewriter's check option,
  `security_barrier` and `security_invoker` are PostgreSQL's.
  `ALTER VIEW ... SET` and `RESET` replace them.
- **The catalog relations**
  ([`../dg/SYSCATALOG.md`](../dg/SYSCATALOG.md)): a view's
  `pg_class` row, `relkind` `v`, `relhasrules` true; its
  `pg_attribute` rows; and its `pg_rewrite` row, the rule's
  oid the view's own, `ev_action` its query. ruleutils.c's
  `pg_get_viewdef()` reads that row over SPI, so psql's `\dv`
  and `\d+ v` work with no change to it.
- **Tests**: `../dg/test/slt/view.test`, `view_alter.test`,
  `view_catalog.test` and `view_temp.test`; SQLite's corpus's
  views, `evidence/slt_lang_createview.test`,
  `slt_lang_dropview.test` and a part of `index/view/`, paths
  in the corpus `make slt-corpus` fetches
  (`../dg/test/slt_corpus.sh`), not files here; the
  regression file `create_view` as a measure; and in
  `server.sh`, two sessions: a replace seen by the other's
  next statement, a drop waiting for a reader, a `DROP TABLE`
  refused for a view made in a transaction open meanwhile.

## 9. Not here

- **Materialized views**: they have storage, and are refused
  as they are now ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
  64).
- **`INSTEAD OF` triggers and user rules** (`CREATE RULE`):
  sm keeps no triggers; a view that is not automatically
  updatable refuses DML, as PostgreSQL does with neither.
- **Dependencies on what is not a relation**: a sequence a
  view's `nextval('s')` names, a user's collation. They wait
  for a dependency index ([`CATALOG.md`](CATALOG.md#11-not-built)
  §11), as the
  same checks for tables do ([`NEXT.md`](NEXT.md#3-not-blocking)
  §3).
- **A view's row type**: a view's whole-row value is a
  record, as a table's in sm is, not a named composite type.
- **`ALTER VIEW ... ALTER COLUMN SET DEFAULT` and `SET
  SCHEMA`**: refused until asked for. `OWNER TO` is built:
  `smstore_alter_table()` changes a view's owner as a table's.
- **One lookup for a relation's name**: `NAME` already says
  which kind a name is, and `smstore_relation_oid()` asks for
  each kind in turn. A call that reads the key once would
  serve every kind; a view adds a fourth ask, not a reason
  to change that now.

## 10. Order of work

1. **sm3**: the kind and its parts, the four DDL calls, the
   lock, the readers' pass, and the refusals of §5, with tests
   in `sm3/test/test_view.cc`: the dependencies, two sessions
   racing a view's create and a table's drop both ways, a
   crash and reopen. `sm.h` and its wrapper → verify: `make
   test` in `sm/`.
2. **digres: CREATE, query, DROP**: §8 to the relation cache,
   the lock, `DROP VIEW` and the cascades → verify: the slt
   view files pass; `make slt-corpus` shows the 24,408
   records gone; `make test`.
3. **digres: what reads the catalog**: the catalog relations,
   `\dv`, `\d+`, `pg_get_viewdef()` → verify: `server.sh`.
4. **digres: the rest of §8**: `ALTER VIEW ... RENAME`,
   updatable views and `WITH CHECK OPTION` → verify: the
   regression file `create_view`, measured.
5. **Temporary views**: §12.7.

## 11. For review

- **A lock of the view's own (§7), or none.** A sequence
  takes none: its record is read in the transaction. A view
  could do the same, its cache checked against a version read
  without a lock. The cost of none: a `DROP VIEW` or `CREATE
  OR REPLACE` does not wait for a transaction reading the
  view, and a REPEATABLE READ reader may expand a view whose
  table another session has since dropped, its query then
  failing on a missing relation. Proposed: the lock, as
  PostgreSQL and a table have it, at one lock a view a query.
- **In memory (§4), or in the store, as a sequence is.** The
  store alone would need a scan of every `VIEW` entry at each
  `DROP TABLE` and `DROP COLUMN` to find a view that reads
  it; SQLite's corpus makes views by the hundred. Proposed: in
  memory, where that is a pass over the views with no read of
  the store.
- **Columns derived by the host (§1), or kept by sm3.** Kept,
  they would let a catalog read list a view's columns
  without parsing its query, and need sm3 to know every
  column's type, which a view's select list may make of any
  type the host has. Proposed: derived; parsing a view's
  query for `pg_attribute` is cheap beside the scan.
- **Reads at the column (§3), or the relation alone.** The
  relation alone would let `DROP COLUMN` take a column a view
  reads, its queries then failing on a dropped column.
  Proposed: columns, as `pg_depend` keeps them.
- **The rule's oid is the view's.** `pg_rewrite` rows need an
  oid, unique among themselves; a view has one rule, so its
  oid serves, and no oid is allocated for it.

---

## 12. Temporary views

**Status: built**, as proposed in §12.8: in sm3
(`sm3/test/test_view.cc`) and in digres
(`../dg/test/slt/view_temp.test`, `../dg/test/server.sh`,
[`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 245).

A temporary view is a session's, as a temporary table is: no
other session sees it, and it is gone when the session ends.
PostgreSQL makes one by `CREATE TEMPORARY VIEW`, and makes a
view temporary when it reads a temporary table or view,
saying so in a notice. It is the largest part of what
`create_view` still fails on
([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 240).

### 12.1 What PostgreSQL does

- **It may read anything**: a temporary table or view of its
  session, and a permanent table or view. A permanent view
  may read no temporary relation; one that would is made
  temporary.
- **What it reads is kept true for it**, as for a permanent
  view, by `pg_depend`, which every session shares. So
  another session's `DROP TABLE t` of a permanent table that
  a temporary view reads is refused, naming that view:

  ```
  ERROR:  cannot drop table pt because other objects depend on it
  DETAIL:  view pg_temp_1.tv depends on table pt
  ```

- **CASCADE drops the other session's view.** The drop
  succeeds, and the view's own session next finds no
  relation of that name. Checked on PostgreSQL 18, with two
  sessions.

### 12.2 The view: the temporary catalog

- **A temporary view is in the session's temporary catalog**
  ([`TEMPTABLE.md`](TEMPTABLE.md#4-sm3-the-temporary-catalog)
  §4), in namespace 0 (`sm3::nsp_temp`), as a
  temporary table is: made in the transaction's overlay, kept
  in the temporary catalog from commit to commit, dropped
  when the session closes, and written nowhere. Nothing of it
  is in store 0, so nothing survives a crash, and a database
  copied as its directory has none.
- **`create_view` takes namespace 0** and makes one. Its oid
  comes from `sm3.oid`, as a temporary table's does.
- **Its name** is one of the session's temporary names,
  shared with its temporary tables and their indexes, and
  checked in memory.
- **No lock.** `cat_lock_view` of a temporary view returns
  its definition and takes nothing, as `cat_lock_table` does
  for a temporary table: no other session can reach it.
- **What it reads is checked** as a permanent view's is
  (§5), and each permanent relation it reads is locked IS to
  the end of the transaction. A temporary relation takes no
  lock.
- **A permanent view still reads no temporary relation**:
  `SM_ERR_INVALID`, as now. The host makes such a view
  temporary first, as PostgreSQL does.
- **In its own session it is a reader like any other**:
  `cat_view_readers` lists it, so the session's own drop of a
  table, a column or a view it reads is refused or cascades
  as §5 and §8 say, a temporary table's among them.

### 12.3 The hazard: another session's drop

A temporary view that reads a permanent relation is in no
catalog another session reads. Another session's `DROP
TABLE` of that relation would find no reader, drop it, and
leave the view reading nothing. §12.1 says PostgreSQL refuses
the drop; for that, the other session must learn of the view.

- **The shared catalog keeps a register of temporary
  readers**: every session's committed temporary views, by
  oid, each with its session and its definition, so its name
  and what it reads, columns and all. A view that reads only
  temporary relations is in it too, and harmless: no other
  session has a relation of an oid it reads. It is in memory,
  and never in store 0, as the views it names are not. It is
  all a session learns of another's temporary views.
- **It changes at commit**, with the temporary catalog: a
  commit that makes, replaces or drops a temporary view
  changes the register under the shared catalog's mutex, as
  a commit of shared DDL does
  ([`CATALOG.md`](CATALOG.md#4-in-memory) §4). Such a
  commit counts as shared DDL.
- **A session's close takes its entries out**, under the
  same mutex, as it drops its temporary catalog.
- **`cat_view_readers` lists them**, so the checks of §5 on a
  permanent relation see another session's temporary views,
  though `cat_get_view` does not find them:
  `drop_table`, `drop_column` and `drop_view` are
  `SM_ERR_INUSE` while one reads it.
- **No race**, by the locks of §7: the view's create holds
  the relation IS until it commits, and the register changes
  before that commit's locks are seen to go, so a drop that
  waited for them finds the view; a drop that came first
  holds X, and the create waits, then finds no relation.

### 12.4 CASCADE: another session's view

Of the three ways to answer `DROP TABLE pt CASCADE` while
another session's temporary view reads `pt`, the first is
proposed:

- **Drop it, as PostgreSQL does.** The drop records, in its
  transaction's overlay, that the other session's view goes.
  At its commit the register loses the view's entries, and
  the other session's list of views dropped by others gains
  it, under the mutex. That session takes the list at its
  next `begin`, and drops each view from its temporary
  catalog, with every temporary view of its own that reads
  one, directly or through others: one made on a doomed view
  while the drop was under way, which the drop could not
  see, as its make took no lock. Until then it sees none of
  them, in a transaction or out of one, as PostgreSQL's
  session sees no relation another has dropped: a view kept
  to its next `begin` would read a relation that is gone.
  A query on the view
  meanwhile takes `pt` IS: the drop waited out any that held
  it, and one that comes later waits for the drop's X, then
  finds `pt` gone, as it would in PostgreSQL. A drop rolled
  back records nothing.
- **It takes no lock** on the other session's view, which
  has none: that session alone uses it, and drops it itself
  at its next `begin`.
- **Refuse it**, a divergence: `cannot drop table pt because
  view pg_temp.tv of another session depends on it`, with
  CASCADE too. Simpler by the list above, and a session never
  loses a view to another.
- **Let it dangle**: no register; the view's next query
  fails on the dropped relation. Simplest, and wrong where
  PostgreSQL is right.

### 12.5 The calls in `sm.h`

- **`sm_cat_create_view`** takes `SM_NSP_TEMP` for a
  temporary view.
- **`sm_cat_view_readers`** gives, for each reader, whether
  it is another session's, its name and what it reads, which
  `sm_cat_view_def` cannot give for a view the caller does
  not see: a reader becomes a struct, `sm_view_reader_t`,
  its oid, its name, its reads, for a column's drop to ask,
  and that flag. A view the transaction has dropped is no
  reader. sm3's `cat_view_readers` gives `view_reader_t`,
  the view's definition and the flag.
- **`sm_cat_drop_view`** of another session's temporary view,
  an oid from the register, is §12.4's drop. Of any other oid
  it does what it does now.

### 12.6 What digres builds on it

- **`DefineVirtualRelation()`** stops refusing a temporary
  view, and passes `SM_NSP_TEMP` for one in `pg_temp`, as
  `smstore_create_table()` does for a temporary table.
- **The relation cache** makes a temporary view's entry in
  `pg_temp`, `relpersistence` `t`, `rd_islocaltemp`.
- **The lookups and the listing** ask `SM_NSP_TEMP` as they
  do for a temporary table: `smstore_relation_oid()` already
  asks sm for a view in it, and `smstore_relations()` adds
  the session's temporary views to `pg_class`.
- **The messages** name another session's view as
  `pg_temp.tv`: digres has one temporary namespace,
  `pg_temp`, for every session, where PostgreSQL has one a
  backend, `pg_temp_1`.
- **`DISCARD TEMP`** and the session's end drop its
  temporary views, readers first, before its temporary
  tables (`smstore_drop_temp_tables()`).

### 12.7 Order of work

1. **sm3: the temporary view**, §12.2: namespace 0 in
   `create_view`, the temporary catalog's views, their
   names and no lock, with tests in
   `sm3/test/test_view.cc` → verify: `make test` in `sm/`.
2. **sm3: the register**, §12.3, and its tests: a temporary
   view's create racing another session's drop both ways, a
   close taking its entries out → verify: the same.
3. **sm3: the cascade**, §12.4, and `sm.h`, §12.5: the
   other session's view gone at its next transaction, and
   kept if the drop rolls back → verify: the same.
4. **digres**, §12.6 → verify: an slt file of a session's
   own temporary views; `server.sh` for another session's
   drop, refused and cascaded; `create_view` measured;
   `make test`, `make tsan`.

### 12.8 For review

- **CASCADE across sessions (§12.4)**: drop the other
  session's view, as PostgreSQL does, or refuse. Proposed:
  drop it. Refusing saves the list of views dropped by
  others and its handling at `begin`, about a quarter of the
  work, and leaves a `DROP ... CASCADE` that PostgreSQL runs
  failing in digres.
- **A register at all (§12.3)**, or let a view dangle.
  Proposed: the register. It is small, in memory alone, and
  without it a drop PostgreSQL refuses goes through.
- **Columns in the register**, so another session's
  `DROP COLUMN` of a column a temporary view does not read
  goes through, as PostgreSQL's does. Proposed: kept, as a
  permanent view's reads keep them.


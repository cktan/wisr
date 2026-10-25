# FOREIGNKEY — foreign keys: sm3 keeps them, the host checks them

**Status: built**: sm3's part and `sm.h`'s (§3 to §6,
`sm3/test/test_fkey.cc`), digres's (§8, `../dg/WORKNOTE.md`
step 177), and `rel`'s verbs (`rel/REL.md`,
`rel/test/test_fkey.rel`). Not yet: a DEFERRABLE one.

`DESIGN.md` §5 settles whose a foreign key is: the host's.
PostgreSQL checks one with triggers that run queries, and the
stack runs none; its share is the lock on the parent row,
`sm_tbl_lock`. What it did not have was somewhere to keep the
constraint, and digres refused `REFERENCES` and `ALTER TABLE
... ADD FOREIGN KEY` for that reason alone.

This file is that place: a new kind of entry in sm3's catalog,
what DDL does to it and to the tables it joins, and the calls in
`sm.h`. §8 outlines what digres then builds on it.

---

## 1. What is whose

- **sm3 keeps the constraint**, and keeps it true to the tables
  it joins: a table, an index or a column it uses cannot be
  dropped from under it (§5).
- **The host checks the rows.** An insert or an update of the
  child looks the parent row up and locks it; a delete or an
  update of the parent's key looks for children and acts as the
  constraint says. In digres these are PostgreSQL's own RI
  triggers (`ri_triggers.c`), unchanged, over SPI.
- **The host checks the rows already there** when a constraint
  is added to a table that has some, in the same transaction,
  and tells sm3 whether it did (`validated`).
- **sm3 interprets what it must, and keeps the rest opaque**:
  the tables, the columns at each end and the parent's unique
  index are its, since DDL must keep them true; the actions,
  the match type and whether it is deferrable are kept as
  fields, for a reader of the catalog, but sm3 acts on none of
  them; the comparison operators are the host's, in a blob.

## 2. What PostgreSQL needs of one

PostgreSQL keeps a foreign key in `pg_constraint`, and its four
or more RI triggers in `pg_trigger`. Of `pg_constraint`'s row
these are the ones a foreign key uses:

| Field | Here |
| --- | --- |
| `oid`, `conname`, `connamespace` | the constraint's oid and name; the child's namespace |
| `conrelid`, `conkey` | the child table, and its columns |
| `confrelid`, `confkey` | the parent table, and its columns |
| `conindid` | the parent's unique index whose keys are `confkey` |
| `confupdtype`, `confdeltype` | the actions: `a` no action, `r` restrict, `c` cascade, `n` set null, `d` set default |
| `confmatchtype` | `s` simple, `f` full |
| `condeferrable`, `condeferred` | deferrable, and initially deferred |
| `convalidated` | the rows already there were checked |
| `conpfeqop`, `conppeqop`, `conffeqop` | the equality operators per column pair: host blob |
| `confdelsetcols` | `SET NULL (cols)`: host blob |

`pg_trigger`'s rows are derived: one set on the child, one on
the parent, each naming the constraint. The host makes them from
the constraint and keeps none (§8).

## 3. The entry

**A new kind, `CONSTRAINT`, 12**, keyed by the child table's oid
and the constraint's oid, as an index is keyed by its table's
(`sm3/CATALOG_ONDISK.md` §2):

| Kind | Value | Key after the kind | Holds |
| --- | --- | --- | --- |
| `CONSTRAINT` | 12 | table oid `int8`, constraint oid `int8` | one constraint of the table: a foreign key, and a `CHECK` later |

```
CONSTRAINT value = constraint | fkey | pair × npairs
```

| Part | Fields |
| --- | --- |
| constraint | oid `int8`, name `text`, type `int2` (`f`), validated `bool`, host `bytea` or null |
| fkey | parent table `int8`, parent index `int8`, npairs `int4`, on update `int2`, on delete `int2`, match `int2`, deferrable `bool`, deferred `bool` |
| pair | child attnum `int4`, parent attnum `int4` |

- **The child's, because it is the child's constraint**: it is
  `conrelid`'s in PostgreSQL, its name is unique among the
  child's constraints, and `DROP CONSTRAINT` names it on the
  child. Nothing is written under the parent.
- **The kind is a constraint's, not a foreign key's.** A `CHECK`
  is the other kind of constraint; it would be type `c`, with a
  part of its own in place of `fkey`, the host's expression in
  `host`. It is not built here.
- **Its name is not a `NAME` key.** A constraint's name is unique
  per table, not per namespace, in PostgreSQL; sm3 checks it
  against the child's constraints, under the child's lock X,
  and refuses a taken one with `SM_ERR_DUPKEY`.
- **A new kind changes no key's layout**, so `META` stays at 4
  (`sm3/CATALOG_ONDISK.md` §3).
- **Its oid** is from `cat_new_oid()`, as a table's and an
  index's.

## 4. In memory

- **`table_def_t` gains its constraints**: `fkeys`, each with
  its pairs, from the table's `CONSTRAINT` entries; and
  `referenced_by`, the foreign keys of other tables, and of
  itself, whose parent it is, as (child table, constraint) oids.
- **`referenced_by` is derived, never stored.** `cat_open`
  builds it with one pass over the `CONSTRAINT` entries, and
  each DDL that adds or drops a foreign key puts the parent's
  definition back with it changed. This is the reverse lookup a
  `DROP TABLE` of a parent and the host's parent-side triggers
  need, with no dependency index (`sm3/CATALOG_INMEM.md` §4: a
  dependency is derived, and the catalog is in memory).
- **A constraint is found by its oid alone**, as PostgreSQL's
  `CONSTROID` syscache finds one. There is no map from
  constraint oid to table: `cat_get_fkey` scans the tables'
  `fkeys`, the transaction's own changes first, then the
  session's temporary tables, then the shared catalog's.
- **Both tables' versions move**: adding or dropping a foreign
  key publishes both definitions, and validating or renaming one
  the child's, so a host's cache of either, the parent's triggers
  among it, is seen to be stale
  (`DESIGN.md` §5, a cache is kept fresh by a version).
- **Temporary tables**: a foreign key between two of one
  session's temporary tables is kept in its temporary catalog
  and written nowhere (`TEMPTABLE.md` §4); one between a
  temporary table and a permanent one is refused,
  `SM_ERR_INVALID`, as PostgreSQL refuses it.

## 5. What DDL does

- **`create_fkey(session, table, def, oid)`** takes the child's
  and the parent's table-oid locks X, the child first, and
  checks:
  - 1 to 32 pairs, `INDEX_MAX_KEYS`, every column live, at both
    ends;
  - the parent index is the parent's, unique, valid, plain (no
    predicate, no expression), and its key columns are the
    parent's columns of the pairs, in any order, as PostgreSQL
    asks;
  - the name is free among the child's constraints.
  It does not compare the columns' types: an `int4` may
  reference an `int8`, by an operator the host chose. It does
  not read a row: `validated` is the host's word. It puts the
  entry and the parent's definition.
- **`drop_fkey(session, table, oid)`** dels the entry, under both
  locks X.
- **`validate_fkey(session, table, oid)`** sets `validated`, for
  `ALTER TABLE ... VALIDATE CONSTRAINT`, once the host has
  checked the rows.
- **`rename_fkey(session, table, oid, name)`** renames it, under
  the child's lock X.
- **`drop_table`** of a child dels its constraints, and puts each
  parent's definition back without them. Of a parent that
  another table's foreign key references, it is `SM_ERR_INUSE`:
  the host drops the constraints first for `CASCADE`. A table's
  foreign key to itself goes with it.
- **`drop_index`** of a parent index a foreign key uses is
  `SM_ERR_INUSE`.
- **`drop_column`** of a child's column a foreign key uses is
  `SM_ERR_INUSE`; the host drops the constraint first for
  `CASCADE`. A parent's is a key of the parent index, which
  `drop_column` already refuses.
- **`truncate_table`** is unchanged. PostgreSQL refuses to
  truncate a referenced table unless its referencing tables are
  truncated in the same statement; that is the host's to
  check, since only it knows the statement.
- **`rename_table`, `rename_column`** change nothing: a
  constraint names its tables by oid and its columns by attnum.

## 6. The calls in `sm.h`

```c++
/* A foreign key of a table: the table it references, and how. */
struct sm_fkey_def_t {
    uint64_t             oid;
    std::string          name;
    uint64_t             ref_table;   /* the parent */
    uint64_t             ref_index;   /* its unique index on ref_columns */
    std::vector<int32_t> columns;     /* the child's, 0-based places */
    std::vector<int32_t> ref_columns; /* the parent's, pair by pair */
    char                 on_update, on_delete; /* 'a','r','c','n','d' */
    char                 match;                /* 's', 'f' */
    bool                 deferrable, deferred;
    bool                 validated;
    std::string          host;        /* the host's: operators, ... */
};

/* A foreign key of another table, or of this one, that references it. */
struct sm_fkey_ref_t {
    uint64_t table;  /* the child */
    uint64_t fkey;   /* its constraint's oid */
};
```

- **`sm_table_def_t` gains `fkeys` and `referenced_by`.**
- **`sm_cat_create_fkey(session, table, def, &oid)`**,
  **`sm_cat_drop_fkey(session, table, oid)`**,
  **`sm_cat_validate_fkey(session, table, oid)`**,
  **`sm_cat_rename_fkey(session, table, oid, name)`**: §5.
- **`sm_cat_fkey(session, oid, &table, &def)`**: the constraint
  with this oid and its child table, as the session's
  transaction sees them, under the child's lock IS; 1 if there
  is none.
- Errors as the rest of `sm_cat_*`: `SM_ERR_INVALID`,
  `SM_ERR_NOTFOUND`, `SM_ERR_DUPKEY`, `SM_ERR_INUSE`.

## 7. Locks and what they cost

- **Both tables X, for the DDL's transaction.** PostgreSQL takes
  `SHARE ROW EXCLUSIVE` on both, which lets the parent's readers
  through. sm3's DDL takes X on what it changes, and the
  parent's definition changes; so while a foreign key is added,
  and its child's rows checked, the parent can be neither read
  nor written. That is coarser than PostgreSQL, for the length
  of one DDL transaction.
- **A check takes the parent row's lock SH**, `sm_tbl_lock`, as
  `FOR KEY SHARE` is `FOR SHARE` here (`DESIGN.md` §5): an
  update of the parent's other columns waits for the child's
  transaction, as it does today.

## 8. What digres builds on it

Outlined here; designed with the work, in `../dg`.

- **DDL**: `REFERENCES` and `FOREIGN KEY` in `CREATE TABLE`;
  `ALTER TABLE ... ADD`, `DROP`, `VALIDATE` and `RENAME
  CONSTRAINT`, `NOT VALID`; `DROP TABLE ... CASCADE` dropping
  the constraints that reference it first. PostgreSQL's
  messages.
- **The host blob**: the three operator arrays and the `SET
  NULL`/`SET DEFAULT` column list, as `pg_constraint` has them,
  oids of compiled-in operators, the same in every instance
  (`../dg/CODE.md`).
- **The relation cache** builds `rd_fkeylist` and the
  relation's `TriggerDesc` from `fkeys` and `referenced_by`:
  PostgreSQL's RI trigger set, each naming the constraint by
  oid. The lookups `ri_triggers.c` makes, `CONSTROID` and
  `pg_constraint` and `pg_trigger` rows, are answered from
  `sm_cat_fkey` by the lookup layer, as `pg_class` is today
  (`../dg/SYSCATALOG.md`). `pg_constraint` lists each foreign
  key, so `\d` shows it (`../dg/WORKNOTE.md` step 246).
- **The check of rows already there** is PostgreSQL's
  `RI_Initial_Check`, over SPI, before `validated` is passed
  true.
- **Tests**: PostgreSQL's `foreign_key` regression cases that
  apply, as sqllogictest files with PostgreSQL 18's answers; the
  isolation specs `fk-contention`, `fk-deadlock`,
  `update-locked-tuple` and `propagate-lock-delete` (the last
  also needs `char(n)`).

## 9. Not here

- **`CHECK`**: the same kind, another type (§3).
- **A dependency index** (`sm3/CATALOG_INMEM.md` §4):
  `referenced_by` is the one reverse lookup foreign keys need,
  and it is derived in memory.
- **`FOR KEY SHARE` and `FOR NO KEY UPDATE`** in sm1: they would
  let an update of the parent's other columns past a child's
  check (`NEXT.md` §2). Foreign keys work without them, more
  coarsely.
- **`MATCH PARTIAL`**: PostgreSQL does not implement it either.

## 10. For review

- **The parent's lock X while a foreign key is added** (§7):
  acceptable, or should sm3 change the parent's definition under
  a lock that lets readers through? That would be the first DDL
  to do so.
- **`drop_table` of a referenced parent refused in sm3** (§5),
  rather than left to the host: it keeps the catalog true
  whoever the caller is, `rel` included.
- **The actions as fields sm3 keeps but does not act on**
  (§1), rather than all in the host's blob: they make the
  catalog readable to `rel` and to a reader of its bytes.

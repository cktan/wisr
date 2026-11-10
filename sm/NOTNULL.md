# NOTNULL — NOT NULL constraints by name

Status: built ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
410). §6 lists the choices taken while the user was away.

---

## 0. What this is

PostgreSQL 18 makes each NOT NULL column a constraint of its own,
with a name and an oid, a row of type `n` in `pg_constraint`:

```sql
create table job (id int primary key, owner text not null,
                  note text constraint note_set not null);
select conname, contype, conkey from pg_constraint
  where conrelid = 'job'::regclass and contype = 'n';
--       conname       | contype | conkey
-- --------------------+---------+--------
--  job_id_not_null    | n       | {1}
--  job_owner_not_null | n       | {2}
--  note_set           | n       | {3}
alter table job drop constraint job_owner_not_null;  -- DROP NOT NULL
create table job2 (like job including all);          -- copies them
```

PostgreSQL's own code reads these rows, so digres has them:

- `CREATE TABLE ... (LIKE t INCLUDING ALL)` copies each NOT NULL
  constraint, and looks each one up by name in `pg_constraint`
  (`expandTableLikeClause()`).
- `DROP CONSTRAINT t_a_not_null` and `RENAME CONSTRAINT` find one
  by its name.
- psql's `\d+` lists them under "Not-null constraints:".

## 1. What is whose

- **sm3 keeps the name and the oid** with the column, as it keeps
  `not_null`. A NOT NULL column has both; a nullable one neither.
  The name is unique among the table's constraints, as a check's
  is ([`CHECK.md`](CHECK.md#3-in-the-catalog) §3).
- **The host chooses the name**, as PostgreSQL does:
  `ChooseConstraintName(table, column, "not_null")`, or the name
  the statement gives. sm3 makes one of the same form when the
  caller gives none, for callers that are not PostgreSQL.
- **sm3 makes the oid** with `cat_new_oid()`, when the column
  becomes NOT NULL.

## 2. In the catalog

The column's part of the table's record gains two fields at its
end:

| Field | Type | Holds |
| --- | --- | --- |
| not_null_oid | `int8` | the constraint's oid; 0 for a nullable column |
| not_null_name | `text` | its name; empty for a nullable column |

No key layout changes. No database on disk is kept across this
change ([`../dg/CODE.md`](../dg/CODE.md)).

## 3. What DDL does

| Statement | sm3 | name |
| --- | --- | --- |
| `CREATE TABLE`, a NOT NULL column or a primary key's | `create_table`, each NOT NULL column's spec with its name | the host's choice |
| `ADD COLUMN ... NOT NULL` | `add_column` | the host's choice |
| `SET NOT NULL`, `ADD CONSTRAINT n NOT NULL a` | `set_not_null(name)`; already NOT NULL keeps its name | the host's choice, or `n` |
| `DROP NOT NULL`, `DROP CONSTRAINT` of one | `drop_not_null`: oid and name go | — |
| `RENAME CONSTRAINT` of one | `rename_not_null` | the new name, if free |
| `DROP COLUMN` | the column is dropped, its constraint with it | — |
| `ALTER COLUMN TYPE` | kept | — |
| `ADD PRIMARY KEY` on a nullable column | `set_not_null`, as now | the host's choice |

`DROP NOT NULL` of a primary key's column, or of an identity
column, stays refused (step 412).

## 4. digres

- **`pg_constraint`** (`catrel.c`) has a row of type `n` for each
  NOT NULL column: `conkey` its column, validated, enforced, not
  `NO INHERIT`, local, no `conbin`.
  `RelationGetNotNullConstraints()` and psql read it.
- **The names** are chosen where the column becomes NOT NULL,
  with `ChooseConstraintName()`, which sees every constraint's
  name in the namespace.
- **`DROP CONSTRAINT`** of a NOT NULL constraint is DROP NOT NULL
  of its column; **`RENAME CONSTRAINT`** renames it.

## 5. Cases

| Case | What happens |
| --- | --- |
| `LIKE ... INCLUDING ALL` | each NOT NULL constraint is found by name; the copy chooses its own names |
| a temporary table | its constraints are in the session's temporary catalog |
| a transaction that adds one, then aborts | the name and oid go with the record |
| a name a check or a foreign key has | `SM_ERR_DUPKEY`; the host chooses another first |
| an identity column | NOT NULL, so it has one |
| `ADD CONSTRAINT n NOT NULL a NOT VALID` | refused: digres has no NOT NULL that rows may break |
| `NOT NULL NO INHERIT` | accepted, and changes nothing, with no inheritance; `connoinherit` is false |

## 6. Decided while away

1. **sm3 keeps an oid and a name per NOT NULL column**, against
   making rows up in `catrel.c` with no oid of sm's: a made-up
   oid could be another object's, and a name given by the user,
   `CONSTRAINT note_set NOT NULL`, would be lost.
2. **`NOT VALID` of a NOT NULL constraint is refused**: digres
   has no NOT NULL that rows may break. **`NO INHERIT` is
   accepted and changes nothing**, as digres accepts it today and
   as a check's is ([`CHECK.md`](CHECK.md#7-decided) §7).

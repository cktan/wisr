# CHECK — check constraints

Status: built ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
411). §7 lists what the user decided.
[`FOREIGNKEY.md`](FOREIGNKEY.md) is the pattern this follows: sm3
keeps the constraint, the host checks the rows.

---

## 0. What this is

A **check constraint** is a boolean expression over a row's
columns that every row of a table must satisfy. A row for which
it is false is refused; one for which it is null passes.

```sql
create table orders (
  id     int8 primary key,
  qty    int4 check (qty > 0),
  price  numeric,
  disc   numeric,
  check (disc <= price)
);
insert into orders values (1, 0, 10, 1);
-- ERROR:  new row for relation "orders" violates check
--         constraint "orders_qty_check"
alter table orders add constraint disc_pos check (disc >= 0);
```

digres keeps and checks both. The regression files of
PostgreSQL run their checks
([`../dg/UNSUPPORTED.md`](../dg/UNSUPPORTED.md)).

## 1. What is whose

- **sm3 keeps the constraint** in the table's catalog, so a
  database's directory holds it
  ([`../dg/CODE.md`](../dg/CODE.md#the-overriding-objective-a-database-is-its-directory)).
  It keeps the constraint true to the table's columns: a column
  it reads cannot be dropped or retyped from under it (§5).
- **sm3 never evaluates the expression.** It keeps it as the
  host's bytes, as it keeps a column's DEFAULT
  (`column_def_t::default_expr`), and records the columns it
  reads, as it does for an expression index's `eval`.
- **The host checks the rows.** Every insert and update in
  digres goes through PostgreSQL's executor: `ExecConstraints()`
  and `ExecRelCheck()` in `executor/execMain.c`, called from
  `nodeModifyTable.c` and from `copyfrom.c`, the bulk load
  included. They read the table's checks from its relation
  descriptor (`rd_att->constr->check`). So the host's work is to
  fill that from sm3, and every write path is checked unchanged.
- **The host checks the rows already there** when a constraint
  is added to a table that has some, in the same transaction,
  and tells sm3 whether it did (`validated`), as for a foreign
  key.

## 2. What PostgreSQL needs of one

`pg_constraint`'s row of a check, and what psql's `\d` and
`pg_get_constraintdef()` read of it:

| Field | Here |
| --- | --- |
| `oid`, `conname`, `connamespace` | the constraint's oid and name; the table's namespace |
| `contype` | `c` |
| `conrelid`, `conkey` | the table, and the columns the expression reads |
| `convalidated` | the rows already there were checked: `false` for `NOT VALID` until `VALIDATE CONSTRAINT` |
| `conenforced` | PostgreSQL 18's `NOT ENFORCED`: the constraint is kept, and not checked (§7) |
| `connoinherit` | `NO INHERIT`: meaningful only with inheritance, which digres refuses |
| `conbin` | the expression, `nodeToString()` of the cooked tree: what `ExecRelCheck()` evaluates |

The executor needs, per check, its name, `conbin`, whether it is
valid and whether it is enforced: `ConstrCheck`'s `ccname`,
`ccbin`, `ccvalid` and `ccenforced`.

## 3. In the catalog

The `CONSTRAINT` entry of a table already has a kind
([`FOREIGNKEY.md`](FOREIGNKEY.md#3-the-entry) §3). A check
is kind `c`:

```
CONSTRAINT value = constraint | check
```

| Part | Fields |
| --- | --- |
| constraint | oid `int8`, name `text`, type `int2` (`c`), validated `bool`, host `bytea`: the expression and its flags, the host's |
| check | nreads `int4`, then each column read, 0-based `int4` |

- **The columns read are sm3's**, as they are for an index's
  `eval`: DDL on a column must know which constraints read it
  (§5). The host fills them from the cooked expression's `Var`s.
- **The rest is the host's**: the expression, `NOT ENFORCED` and
  `NO INHERIT`, in `host`. sm3 reads none of it.
- **No key layout changes**, so `META` stays as it is
  ([`CATALOG.md`](CATALOG.md#35-values) §3.5).
- **Its oid** is from `cat_new_oid()`, and its name is unique
  among the table's constraints, foreign keys, primary key and
  unique constraints included, as in PostgreSQL.

## 4. In memory

- **`table_def_t` gains `checks`**, each its oid, name,
  validated, columns read and host bytes, from the table's
  `CONSTRAINT` entries of kind `c`.
- **A check is found by its oid**, as a foreign key is, through
  the table's definition.
- **The table's version moves** when a check is added, dropped,
  validated or renamed, so the host's relation cache of it is
  seen to be stale.
- **Temporary tables** keep theirs in the session's temporary
  catalog, as their foreign keys.

## 5. What DDL does

### 5.1 sm3

- **`create_check(session, table, def, oid)`** takes the table's
  oid lock X, checks every column read is live and the name is
  free, and puts the entry. It reads no row: `validated` is the
  host's word.
- **`drop_check`**, **`validate_check`**, **`rename_check`**, as
  for a foreign key.
- **`drop_column`** of a column a check reads is
  `SM_ERR_INUSE`: the host drops the check first (§5.2).
- **`alter_column_type`** of a column a check reads is
  `SM_ERR_INUSE` (§7).
- **`drop_table`** dels its checks with it.
- The C wrapper gains the same calls (`sm_cat_create_check()`
  and the rest), and `sm_table_def_t` its checks.

### 5.2 digres

- **`CREATE TABLE`** takes column and table checks. Each is
  cooked as PostgreSQL's `cookConstraint()` does: transformed,
  coerced to boolean, its collations assigned; a subquery, an
  aggregate, a window function or a reference to another table
  refused with PostgreSQL's errors. It is named as PostgreSQL
  names it (`ChooseConstraintName()`, `table_column_check`). The
  table is new and empty, so each is valid.
- **`ALTER TABLE ... ADD CONSTRAINT ... CHECK`** cooks it the
  same way, then, unless `NOT VALID`, reads every row and
  refuses the first for which it is false, with PostgreSQL's
  error, `check constraint "x" of relation "t" is violated by
  some row`. Then it creates it, valid or not.
- **The rows are read at sm's latest view** (`sm_view_latest()`,
  in `check_rows()`), not the transaction's snapshot. A
  REPEATABLE READ transaction's snapshot can miss a row that
  another transaction committed since; the check must see it.
- **`ALTER TABLE ... ADD COLUMN ... CHECK`** adds the column,
  then adds the check as above: the rows already there are
  checked with the new column's value. `IF NOT EXISTS` skips the
  column's checks when the column exists. A skipped serial or
  identity column creates no sequence.
- **`VALIDATE CONSTRAINT`** of a check reads the rows, and sets
  it valid.
- **`DROP CONSTRAINT`** and **`RENAME CONSTRAINT`** of a check.
- **`DROP COLUMN`** drops the checks that read the column first,
  with no `CASCADE` asked, as PostgreSQL does.
- **The relation cache** (`RelationBuildSmDesc()` in
  `utils/cache/relcache.c`) fills `constr->check` from the
  table's checks, as it fills `defval` from its defaults.
- **`pg_constraint`** gains rows of type `c` (`catrel.c`), so
  `\d` shows "Check constraints:" and `pg_get_constraintdef()`
  answers.

## 6. Cases

| Case | What happens |
| --- | --- |
| an insert, an update, `ON CONFLICT DO UPDATE` | `ExecConstraints()`, as in PostgreSQL |
| `COPY FROM`, the bulk load into an empty table | `CopyFrom()` calls `ExecConstraints()` for each row before it hands the row to sm |
| `INSERT ... SELECT` | the executor's insert path, so each row is checked |
| a foreign key's `ON UPDATE SET NULL` or `SET DEFAULT` | the RI trigger's update, through the executor: checked |
| the expression is null for a row | it passes, as in PostgreSQL |
| a check that reads no column | allowed, as in PostgreSQL; `nreads` 0 |
| `ADD CONSTRAINT ... NOT VALID`, then `VALIDATE CONSTRAINT` | invalid until validated; new rows are checked meanwhile |
| a temporary table | its checks are in the session's temporary catalog |
| `DROP COLUMN` of a column a check reads | the check is dropped first |
| `ALTER COLUMN TYPE` of a column a check reads | refused (§7) |
| `TRUNCATE` | the check stays |
| a transaction that adds a check, then aborts | the entry goes with it, as any catalog change |
| a check calling a volatile function | allowed, except sequence functions (§7) |
| a database copied to another instance | the check is in its directory, with its expression |

## 7. Decided

Decided by the user, 2026-11-07:

1. **`ALTER COLUMN TYPE` of a column a check reads is refused**
   until the check is dropped, as digres refuses the dependents
   it cannot rebuild, an expression or a partial index among
   them ([`../dg/ALTERTYPE.md`](../dg/ALTERTYPE.md) §8). The
   rewrite is sm's, and checking rows in it would mean sm calling
   the host's evaluator for each row.
2. **`DROP COLUMN` of a column a check reads drops the check
   too**, with no `CASCADE` asked, as PostgreSQL does.
3. **`NOT ENFORCED` and `NO INHERIT` are accepted and kept.** A
   check not enforced is kept and never evaluated; `NO INHERIT`
   changes nothing, with no inheritance.
4. **Sequence functions in CHECK constraints are refused.** sm3
   does not track dependencies from checks to sequences. Digres
   rejects sequence value and metadata functions when it cooks
   the check.

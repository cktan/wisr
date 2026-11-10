# GENERATED — generated columns

Status: built for CREATE TABLE
([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 415); ADD COLUMN
of one is refused (§6). §6 lists the choices taken while the user
was away.
[`CHECK.md`](CHECK.md) is the pattern this follows: sm3 keeps the
expression and the columns it reads, the host computes the
values.

---

## 0. What this is

A **generated column** is computed from the other columns of its
row. PostgreSQL 18 has two kinds:

```sql
create table item (
  price  numeric,
  qty    int4,
  total  numeric generated always as (price * qty) stored,
  label  text    generated always as ('#' || qty) virtual
);
insert into item (price, qty) values (2.5, 4);
select * from item;   -- 2.5 | 4 | 10.0 | #4
insert into item (total) values (1);
-- ERROR:  cannot insert a non-DEFAULT value into column "total"
```

- **STORED**: computed when a row is inserted or updated, and
  kept in the row, as any other value.
- **VIRTUAL**, PostgreSQL 18's default: computed when a row is
  read; the row keeps nothing for it.

## 1. What is whose

- **sm3 keeps the kind, the expression and the columns it reads**
  with the column. The expression is the host's bytes, in the
  column's default, as PostgreSQL keeps it in `pg_attrdef`.
- **sm3 never evaluates the expression.** A virtual column's
  value in a row is null; a stored one's is what the host wrote.
- **The host computes the values**, with PostgreSQL's own code:
  `ExecComputeStoredGenerated()` in the executor for a stored
  column, on every INSERT, UPDATE and COPY; the rewriter's
  `expand_generated_columns_in_query()` for a virtual one. Both
  read the relation descriptor: `attgenerated` and the
  expression in `constr->defval`. The host's work is to fill
  those from sm3.

## 2. In the catalog

The column's part gains, at its end:

| Field | Type | Holds |
| --- | --- | --- |
| generated | `int2` | `s` stored, `v` virtual, or null for a plain column |
| generated reads | `bytea` | the 0-based places of the columns the expression reads, `int4` each, or null |

The expression is the column's default (`default_expr`). No key
layout changes.

## 3. What DDL does

| Statement | What happens |
| --- | --- |
| `CREATE TABLE` with a generated column | the table is made with the column plain; then the expression is cooked against it by PostgreSQL's `cookDefault()`, with its checks: no other generated column, no subquery, immutable functions only; then `set_generated()` makes the column generated |
| `ADD COLUMN ... GENERATED` | refused (§6) |
| `CREATE TABLE ... (LIKE t INCLUDING GENERATED)` | refused: PostgreSQL gives the copy's expression after the table is made, as a cooked default; without `INCLUDING GENERATED` the copy is a plain column, as in PostgreSQL |
| `DROP COLUMN` of a generated column | its kind and reads go with it, so a row written after has no value computed for it |
| `DROP COLUMN` of a column an expression reads | refused, PostgreSQL's error, unless `CASCADE`, which drops the generated column too |
| `ALTER COLUMN TYPE` of a column an expression reads | refused, PostgreSQL's error: `cannot alter type of a column used by a generated column` |
| `ALTER COLUMN TYPE` of a generated column | refused (§6); PostgreSQL computes it again |
| `SET EXPRESSION`, `DROP EXPRESSION` | refused (§6) |
| `SET DEFAULT`, `DROP DEFAULT` of a generated column | refused with PostgreSQL's error (step 412's code) |
| `CREATE INDEX` on a stored column | taken: the value is in the row |
| `CREATE INDEX` on a virtual column | refused, as PostgreSQL 18 refuses it |

## 4. digres

- **The relation cache** (`RelationBuildSmDesc()`) sets
  `attgenerated` and puts the expression in `constr->defval`,
  with `has_generated_stored` and `has_generated_virtual`.
- **`pg_attribute`** has `attgenerated`, and **`pg_attrdef`** the
  expression, so `\d` shows "generated always as (...)".
- **NOT NULL** on a stored column is checked as on any column;
  on a virtual one it is refused (§6).

## 5. Cases

| Case | What happens |
| --- | --- |
| INSERT, UPDATE, `ON CONFLICT DO UPDATE` | the executor computes a stored column; an explicit value is refused by PostgreSQL's rewriter |
| `COPY FROM`, the bulk load | `CopyFrom()` computes a stored column before it hands the row to sm |
| UPDATE of a column a stored expression reads | the executor computes it again |
| a read of a virtual column | the rewriter puts the expression in its place |
| a row written before ADD COLUMN ... VIRTUAL | its missing value is null; the expression is what is read |
| a temporary table | as any column |
| a transaction that adds one, then aborts | the record goes with it |
| `'#' \|\| qty` with qty an integer | refused, `generation expression is not immutable`: PostgreSQL's `textanycat()` is a STABLE `sql` function its planner inlines to immutable parts; digres has no `sql` function bodies to inline. `'#' \|\| qty::text` is taken |

## 6. Decided while away

1. **Both kinds**: VIRTUAL is PostgreSQL 18's default, so a
   statement that names neither makes one.
2. **ALTER COLUMN TYPE of a generated column is refused**, as
   for a check ([`CHECK.md`](CHECK.md#7-decided) §7): the
   rewrite is sm's, and computing in it would mean sm calling
   the host's evaluator for each row. PostgreSQL refuses the
   type of a column an expression reads too.
3. **SET EXPRESSION and DROP EXPRESSION are refused** for now: a
   stored column's SET EXPRESSION rewrites every row. **ADD
   COLUMN ... GENERATED is refused** too: each row already there
   needs its value computed from its own columns, which
   `fill_column()`, made for a volatile default, does not do.
4. **NOT NULL on a virtual column is refused**: sm keeps a null
   in its place, and its NOT NULL is sm's check of the row.
   PostgreSQL 18 takes it, and computes the value to check it.

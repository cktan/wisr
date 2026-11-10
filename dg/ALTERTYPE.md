# ALTERTYPE — ALTER COLUMN TYPE, key columns among them

**Status: built** ([`WORKNOTE.md`](WORKNOTE.md) step 378). §8 has
what was
decided, §9 what building it found. It is
[`../sm/CLUSTER.md`](../sm/CLUSTER.md)
§11's step 3, the key-column ALTERs, and the general `ALTER
COLUMN TYPE` they wait for
([`UNSUPPORTED.md`](UNSUPPORTED.md#3-ddl) §3,
[`PKDDL.md`](PKDDL.md)
§3).

```sql
CREATE TABLE t (a int4 PRIMARY KEY, b text, c int4 DEFAULT 7);
ALTER TABLE t ALTER COLUMN b TYPE int4 USING b::int4;
ALTER TABLE t ALTER COLUMN a TYPE text;  -- the key: rows re-keyed
ALTER TABLE t ALTER COLUMN c TYPE int8;  -- the default re-cast
```

Today digres refuses every `ALTER COLUMN TYPE`:
`smstore_alter_table()` lets the subcommand through to no
handler.

---

## 1. What PostgreSQL 18 does

Run on PostgreSQL 18.6:

| Statement | PostgreSQL 18 |
| --- | --- |
| `text` to `int4`, no `USING` | `column "b" cannot be cast automatically to type integer`, `HINT: You might need to specify "USING b::integer".` |
| `... USING b::int4` | the rows converted, the table rewritten |
| a key column, `int4` to `text` | the rows re-keyed; the order is now `text`'s |
| `varchar(5)` to `varchar(10)`, or to `text` | the catalog alone: no rewrite |
| `varchar(5)` to `varchar(2)`, a value of 3 | `value too long for type character varying(2)` |
| a column with a default | the default cast to the new type |
| an error in `USING`, `b::int4/0` | `division by zero`; the table as it was |
| two rows with one key after the cast | `could not create unique index "dup_pkey"`, `DETAIL: Key (a)=(1) is duplicated.` |
| a column a view reads | `cannot alter type of a column used by a view or rule`, `DETAIL: rule _RETURN on view v depends on column "b"` |
| a column a generated column reads | `cannot alter type of a column used by a generated column`, `DETAIL: Column "x" is used by generated column "y".` |
| a column an expression or a partial index reads | the index made again, as `pg_get_indexdef()` gives it |
| a foreign key's column, either side | the constraint made again; refused if the types become incompatible, `foreign key constraint "ch_k_fkey" cannot be implemented` |
| `COLLATE` on a type with none | `collations are not supported by type integer` |
| an open cursor on the table | `cannot ALTER TABLE "t" because it is being used by active queries in this session` |
| two `ALTER COLUMN TYPE` in one statement | one rewrite for both |

## 2. What sm has

- **The rewrite**
  ([`../sm/CLUSTER.md`](../sm/CLUSTER.md#8-a-rewrite) §8,
  `sm3/src/table/
  table.cc`'s `rewrite_table()`): a new store, the table's
  definition after the change over it, every row deformed by
  the old definition and inserted by the new one, every index
  filled as the rows go in, and the old store dropped at
  commit. A duplicate key fails it, with the key, for the host
  to report. ADD and DROP PRIMARY KEY use it
  ([`PKDDL.md`](PKDDL.md)).
- **A column's type is its record's** (`column_rec_t`:
  `type_code`, `typmod`, `collation`). A plain index key column
  takes its type from the column, so the rewrite builds every
  plain index on the column, the primary key among them, under
  the new type. An expression key keeps a type of its own.
- **The host's expressions are opaque to sm**: a default, an
  index's `eval`. sm calls the host's evaluator for an
  expression index ([`EXPRINDEX.md`](EXPRINDEX.md#3-what-sm-has)
  §3, `sm_index_eval_fn`), on
  the session's thread for a build inside the transaction.
- **sm's datums are PostgreSQL's**, bit for bit
  ([`DECIMAL.md`](DECIMAL.md#23-stored-as-sm2s-decimal-at-the-columns-scale)
  §2.3). `text` and `varchar` store the same
  bytes and the same key
  ([`../sm/sm2/TYPE.md`](../sm/sm2/TYPE.md#3-the-types) §3).

## 3. Where the conversion runs

A row's new value is PostgreSQL's cast, or the `USING`
expression, which may read any column of the row. Only digres
can evaluate it.

| | A. sm rewrites, digres converts | B. digres rewrites |
| --- | --- | --- |
| What | one new call, `sm_cat_alter_column_type()`: the column's new type, its default, and the host's conversion; the rewrite calls a converter of digres's for each row | new calls to begin a rewrite, insert a row into the new store, and end it; digres scans the old rows with its executor and evaluates the conversion |
| sm's surface | one call, and a converter type beside `sm_index_eval_fn` | three calls, and a rewrite's state kept between them |
| A key column | the rewrite as ADD PRIMARY KEY's: the same code | digres inserts in any order; sm sorts or checks |
| A duplicate key | the rewrite's error, with the key, as now | found at an insert, reported by digres |
| digres | the converter: the row in a slot, the expression made ready once, as the index evaluator does ([`EXPRINDEX.md`](EXPRINDEX.md#43-the-evaluator) §4.3) | two descriptors of one table at once: the old to read by, the new to write by |
| Crash, abort | as ADD PRIMARY KEY: the new store unreferenced, or dropped | the same, if the three calls keep it so |

**Recommended: A.** It keeps the rewrite whole in sm, as ADD
PRIMARY KEY has it, and reuses the evaluator's pattern. B
spreads one atomic step over three calls.

## 4. Without a rewrite

PostgreSQL rewrites no rows when the bytes do not change
(`ATColumnChangeRequiresRewrite()`): `varchar(n)` to a longer
`varchar`, or to `text`; any type to itself with no `USING`.
digres asks PostgreSQL's own test, and passes no conversion; sm
then changes the column's record alone. sm accepts that only
when the stored bytes and the key encoding stay the same: the
same type code, or `text` and `varchar`.

## 5. The cases

| Case | What happens |
| --- | --- |
| No `USING`, no implicit cast | PostgreSQL's error and hint, from parse analysis (`transformAlterTableStmt()`) |
| A plain column | rewritten into a new store; its plain indexes filled under the new type |
| A key column | the same rewrite: the rows keyed under the new type's encoding; a duplicate fails with PostgreSQL's words |
| A conversion error (`USING b/0`, a value too long) | PostgreSQL's error, re-raised on the session's thread ([`EXPRINDEX.md`](EXPRINDEX.md#44-errors) §4.4); the table as it was |
| A NULL in a NOT NULL column after `USING` | the rewrite's insert fails: PostgreSQL's `column "b" of relation "t" contains null values` |
| A default | coerced to the new type by digres, as PostgreSQL's `ATExecAlterColumnType()` does, and stored with the type |
| A missing value (a column ADDed with a default) | the rewrite stores every row's value; the record's missing bytes go |
| No rewrite needed (§4) | the record alone |
| A view reads the column | refused, PostgreSQL's words (`sm_cat_view_readers()`) |
| A generated column reads it | refused, PostgreSQL's words |
| An expression or a partial index reads it | §8, question 3 |
| A foreign key's column, either side | §8, question 3 |
| An identity column | §8, question 3 |
| A check constraint reads it | refused, "not supported yet" ([`../sm/CHECK.md`](../sm/CHECK.md) §7) |
| An index key changed to interval | refused, as CREATE INDEX refuses an interval key: sm orders intervals PostgreSQL calls equal as two keys. An INCLUDE column, or one no index has, is taken |
| The column's statistics | dropped: ANALYZE makes them again |
| `COLLATE` | the collations digres takes at CREATE TABLE; PostgreSQL's error for a type with none |
| Two in one statement | one rewrite each (§8, question 4). One column twice is refused, PostgreSQL's `cannot alter type of column "a" twice`; a `USING` that reads another column the statement retypes, "not supported yet" |
| Abort, savepoint rollback | the new store dropped; the catalog as it was |
| Crash before commit | the new store unreferenced, as a TRUNCATE's swap leaves one |
| Other sessions | the table-oid lock X; a prepared plan's descriptor invalidated (`forget_table()`) |
| An open cursor of this transaction | refused, as `alter_table_rel()` refuses DDL now |
| A temporary table | as any; sm's rewrite of a temporary store |

## 6. Tests

- **mvcc**: none; sm1 does not change.
- **sm**: sm3's unit tests, a converter of the test's own: a
  plain column, a key column re-keyed and a duplicate found, a
  converter's error, the record alone (§4), an abort.
  `rel/test`, the same through `sm.h`, if `rel` can name a
  converter; if not, the reason stated.
- **dg**: `test/slt/alter_type.test`, §5's cases, PostgreSQL
  18's results; `server.sh`, a crash before the commit;
  pgregress's files that alter a column's type, trimmed again.
- **Shown to fail** with each part disabled, one at a time.

## 7. Order of work

1. sm3: `alter_column_type()`, its rewrite with the converter,
   and the record alone. → verify: sm3's tests, `rel`.
2. `sm.h`: the call, its converter passed with it. → verify:
   `rel`.
3. digres: parse analysis, the converter, the default, the
   refusals. → verify: slt.
4. The key column. → verify: slt;
   [`CLUSTER.md`](../sm/CLUSTER.md#11-what-is-left) §11 step 3
   built.
5. pgregress trimmed again;
   [`UNSUPPORTED.md`](UNSUPPORTED.md#3-ddl) §3,
   [`NEXT.md`](../sm/NEXT.md#4-done-and-dropped) §4
   item 1, [`WORKNOTE.md`](WORKNOTE.md).

## 8. Decided

1. **Where the conversion runs** (§3): A. sm rewrites, and
   digres's converter gives each row's new value.
2. **The change without a rewrite** (§4): in this build.
3. **Dependents PostgreSQL makes again** (an expression or a
   partial index, a foreign key on either side, an identity
   column's sequence, a check constraint): refused, "not
   supported yet", in this build. Making each again is a step of its own.
4. **Two in one statement**: one rewrite each. A later `USING`
   would then read a column an earlier one converted, where
   PostgreSQL reads every column as it was; so a `USING` that
   reads another column the statement retypes is refused, and
   one column twice is, in PostgreSQL's words. What is taken
   gives PostgreSQL's values.

## 9. What building it found

- **The converter is passed to the call**, with its context,
  not registered once for the environment as the index
  evaluator is: it runs only inside the session's own call,
  never on a build worker, and needs what that call parsed.
- **Parse analysis of `USING` is digres's own**:
  `transformAlterTableStmt()` looks the column up with
  `get_attnum()`, which reads a syscache digres has not
  ([`PKDDL.md`](PKDDL.md#8-what-building-it-found) §8).
  `alter_column_type()` in `smstore.c` makes
  the transform as `ATPrepAlterColumnType()` does, the table
  locked first, as parse analysis assumes.
- **`timestamp` and `timestamptz`** are one change PostgreSQL
  can make without a rewrite; sm stores them as two types, so
  digres rewrites the rows, each value unchanged.
- **A prepared statement** whose result type changed fails as
  PostgreSQL's does: `cached plan must not change result type`.
- **A generated column** does not arise: digres refuses
  `GENERATED ALWAYS AS` at CREATE TABLE.
- **A key duplicated by the cast** is reported as PostgreSQL
  reports it, `Key (test)=(0) is duplicated.`: `dup_key()` reads
  the row from the old store, and gives the column's value as
  the transform makes it from the row. pgregress's
  `alter_table` shows it; slt matches the message alone.
- **sm3's rewrite named the primary key for any duplicate**, a
  secondary unique index's too, with no row for the host to
  show. ADD PRIMARY KEY never met one there, as the rows were
  unique in every other index already; a type change can. It
  now names the index the insert met it in, and the row by its
  key in the old store (`test_altertype.cc`).

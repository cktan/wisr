# ONROWUPDATE — a column stamped on every update

**Status: approved, not built.** §11 has what was decided,
and §12 the order of work.

```sql
CREATE TABLE account (
    id      bigint PRIMARY KEY,
    balance numeric(12,2) NOT NULL DEFAULT 0,
    ctime   timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP,
    mtime   timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP
                                 ON ROW UPDATE CURRENT_TIMESTAMP
);

INSERT INTO account (id) VALUES (1);           -- ctime = mtime = DEFAULT
UPDATE account SET balance = 10 WHERE id = 1;  -- mtime stamped, ctime kept
UPDATE account SET mtime = '2020-01-01' WHERE id = 1;  -- the value given wins
```

A column declared `ON ROW UPDATE CURRENT_TIMESTAMP` is set to
the transaction's start time by every UPDATE of its row that
does not assign it. That is a row's modification time, with no
trigger and no language to write one in: digres has neither
(`UNSUPPORTED.md` §6), and would rather not.

---

## 0. In brief

- **Syntax**: a column constraint, `ON ROW UPDATE` followed by
  `CURRENT_TIMESTAMP` or `LOCALTIMESTAMP`. A keyword, not an
  expression: nothing is evaluated (§2).
- **sq** takes one rule and one token more than PostgreSQL's
  grammar (§4). It is the one change here to what PostgreSQL
  accepts.
- **sm** keeps one byte a column in its catalog, as it keeps an
  identity column's kind (§5).
- **digres** stamps the column in `ExecUpdatePrepareSlot()`,
  where PostgreSQL computes stored generated columns, so every
  UPDATE, ON CONFLICT DO UPDATE and cascade passes through it
  (§6).
- **ctime needs nothing new**: `DEFAULT CURRENT_TIMESTAMP` is
  PostgreSQL's, and digres evaluates defaults already
  (`test/slt/add_column_volatile.test`).

## 1. Why

A table's rows commonly carry when they were made and when they
last changed. PostgreSQL gives the first by a default and the
second by a trigger:

```sql
CREATE FUNCTION touch() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN NEW.mtime := now(); RETURN NEW; END $$;
CREATE TRIGGER account_touch BEFORE UPDATE ON account
    FOR EACH ROW EXECUTE FUNCTION touch();
```

digres has neither `CREATE FUNCTION` in a language nor
`CREATE TRIGGER`. Without either, the application must set
mtime in every UPDATE it writes, and nothing holds it to that.

## 2. What it means

1. **When.** Every UPDATE of a row stamps each of its
   `ON ROW UPDATE` columns that the statement does not assign:
   a column in the statement's `SET` list, or its ON CONFLICT
   DO UPDATE's, keeps what the statement gives it, `DEFAULT`
   included. An UPDATE that changes no other value stamps too
   (§11).
2. **What.** `CURRENT_TIMESTAMP` writes what the keyword reads:
   the transaction's start time, a `timestamptz`.
   `LOCALTIMESTAMP` writes the same instant as a `timestamp`
   in the session's time zone. The column's precision rounds
   it, as an assignment does.
3. **Not on INSERT or COPY.** A new row takes the column's
   default, or the value given. A column with no default is
   null until the row's first UPDATE.
4. **The column is an ordinary column otherwise.** It can be
   read, written, indexed, given NOT NULL, and dropped.

Only these two keywords are taken. `CURRENT_TIME`,
`LOCALTIME` and `CURRENT_DATE` are a time of day or a date,
not an instant; a function such as `now()` or
`clock_timestamp()` would make the clause an expression, which
it is kept from being.

## 3. What others do

| System | Modification time |
| --- | --- |
| PostgreSQL 18 | a BEFORE UPDATE trigger (§1) |
| MySQL, MariaDB | `ts TIMESTAMP ... ON UPDATE CURRENT_TIMESTAMP`; it stamps only when another value of the row changes |
| SQL:2011 | `GENERATED ALWAYS AS ROW START` in a system-versioned table, which also keeps every old version |

`ROW` is digres's: PostgreSQL's grammar already reads `ON
UPDATE` after `REFERENCES t` as a foreign key's action, so the
bare MySQL form would take a different meaning on a column with
both (§4).

## 4. Parsing, in sq

sq's grammar is PostgreSQL's, its rules untouched
(`../sq/DESIGN.md` §2). This adds to it:

- **A rule** in `ColConstraintElem` (`../sq/src/gram.y`):
  `ON_ROW_UPDATE CURRENT_TIMESTAMP` and `ON_ROW_UPDATE
  LOCALTIMESTAMP`, each making a `Constraint` of a new
  `contype`, `CONSTR_ON_ROW_UPDATE`, whose `raw_expr` is the
  keyword's `SQLValueFunction`, as the keyword makes in an
  expression.
- **A token**, `ON_ROW_UPDATE`, which the lookahead filter
  (`../sq/src/parser.cc`) makes of `ON ROW UPDATE` in a row.
  After `REFERENCES t`, bison sees only `ON` and cannot tell a
  foreign key's `ON UPDATE` from this clause; one token is not
  enough either, since `ON ROW` begins a join condition, `ON
  ROW(a, b) = ROW(c, d)`, and `ON row` names a table called
  `row`. No PostgreSQL 18 text has `ON ROW UPDATE` in a row,
  so the three are taken as one; the filter, which holds one
  token of lookahead, holds two.
- **The enum value** `CONSTR_ON_ROW_UPDATE`, added last to
  `ConstrType` in `../sq/spec/parsenodes.h` and in digres's
  `src/include/nodes/parsenodes.h`, so no other value moves.

The cost is to sq's claim to parse as PostgreSQL does, accepted
for this clause (§11). It is kept checkable: `make corpus-test`
holds sq to what PostgreSQL's parser printed for all 49,034
pieces of its regression scripts, and must still pass, which
shows no PostgreSQL text parses differently; and `make test`
gets new files for the clause, its refusals included, whose
expected output is sq's own, not PostgreSQL's. `DESIGN.md` §2
and §9 say so.

## 5. The catalog, in sm

`sm_column_def_t` (`../sm/include/sm.h`) takes one field:

```c
char on_update;   /* 'c' CURRENT_TIMESTAMP, 'l' LOCALTIMESTAMP, or 0 */
```

- **Stored** in the column's catalog record as an optional
  field, as an identity column's kind is
  (`../sm/sm3/src/catalog/blob.cc`); absent means 0, so a
  catalog written before reads the same.
- **Checked** by `create table` and `add column`
  (`../sm/sm3/src/ddl/ddl.cc`): `'c'` only on a `timestamptz`,
  `'l'` only on a `timestamp`, anything else `SM_ERR_INVALID`.
- **Not acted on.** sm stores the byte and gives it back; it
  stamps nothing. What the time is, and when a statement
  assigns a column, are the host's to know.
- **rel** shows it, as `:on-row-update :current-timestamp` in
  a column, and takes it in `table-create`, so the field and
  its checks are tested where sm is (`../sm/rel/REL.md`).

## 6. Stamping, in digres

- **CREATE TABLE** (`smstore.c`, the column loop that refuses
  what it does not take, `only_taken()`): a
  `CONSTR_ON_ROW_UPDATE` sets `on_update` from the keyword;
  the type check is sm's, given the column's type code.
- **The table's cached definition** keeps a byte a column
  beside its identity kinds, and `smstore_on_row_update(relid)`
  gives the array, or NULL for a table with none.
- **`ExecUpdatePrepareSlot()`**
  (`src/backend/executor/nodeModifyTable.c`): after the
  generated columns, each column whose byte is set and which is
  not in `ExecGetUpdatedCols()` is set in the slot, from
  `GetSQLCurrentTimestamp(typmod)` or
  `GetSQLLocalTimestamp(typmod)`, the functions the keywords
  call. The array is fetched once a result relation, at its
  first UPDATE, and kept in its `ResultRelInfo`; a table with
  none costs a pointer test a row.

Every UPDATE path reaches `ExecUpdatePrepareSlot()`: a plain
UPDATE, `WHERE CURRENT OF`, ON CONFLICT DO UPDATE (which calls
`ExecUpdate()`), an UPDATE through an updatable view, and a
foreign key's `ON UPDATE CASCADE` or `SET NULL`, which updates
the referencing rows by SPI. RETURNING reads the slot after, so
it returns the stamp.

## 7. DDL

- **ALTER TABLE ADD COLUMN** takes the clause. The rows there
  take the column's default, as any added column does; the
  clause acts from their next UPDATE.
- **DROP COLUMN** drops it with the column.
- **No ALTER COLUMN form** sets or drops the clause: PostgreSQL
  has no syntax to borrow for it, and none is added. A column
  is dropped and added again instead.
- **CREATE TABLE LIKE** does not copy it, as it is no default.

## 8. The cases

| Case | What happens |
| --- | --- |
| INSERT, COPY, a bulk load into an empty table | the default or the value given; no stamp |
| UPDATE that does not assign the column | stamped |
| UPDATE that assigns it, `DEFAULT` included | what it assigns |
| ON CONFLICT DO UPDATE | as an UPDATE; ON CONFLICT DO NOTHING stamps nothing |
| An UPDATE that changes no other value; A → B → A | stamped each time (§11) |
| An UPDATE of the primary key, which moves the row (`../sm/CLUSTER.md`) | stamped; the other columns move with it, ctime too |
| DELETE, then INSERT of the same key | a new row: its default |
| Abort, rollback to a savepoint, a bulk load rolled back | an ordinary column's value, undone with the row |
| Two transactions | each its own start time; a later commit can carry the earlier time, as `now()` in a trigger does |
| An old snapshot, SERIALIZABLE, an open cursor | an ordinary column |
| An index on the column | every stamping UPDATE changes its entry; other indexes are as before |
| A cascade from a foreign key | the referencing rows are updated, so stamped |
| Temporary tables, tables without a primary key, TRUNCATE | the same path |
| A table with no such column | one pointer test a row |

## 9. What it does not do

- **PostgreSQL does not take it.** A schema with the clause is
  digres's; PostgreSQL refuses it as a syntax error.
- **psql's `\d` does not show it**, nor does `pg_dump` dump it:
  both read PostgreSQL's catalog, which has no place for it
  (`SYSCATALOG.md`). A dump restored keeps the column and its
  default and loses the clause.
- **It is not a commit time.** The stamp is the transaction's
  start, known when the row is written; when it commits is
  not.
- **It does not keep old versions** as SQL:2011's
  system-versioned tables do.

## 10. Alternatives not taken

- **`GENERATED ALWAYS AS (now()) STORED`**: PostgreSQL's syntax,
  but PostgreSQL refuses it, `now()` not being immutable, and
  would not recompute it on UPDATE, as it reads no column.
  digres would change the meaning of `GENERATED` to take it,
  and `GENERATED ALWAYS` refuses a value given, so a restore or
  a COPY of old rows would lose their times.
- **`ALTER TABLE ... ALTER COLUMN mtime SET (on_row_update =
  current_timestamp)`**: an attribute option, which
  PostgreSQL's grammar already parses, so sq is untouched; but
  only by ALTER, never in CREATE TABLE, and a misspelt option
  is a runtime error, not a syntax one.
- **Triggers**: no language to write one in, and none is
  wanted.
- **sm stamps the commit time**: a storage trick PostgreSQL
  does not have, and the commit time is not known when the row
  is written.
- **`commit_ts` made a clock** (`../sm/sm1/SM1_DESIGN.md` §4.1),
  read as a row's last commit time: a change to sm1's clock and
  its leases, and no creation time from it.

## 11. Decided

Both on 2026-10-27:

1. **An UPDATE that changes nothing is stamped**, as the
   trigger of §1 stamps it; not only when another value
   changes, as MySQL does, which would compare the row before
   and after.
2. **sq's grammar** takes §4's rule and token, the first
   change to what sq accepts beyond PostgreSQL's.

## 12. Order of work

1. **sm**: the field, its record, its checks; rel's column
   option. Verify: a rel test that a column keeps its byte
   across a reopen, that a wrong type is `SM_ERR_INVALID`, and
   that a catalog with no byte reads 0.
2. **sq**: the token, the rule, the enum value. Verify: `make
   test` with new files for the clause and its refusals, and
   `make corpus-test` unchanged.
3. **digres, CREATE TABLE and ADD COLUMN**. Verify: an slt
   test that each is taken on its type and refused on another.
4. **digres, the stamp**. Verify: an slt test for each row of
   §8, each shown to fail with the stamp taken out; and Q7 of
   `../bench/DESIGN.md`, an UPDATE-free read, and pgbench's
   UPDATEs, at their instructions a transaction before and
   after, on a table with no such column.
5. **The documents**: `UNSUPPORTED.md` lists the clause as
   digres's own; `../sq/DESIGN.md` §2 and §9 say what sq added.

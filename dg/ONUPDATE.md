# ONUPDATE — a column's default on UPDATE

**Status: built** ([`WORKNOTE.md`](WORKNOTE.md) step 307). §11
has what was
decided, and §12 the order of work.

```sql
CREATE TABLE job (
    id         bigint PRIMARY KEY,
    sync_state int         NOT NULL DEFAULT 0 DEFAULT 1 ON UPDATE,
    ctime      timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP,
    mtime      timestamptz NOT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE,
    mdate      date        NOT NULL DEFAULT CURRENT_DATE ON UPDATE,
    edited     timestamptz          DEFAULT NULL DEFAULT CURRENT_TIMESTAMP ON UPDATE
);

INSERT INTO job (id) VALUES (1);
-- sync_state 0; ctime, mtime now; mdate today; edited null
UPDATE job SET ctime = ctime WHERE id = 1;
-- sync_state 1; mtime now; mdate today; edited now
UPDATE job SET sync_state = 2 WHERE id = 1;
-- sync_state 2, as assigned; the others as above
```

A column may have an **update default**, written `DEFAULT x ON
UPDATE`: the value an UPDATE writes in it when it does not
assign it. Its **insert default** is its plain `DEFAULT y`, or,
without one, `x`. So a row's modification time, a "changed
since" flag, or a state an edit moves a row to is kept by the
table, with no trigger and no language to write one in: digres
has neither
([`UNSUPPORTED.md`](UNSUPPORTED.md#6-functions-and-languages)
§6), and would rather not.

---

## 0. In brief

- **Syntax**: `DEFAULT x ON UPDATE`, alone or beside a plain
  `DEFAULT y`. sq takes one rule more than PostgreSQL's
  grammar, and no token (§4).
- **What may be a default** beside `ON UPDATE`: a constant, or
  `CURRENT_TIMESTAMP`, `LOCALTIMESTAMP` or `CURRENT_DATE` (§2).
- **sm** keeps the update default as it keeps the insert one:
  the host's text, opaque (§5).
- **digres** puts the update default in an UPDATE's target list
  for each column the UPDATE leaves out, in the rewriter, where
  PostgreSQL puts a column's default for `SET c = DEFAULT`
  (§6). Every UPDATE path, ON CONFLICT DO UPDATE and a foreign
  key's cascade among them, is rewritten there.

## 1. Why

A table's rows commonly carry when they were made and when they
last changed, and often a state an edit moves them to.
PostgreSQL gives the first by a default and the others by a
trigger:

```sql
CREATE FUNCTION touch() RETURNS trigger LANGUAGE plpgsql AS $$
BEGIN NEW.mtime := now(); NEW.sync_state := 1; RETURN NEW; END $$;
CREATE TRIGGER job_touch BEFORE UPDATE ON job
    FOR EACH ROW EXECUTE FUNCTION touch();
```

digres has neither `CREATE FUNCTION` in a language nor
`CREATE TRIGGER`. Without either, the application must set
these columns in every UPDATE it writes, and nothing holds it to
that.

## 2. What it means

1. **INSERT** writes the insert default in a column it gives no
   value: the plain `DEFAULT y`, or the update default `x` if
   there is no plain one. `ON UPDATE` changes nothing else an
   INSERT or a COPY does.
2. **UPDATE** writes the update default in a column it does not
   assign, in its `SET` list or its ON CONFLICT DO UPDATE's. A
   column it assigns keeps what it assigns.
3. **Every such UPDATE writes it**, one that changes no other
   value too (§11).
4. **`SET c = DEFAULT`** writes the insert default, as it does
   in PostgreSQL: "the column's default". It is how a row is
   put back as it was made, `sync_state` to 0.
5. **A default beside `ON UPDATE`**, insert or update, is a
   constant — a literal of any type, signed, or cast to a type:
   `0`, `-1`, `'new'`, `'2020-01-01'::date`, `NULL`, `true` —
   or exactly one of `CURRENT_TIMESTAMP`, `LOCALTIMESTAMP` and
   `CURRENT_DATE`, which read the transaction's start. A
   function call, an operator, or `CURRENT_TIMESTAMP(3)` is
   refused when the table is made: nothing is computed but what
   the keyword reads. A plain `DEFAULT` with no `ON UPDATE`
   beside it is as free as PostgreSQL's.
6. **The column is an ordinary column otherwise.** It can be
   read, written, indexed, given NOT NULL, and dropped.

## 3. What others do

| System | Modification time |
| --- | --- |
| PostgreSQL 18 | a BEFORE UPDATE trigger (§1) |
| MySQL, MariaDB | `ts TIMESTAMP DEFAULT CURRENT_TIMESTAMP ON UPDATE CURRENT_TIMESTAMP`, a timestamp's only; it writes only when another value of the row changes |
| SQL:2011 | `GENERATED ALWAYS AS ROW START` in a system-versioned table, which also keeps every old version |

digres's form is MySQL's taken to any type, each default
written as its own clause: `DEFAULT y DEFAULT x ON UPDATE`. A
column with one value for both writes it once.

## 4. Parsing, in sq

sq's grammar is PostgreSQL's, its rules untouched
([`../sq/DESIGN.md`](../sq/DESIGN.md#2-what-is-postgresqls-and-what-is-sqs)
§2), but for one alternative in
`ColConstraintElem` (`../sq/src/gram.y`):

```
| DEFAULT b_expr ON UPDATE
```

- **One token of lookahead is enough.** After `DEFAULT b_expr`
  nothing in PostgreSQL's grammar follows with `ON`, so bison
  shifts it into this rule. A clause of its own, `ON UPDATE x`
  or `ON ROW UPDATE x`, is not: after `REFERENCES t`, `ON` may
  begin the foreign key's `ON UPDATE CASCADE`. Bison reported 3
  shift/reduce conflicts on `ON` for each of those, and none for
  this rule, against the grammar's `%expect 0`.
- **Two defaults are two clauses.** `DEFAULT 0 DEFAULT 1 ON
  UPDATE` is the plain rule, ended by the next `DEFAULT`, then
  this one.
- **The node is the DEFAULT's.** The rule makes the same
  `Constraint` as `DEFAULT b_expr`, of `contype`
  `CONSTR_DEFAULT`, with `generated_when` set to `'u'`: a field
  a default does not otherwise use (an identity's `'a'` or
  `'d'`). No node header changes. sq's printer writes every
  field of a `Constraint`, so its tests tell the two apart.

The cost is to sq's claim to parse as PostgreSQL does, accepted
for this rule (§11). It is kept checkable: `make corpus-test`
holds sq to what PostgreSQL 18.6's parser printed for all
49,034 pieces of its regression scripts, and passes; `make
test` has `test/on_update.sql` and two files of what it
refuses, whose expected output is sq's own.
[`../sq/DESIGN.md`](../sq/DESIGN.md)
§2, §8 and §9 say so.

## 5. The catalog, in sm

`sm_column_spec_t` and `sm_column_def_t` (`../sm/include/sm.h`)
take the update default beside the insert one, as they take it:

```c
const char* on_update_expr = nullptr;   /* spec: the host's, opaque */
bool        has_on_update;              /* def */
std::string on_update_expr;             /* def: "" if none */
```

- **Stored** in the column's catalog record as an appended
  `bytea` field (`../sm/sm3/src/catalog/blob.cc`,
  [`../sm/CATALOG.md`](../sm/CATALOG.md)); a record written
  before reads
  it null.
- **Opaque.** sm keeps the text and gives it back, as it keeps
  the insert default: it runs neither, and checks neither's
  type. An identity column, which may have no default, may
  have no update default either: `SM_ERR_INVALID`.
- **rel** takes it in `table-create` as `:on-update "text"` in
  a column, and shows it so in `describe-table`, so the field
  and its checks are tested where sm is
  ([`../sm/rel/REL.md`](../sm/rel/REL.md)).

## 6. In digres

- **Parse analysis** (`transformColumnDefinition()`,
  `src/backend/parser/parse_utilcmd.c`) takes at most one plain
  and one update default on a column, where it took one
  default: two of either kind are still "multiple default
  values". The plain one, or the update one if there is no
  plain one, becomes the column's `raw_default`, PostgreSQL's
  default. The update default stays in the column's
  constraints, a `CONSTR_DEFAULT` marked `'u'`.
- **CREATE TABLE and ADD COLUMN** (`smstore.c`): with an update
  default, each of the column's defaults is checked to be a
  constant or one of the three keywords (§2, rule 5), then
  cooked as a default is (`cookDefault()`, coerced to the
  column's type) and kept as text, the insert default in
  `default_expr` as now and the update default in
  `on_update_expr`.
- **The table's cached definition** keeps each column's update
  default, and `smstore_update_default(rel, attno)` gives it as
  an expression, or NULL.
- **The rewriter** (`rewriteTargetListIU()`,
  `src/backend/rewrite/rewriteHandler.c`): for an UPDATE, a
  column with no entry in the target list and an update
  default gets one, the update default, as `SET c = DEFAULT`
  gets the insert default there. A table with none is asked
  once a statement, and costs nothing a row.

Every UPDATE is rewritten there: a plain UPDATE, `WHERE CURRENT
OF`, ON CONFLICT DO UPDATE (whose `SET` list the rewriter
treats as an UPDATE's), an UPDATE through an updatable view, and
a foreign key's `ON UPDATE CASCADE` or `SET NULL`, which
updates the referencing rows by SPI. RETURNING returns what was
written. An index on the column stays right: sm3 compares each
index's entry of the row before and after
(`../sm/sm3/src/table/table.cc`, `unchanged()`), whichever
columns a statement named.

## 7. DDL

- **ALTER TABLE ADD COLUMN** takes both. The rows there take
  the insert default, as with any added column; the update
  default acts from their next UPDATE.
- **DROP COLUMN** drops both with the column.
- **ALTER COLUMN SET DEFAULT and DROP DEFAULT** are refused in
  digres today ([`UNSUPPORTED.md`](UNSUPPORTED.md#3-ddl) §3).
  Once built, they change
  the insert default and leave the update default alone; no
  ALTER COLUMN form sets or drops an update default, so a
  column is dropped and added again to change it.
- **CREATE TABLE LIKE INCLUDING DEFAULTS** copies the insert
  default and not the update default.

## 8. The cases

| Case | What happens |
| --- | --- |
| INSERT, COPY, a bulk load into an empty table | the insert default, or the value given |
| A column with only an update default, on INSERT | the update default |
| UPDATE that does not assign the column | the update default |
| UPDATE that assigns it | what it assigns |
| UPDATE `SET c = DEFAULT` | the insert default |
| ON CONFLICT DO UPDATE | as an UPDATE; ON CONFLICT DO NOTHING writes nothing |
| An UPDATE that changes no other value; A → B → A | written each time (§11) |
| An UPDATE of the primary key, which moves the row ([`../sm/CLUSTER.md`](../sm/CLUSTER.md)) | written; the other columns move with it |
| NOT NULL, with an update default of NULL | refused on the first UPDATE that leaves the column out, as an UPDATE writing NULL is |
| DELETE, then INSERT of the same key | a new row: its insert default |
| Abort, rollback to a savepoint, a bulk load rolled back | an ordinary column's value, undone with the row |
| Two transactions | each its own start time; a later commit can carry the earlier time, as `now()` in a trigger does |
| A transaction begun before midnight that updates after it | `CURRENT_DATE` is its start's date, as the keyword reads in it |
| An old snapshot, SERIALIZABLE, an open cursor | an ordinary column |
| An index on the column | its entry changes when the value does (§6) |
| A cascade from a foreign key | the referencing rows are updated, so written |
| Temporary tables, tables without a primary key, TRUNCATE | the same path |
| A table with no update default | one lookup a statement, none a row |

## 9. What it does not do

- **PostgreSQL does not take it.** A schema with it is
  digres's; PostgreSQL refuses it as a syntax error.
- **psql's `\d` shows the insert default only**, and `pg_dump`
  dumps it alone: both read PostgreSQL's catalog, which has a
  place for one default ([`SYSCATALOG.md`](SYSCATALOG.md)). A
  dump restored
  keeps the column and its insert default and loses the update
  default.
- **It is not a commit time.** A time is the transaction's
  start, known when the row is written; when it commits is
  not.
- **It computes nothing from the row.** An update default is a
  constant or a time: no counter from the old value, no value
  from another column.

## 10. Alternatives not taken

- **`ON ROW UPDATE x`**, or `ON UPDATE x`, as a clause of its
  own: either conflicts with a foreign key's `ON UPDATE` after
  `REFERENCES t` (§4), and would need sq's lookahead filter,
  which holds one token, to hold two.
- **MySQL's `DEFAULT y ON UPDATE x`**, both in one clause: as
  free of conflicts, but it reads as an override whatever the
  statement assigns; two clauses say which value is whose.
- **Any expression as an update default**, `now()`,
  `nextval()`: PostgreSQL's default evaluation would run it
  unchanged, but it was kept to constants and the three
  keywords (§11).
- **A stamp written in the executor** for the three keywords
  alone: a time only, where the rewriter gives any type's
  default with no new code to evaluate it.
- **`GENERATED ALWAYS AS (now()) STORED`**: PostgreSQL refuses
  it, `now()` not being immutable, and would not recompute it
  on UPDATE, as it reads no column; and `GENERATED ALWAYS`
  refuses a value given, so a restore of old rows would lose
  their times.
- **Triggers**: no language to write one in, and none is
  wanted.
- **sm stamps the commit time**: a storage trick PostgreSQL
  does not have, and the commit time is not known when the row
  is written.

## 11. Decided

All on 2026-10-27:

1. **The syntax is `DEFAULT x ON UPDATE`**, beside an optional
   plain `DEFAULT y`; not `ON ROW UPDATE x` nor MySQL's form
   (§10).
2. **The insert default is the plain one, or the update one**
   where there is no plain one; `SET c = DEFAULT` writes the
   insert default.
3. **An UPDATE that assigns the column keeps its value**; one
   that does not writes the update default.
4. **An UPDATE that changes nothing writes it too**, as the
   trigger of §1 does; not only when another value changes, as
   MySQL does, which would compare the row before and after.
5. **Defaults beside `ON UPDATE` are constants or
   `CURRENT_TIMESTAMP`, `LOCALTIMESTAMP` and `CURRENT_DATE`**,
   bare.
6. **sq's grammar** takes §4's rule, the first change to what
   sq accepts beyond PostgreSQL's.

## 12. Order of work

1. **sm**: the update default in the column's record, opaque;
   refused on an identity column; rel's `:on-update`. Verify: a
   rel test that a column keeps it across a reopen, and an sm3
   test of `add_column` and of an identity column.
2. **sq**: the rule. Verify: `make test` with new files for the
   rule and for what it refuses, and `make corpus-test`
   unchanged.
3. **digres, parse analysis, CREATE TABLE and ADD COLUMN**.
   Verify: an slt test that both defaults are kept, either
   alone or both, and that two of a kind, a function, an
   operator and `CURRENT_TIMESTAMP(3)` are refused.
4. **digres, the rewriter**. Verify: an slt test for each row
   of §8, each shown to fail with the rewrite taken out; and
   pgbench's UPDATEs at their instructions a transaction before
   and after, on tables with no update default.
5. **The documents**: [`UNSUPPORTED.md`](UNSUPPORTED.md) lists
   `ON UPDATE` as
   digres's own (§10); [`WORKNOTE.md`](WORKNOTE.md) step 307.

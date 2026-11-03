# PKDDL — primary keys and unique constraints added and dropped

**Status: built** (`WORKNOTE.md` step 312). §6 has what was
decided, §7 the order of work, §8 what building it found. It
is `../sm/CLUSTER.md` §11's step 3, digres's part; sm's is
built.

```sql
CREATE TABLE p (a int4, b text);
INSERT INTO p VALUES (1, 'x'), (2, 'y');

ALTER TABLE p ADD PRIMARY KEY (a);
-- a is NOT NULL now; the rows are keyed by a, in a new store
ALTER TABLE p ADD CONSTRAINT p_b UNIQUE (b);
ALTER TABLE p DROP CONSTRAINT p_pkey;
-- the rows are keyed by _id again; a stays NOT NULL
ALTER TABLE p DROP CONSTRAINT p_b;

DROP INDEX p_pkey;
-- ERROR: cannot drop index p_pkey because constraint p_pkey on
--        table p requires it
```

Before step 312 digres refused `ADD CONSTRAINT` for anything
but a foreign key, found no primary key or unique constraint
to `DROP CONSTRAINT`, and let `DROP INDEX` drop a constraint's
index, which PostgreSQL refuses.

---

## 0. In brief

- **sm has it all** (`../sm/CLUSTER.md` §8): a primary index
  made on a table with rows copies them into a new store under
  the new key (`sm3::create_index`, `rekey()`), and one dropped
  keys them by `_id` again; a unique constraint's index is an
  index marked `constraint`; `sm_cat_set_not_null` refuses a
  column holding a null.
- **digres's part**: parse analysis as PostgreSQL's
  (`transformAlterTableStmt()`) turns `ADD PRIMARY KEY` and
  `ADD UNIQUE` into an `IndexStmt` and the key columns' `SET
  NOT NULL`; `smstore.c` runs them through what CREATE TABLE
  and CREATE INDEX use. `DROP CONSTRAINT` finds a constraint's
  index by name and drops it.
- **One sm fix** (§8): sm3's rewrite wrote a store it made
  inside a statement, which the statement's view could not
  see.

## 1. What PostgreSQL 18 does

Run on PostgreSQL 18.6:

| Statement | PostgreSQL 18 |
| --- | --- |
| `ADD PRIMARY KEY (a)`, a nullable | a made NOT NULL, then the index |
| a holds a NULL | `column "a" of relation "q" contains null values` |
| a holds a duplicate | `could not create unique index "r_k"`, `DETAIL: Key (a)=(1) is duplicated.` |
| a second primary key | `multiple primary keys for table "u" are not allowed` |
| `DROP CONSTRAINT p_pkey` | the index goes; a stays NOT NULL |
| … a foreign key references it | refused, `cannot drop constraint s_pkey on table s because other objects depend on it`; CASCADE drops the foreign key |
| `DROP INDEX p_pkey` | refused, `HINT: You can drop constraint p_pkey on table p instead.` |
| `ADD … UNIQUE USING INDEX ui` | turns an index into the constraint's |

## 2. What is built

1. **ADD PRIMARY KEY, ADD UNIQUE**, named or not, with
   INCLUDE: through `transformAlterTableStmt()`, as
   `add_generated_column()` uses it. Its NOT NULL subcommands
   go to `sm_cat_set_not_null`; its `IndexStmt` to
   `smstore_create_index()`, which passes `primary` and
   `constraint` to sm already.
2. **DROP CONSTRAINT** of a primary key or unique constraint:
   its index found by name among the table's constraint
   indexes, and dropped (`sm_cat_drop_index`). A foreign key
   that references it: RESTRICT refuses with PostgreSQL's
   error; CASCADE drops each such foreign key first, with its
   NOTICE.
3. **DROP INDEX** of a constraint's index: refused, with
   PostgreSQL's error and hint.

## 3. Not built

- **`USING INDEX`**: refused, as now. sm would have to mark an
  existing index a constraint's, or re-key by it.
- **The key-column ALTERs** (`ALTER COLUMN TYPE`, a collation
  change) that §11 step 3 names: built later, as ALTER COLUMN
  TYPE of any column (`ALTERTYPE.md`).
- **DEFERRABLE**: refused, as for CREATE TABLE.

## 4. The cases

| Case | What happens |
| --- | --- |
| Empty table | sm keys it anew in place, under the table-oid lock X |
| Table with rows | sm copies it into a new store; the old is dropped at commit |
| TOAST | sm copies the chunks with the rows, the pointers renamed (`CLUSTER.md` §8) |
| Secondary, partial and expression indexes | refilled by the rewrite, through the evaluator on the session's thread (`EXPRINDEX.md`) |
| A NULL in a key column | `sm_cat_set_not_null` fails first: PostgreSQL's words |
| A duplicate key | the rewrite fails: PostgreSQL's `could not create unique index`, with the key |
| ADD UNIQUE on a table with rows | built after the commit, as CREATE UNIQUE INDEX is (`DESIGN.md` §4): in a transaction block a duplicate is reported at COMMIT |
| A second primary key | sm refuses; PostgreSQL's words |
| Abort, savepoint rollback | the new store is dropped; the catalog as it was |
| Crash before commit | the new store is unreferenced, as a TRUNCATE's swap leaves one: after a restart the table has its rows, no index, its column nullable (`server.sh`) |
| Other sessions | the table-oid lock X shuts them out until commit; a prepared plan's cached descriptor is invalidated |
| Open cursor in this transaction on the table | refused, as PostgreSQL refuses it: `alter_table_rel()` calls `CheckTableNotInUse()` |
| A foreign key referencing a unique constraint dropped | RESTRICT refuses; CASCADE drops it |
| A foreign key of this table | unchanged: its columns keep their places |
| Temporary table | as any; sm's rewrite of a temporary store |
| A table with no primary key, `_id`s | DROP gives the rows `_id`s again from the table's sequence |
| Views | unchanged: no column moves |
| pg_index, `\d`, `pg_constraint` | read sm's definition, as for CREATE TABLE's |

## 5. Tests

- **mvcc**: none; sm1 is unchanged.
- **sm**: `rel/test/test_pkey.rel` step 12, a primary key
  added and dropped inside a statement, as digres runs them,
  and a duplicate found there (§8).
- **dg**: `test/slt/pk_ddl.test`, §4's cases SQL reaches, its
  results PostgreSQL 18's; `server.sh`, a crash before the
  commit; pgregress's 51 files whose SQL adds or drops a
  constraint or an index, trimmed again.
- **Shown to fail** with each part disabled, one at a time:
  the NOT NULL of a primary key's columns, the filter of the
  foreign keys on the dropped index, the DROP INDEX refusal,
  the INCLUDE columns in a name, the constraint found by
  name, CASCADE's drop, the second primary key's check, and
  sm3's statement advance.

## 6. Decided

1. **Unique constraints too** (§2.1, §2.2): the same code path
   as the primary key; leaving them out leaves `ADD UNIQUE`
   refused.
2. **DROP INDEX of a constraint's index refused** (§2.3): a
   change to what digres takes today, to PostgreSQL's.
3. **CASCADE drops referencing foreign keys** (§2.2).
4. **`USING INDEX` and the key-column ALTERs left out** (§3).

## 7. Order of work

1. ADD PRIMARY KEY, ADD UNIQUE. → verify: slt.
2. DROP CONSTRAINT, RESTRICT and CASCADE. → verify: slt.
3. DROP INDEX refused. → verify: slt, pgregress.
4. pgregress trimmed again; `UNSUPPORTED.md` §3 updated.

## 8. What building it found

- **sm3's rewrite inside a statement.** `rewrite_table()`
  makes a store and copies the rows into it in one call; a
  statement's view sees only the writes made before its mark,
  so the store's catalog row was not there to the copy, and
  ADD PRIMARY KEY on any table with rows failed, `store N
  does not exist`. `rel`'s test ran with no statement begun.
  The rewrite now advances the mark after making the store
  (`sm1::stmt_advance()`, which says a store made in a
  statement is used so).
- **An unnamed index with INCLUDE** was named by its key
  columns alone; PostgreSQL's `DefineIndex()` names it by its
  INCLUDE columns too: `u_b_a_key`, not `u_b_key`. CREATE
  INDEX and CREATE TABLE had it wrong as well, and
  `test/slt/key_constraint_catalog.test` had kept the wrong
  name, `kp_b_c_key` for PostgreSQL's `kp_b_c_d_key`.
- **`get_attnum()`** reads a syscache digres has not; the key
  columns are looked up in the relation's descriptor.

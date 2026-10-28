# EXPRINDEX — partial and expression indexes

**Status: built** (`WORKNOTE.md` step 311). §9 has what was
decided, and §10 the order of work and the tests.

```sql
CREATE TABLE account (
    id         bigint PRIMARY KEY,
    email      text   NOT NULL,
    deleted_at timestamptz
);

-- partial: only the rows the WHERE takes are in the index
CREATE INDEX account_live ON account (id) WHERE deleted_at IS NULL;

-- expression: the key is lower(email), not email
CREATE UNIQUE INDEX account_email ON account (lower(email));

-- both
CREATE UNIQUE INDEX account_live_email ON account (lower(email))
    WHERE deleted_at IS NULL;

SELECT id FROM account WHERE lower(email) = 'a@b.c';
-- Index Scan using account_email
SELECT id FROM account WHERE deleted_at IS NULL AND id < 10;
-- Index Only Scan using account_live
INSERT INTO account VALUES (1, 'A@b.c', NULL), (2, 'a@B.C', NULL);
-- ERROR: duplicate key value violates unique constraint
--        "account_email"
-- DETAIL: Key (lower(email))=(a@b.c) already exists.
```

A **partial index** holds only the rows its predicate, the
`WHERE`, is true for. An **expression index** has a key
column that is an expression over the row, not a column. One
index may be both. digres refused both, in
`check_create_index()` (`src/backend/access/sm/smstore.c`),
until step 311.

---

## 0. In brief

- **sm has the hard part, built and tested.** sm3 keeps an
  index's predicate and expressions as bytes it does not read,
  and asks the host's **evaluator**, a callback, whether a row
  belongs and what its expression keys are, on every write,
  build and ANALYZE (`../sm/sm3/SM3_DESIGN.md` §3,
  `sm_set_index_evaluator` in `../sm/include/sm.h`). `rel` has
  an evaluator of its own and tests it
  (`../sm/rel/test/test_partial.rel`).
- **digres's part**: register an evaluator that runs
  PostgreSQL's executor on the expressions; pass them to sm at
  CREATE INDEX; and show them in `pg_index`, from which the
  relcache and planner take the rest as PostgreSQL does.
- **The one hard question** is threads (§5): sm3 calls the
  evaluator from its build workers, threads digres did not
  start, where there is no memory context to evaluate in. Such
  a thread is **adopted** at its first call, set up as a
  session thread is, without a session.
- **No sm change.** No sq change.

## 1. Why

- `NEXT.md` §1's cheapest gap: 67 statements of PostgreSQL's
  regression files are taken out for it.
- Common in schemas: `WHERE deleted_at IS NULL`, a unique
  `lower(email)`, a unique key over the live rows only.
- A partial index is smaller than the full one and costs a
  write nothing for a row it does not hold.

## 2. What is taken, and what is refused

Taken, as PostgreSQL takes it:

- A predicate, `WHERE`, of the table's columns, constants and
  immutable functions and operators.
- A key column that is an expression of the same, of a type sm
  has (`type_code()` in `smstore.c`), ASC or DESC, NULLS FIRST
  or LAST.
- UNIQUE with either; INCLUDE with either (INCLUDE columns are
  plain columns).

Refused, as PostgreSQL refuses it (its own checks, in
`indexcmds.c`'s `CheckPredicate()` and `ComputeIndexAttrs()`):

- A function that is not IMMUTABLE: "functions in index
  expression must be marked IMMUTABLE".
- A subquery, an aggregate, a window function, a set-returning
  function, a system column.

Refused, as digres refuses a column key today:

- An expression of type interval, or of a type sm lacks.
- An expression of type numeric with no precision and scale,
  `(a * 2)`: sm keys a decimal at its typmod's scale, which
  would round its values. `((a * 2)::numeric(12, 2))` is taken.
- A collation or an operator class other than the default: an
  expression's collation the default, the database's, C's
  order, and not `COLLATE "C"` written, which PostgreSQL keeps
  as the key's collation; `(column COLLATE c)` the column's
  own, as a column key's.
- An expression that reads the whole row, which digres's
  parse analysis refuses before (`relation "t" does not have a
  composite type`).

Refused by sm, as PostgreSQL does too:

- A primary key, or a UNIQUE or PRIMARY KEY constraint, over an
  expression or with a predicate: SQL cannot write one, and
  sm3's `create_index` refuses it.
- A foreign key referencing such an index (`sm.h`, the parent's
  unique index "with no predicate or expression").

## 3. What sm has

- `sm_cat_create_index(..., eval, reads, ...)`: `eval` is the
  host's bytes, `reads` the table columns they read. A key
  column that is an expression has `attnum = -1` and gives its
  own `type_code` and `typmod`.
- The evaluator, `sm_index_eval_fn`: given the table and index
  oids, `eval`, and a row as one `sm_datum_t` and null flag a
  column, it sets `*belongs` and fills each expression key's
  bytes. It returns 0, or -1 with `sm_seterr`. It is
  `noexcept`, deterministic, callable from any thread, and does
  not call sm on the writing session.
- An update that changes none of `reads`, nor a plain key or
  INCLUDE column, leaves the index alone without a call. One
  that does evaluates both images.
- A value out of line that the evaluator reads is fetched
  before the call. A dropped column keeps its place, so a
  stored `Var` stays right; a column an index reads cannot be
  dropped (`ddl.cc`'s `drop_column`).

## 4. In digres

### 4.1 CREATE INDEX

`smstore_create_index()` took column names alone. It is given
what `transformIndexStmt()` already made: each `IndexElem`'s
`expr`, transformed, and `whereClause`.

1. `check_create_index()` stops refusing `whereClause` and an
   `IndexElem` with `expr`; it refuses an expression among the
   INCLUDE columns, as `ComputeIndexAttrs()` does.
2. `(column)` and `(column COLLATE c)` are the column, as
   `ComputeIndexAttrs()` takes them.
3. The predicate and each expression get PostgreSQL's checks
   (`check_index_expr()`): no function that is not immutable
   (`contain_mutable_functions_after_planning()`, as
   `CheckPredicate()` and `ComputeIndexAttrs()` call it, with
   their words), no system column (`DefineIndex()`'s). Those
   functions are static and do more, so the checks are written
   again, a few lines.
4. An expression key column gives sm its `type_code()` and
   typmod (`exprType()`, `exprTypmod()`), and is refused as §2
   says.
5. `eval` is `nodeToString()` of a List of three
   (`index_eval_string()`): the expressions in key order, the
   predicate as an implicit-AND list, as `pg_index.indexprs`
   and `indpred` hold them, and the expression columns' names,
   as `ChooseIndexColumnNames()` chose them (`lower`, `expr`).
   `reads` is `pull_varattnos()` of both, made 0-based.

### 4.2 The catalog digres shows

PostgreSQL's planner needs nothing new: it matches an
expression key (`match_index_to_operand()`) and proves a
predicate (`predicate_implied_by()`) from the relcache's
`rd_indexprs` and `rd_indpred`. digres has to fill them in.

- **The relcache**: digres builds an index's descriptor from
  sm's definition, with no `pg_index` tuple
  (`RelationBuildSmIndexDesc()`). It now reads `eval`, types
  and names each expression column from its expression, and
  sets `rd_indexprs` and `rd_indpred` as
  `RelationGetIndexExpressions()` and
  `RelationGetIndexPredicate()` would make them ready: folded,
  with their operators' functions set.
- **`pg_index`**: `index_row()` (`catalog/catrel.c`) sets
  `indexprs` and `indpred` from `eval`, in place of null;
  `indkey` holds 0 for an expression column, as PostgreSQL's;
  an expression column's `pg_attribute` row takes its type,
  typmod and name from the expression
  (`index_expr_columns()`).
- **`pg_get_indexdef()`** is digres's own for an index in sm
  (`sm_indexdef()`, `ruleutils.c`), which wrote columns alone.
  It writes an expression and the `WHERE` as PostgreSQL's
  does, from `eval`, unfolded; psql's `\d` reads it.

### 4.3 The evaluator

Registered once, with `sm_set_index_evaluator`, where digres
makes its sm environment, before a database opens (a restart
may resume a build at once).

On a call:

1. Find this thread's prepared state for the index oid
   (`index_eval_ready()`): the expressions and the predicate,
   read from `eval` and made ready with `ExecPrepareExpr()` and
   `ExecPrepareQual()`, in a memory context of the thread's
   own. Made at the first call for that index on that thread,
   and kept: an index's `eval` never changes, and an oid is
   never reused. A row of more columns, one added since, makes
   it again.
2. Put sm's row in a virtual slot as it is: sm's datums are
   PostgreSQL's, the decimals among them, bit for bit
   (`DG_DECIMAL_PLAN.md`). The slot's descriptor has the type
   of each column a Var reads, taken from the Var, as the
   executor checks a Var against its slot; the table's own
   descriptor would need the relcache.
3. Evaluate the predicate; if false or null, `*belongs` is
   false, and no expression is evaluated, as PostgreSQL
   evaluates none for a row its predicate turns away.
4. Evaluate each expression, and give sm each value's bytes:
   by value, fixed-width, or a varlena's whole
   (`key_value_bytes()`).
5. Each call's memory is the per-tuple context, reset at its
   end.

Its catalog lookups (`fmgr_info()`, a type's I/O, a collation)
land in the compiled-in catalogs alone: `pg_proc`, `pg_type`,
`pg_operator`, `pg_cast`, `pg_collation`
(`catrel_is_builtin()`). digres makes no function, type,
operator or collation of its own, so an index expression can
name no other. So the evaluator never reads sm, and the rule
that it not call sm on the writing session holds by
construction.

### 4.4 Errors

A PostgreSQL error is a C++ exception in digres (`CODE.md`
§2), which must not cross sm's frames: the evaluator is
`noexcept`.

- The call is wrapped in `PG_TRY()`. A caught error is copied
  (`CopyErrorData()`), flushed, kept in the thread's control
  block, and the call returns `sm_seterr(SM_ERR_INVALID,
  message)`.
- **On a session's own thread** (a write, a build in the
  transaction that made the table, ANALYZE, COPY), when sm's
  call returns the failure, digres re-raises the kept error
  (`ReThrowError()`): the user sees PostgreSQL's error,
  "division by zero", SQLSTATE 22012, as PostgreSQL says it.
- **On a build worker**, the error is the build's reason;
  `smstore_wait_indexes()` raises it as now, `could not create
  index "x": division by zero`, with sm's SQLSTATE. PostgreSQL
  says `division by zero` alone, 22012. A small difference,
  noted in `UNSUPPORTED.md`.

## 5. Threads: the one hard question

sm3 calls the evaluator from four places:

| Caller | Thread | digres context |
| --- | --- | --- |
| A write: INSERT, UPDATE, DELETE, upsert | the session's | yes |
| A build of a table this transaction made | the session's | yes |
| ANALYZE; COPY's bulk load | the session's | yes |
| A build after commit, and one a restart resumes | sm3's builder workers (`builder.cc`, up to 4) | **none** |

A builder worker is a `std::thread` sm3 starts. Its `tcb` is
NULL: no memory context, no error context, no GUCs, no default
collation. PostgreSQL's executor cannot run there as it is.

**Built (A): adopt the thread** (`session_thread_adopt()`,
`utils/init/session_thread.c`). At a call with `tcb` NULL, the
evaluator sets the thread up as `session_thread_main()` does a
session's, without a session: `tcb_init()`,
`MemoryContextInit()`, `InitializeGUCOptions()`, the relation
and catalog caches, empty (`RelationCacheInitialize()`,
`InitCatalogCache()`), the portal manager a commit visits,
`init_database_collation()` (C, digres's only one), and the
superuser as its user, whom `ExecInitFunc()`'s permission
check asks after. A `thread_local` object's destructor tears it
down as `session_thread_end()` does, when sm3's worker ends.

The catalogs are read only in a transaction (the relcache
asserts one), so `index_eval_ready()` runs in one of its own
on a thread that has none: a transaction of digres's, which
touches nothing of sm's, as a compiled-in catalog's rows are
digres's own. The evaluation after it runs in none.

- Runs as many evaluators as sm3 has workers, with no handoff.
- No sm change: sm3 already promises a call "may be called from
  any thread".
- Costs a set-up per worker start, milliseconds, against a
  build of a table with rows.
- Settings are their defaults on a worker, not the session's.
  An IMMUTABLE function depends on none; one that does is one
  PostgreSQL also calls wrongly marked (§8).

## 6. Scans and plans

- **Index scan**: a key on an expression column is converted to
  sm's bytes by the index column's type, as a column key is;
  the index's tuple descriptor has the expression's type
  (§4.2). sm's scan bounds the range by it, but digres's check
  of each row against its keys (`keys_met()`) reads the row's
  column, and an expression has none: the executor checks such
  a scan's rows against the original quals instead, as a lossy
  index's (`smstore_index_recheck()`, `IndexNext()`).
- **Index-only scan**: the same, its rows checked against the
  quals on the index's columns (`recheckqual`), the entry
  giving the expression's value, when a key is on an
  expression. PostgreSQL plans one only when every column the
  query reads is a plain column of the index, as in an index
  on `(lower(e), e)`.
- **Upsert**: `ON CONFLICT (lower(email))` and `ON CONFLICT (id)
  WHERE deleted_at IS NULL` infer their arbiter from
  `indexprs`/`indpred` (`infer_arbiter_indexes()`), and sm's
  `sm_tbl_insert_arbiter` checks by index oid, a row not
  belonging meeting no conflict.
- **Duplicate key**: a write's (`duplicate_write()`) forms
  the key with PostgreSQL's `FormIndexDatum()`, which
  evaluates an expression on the row. A build's (`dup_key()`)
  evaluates it on the row read back, on the session's thread,
  and names it as `pg_get_indexdef_columns()` would
  (`pg_get_index_expr_column()`): `Key (lower(email))=(a@b.c)`,
  `Key ((id % 10::bigint))=(1)`, PostgreSQL's words.
- **Size and statistics**: a partial index's row estimate is
  its own entries' count, which sm already gives per index
  (`sm_tbl_idx_size`); `STATISTICS.md` already lays out an
  expression's statistics as the index's rows. No plan of the
  tests needed more, so §10 step 6 is not built.

## 7. The cases

| Case | What happens |
| --- | --- |
| TOAST | sm fetches an out-of-line value the evaluator reads before the call; the evaluator's own result is bytes sm stores inline in the key |
| Non-unique / unique | sm3's own write paths, with `belongs` and the computed key |
| Primary key | cannot be partial or an expression (§2) |
| INCLUDE | plain columns, as now |
| Cut keys | a long expression key in a unique index is refused as a long column key is (`SM3_DESIGN.md` §7) |
| Update A→B→A | sm3 compares the `reads` columns' bytes; equal images leave the index alone |
| INCLUDE-only update | as now: entry rewritten, evaluator called only if `reads` changed |
| Update into, out of, the predicate | sm3 evaluates both images: entry removed if the old belonged, added if the new does |
| Primary key change (row moves) | both images evaluated, as any update |
| Delete then reinsert | delete removes by the old image's key; insert adds by the new |
| Abort, savepoint rollback, undo overflow | sm1 undoes its writes by key; no evaluator |
| Crash; a build resumed | the evaluator is registered before open; the worker is adopted (§5) |
| 2PC across restart | no evaluation at commit |
| Two writers, RR, RC re-read, SSI, open cursor | sm3's, as for any index; EvalPlanQual re-checks the statement's qual, not the predicate |
| GC watermark, CF:txn sweep | no evaluator |
| CREATE INDEX pending | built after commit by workers (§5); a write meanwhile evaluates on its own thread |
| DROP INDEX, TRUNCATE | as now |
| ALTER rewrite (primary key DDL) | sm3 rewrites the entries on the session's thread |
| DROP COLUMN the index reads | sm refuses, as for a key column: "Drop the index first."; PostgreSQL drops the index |
| Temporary table | as a plain index on one |
| Keyless (`_id`) table | as a plain index on one |
| Bulk load, and its rollback | COPY's load evaluates on the session's thread; rollback as any load's |
| An error in the expression | §4.4 |
| A function wrongly marked IMMUTABLE | an entry a delete cannot find; PostgreSQL's hazard too (§8) |

## 8. What it does not do

- **Settings on a worker are their defaults.** A function whose
  result depends on a setting is not immutable; PostgreSQL's
  `extra_float_digits` and `float8::text` are the known case,
  where PostgreSQL also gives keys that differ by session.
- **A build worker's error is wrapped** (§4.4).
- **No collation but C**, as for every key.

## 9. Decided

1. **Threads: A, adopt the worker** (§5), not §11's B or C.
2. **Both kinds at once**: they share the evaluator, the blob
   and the catalog; partial alone would save only the key's
   type and conversion.
3. **Index-only scans of an expression column off** (§6), as
   proposed; built, they proved to need no such rule, only
   the recheck.
4. **A build worker's error wrapped, with sm's SQLSTATE**
   (§4.4).

## 10. Order of work

Tests at each layer, each shown able to fail:

- **mvcc**: none. sm1 knows no index and no evaluator.
- **sm**: none new; sm changes nothing. `rel`'s
  `test_partial.rel` and sm3's tests cover the evaluator's
  callers.
- **dg**: `test/slt/expr_index.test`, the §7 cases SQL can
  reach, its results PostgreSQL 18's; two `server.sh` checks,
  one of PostgreSQL's DETAIL for a duplicate in a write, in a
  build on the session's thread and on a worker, an error in
  an expression, `pg_get_indexdef()` and COPY, and one of a
  build a crash cut short, resumed on an adopted worker; and
  pgregress's files put back and trimmed again. A `run.sh`
  file stops at its first error, so took none of it.
- **Shown able to fail**, each check with one part disabled:
  the predicate, the index scan's and the index-only scan's
  recheck, the error raised again, the adoption, `reads`,
  `rd_indpred`, `pg_index.indexprs` and the duplicate's
  expression text.

Steps:

1. The evaluator on a session's thread, and CREATE INDEX on a
   table made in the transaction: writes, unique, errors
   re-raised. → verify: slt.
2. `pg_index` and `pg_attribute`: the planner chooses the
   indexes; `\d` prints them. → verify: EXPLAIN in slt.
3. Adopted worker threads: a build of a table with rows, and
   one resumed after a restart. → verify: `server.sh`.
4. Upsert arbiters and the duplicate key's detail. → verify:
   slt.
5. pgregress statements put back; `UNSUPPORTED.md` updated.
6. If a plan needs it: a partial index's row count and an
   expression's statistics.

## 11. Alternatives not taken

- **B: hand a worker's rows to a digres thread.** The worker
  queues each row to an evaluator thread digres starts, and
  waits. Nothing to adopt, but a handoff a row, and the builds
  of four workers run through one thread.
- **C: build such an index on the session's thread.** No worker
  evaluates. It needs sm3 to build in the caller, a second
  build path, and a CREATE INDEX that holds its session for the
  build, as sm3's design chose not to (`SM3_DESIGN.md` §3).
- **D: an evaluator in sm** over a subset of expressions. sm3's
  design refused it: a second implementation of PostgreSQL's
  functions, which must agree row for row.

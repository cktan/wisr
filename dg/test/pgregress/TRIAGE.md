# Triage of the `--dg-differs` statements

Each `--dg-differs` statement of `sql/`, put back alone and
run on PostgreSQL 18 and on digres (`pgregress.py differs`,
which writes `build/test/pgregress/differs/FILE.json`), sorted
by what the two outputs show. At `d383a2f2`, 1,483 statements;
the bugs below reproduce again at `89cbdc2f`, where pgregress
passes all 231 files. `triage.py` sorts a dump as below.

## By category

| Statements | Category | Verdict |
| --- | --- | --- |
| 513 | PostgreSQL tests an error; digres stops earlier, at another (`function does not exist` 320, `language does not exist` 74, numeric NaN 43, ...) | missing features, not differences: should be `skip`; but issues 6, 9 and 10 |
| 407 | PostgreSQL tests an error; digres refuses the feature first (`not supported`, no catalog tables) | missing features: should be `skip` |
| 159 | `EXPLAIN` gives another plan | expected: sm's statistics and costs |
| 120 | the same rows in another order: no `ORDER BY` | expected |
| 57 | only notices differ: digres's own (`REINDEX does nothing`, `CLUSTER does nothing`, no statistics for a temporary table, ...) | by design |
| 20 | the same error with another DETAIL or HINT | minor |
| 59 | digres accepts what PostgreSQL rejects | below |
| 147 | another result | below |
| 1 | digres errors where PostgreSQL does not | a refused ALTER TABLE: `skip` |

So 920 of the 1,483 are missing features that the trim
counted as differences, because PostgreSQL errors on them
too: `classify()` should call one `skip` when digres's error
is one a statement meets on its own (`INTRINSIC`), whatever
PostgreSQL says.

## Other results, by cause

Of the 147, all but the bugs below follow from a decision:

- **The decimal types** (`../../DECIMAL.md`): a bare
  `numeric` column is `numeric(17,3)`, so `-5.499999` is
  stored as `-5.500`; a product past the type's range, and
  jsonb's numbers, are double precision; `^`, `sqrt` lose
  digits. `numeric` 24, `combocid`, `matview`, `subselect`,
  `insert_conflict`, `expressions`, `jsonb` 3.
- **The catalog's contents**: three access methods, no
  descriptions of built-in objects, no `lo_*` functions, no
  TOAST relations, no primary keys on catalogs, `numeric`'s
  storage `plain`. `psql` 10, `opr_sanity` 7, `create_view`
  2, `misc_sanity` 2, `type_sanity` 3, `amutils`,
  `btree_index`, `rules`, `sanity_check`, `replica_identity`
  2, `alter_table` 3, `tablespace`.
- **The hash access method is in name only**: digres has no
  hash index, and the method has no callback to build or scan
  one, so `pg_indexam_has_property(hash, 'can_exclude')`,
  computed from its `amgettuple`, is false; true would promise
  what CREATE INDEX USING hash refuses. `amutils`
  (`../../WORKNOTE.md` step 310).
- **A serial column's sequence is `bigint`**, to
  9223372036854775807, not `integer`: sm's sequences have no
  type. `psql` 18, `identity`, `sequence`.
- **A not-null constraint is no row of `pg_constraint`**, so
  `\d+` lists none. `constraints` 9, `inherit` 2, `cluster`,
  `foreign_data`.
- **No compression**: `pg_column_compression()` is null.
  `compression` 9.
- **Sampling reads sm's blocks**, not heap pages: `TABLESAMPLE`
  picks other rows, or none of a 10-row table. `tablesample` 7.
- **Sizes and estimates**: `pg_relation_size()`, `reltuples`
  of a table never analyzed (-1, not 0). `insert`,
  `maintain_every`.
- **`pg_get_viewdef()` deparses otherwise**: `c` where
  PostgreSQL writes `tt3.c`, and a join alias's column list,
  `j(b, c_1, a, d, e, ax, ay, q)`. `create_view` 21. Not yet
  checked that each reads back as the same view.

## Accepted where PostgreSQL refuses

- **What a no-op command does not check**: REINDEX, VACUUM
  and CLUSTER are refused by PostgreSQL inside a transaction
  block, on a system catalog CONCURRENTLY, into a tablespace
  that does not exist, or with a bad option
  (`BUFFER_USAGE_LIMIT`, `PROCESS_TOAST`); digres does
  nothing and says so. `tablespace` 18, `create_index` 13,
  `vacuum` 6, `transactions` 2, `index_including`, `cluster`,
  `psql_pipeline`.
- **Clauses digres ignores**: `COMPRESSION` (an unknown
  method, or one on an integer), and `STORAGE EXTENDED` on an
  integer. `compression` 4, `alter_table`.
- **Not-null constraints' names and NO INHERIT**, which digres
  does not keep: conflicting names or declarations are
  accepted. `constraints` 7, `inherit` 3.
- Two that are bugs, below.

## Bugs

Each reproduces alone on both servers, and is an issue in
`../../ISSUE.md`, numbered there as here, with its repro.

| Issue | What | Where | Priority |
| --- | --- | --- | --- |
| 3 | Fixed, `../../WORKNOTE.md` step 300: a numeric range inside an array or a multirange came back with a garbage bound: `nummultirange(numrange(5.0, 6.0), numrange(1.0, 2.0))` is `{[1.0,2475880078570760549798248448)}`, `ARRAY[numrange(1.1, 1.2), numrange(12.3, 155.5)]` likewise. `numrange` alone is right. | `multirangetypes`, `rangetypes` | high: a wrong answer |
| 4 | Fixed, `../../WORKNOTE.md` step 304: a cursor saw its own transaction's later DELETE: after `DELETE ... WHERE CURRENT OF c1`, `MOVE BACKWARD ALL` and `FETCH ALL` missed the deleted row, which a cursor's snapshot keeps. | `portals` | high: a wrong answer |
| 5 | Fixed, `../../WORKNOTE.md` step 305: a `FOR UPDATE` cursor in a SERIALIZABLE transaction returned a row the transaction updated after DECLARE; PostgreSQL returns none. | `portals` | medium |
| 6 | Fixed, `../../WORKNOTE.md` step 302: a duplicate key in a table made in a savepoint said `could not open relation with OID N`, not the unique violation: `BEGIN; SAVEPOINT x; CREATE TABLE koju (a int UNIQUE);` and the same INSERT twice. | `transactions` | medium: the row is refused, the error is wrong |
| 7 | Fixed, `../../WORKNOTE.md` step 303: `ON COMMIT DELETE ROWS` on a table a foreign key references committed; PostgreSQL refuses it, as the delete would break the key. | `temp` | medium |
| 8 | Fixed, `../../WORKNOTE.md` step 307: `CREATE TABLE pg_catalog.t()` succeeded; PostgreSQL refuses it. | `alter_table` | low |
| 9 | Fixed, `../../WORKNOTE.md` step 306: `INSERT ... ON CONFLICT` under REPEATABLE READ or SERIALIZABLE failed with sm1's `sm1_read_latest requires READ COMMITTED` on a key its own statement took; DO NOTHING failed where PostgreSQL succeeds. | `insert_conflict` | low: the message |
| 10 | Fixed, `../../WORKNOTE.md` step 309: `CREATE TABLE t (LIKE a, LIKE a)` failed with sm's `create table: a column without a n...`, not `column "x" specified more than once`. | `create_table_like` | low: the message |
| 11 | Fixed, `../../WORKNOTE.md` step 308: a foreign key's `SET DEFAULT (c, c)` kept the column twice, and the parent's row could not be deleted; PostgreSQL keeps it once. | `foreign_key` | low |
| 12 | Not a bug, `../../WORKNOTE.md` step 310: the hash access method says `can_exclude` is false, PostgreSQL's true; digres has no hash index (above). | `amutils` | low |

# INLIST — an index scan of `= ANY(array)` and `IN (...)`

Status: built (`WORKNOTE.md` step 314).

## 1. What

```sql
select c1 from wide_mixed where id = any(array[7, 3, 7, 900]);
select c1 from wide_mixed where id in (7, 3, 900);
select * from orders where cust = any($1) and day > '2026-01-01';
```

PostgreSQL reads each through its index: one descent per
distinct value, in the index's order. digres reads the whole
table: its index access method says it takes no array
(`amsearcharray = false`, `src/backend/access/sm/smindex.c`),
so the planner offers no index path for the clause, and a
sequential scan filters every row; with `enable_seqscan` off
the plan is still `Seq Scan`, disabled. An IN list is what an
ORM's prefetch and a batch loader send, and the benchmark's
Q7 is one (`../bench/DESIGN.md` §5): a million rows read for
100.

## 2. Design

- **The access method takes arrays**: `amsearcharray = true`.
  The planner then makes `col = ANY(array)` an index clause,
  with btree's costs (`btcostestimate()` counts one descent a
  value), and turns `col = 1 OR col = 2` into one, as for
  PostgreSQL's btree. The executor hands the clause to the
  scan as a key with `SK_SEARCHARRAY` and the array as its
  argument, evaluated again at each rescan when it holds a
  parameter (`ExecIndexBuildScanKeys()`).
- **An array on the equality prefix is probed**
  (`smstore_index_beginscan()`, `smstore.c`). An array key of
  the equality strategy, on a column every earlier column of
  which has an equality or such an array, gives that column
  its values: the array's elements as the column's datums,
  as a plain key's are (`plain_key()`, the integer of another
  integer type), nulls and values out of the column's range
  dropped, sorted in the index's order by the comparison
  function of the column type's default btree operator
  class, the one every index in sm has (`relcache.c`), a
  DESC column's descending, and duplicates dropped. The scan
  is then one probe a value -- a combination, in order, when
  several columns have arrays -- each the bounds a single
  equality gives today: a read of one row by its key when the
  equalities cover the primary key, a range otherwise. The
  next probe opens when one ends.
- **In the index's order**: each probe's rows come in the
  index's order and the probes ascend, so the scan's do, as
  the planner assumes of a btree's array scan since
  PostgreSQL 17, and an `ORDER BY` on the index needs no
  sort.
- **Any other array key is checked of each row**: one after
  a range's column, of another strategy (`< ANY`), or of a
  type the bounds do not take. `keys_met()` checks it by the
  key's operator against each element, deconstructed once.
- **No value, no row**: an empty array, or one of nulls only,
  or a null array, makes a scan that returns none.

## 3. Cases

| Case | What happens |
| --- | --- |
| primary key, every column equal | one read by key a value (`sm_tbl_read_key()`), as `id = k` reads one now |
| primary key, a prefix equal | a range of the table a value |
| unique and non-unique index | a range of entries a value, each entry's row read by key |
| expression index | the key on the expression is probed; the executor rechecks the quals of each row, as it does an expression's key now |
| partial index | unchanged: the planner uses it only where the WHERE implies its predicate |
| INCLUDE, index-only scan | the probes give the entries; index-only scans share `smstore_index_beginscan()` |
| cut keys (over-long) | a probe's bounds are an equality's, which takes a cut key today |
| numeric column | a value's bounds are a range at the scale, as an equality's are; the order holds |
| DESC column | values sorted descending |
| duplicates in the array | dropped: each row once, as PostgreSQL returns it |
| a value of the wrong type | an `int4[]` on an `int8` column is widened; another type is checked of each row |
| prepared statement | the array, a parameter or `array[$1, ...]`, is evaluated at each rescan, and the scan begun again |
| read committed, a row updated meanwhile | EvalPlanQual rechecks the quals of the row, not the keys: unchanged |
| SERIALIZABLE | each probe reads as an equality's scan does, its SIREAD entries the same |
| an open cursor | forwards only, as now; a scrollable cursor materializes |
| temporary table, savepoint, abort | nothing new: reads only |
| TOAST, keyless (`_id`) tables, DDL meanwhile | nothing new: a keyless table's indexes are probed alike; an index not yet usable has no path |

## 4. Tests

- `test/slt/in_list.test`, its results PostgreSQL 18's: the
  cases of §3 on a primary key, a unique and a non-unique
  index, an expression and a partial index, two columns with
  an array each, an array after a range, DESC, numeric,
  `int4[]` on `int8`, nulls, duplicates, an empty array, a
  prepared statement with a parameter array, `OR` turned into
  an array, `ORDER BY` with no sort, and each plan's node by
  `EXPLAIN (COSTS OFF)`.
- `rel` and `mvcc` cannot express it: sm is not changed; each
  probe is a scan sm has today.
- Shown to fail with each part disabled: the sort, the
  dedup, the null drop (a null's datum would probe row 0),
  the empty scan, the prefix rule, and the filter of an array
  not probed.

## 5. Left out

- **A skip scan**: an array after a range's column is a
  filter on the range, not a probe of each value within it.
- **A bitmap of an array**: a table in sm has no bitmap scan.

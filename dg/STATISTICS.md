# Statistics in digres

How digres's ANALYZE and planner use the statistics sm keeps.
The strategy is `../sm/STATISTICS.md` (§3.7 is the host's
part); what sm records, and how, is
`../sm/sm3/SM3_DESIGN.md` §6, and its calls are in
`../sm/include/sm.h`: `sm_tbl_analyze`, `sm_tbl_stats` and the
four live calls. All of §3 is built (`WORKNOTE.md` steps 74 to
78).

## 1. Where digres stands

Where it stood before §3 was built.

- **Every table in sm is planned as never analyzed.**
  `estimate_rel_size()` (`plancat.c`) gives an sm relation 10
  pages, and as many rows as fit, always, not only before an
  analyze. Each index is given its table's pages and rows. The
  table AM's `relation_estimate_size` refuses, and nothing
  reaches it.
- **No column has statistics.** `get_statistic_tuple()`
  (`lsyscache.c`) returns NULL, so every selectivity is a
  default.
- **ANALYZE fails.** Its catalog lookups (`RELOID`, `ATTNUM`)
  meet catalogs digres does not have before the sampler, which
  refuses too; it would write `pg_statistic` and `pg_class`,
  which it cannot.
- **Nothing analyzes automatically**: autovacuum is gone.

## 2. Decisions

### 2.1 ANALYZE is `sm_tbl_analyze`

- **ANALYZE goes to smstore, not vacuum.c.** `ExecVacuum()`
  passes an ANALYZE to `smstore_analyze()`, and a VACUUM
  ANALYZE too, once `smstore_vacuum()` has compacted the
  tables, so the sizes it records are the compacted stores'. A named table is
  looked up as any name is; a bare ANALYZE takes every table
  in every schema, from sm's catalog
  (`sm_cat_list_namespaces`, `sm_cat_list_tables`). Each table
  is opened as a reader (IS) and analyzed by
  `sm_tbl_analyze(tbl, default_statistics_target)`.
- **In the statement's transaction.** The statistics commit or
  roll back with it, as sm intends (§6.7 there). PostgreSQL
  commits after each table outside a transaction block; here
  the locks are only IS, so one transaction for the statement
  holds no writer up, and is simpler.
- **One target a table.** `default_statistics_target`; a
  column's `SET STATISTICS` is not supported, and neither are
  extended statistics or ANALYZE of an inheritance tree.
- **`VERBOSE`** reports each table's live rows and rows
  sampled, from `sm_tbl_stats`.

### 2.2 A table's size: live bytes, scaled rows

`estimate_rel_size()`'s sm branch calls
`smstore_estimate_size()`, which follows
`../sm/STATISTICS.md` §3.7:

- **Never analyzed, or a heap of 0 bytes** (new or truncated):
  as now, ten pages, planned as PostgreSQL plans `reltuples`
  -1. That keeps the planner's behaviour for those tables, and
  is the rule sm's design gives (§6.7 there).
- **Analyzed**: with `disk` and `mem` read live
  (`sm_tbl_size`) and `rec` the statistics,
  - `c`, the compression, is `rec.heap_disk / (rec.live_rows ×
    rec.avg_row_bytes)`, at most 1, and 1 when nothing was on
    disk then;
  - bytes now are `disk + mem × c`, and bytes then
    `rec.heap_disk + rec.heap_mem × c`;
  - tuples are `rec.live_rows × now / then`: the row count
    grows and shrinks with the table since the analyze. When
    the analyze found nothing on disk, its compression is
    unknown, and a flush since would read as a shrink: tuples
    are then `rec.live_rows × N now / N then`, by the row
    numbers handed out, which counts inserts exactly and not
    deletes;
  - pages are `⌈now / 8192⌉`, at least 1; `allvisfrac` 1, as
    there is no visibility map.
- **An index** gets the same from its own entry
  (`sm_tbl_idx_size`, `stats_index_t`): its pages from its
  bytes, its tuples its entries scaled as the table's rows are,
  so a partial index is the share of rows that belong to it.
  The scale is the plan's estimate of the table's rows over
  `rec.live_rows`: `get_relation_info()` estimates the table
  first, and passes its rows in, so the table is not sized a
  second time.
  `tree_height` stays -1: costing a lookup by the LSM's probes
  needs calibration, and is left for later.
- **A session keeps the bytes it read** (`size_entry_t` in
  `smstore.c`), a table's heap and each index apart, for 10
  seconds: `sm_tbl_size` reads every sorted file's and
  memtable's size, which cost more than the row itself in a
  plan of one row by its key. An entry is used while the table
  is the version it was read at (`sm_tbl_version`), which a
  DDL, an ANALYZE or an index made valid changes. None is kept
  while the transaction has changed the table itself, as its
  version then stays the same through each change. Within the
  10 seconds, rows written since count as they did when it was
  read; a plan's estimate may be that stale.
  `pg_relation_size()` and ANALYZE read sm each time.

### 2.3 Column statistics: a `pg_statistic` row, made

- **`get_statistic_tuple(relid, attnum, inh)` makes the row**
  from `sm_tbl_stats` instead of returning NULL. For a table,
  the statistic is the column's, at position `attnum - 1`; for
  an expression index, it is its table's statistic for that
  index at key position `attnum - 1`. Inheritance (`inh`) has
  none.
- **The slots are what PostgreSQL's ANALYZE writes**:
  `stanullfrac`, `stawidth` and `stadistinct` as recorded;
  `STATISTIC_KIND_MCV` with `staop` the type's `=`, its numbers
  the frequencies; `STATISTIC_KIND_HISTOGRAM` with the type's
  `<`; `STATISTIC_KIND_CORRELATION` with `<`, when there is
  one. The operators are `lookup_type_cache()`'s, which reads
  the compiled-in operator classes; `stacoll` is the column's
  collation. sm keeps neither: they are the host's.
- **Values are datums as they are.** `sm_datum_of_bytes` gives
  each value's datum; a variable-width one is a varlena in
  PostgreSQL's own bytes (`../sm/sm2/VARLENA.md`), so
  `construct_array()` takes them as they are.
- **`pg_statistic` gets a descriptor and no table.** The row is
  formed, and `get_attstatsslot()` reads its arrays, with
  `pg_statistic`'s tuple descriptor, which `statistic_tupdesc()`
  (`lsyscache.c`) makes on each call from
  `Schema_pg_statistic`: `BKI_SCHEMA_MACRO` on its header makes
  genbki write it. Opening the `STATRELATTINH` cache, or the
  relation, would open `pg_statistic` -- an error in digres --
  and nailing it in the relcache does not hold: its entry is
  rebuilt by a `pg_class` scan once the relcache is built.
  `get_attstatsslot()` reads the element type's length and
  alignment from the compiled-in catalog
  (`get_typlenbyvalalign`), not the `TYPEOID` cache.
- **The row is the caller's**, freed with `heap_freetuple`, not
  `ReleaseSysCache`: each call site that sets
  `vardata->freefunc` for it is changed to match.
- **Made once from each version of the statistics.** A plan
  asks for a column's row several times, and building its
  arrays was 9% of a point query's time, measured (Q7 of
  `../bench`, `WORKNOTE.md` step 280). A session keeps each row
  it makes, as its bytes, with the `sm_stats_t` it was made
  from and the column's type and collation, and hands out a
  copy while sm still hands out those statistics: an ANALYZE
  publishes new ones, and the row is made again. Keeping the
  statistics keeps their address from being reused, so
  comparing it is enough. A session keeps 1,024 rows at most,
  and starts over past them.

### 2.4 What the planner must not do with them

- **`get_actual_variable_range()` is skipped for an index in
  sm.** It runs once a histogram exists, and scans the index
  through the index AM, which an sm index does not offer; the
  histogram's ends stand in, as they do when PostgreSQL finds no
  usable index.

### 2.5 `pg_stats`, as a function for now

- **What SQL reads the statistics through.** PostgreSQL's
  `pg_stats` is a view over `pg_statistic` and `pg_class`;
  digres has neither, nor views on catalogs: `system_views.sql`
  is not loaded. A view in sm has a `pg_rewrite` row since
  `WORKNOTE.md` step 239; a system view has none.
- **So `pg_stats` is a set-returning function**, compiled in
  from `pg_proc.dat` as every built-in is: `pg_stats(regclass)`
  gives the view's rows for one table, and `pg_stats()` for
  every table in sm, by the view's columns -- `schemaname`,
  `tablename`, `attname`, `inherited` (false), `null_frac`,
  `avg_width`, `n_distinct`, `most_common_vals`,
  `most_common_freqs`, `histogram_bounds`, `correlation`, and
  the element and range columns, null. `SELECT * FROM
  pg_stats('t')` reads as the view would.
- **Values are text arrays**: each common value and bound
  through its type's output function, as `psql` shows the
  view's `anyarray`. An expression index's statistics are rows
  too, with the index's name as `tablename` and the
  expression's place as `attname`, as PostgreSQL shows them in
  `pg_stats` only through the index.
- **Built-in oids 8844 and 8845**, `pg_stats_all()` and
  `pg_stats_rel()` in `smstore.c`. A schema's name is
  `get_namespace_name()`'s, which asks sm
  (`sm_cat_namespace_name`) for a schema made by CREATE
  SCHEMA.
- **The view itself waits** for system views in digres; when
  they come, `pg_stats` is a view over the function.

## 3. Order of work

1. ANALYZE goes to sm (§2.1).
   → verify: `server.sh` analyzes a named table and every
   table, in a transaction rolled back and committed, and a
   table never analyzed keeps planning as before.
2. Sizes (§2.2).
   → verify: `EXPLAIN` of a scan shows the live row count
   after ANALYZE, within a few percent, and follows the table
   as it grows without another; an index's estimate follows
   its entries, a partial one's its share.
3. Column statistics (§2.3, §2.4).
   → verify: `EXPLAIN` of `=` on a common value estimates its
   frequency times the rows; of `<` a histogram's share; a
   join's estimate uses the distinct counts; an expression
   index's statistics drive a filter on that expression. The
   last waits: digres has no expression index yet.
4. `pg_stats` (§2.5).
   → verify: after ANALYZE, `pg_stats('t')` gives a row a
   column, with the common values and bounds a known
   distribution should have, and none before.
5. `make tsan`, as statistics are read by every session.

## 4. Left out

- **Automatic analyze.** Nothing launches one; when to analyze
  again is the host's (`../sm/STATISTICS.md` §3.8), and a
  count of rows written since, kept in the write path, is the
  natural trigger. Not designed here.
- **The other statistics views** (`pg_stats_ext`,
  `pg_statistic_ext`), and `pg_stats` as a view (§2.5).
- **Index costing by the LSM's shape** (§2.2).

# BATCHREAD — rows read by key, many in one call

Status: built (`../dg/WORKNOTE.md` step 332).

## 1. What

```sql
-- Q9: 100 rows by a secondary index, their rows next to each other
select sum(c1) from wide_mixed where c2 between :a and :a + 99;
-- Q7: 100 rows by primary key, the keys sorted by the probes
select c1 from wide_mixed where id = any(array[:k1, ..., :k100]::int8[]);
```

A row a secondary index's entry names is read by its key, one
call a row: `sm_tbl_read_key()` down to sm1's `read_chained()`
and a RocksDB `Get` of the key's current version
(`VCHAIN.md` §4). So is each row an IN list's probes name
on the primary key (`../dg/INLIST.md`). Each `Get` starts
afresh: the store's catalog check, a seek of the sorted file's
index block, a look in the block cache, a search of the data
block. From one client, release build
(`../dg/WORKNOTE.md` step 332):

| | PostgreSQL | digres |
| --- | --- | --- |
| Q9, a row behind an entry | heap fetch by TID, ~0.35 µs | `Get` by key, ~2.4 µs, 68% of the query |
| Q7, a row by key | btree descent + heap fetch, ~3.1 µs | `Get`, ~3.7 µs, 74% of the query |
| a query | Q9 0.212 ms, Q7 0.405 ms | Q9 0.357 ms, Q7 0.498 ms |

RocksDB's `MultiGet` reads keys of one column family in one
call, sorted, sharing the index and data block lookups of keys
in one block and the per-call work; MyRocks reads rows behind
secondary entries and IN lists so, through MySQL's multi-range
read. Q9's 100 rows lie in one or two data blocks.

## 2. Design

### 2.1 sm: a batch read call at each level

- **sm1**, `chain_read_batch(session, storeid, keys, n, pins,
  data, rc)`: the store's catalog check once (`enter()`, at the
  read's view), each key's check and SIREAD entry as a single
  read makes them; one read epoch; the keys' current versions
  by one `MultiGet` of the store's column family; then each
  key resolved as `chain_get()` resolves one, from its current
  version, through `get_old()` one by one where its view needs
  an older one. Each key's answer in `rc[i]`: 0 with its row in
  `pins[i]`, 1 for none it sees; the call -1 on an error.
  Keys sorted or not: it sorts a copy for `MultiGet`, and
  answers in the caller's order. Keys in ascending order, as a
  range of an index names its rows when they are close, are
  read by one iterator instead, stepping up to four keys from
  each to the next and seeking past a gap; from the first key
  the steps miss on, the rest go to `MultiGet`, as keys far
  apart do not gain from it. Q9, 100 rows of a `c2` range,
  1,659k to 1,338k user instructions with today's entries,
  1,548k to 1,142k through a tree (`../dg/WORKNOTE.md` step
  349).
- **A kept iterator.** A tree's cursor reads its rows in
  batches of 1, 2, 4, up to 64, and each batch made and freed
  its own iterator. The cursor now keeps one,
  `sm1::batch_iter_t`, made at its first batch of two or more,
  and passes it to each later batch (`iter`). Each batch calls
  RocksDB's `Iterator::Refresh()` on it. The iterator then
  holds the store as it is now, as a new one would. While the
  store's memtables and files stay the same, `Refresh()` only
  moves the read sequence; when they change, it makes the
  iterator again. The refresh is where a new iterator was
  made: after the batch's checks and SIREAD entries, inside
  its read epoch.
  The session keeps a list of the kept iterators, and lets go
  of them where it closes its cursors: at the transaction's
  end, at a rollback to a savepoint, and at its close. A
  caller that does not free one (digres drops a scan on an
  error) then holds no RocksDB file. q3 of `../bench`, about
  10 rows a probe through a tree, 283k to 249k instructions a
  statement (`../dg/WORKNOTE.md` step 369).
- **Scattered keys skip the iterator.** The steps help only
  when a batch's rows are close in the store, as an index
  correlated with the key finds them. An uncorrelated index's
  rows are far apart: the batch seeks the first key, misses
  the second, and reads the rest by `MultiGet`. A seek costs
  more than a key of `MultiGet`. So a batch whose steps reach
  no key past the first marks the kept iterator, and lets its
  RocksDB iterator go. The cursor's later batches of the store
  are read by `MultiGet` alone. A batch that steps through
  some keys before a gap keeps the iterator. q3, whose
  lastnames are spread over the table at random, 249k to 234k
  instructions a statement (`../dg/WORKNOTE.md` step 372).
- **ANALYZE says first.** The first batch that misses still
  makes the iterator and seeks once a scan. A tree's cursor
  now asks the table's statistics first: if the correlation
  of the tree's leading key with the rows' order is below 0.5
  in absolute value, its rows are scattered, and the cursor's
  iterator is made scattered (`batch_iter_new(true)`). Every
  batch then goes to `MultiGet` alone. With no statistics, or
  a stronger correlation, a batch tries the steps, as above.
  Statistics that are stale cost speed, never a wrong row.
  q3, 234k to 203k instructions a statement; a tree on
  `id / 10`, whose rows are together, 189.7k to 189.9k
  (step 373).
- **sm2**, `rel_read_batch()`: each key put under the table's
  relid, as `rel_read()` does one; a kept iterator passed on.
- **sm3**, `tbl_read_keys(table, keys, n, refs, tuples, rc)`:
  a pin from the session's pool for each key, as
  `read_into()` takes one, back to the pool for each key not
  seen.
- **sm**, `sm_tbl_read_keys(tbl, keys, lens, n, refs, tuples,
  tuple_lens, rc)`, documented in `include/sm.h`.

A batch reads what as many single reads at the same view
read: the view is the statement's (`sm1/SM1_DESIGN.md` §6.2),
fixed while it runs, so reading a row early returns the
version reading it late would.

### 2.2 digres: two scans read ahead

- **A secondary index's scan that reads rows**
  (`smstore_index_getnext()`, `next_read_row()`,
  `../dg/src/backend/access/sm/smstore.c`): it reads the next
  entries from the index cursor into a buffer -- each entry's
  rowno and its row's key, copied, as the cursor moves on --
  reads their rows in one `sm_tbl_read_keys()`, and hands them
  out in the entries' order, a row not seen passed over. The
  batch starts at 1 and doubles to `read_batch_max`, 8, so a
  `LIMIT 1` or an EXISTS reads no more than a row or two ahead.
- **An IN list's probes on the whole primary key**
  (`open_probe()`, `next_probe()`): the next probes' keys,
  already in order, read in one call, batched as above.
- **The row the scan is on** is the buffered row's: its key
  for `scan_key()` and WHERE CURRENT OF (`scan_at()`), its pin
  in the slot; the pins of the rows read ahead are the scan's,
  released when it ends, rescans, or the transaction does.
- **Left as now**: an index-only scan, whose entries give their
  columns with no row read; a primary key's range scan, a
  cursor over rows already; a keyless table's rowno reads.

`read_batch_max` is 8, a constant: enough keys a call to share
a data block's lookups among rows next to each other, and few
enough that a scan stopped early has read little it did not
use.

## 3. Cases

| Case | What happens |
| --- | --- |
| a row deleted or not yet committed at the view | `rc[i]` 1: passed over, as a single read passes it |
| a row updated since the view | resolved through OLD, one `get_old()` at a time, as now |
| this transaction's own writes, earlier statements | seen, as a single read sees them |
| this statement's own writes | not seen (§6.2), read early or late alike |
| a row whose key moved (an update of the primary key) | `rc[i]` as `rel_read()` answers it now |
| TOAST | a row's out-of-line values read when its columns are, as now |
| unique, non-unique, expression, partial, INCLUDE index | rows read by the keys their entries give: alike |
| cut keys (over-long) | the entry gives the row's key whole, as now |
| READ COMMITTED, a row the plan locks (UPDATE, FOR UPDATE) | read at the statement's view, then locked and rechecked by EvalPlanQual, as now |
| SERIALIZABLE | a SIREAD entry for each key read, read ahead or not |
| an open cursor | its rows read at its view, its read-ahead pins its own until it closes; WHERE CURRENT OF names the row handed out |
| a savepoint rolled back, abort | pins released with the transaction's, as now |
| temporary table | its store, no SIREAD; alike |
| DROP or TRUNCATE meanwhile | the table's lock keeps them out while the scan runs, as now |
| LIMIT, EXISTS, a nested loop's inner scan | the batch starts at 1 and doubles: little read past what is used |

## 4. Tests

- sm1 unit tests (`sm1/test/test_chain.cc`): a batch and as
  many single reads give the same rows, keys in and out of
  order, with versions in OLD, tombstones, the transaction's
  own writes, another's uncommitted ones, an old snapshot, and
  keys that are not there.
- `rel`: a verb for `sm_tbl_read_keys()`, and a test of a
  batch against single reads over a table with an update, a
  delete and an uncommitted insert of another session.
- dg: `test/slt/batch_read.test`, PostgreSQL 18's results: a
  unique index whose order is the reverse of the primary key's,
  over rows updated and deleted, past several batches; LIMIT
  and OFFSET; a cursor with UPDATE and DELETE WHERE CURRENT OF;
  an UPDATE whose rows the index finds; IN lists on both
  indexes; a table with no primary key. Another session's
  uncommitted writes are sm1's test's.
- Shown to fail: with the batch's rows handed out in the
  sorted order, not the entries'; with a row not seen handed
  out; with the read-ahead's pins released before their rows
  are.
- Measured: Q7, Q9, Q10 and Q11, before and after.

## 6. What building it found

- **The probes begin again when asked past their end**: an
  array's probes, once the last ended, have their odometer
  back at its first value (`advance_probe()`). Nothing asked
  again before; the read-ahead did, and a scan of an IN list
  on a text index whose rows it read never ended. It keeps
  `ra_done` now (`in_list.test` hung without it).
- **Keys far apart share little**: half of Q7's 100 Zipfian
  keys are 100 or below, a few data blocks; the rest lie one a
  block. Q7 stays at 0.49 ms. Q9's rows lie together, and it
  went from 0.357 ms to 0.214 ms in its best run,
  PostgreSQL's 0.212; it typically runs 0.3 to 0.4 ms
  (`../dg/WORKNOTE.md` step 333).

## 5. Left out

- **A nested loop's inner index scans**, one a row of the
  outer: batching keys across them is MySQL's batched key
  access, a change to the executor.
- **An index-only scan's rows**, read only where its key does
  not give the columns back.

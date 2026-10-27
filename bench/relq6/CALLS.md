# The RocksDB calls of one q6.rel query

One query of `q6.rel`'s timed loop, at `a` = 500000:

```janet
(begin :rc)
(assert-eq (scan-count wide_mixed :from a :to (+ a span -1)) span)
(commit)
```

in SQL `select sum(c1) from wide_mixed where id between :a and
:a + 9999`, its rows counted, not summed. These are the calls it
makes into RocksDB, in order, with the sm function that makes
each (`../../sm`).

| # | Calls | RocksDB call | Called from | What for |
|---|---|---|---|---|
| | | **`(begin :rc)`** | | none |
| 1 | 1 | `DBImpl::NewIterator` | `access_read` ← `catalog_lookup` ← `access_enter` ← `sm1::chain_scan` | the store's catalog row: first use only, below |
| 2 | 1 | `Iterator::Seek` | `access_read` | |
| 3 | 1 each | `Valid`, `key`, `value` | `access_read` | |
| 4 | 1 | delete the iterator | `access_read` | |
| 5 | 1 | `DBImpl::GetSnapshot` | `sm1::chain_scan` | the cursor's RocksDB snapshot |
| 6 | 1 | `DBImpl::NewIterator` | `sm1::chain_scan` | the snapshot, `auto_prefix_mode`, the upper bound just past `_id` a + 9999 |
| 7 | 1 | `Iterator::Seek` | `chain_scan_seek` | to the row of `_id` a |
| 8 | 10,001 | `Iterator::Valid` | `chain_scan_next` | a row each, and the end |
| 9 | 10,000 | `Iterator::key` | `chain_scan_next` | a row each |
| 10 | 10,000 | `Iterator::value` | `chain_scan_next` | a row each: the row itself |
| 11 | 10,000 | `Iterator::Next` | `chain_scan_next` | a row each |
| 12 | 1 | `Iterator::status` | `chain_scan_next` | at the end of the range |
| 13 | 1 | delete the iterator | `scan_release` | the cursor's end |
| 14 | 1 | `DBImpl::ReleaseSnapshot` | `scan_release` | |
| | | **`(commit)`** | | none |

- **A query is 40,007 calls**: rows 5 to 14. The 40,001 of rows
  8 to 11 are the scan; the 6 around them are its cursor's.
- **The catalog read, rows 1 to 4, is a session's first use of
  the store only.** `access_enter` finds the store live in the
  session's cache (`sm1/src/catalog.cc`, `store_cache`) after
  that, until a DDL. In `q6.rel` the warm-up's count takes it,
  so no timed query does.
- **A read-only transaction's begin and commit call nothing.**
- **No row is read twice.** The iterator walks the table's own
  rows, and `value()` is the row: there is no index apart from
  it, so no `Get` a row.
- **The iterator is `ArenaWrappedDBIter`**, whose calls go on
  to `DBIter`'s, where the work is: `DBIter::Next` is 1,272 of
  the 1,382 instructions a row that `run.sh` counts, by a
  profile of the timed loop.

## The same calls in digres

digres's Q6 makes these calls too. Its plan is an index scan
of `wide_mixed_pkey`, and a primary key is its table
(`../../sm/CLUSTER.md` §0): `smstore_index_beginscan` opens
`sm_tbl_idx_scan_rows`, which reaches `sm1::chain_scan` on the
table's store over the rows' keys. A row's key is there
`relid ‖ encode(id)`, here `relid ‖ _id`, which the load makes
the row's id; the calls are the same.

## How they were found

gdb on `sm/build/release/rel`, running one iteration in a
session of its own: a breakpoint on every RocksDB function
`sm/build/release/libsm.a` names, and on every method of
`DBImpl` and `ArenaWrappedDBIter`, where the calls through
`rocksdb::DB` and `rocksdb::Iterator` land. A call is counted
when its caller is not RocksDB's, so RocksDB's calls to itself
are left out; `DBIter`'s methods are left out, as
`ArenaWrappedDBIter`'s pass to them. Each call was counted
under the rel command it was made in: `begin`, `scan-count` or
`commit`.

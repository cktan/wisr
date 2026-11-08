# PURGE_PLAN — the tombstone purge by SingleDelete

Status: built ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
411), as the user decided (§8). It fixes
[`../dg/ISSUE.md`](../dg/ISSUE.md) issue 22: the purge of a mass
delete stalls every writer.

---

## 1. The problem

A delete of a version key writes a tombstone version. The
compaction filter cannot drop the tombstone: older versions of
the key may sit in files the compaction does not read
([`GC.md`](GC.md#5-the-tombstone-purge) §5). So the filter queues
it, and `purge_one()` (`sm1/src/gc.cc`) erases it:

```
DeleteRange(cf, tombstone, userkey | FF x 12)
```

- **One range tombstone a purged key.** A store CF's memtable is
  flushed every 1,000 of them (`store_max_range_deletions`), so a
  purge of 500,000 keys makes about 500 level-0 files.
- **RocksDB then stalls writes** for the level-0 files: the
  purge's and every session's, which share its write groups.
- **The keys are versioned keys** (class `0x00`): mostly the
  entries of lsm indexes, the default kind. A chained row's
  tombstone, in CURR, is dropped by the filter itself
  ([`ROWSTORE.md`](ROWSTORE.md#37-the-garbage-collector-and-a-chain)
  §3.7), so it needs no purge.

## 2. What was found

### 2.1 sm1 writes each version key once

A version key is `class | userkey | ~wts | ~seq`. Its writers:

| Writer | Where | Writes |
| --- | --- | --- |
| a write, a delete's tombstone among them | `access.cc`, `access_write()` | one Put; `wts` is a new timestamp each write (`db_alloc_ts()`) |
| a bulk load | `load.cc` | one Put, or one key of an ingested SST, each key once |
| a savepoint rollback | `xct.cc`, `rollback()` | one Delete of the session's own uncommitted version, never Put again |
| the compaction filter | `gc.cc`, `gc_filter_t` | a removal, and a stamp, which changes the value in place |
| the purge | `gc.cc`, `purge_one()` | one range delete |
| a loaded store's rollback | `xct.cc` | one range delete of the whole store |

No version key is Put twice.

### 2.2 The filter's removal is a Delete

`gc_filter_t` uses RocksDB's `Filter()`, whose `true` is
`Decision::kRemove`. RocksDB 8.9.1 turns that entry into a
`kTypeDeletion` tombstone
(`db/compaction/compaction_iterator.cc`,
`InvokeFilterIfNeeded()`). The filter removes exactly the
purge's keys: a tombstone's older versions, and the tombstone
itself once a newer version of its row is at or below the
watermark.

### 2.3 How a compaction meets a SingleDelete

From `CompactionIterator::NextFromInput()`, 8.9.1:

| SingleDelete over | What the compaction does |
| --- | --- |
| a Put | drops both, at any level, once no snapshot is between them |
| another SingleDelete | skips the first; counts `num_single_del_mismatch`; no error |
| a Delete | `Status::Corruption`: the contract is enforced (`enforce_single_del_contracts`, true by default) |
| a key a range tombstone covers | the range tombstone drops it, as any key |
| nothing below it in the output level | dropped (`KeyNotExistsBeyondOutputLevel()`) |

And `Decision::kPurge` turns a filtered entry into a
`kTypeSingleDeletion`, not a Delete.

So a SingleDelete of a version key is safe if and only if no
Delete of that key is ever written: by sm1, or by the filter.
A test stages both (`test_gc.cc`): two SingleDeletes of one key
compact cleanly, and a SingleDelete over the filter's Delete
fails the compaction with `Corruption: Found SD and type: 0 on
the same key`.

### 2.4 The queue held each key many times

The filter queued a tombstone each time a compaction met it
before its purge, and the queue kept every entry. Measured on
issue 22's case, with a probe: one batch the worker took held
24,932,620 entries for about 1,000,000 tombstones, 25 a key. That
was the server's memory, which grew to 5.9 GB until the kernel
killed it, and 25 range deletes a key. So the queue keeps a key
once (`purge_ids`).

## 3. The design

Every deletion of a version key is a SingleDelete. Chained keys
and pages keep Deletes: CURR and OLD keys are written more than
once.

1. **The filter removes a version key with `kPurge`.**
   `gc_filter_t` moves from `Filter()` to `FilterV2()`, which
   returns a `Decision`: `kPurge` for a version key it removes,
   `kRemove` for a chained key, `kChangeValue` for a stamp,
   `kKeep` otherwise. Its rules stay as they are.
2. **The purge erases by SingleDelete.** `purge_one()` opens an
   iterator, with no snapshot, over the range it deletes today,
   and writes a SingleDelete of each key the iterator gives, in
   one batch. After GC that is the tombstone and seldom more. No
   range tombstone, so no forced flush.
3. **A savepoint rollback erases by SingleDelete**: its version
   was Put once, by the session.
4. **The purge's writes are `low_pri`** (`WriteOptions`): when
   compaction is behind, RocksDB slows them, not the sessions'.
5. **The queue holds a key once** (§2.4).

`store_max_range_deletions` stays, for the loaded store's
rollback and the pages' range deletes.

## 4. Cases

| Case | What happens |
| --- | --- |
| delete, then purge | the iterator finds the tombstone and its older versions still Put; each gets a SingleDelete; each pair goes at its first compaction |
| delete, then a re-insert of the same row | the re-insert is a new version key, a newer `wts`, outside the purge's range; a compaction that sees it removes the old tombstone by `kPurge`; a purge SingleDelete then meets a SingleDelete: skipped, no error |
| a compaction filters a version while the purge reads it | both write a SingleDelete; the compaction skips one |
| update A to B to A | three version keys; the filter purges the old ones; none is a tombstone, so no purge |
| savepoint rollback | a SingleDelete of the session's version; a later filter `kPurge` of it, if any, meets a SingleDelete |
| abort, crash | the filter removes a dead version by `kPurge`, as now by `kRemove` |
| an open scan's RocksDB snapshot | a SingleDelete and its Put both stay until the snapshot is released, as a range delete leaves what a snapshot sees |
| an old REPEATABLE READ snapshot | it holds the watermark down; nothing it sees is purged, as now |
| TOAST | a value out of line is erased by a tombstone version (`erase()`), a Put; its purge is as any key's |
| unique, non-unique, expression, partial, INCLUDE index entries | all version keys: as above |
| a primary key's rows | chained: no purge, unchanged |
| CREATE INDEX pending, DROP INDEX | the entries' tombstones are purged as any |
| TRUNCATE, ALTER rewrite | a new store; the old CF dropped whole, unchanged |
| a temporary table | its store has the same filter and purge |
| a keyless table's `_id` rows | chained: unchanged |
| bulk load, then rollback | the store range-deleted, unchanged: a range tombstone covers SingleDeletes and Puts alike |
| 2PC across a restart | a prepared version is in flight: kept, as now |
| the `CF:txn` sweep | unchanged: it reads stamps, not deletes |
| a purge lost to a crash | the next compaction that meets the tombstone queues it again, as now |

## 5. Tests

- **sm1 unit tests** (`sm1/test/test_gc.cc`):
  1. delete, purge, compact to the bottom: the key is gone, and
     no compaction fails;
  2. delete, re-insert, compact, purge, compact: the new version
     is there, the old is not;
  3. savepoint rollback, then a forced compaction: no error, the
     version gone;
  4. a compaction counts no Delete over a SingleDelete: the
     contract stays enforced, so a mix fails the test.
- **No mvcc scenario**: the driver has no verb that runs a
  compaction or the GC worker, and the purge changes nothing a
  transaction sees: it erases only what no snapshot can read.
- **Issue 22's script**, timed before and after: a DELETE of
  100,000 rows after a mass delete and update.

## 6. Measured

`bench/build/purge/issue22.sh`, `digres-release`, a DELETE of
100,000 indexed rows:

| | before | after |
| --- | --- | --- |
| on a new server | 1.5 s, 1.3 s | 1.2 s, 1.1 s |
| after a DELETE and an UPDATE of 500,000 rows each | 64 s, 46 s, 60 s | 1.1 s, 1.3 s |
| RocksDB's lines `Stalling writes` | 1,520, 1,226 | 0, 0 |
| the server's peak memory | 5.9 GB, then killed | 0.7 GB, 0.6 GB |

## 7. Cost

- **One seek a purged key**, on the purge thread, against one
  range delete today.
- **No forced flush**, so no level-0 pile-up and no stall.
- **A SingleDelete and its Put vanish at their first
  compaction**, where a Delete must reach the bottom level.

## 8. Decided by the user

Decided 2026-11-08, as recommended:

1. **SingleDelete for every version key**, not plain Delete for
   the purge alone: a SingleDelete's pair goes at its first
   compaction, where a Delete's tombstone lives to the bottom
   level.
2. **`low_pri` for the purge's writes.**
3. **No `NewCompactOnDeletionCollectorFactory`**: a
   SingleDelete's pair goes at its first compaction anyway.

# GROUPSYNC — a commit's sync outside RocksDB's write queue

Status: built (`../NEXT.md` §1 item 5,
`../../dg/WORKNOTE.md` step 374). Paths are relative
to `sm1/`.

## 1. What

```sql
-- 16 clients, each in a loop:
begin;
insert into t values (...);  -- the version: Put, unsynced
commit;                      -- the record: Put, sync = true
```

Each transaction makes two writes through RocksDB's one write
queue. RocksDB forms a group of the writers waiting, and the
group's leader writes the WAL for all of them. A group led by a
synced write syncs the WAL once, and takes in the unsynced
writes queued behind it (`WriteThread::EnterAsBatchGroupLeader`,
`db/write_thread.cc`, 8.9.1). So the insert's version waits for
a sync it does not need.

Measured (`../../dg/WORKNOTE.md` step 344, `NEXT.md` §1 item 5):

- At 16 clients, about 85% of a session's samples wait in
  `WriteThread::BlockingAwaitState`: 42 to 45% under the
  version's write, 40 to 43% under the commit record's.
- `bench.old/rowlocks.sh` at 16 clients: `ins` 4,602 to 4,753
  tps, `fk` 4,293 to 4,551; PostgreSQL 18 10,073 and 10,523.
- Time outside the queue makes the groups smaller: 100 µs of
  `pg_sleep` an insert takes `ins` from about 5,600 tps to
  4,100.
- `enable_pipelined_write` does not help (2026-11-01).

PostgreSQL's insert writes its WAL record to a buffer and does
not wait. Only the commit waits, in `XLogFlush()`, and one
backend's flush covers every record before it.

## 2. Options

| | A. sm1 syncs | B. versions at commit | C. versions skip the WAL |
| --- | --- | --- | --- |
| What | the record written unsynced; sm1 then waits for one sync of the WAL past it, one leader at a time | a transaction's versions kept in a batch, written with its record in one synced write | the versions written with `disableWAL` |
| Writes a transaction | 2 unsynced, 1 sync outside the queue | 1 synced | 1 unsynced, 1 synced |
| The uncommitted version is the lock (`WRITELOCK.md`) | kept | lost: another writer cannot see it | kept |
| Reads of a transaction's own writes | unchanged | every read path merges the batch | unchanged |
| Durability | unchanged | unchanged | lost: a crash keeps the record, loses the versions |
| Size | `finish_commit()` and a module of about 60 lines | a rewrite of the write path | small, and wrong |

**Recommended: A.** B breaks the write path's design, and C
loses committed data. A changes only the commit.

## 3. Design (option A)

- **The record is written unsynced**: `finish_commit()`'s
  `Put` drops `sync = true`. The record is in the WAL file and
  in the memtable, and is not durable yet.
- **Then the commit waits for a sync past its record**, as
  PostgreSQL's `XLogFlush()` does:
  1. Read `GetLatestSequenceNumber()`: the target. RocksDB sets
     the last sequence after the group's WAL write and memtable
     insert, and before its writers return
     (`db_impl_write.cc`), so the target covers the record.
  2. Lock `sync_mu`.
  3. If `synced_seq` is at or past the target, unlock and
     return: another commit's sync covered this record.
  4. If another commit is syncing, wait on `sync_cv`, and go
     to step 3.
  5. Become the leader: read `GetLatestSequenceNumber()` again,
     as `upto`, and unlock. Every write at or below `upto` is
     in the WAL file now.
  6. Call `FlushWAL(true)`, outside the write queue. Under
     `manual_wal_flush`, it first writes RocksDB's WAL buffer
     to the file, which `SyncWAL()` alone does not. Then it
     calls `SyncWAL()`. Writers go on meanwhile: `SyncWAL()`
     syncs the files without flushing a buffer (`db_impl.cc`).
  7. Lock `sync_mu`, set `synced_seq` to `upto`, and wake the
     waiters on `sync_cv`.
- **Then publish, as now**: `enter_cache()` takes the xid out
  of the active table, and `db_commit_done()` moves
  `visible_ts`. Until then every reader, the compaction filter
  among them, finds the xid in the active table before CF:txn
  (`xct_status()` step 1), so the unsynced record is never
  read as a commit.
- **A failed sync is fatal**, as a failed record write is
  (`SM1_DESIGN.md` §8.2). The records it was to cover may be
  in the WAL or not, and a second sync proves nothing. So the
  leader aborts the process (`sm0::fatal()`), and no commit
  waiting behind it returns. The reopen's WAL replay decides
  each commit.
- **Only the commit record changes.** The PREPARED record
  (§8.4) and a prepared transaction's ABORTED record keep
  `sync = true`: they are rare, and a group led by one of them
  syncs as today.
- **The wait is not cut short by an interrupt**, unlike every
  other wait in sm (`../CODE.md` §7). The record is written, so
  an interrupted commit has an unknown outcome, and only an
  abort could settle it. The wait is bounded by one sync, which
  finishes or aborts the process.
- **`durability_sync = false` skips the wait**, as it skips the
  sync now.
- **`group_commit_us` stays unused**: a leader does not wait
  for followers. The waiters behind a sync make the next group.

## 4. Cases

| Case | What happens |
| --- | --- |
| a reader, any isolation, before the sync | the xid is in the active table: in flight, as now |
| a second writer of the key | waits for the xid (`WRITELOCK.md` §3.4), woken by `enter_cache()`, after the sync, as now |
| an old RR snapshot; an RC re-read | `visible_ts` moves after the sync, as now |
| SSI | validated before the claim, as now; unchanged |
| the compaction filter, stamping a version | `xct_status()` finds the xid active until the sync: no stamp from an unsynced record |
| CF:txn's sweep (`xct_drop_committed_records`) | drops records at or below W0, and W0 is below `visible_ts`: never an unsynced one |
| process crash after the record, before the sync | the record is in the OS's page cache: the reopen finds it committed; the client was not told. As a crash inside today's synced `Put` |
| power loss before the sync | the record may be lost: the transaction is dead at reopen; the client was not told |
| power loss after `sm1::commit` returns | the record was synced: committed |
| the sync fails | the process aborts; the reopen's WAL replay decides each commit |
| abort, savepoint rollback | no record written: unchanged |
| 2PC: PREPARE, COMMIT PREPARED, across restart | PREPARE stays synced. COMMIT PREPARED goes through `finish_commit()`: the same wait |
| a commit of DDL: CREATE INDEX, DROP, TRUNCATE, ALTER | the same record and wait; `last_ddl_ts` set after the sync, as now |
| a temporary-only transaction | the same wait as now. Skipping it is `NEXT.md` §3, and not part of this |
| bulk load, and its rollback | the load's files go in before the record; unchanged |
| a log switch during the sync | `SyncWAL()` syncs every log up to the current one |
| `manual_wal_flush=true` | `FlushWAL(true)` writes the WAL buffer to the file before the sync |
| `durability_sync = false` | no sync, no wait |
| older commits in progress (`COMMITVISIBLE.md`) | a commit waits for the older ones' syncs as now |

## 5. Tests

- **sm1 unit tests** (`test/test_groupsync.cc`), each shown to
  fail with the code it covers broken:
  - A commit survives a power cut once `sm1::commit` returns.
    The database runs on a file system of the test's own, which
    refuses every write after the cut and then truncates each
    file to its last sync. With the sync left out, the commit
    is lost.
  - The same holds with `manual_wal_flush=true`. With
    `SyncWAL()` in place of `FlushWAL(true)`, the commit is
    lost.
  - A commit is not visible, and does not return, until its
    sync is done: a test hook stalls the leader's sync, and
    another session does not see the row. With the sync moved
    after the publish, it sees the row.
  - A failed sync aborts the process, run in a child. After
    the reopen, the commit whose record reached the OS is
    committed. With fail-stop in place of the abort, the child
    returns.
  - Fifteen commits waiting behind a stalled sync share one
    more sync. With a leader covering only its own record, they
    make more.
  - `durability_sync = 0` makes no sync.
- **mvcc**: every scenario passes unchanged. A scenario cannot
  stall a sync, so none is added.
- **rel, dg**: the suites, unchanged. No SQL-visible change.

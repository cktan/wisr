# COMMITORDER_PLAN — commit records in commit_ts order

Status: built ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
406). §9 lists what the user decided.
[`XCTMGR.md`](XCTMGR.md#32-visible_ts-and-the-commit-queue) §3.2,
§8.2 and §8.5 and [`DBMGR.md`](DBMGR.md#82-group-sync) §8.2 are
the design of record.

---

## 1. What this is

A commit that has made its record durable still waits when an
older commit's record missed the same sync
([`XCTMGR.md`](XCTMGR.md#85-records-in-commit_ts-order) §8.5). At
16 clients two commits in three wait, about 220 µs each, 0.4 of a
sync of 600 µs ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
405). PostgreSQL has no such wait.

The cause is the order of the commit path today:

1. Claim `commit_ts`, under `alloc_mu`, into the sequencer's set
   `committing` (`db_claim_commit_ts()`).
2. Validate under SSI (`ssi_precommit()`).
3. Write the COMMITTED record through RocksDB's write queue
   (`finish_commit()`).
4. Sync the WAL, a leader for many commits (`db_sync_wal()`).
5. Publish: raise `visible_ts` to one below the oldest
   `commit_ts` still in `committing` (`db_commit_done()`).
6. Wait until `visible_ts` covers this commit
   (`db_commit_wait()`).

Steps 1 and 3 are apart. A commit claimed first can reach the
WAL last, and a sync covers the WAL as it is when it starts. So
a sync covers newer commits and misses an older one, and the
newer ones wait in step 6 for the older one's sync.

This plan puts the records into the WAL in `commit_ts` order.
Then a sync that covers a commit covers every older one too, and
step 6 never waits for a sync.

## 2. A worked example

The case of
[`XCTMGR.md`](XCTMGR.md#85-records-in-commit_ts-order) §8.5: A
claims 100, B claims 101, a WAL sync takes 600 µs.

Today:

```
t=0     A claims 100; its record waits in RocksDB's write queue
t=5     B claims 101, writes its record, leads sync S1
t=605   S1 ends; B waits for A, as 100 is still in committing
t=1205  A's sync S2 ends; visible_ts reaches 101; B returns
```

With this plan:

```
t=0     A claims 100, and puts its record in the commit queue
t=5     B claims 101, and puts its record in the queue behind A's
        B leads: it writes A's and B's records in one batch,
        100 then 101, and starts sync S1
t=605   S1 ends: both records are durable, and none older is
        missing. visible_ts reaches 101; A and B return
```

B returns at t=605, as in PostgreSQL. A returns 600 µs sooner
too, as B's sync took its record.

## 3. The design

### 3.1 The commit queue

A **commit queue** holds the COMMITTED records not yet written,
in `commit_ts` order. One mutex, `commit_mu`, guards it. Each
entry is on its committing session's stack: the xid, the record,
`commit_ts`, and a semaphore.

A commit enters the queue in one hold of `commit_mu`:

1. Claim `commit_ts`: `global_ts`'s next value, under its lease.
2. Validate under SSI with that `commit_ts`.
3. If validation fails, let `commit_mu` go and fail: the
   `commit_ts` is never used, as a failed claim's is today.
4. Else append the entry at the queue's tail.

So the queue is in `commit_ts` order by construction, and every
claimed `commit_ts` not yet published is in the queue or in the
batch a leader is writing. `committing` is not needed.

### 3.2 The leader

The sync's leader of `db_sync_wal()` becomes the queue's writer.

1. A commit that finds no leader leads. Else it sleeps on its
   semaphore, as a commit in the sync's line does today.
2. The leader takes the whole queue, under `commit_mu`: the
   batch.
3. It writes the batch's records in one `WriteBatch`, unsynced,
   in `commit_ts` order. One trip through RocksDB's write queue
   for the batch, not one a commit.
4. It syncs the WAL (`FlushWAL(true)`), unless
   `durability_sync` is off.
5. It enters each batch member into the commit cache
   (`enter_cache()`), so a reader that misses the xid in the
   active table finds the commit.
6. Under `commit_mu`, it raises `visible_ts`: to one below the
   queue's first `commit_ts`, or to `global_ts - 1` if the queue
   is empty. It hands the next batch to the queue's first
   commit, if any, as the oldest leads today.
7. It lets `commit_mu` go, and wakes the next leader first, then
   the batch's members.

A woken member returns at once to the rest of its commit:
release its locks, queue its dropped stores, end its SSI node.
`db_commit_done()` and `db_commit_wait()` go, and the list of
visible waiters with them.

### 3.3 Why it is safe

- **No gap**: a snapshot never sees a `commit_ts` whose record is
  missing. `visible_ts` rises in step 6 only to below the first
  entry still queued. Every entry below it was in a batch whose
  write and sync ended.
- **No commit returns unseen**: a member is woken in step 7,
  after step 6 raised `visible_ts` past its `commit_ts`. So its
  next statement sees it, as
  [`XCTMGR.md`](XCTMGR.md#84-a-commit-returns-once-it-is-visible)
  §8.4 requires.
- **Monotonic**: a claim made after step 2 has a larger
  `commit_ts` than every batch member, so step 6 never lowers
  `visible_ts`.
- **Durable before visible**: a record is in a synced batch
  before `visible_ts` covers it, as now.
- **A failed write or sync aborts the process**, as now: the
  batch's outcome is unknown, and WAL replay decides. A
  `WriteBatch` reaches the WAL whole or not at all.

## 4. What stays as it is

- **The record's format and key** in `CF:txn`: recovery reads
  the same records, so WAL replay does not change.
- **The PREPARED and ABORTED records** of two-phase commit. They
  have no `commit_ts` and no snapshot waits on them; each stays
  a synced write of its own
  ([`XCTMGR.md`](XCTMGR.md#10-two-phase-commit) §10).
- **A transaction that wrote nothing** commits with no record and
  no `commit_ts`, as now.
- **The versions** a transaction wrote: unsynced writes before
  its commit, in the WAL before its record, so a sync that covers
  the record covers them.

## 5. Cases

| Case | What happens |
| --- | --- |
| SSI validation fails | `commit_mu` is let go, nothing is queued, the transaction aborts; the `commit_ts` is a gap, harmless as an unused xid is |
| a commit of a prepared transaction | claims and queues as any commit; its COMMITTED record overwrites the PREPARED one, in the batch |
| `durability_sync` off | the leader writes the batch and skips the sync; members return after step 6 |
| one client | a batch of one: one write, one sync, as today |
| two writers of one row | the second waits for the first's row lock, which is released after the first returns, as today |
| a repeatable read snapshot, old | unaffected: it reads at its own `snapshot_ts` |
| a read committed statement after a commit | its snapshot is at or past the commit's `commit_ts`, as step 7 follows step 6 |
| an open cursor | unaffected: its view is frozen |
| crash with a batch written, not synced | WAL replay keeps the batch or loses it whole; no member had returned |
| crash before the batch's write | no record: the members are dead, as an unwritten commit is today |
| 2PC across a restart | unchanged: `restore_prepared` reads the PREPARED records |
| the `CF:txn` sweep, the GC watermark | both read `visible_ts`, which keeps its meaning |
| a commit that made or dropped a store | raises `last_ddl_ts` before it queues: before its publication, as now |
| a temporary store only | has an xid and a record: queued as any commit |
| a bulk load open | refused before the claim, as now |
| savepoint rollback, abort | no commit record: not queued |

## 6. Costs

- **One mutex for claim, validation and enqueue.** Commits pass
  through `commit_mu` one at a time, for the claim, the SSI check
  and an append. Without SSI that is well under a microsecond. A
  SERIALIZABLE transaction's check already runs under `ssi_mu`,
  one for the database, so this adds little.
- **The leader's work grows with the batch**: one `WriteBatch` of
  about 7 records, and 7 commit cache entries. It replaces 7
  trips through RocksDB's write queue.
- **A member waits for the whole batch's sync**, not its own.
  That is the same sync.

## 7. Expected effect

At 16 clients a sync covered about 6.7 commits, PostgreSQL's
7.5. With no wait for older commits, a digres commit should span
about 2.1 sync times, as PostgreSQL's does, so about 7.5 commits
a sync: about 12% more commits a second at an equal sync time.
Fewer trips through RocksDB's write queue also cut the CPU of a
commit, which is about twice PostgreSQL's
([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md)
step 376).

## 8. Steps

1. **Measure the start**: `ins` and `fk` at 16 clients, the
   commits a sync and the share of time in `db_commit_wait()`
   (`bench/build/prof5/prof.sh`,
   [`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 405).
2. **Build the queue and the leader** in `sm1/src/db.cc` and
   `sm1/src/xct.cc`, as §3. Remove `committing`,
   `db_commit_done()`, `db_commit_wait()` and the visible
   waiters.
3. **Test at every layer**:
   - sm1 unit tests: a stalled leader's batch holds back every
     newer commit and no older one; a failed SSI check queues
     nothing; a batch of a prepared and a plain commit; with
     `durability_sync` off.
   - the isolation scenarios in `sm1/mvcc`: all must still pass,
     unchanged.
   - rel and dg: the suites as they are; dg's `select_cursor.sql`
     in 8 sessions checks that a commit is seen by its own next
     statement.
4. **Measure again**, as step 1, both systems alternated.
5. **Update the documents**:
   [`XCTMGR.md`](XCTMGR.md#32-visible_ts-and-the-commit-queue)
   §3.2, §8 and §8.5, [`DBMGR.md`](DBMGR.md#82-group-sync) §8.2,
   [`../dg/WORKNOTE.md`](../dg/WORKNOTE.md).

## 9. Decided

Decided by the user, 2026-11-07:

1. **One mutex for claim, SSI check and enqueue** (§3.1). Order
   holds by construction, and the section is short.
2. **The leader publishes the batch** (§3.2 steps 5 to 7). A
   member publishing itself would again wait for the older ones.
3. **The no-sync mode on the same path**: the leader writes the
   batch and skips the sync. One way to commit.

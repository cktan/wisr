# XCTMGR: the Transaction Manager

The Transaction Manager decides what each read sees and when each
write takes effect. It hands out timestamps and transaction ids,
takes snapshots, and runs a transaction from `sm1::begin` to its
end. It implements the isolation levels, statements, savepoints,
SSI, commit, the commit cache and two-phase commit. Its code is
in `sm1/src/xct.cc`, `sm1/src/ssi.cc` and `sm1/src/session.h`,
with parts in `sm1/src/db.cc` and `sm1/src/gc.cc` (§11.1).

The rest of a transaction's work is elsewhere. ROWSTORE.md has
the write and read paths, first-committer-wins and read
committed's write protocol. LOCKMGR.md has locks, waits for a
writer and deadlocks. DBMGR.md has `CF:txn` as a column family,
the WAL, group sync and recovery. GC.md has the GC watermark,
stamping and the `CF:txn` sweep.

---

## 1. A worked example

Session A runs at READ COMMITTED, and `visible_ts` is 100. The
example leaves out the savepoints that the levels above set
around each statement.

```sql
BEGIN;
    -- sm1::begin: snapshot_ts = 100. No xid yet.
UPDATE t SET v = 2 WHERE id = 1;
    -- sm1::stmt_begin: stmt_seq = 0.
    -- The first read takes the statement snapshot, 100.
    -- The first write: xid = 101, wts = 102, seq = 1.
SAVEPOINT s;
    -- sm1::savepoint: depth 1, the mark holds seq_hwm = 1.
UPDATE t SET v = 3 WHERE id = 2;
    -- wts = 103, seq = 2, and one entry in the undo list.
ROLLBACK TO s;
    -- sm1::rollback(1): delete the version at wts 103.
    -- seq and seq_hwm go back to 1.
COMMIT;
    -- sm1::commit: claim commit_ts = 104.
    -- Write { COMMITTED, 100, 104 } at CF:txn[101].
    -- Sync the WAL past it: the commit point.
    -- Move xid 101 from the active table to the commit cache.
    -- visible_ts rises to 104, and the commit returns.
```

A snapshot at 104 sees `v = 2` for row 1. A snapshot taken before
the commit, at 100, does not. Row 2 never changes for anyone.

---

## 2. Timestamps and ids

### 2.1 One counter

`global_ts` is a monotonic 64-bit atomic counter. It produces
every timestamp and transaction id.

| Value | Taken | Use |
| --- | --- | --- |
| `xid` | at the transaction's first write | names the transaction |
| `wts` | at each write, once no other writer of the key is in flight | keys the version |
| `commit_ts` | at commit | orders the commits |

- `sm1::begin` takes no value. A snapshot is a read of
  `visible_ts`.
- A transaction that never writes has `xid` 0, unless it prepares
  (§10.1).
- `snapshot_ts <= visible_ts < xid` always holds.
- `commit_ts > wts` for every write of the transaction, and
  `commit_ts > xid`.
- An `xid` or a `wts` is a bare `fetch_add` (`db_alloc_ts`). Only
  a `commit_ts` and a lease renewal take `alloc_mu`.
- Every write takes a value, so consecutive transactions' ids are
  several apart.

### 2.2 The order of a key's versions

**Invariant.** For one key, committed versions in descending
`wts` order are also in descending `commit_ts` order.

*Why.* Two transactions that write key `k` are serialized. A
writer that finds another transaction's version of `k` in flight
waits for that transaction to end. A writer takes its `commit_ts`
before it leaves the active table. Suppose `T2` committed a
version of `k` after `T1` did. Then `T2` took its `wts` after it
found `T1`'s version no longer in flight, so `T2.wts >
T1.commit_ts > T1.wts`.

The argument uses no snapshot, so it holds at every level, read
committed writes that pass a newer commit included. It lets a
reader seek by `wts` and get commit order.

Ordering by `xid` would fail. An `xid` is fixed at the first
write, so a transaction can write a key after a commit newer than
its `xid`. Take a read committed transaction with `xid` 50 that
updates a row committed at 70. Keyed by `xid`, its version sorts
behind the committed version of `xid` 60. Readers return the
older value, and GC drops the newer commit as superseded. Keyed
by a `wts` of 75, the version sorts first, and GC drops the older
one.

### 2.3 Surviving a restart

The counter is volatile, so `CF:meta` holds `next_ts`, advanced
in leases.

1. A renewal persists `n + ts_lease`, synced, before `n` is
   handed out (`cover_ts_locked`). `ts_lease` is 1,000,000 by
   default.
2. If the renewal fails, `n` is burned and the call fails.
3. At open, `global_ts` and `ts_limit` resume at `next_ts + 1`,
   above every id ever handed out.
4. `visible_ts` starts at `next_ts`, so every later `xid` is
   above it.

Each renewal is a synchronous `Put`, and every write takes a
value. A larger `ts_lease` is the first lever if a benchmark
shows renewals. No benchmark has measured it.

Object ids use the same scheme in `next_objid`. Storeids and
seqids share it, so the two never collide. It never hands out a
value below 1000. 0 is the catalog store, and 1 to 999 are
reserved for objects a database is made with. A database made
before this rule resumes at 1000.

A sequence has a lease of its own, `"seq." | be64(seqid)`,
holding the first value the lease does not cover. The step is the
sequence's increment.

- A value is taken under `alloc_mu` and never given back, so a
  sequence is not transactional.
- A crash skips the rest of the lease. A clean close does not: it
  writes each sequence's next value as its lease, in one synced
  batch.
- A database stays open for `idle_close_ms` after its last
  session closes, so sessions that come and go do not close it.
- A sequence with no lease row has handed out nothing, so it
  starts at its `start`.

SEQUENCE.md has the rest.

---

## 3. Snapshots and visibility

### 3.1 A snapshot

A snapshot is one number, `snapshot_ts`, taken as `visible_ts`. A
reader sees every version whose writer committed at or below it.
Visibility tests are integer comparisons, snapshots cost nothing
to take, and no transaction list exists to copy or search.

`db_take_snapshot` reads `visible_ts` and registers the value
under `snap_mu`, the mutex the GC watermark is read under. So no
snapshot lands below a watermark in use. A begin snapshot stays
registered for the transaction's whole life, at every level. A
statement snapshot is at or above it, so it needs no
registration.

### 3.2 `visible_ts` and the commit sequencer

**Warning: a snapshot must not see a gap**, a transaction with
`commit_ts <= snapshot_ts` that has not yet published its status.
So commits publish in `commit_ts` order.

```
commit sequencer: the commit_ts values in progress

  in progress: { 106 }          done: 104, 105, 107

  visible_ts = 105: one below the smallest in progress,
               so 107 stays hidden until 106 is done
```

The sequencer is `db_t::committing`, an ordered set under
`alloc_mu`.

- `db_claim_commit_ts` takes the value and inserts it in the set
  under one hold of `alloc_mu`. The advance reads the set and
  `global_ts` under the same mutex. So no `commit_ts` is ever
  outside both the set and what the advance saw.
- `db_commit_done` removes the value. It then sets `visible_ts`
  to one below the smallest value left, or to `global_ts - 1` if
  the set is empty. No thread does this: each commit advances
  `visible_ts` itself.
- New values are above what was handed out, so `visible_ts` never
  moves backwards.
- An `xid` or a `wts` never enters the set, so only commits in
  progress hold back `visible_ts`. The advance may pass such a
  value. That is harmless: a version it keys resolves through its
  writer's status.
- If a status write or sync fails, the process aborts (§8.3), so
  no value stays in the set for ever.
- The set holds at most one entry a session, so it needs no
  sizing and no back-pressure. A stalled commit holds back
  `visible_ts` but never blocks allocation.
- Every commit at or below `visible_ts` has its record written,
  and synced unless `durability_sync` is off.

The cost is that one slow commit holds back new snapshots
briefly. The WAL sync bounds this, and group sync shares one sync
across the batch.

### 3.3 The visibility test

```
if V.xid == view.xid:              # own write
    visible iff V.seq <= view.seq_hwm
elif V.wts > view.snapshot_ts:     # written after the snapshot,
    invisible                      # so committed after it too
else:
    commit_ts = V's stamp, or resolve V.xid (§9.1)
    visible iff committed and commit_ts <= view.snapshot_ts
```

A visible tombstone means that the key does not exist. The code
is `visible()` in `sm1/src/access.cc`.

A `view_t` holds the snapshot, the reader's `xid`, and a `seq`
limit on its own writes. `xct_view` sets that limit to `seq_hwm`,
or once a statement has begun to `min(seq_hwm, stmt_seq)`. So
savepoints and statements both act through one number.
`xct_read_view` also takes a statement's snapshot if the
statement has not read yet (§5.1). A cursor freezes its view when
it opens.

---

## 4. The transaction lifecycle

### 4.1 States

```
                sm1::begin
                   |  snapshot_ts = visible_ts
                   v
              +---------+   first write: xid from global_ts
              | ACTIVE  |<--- sm1::savepoint, rollback, forget
              +----+----+
     sm1::prepare  |        | sm1::commit
                   v        v
            +----------+  claim commit_ts, validate, write the
            | PREPARED |  status record, sync, publish, release
            +----+-----+
   sm1::commit,  |
   sm1::abort    v
            COMMITTED or ABORTED
```

A session runs at most one transaction, with no nesting. A second
`sm1::begin` fails with `SM_ERR_INTRANS`, as it does while the
session's prepared transaction is in doubt. `sm1::begin` takes
`begin_seq`, the begin order that deadlock victim choice uses. It
takes and registers the snapshot, and gives a SERIALIZABLE
transaction its SSI node.

The first write calls `xct_alloc_xid`, before any version exists.
It takes the `xid`, enters the active table and `xid_owner` under
`txn_mu`, and maps the `xid` to the SSI node. So a reader that
steps past a version always finds its writer.

A session set by `sm1::set_no_transaction` has no transaction. It
serves settled reads and page calls (`xct_check_cleaner`,
BTREEINDEX.md).

### 4.2 Failures

Every data call starts with `xct_check_active`:

- no active transaction: `SM_ERR_PREPARED` if the session's
  transaction is in doubt, else `SM_ERR_NOTRANS`;
- `T.doomed` is `SM_ERR_DOOMED`: fail with it, naming the failed
  rollback's reason (§6.3);
- `T.doomed` is `SM_ERR_DEADLOCK`: abort the transaction and fail
  with it.

`SM_ERR_CONFLICT`, `SM_ERR_DEADLOCK` and `SM_ERR_SERIALIZATION`
are retryable. `xct_fail_retryable` aborts the transaction on the
engine's side before it reports one, so the caller must not call
`sm1::abort`. `SM_ERR_UPDATED`, `SM_ERR_SELFMODIFIED`,
`SM_ERR_BLOCKED` and `SM_ERR_INTERRUPTED` leave the transaction
active.

**Two different dooms.** `session_t::doomed` fails the next call
on the transaction. `ssi_node_t::doomed` (§7.4) fails only the
commit or the prepare.

**Declared read-only.** `sm1::begin` can declare the transaction,
as SQL's `READ ONLY`. A write, `sm1::mkstore`, `sm1::rmstore`,
`sm1::mkseq` or `sm1::rmseq` then fails with `SM_ERR_READONLY`. A
temporary store may still be written.

### 4.3 Isolation levels

| Level | Reads at | A write meets a commit after `snapshot_ts` |
| --- | --- | --- |
| READ COMMITTED | a statement snapshot (§5.1) | waits for a writer in flight, fails with `SM_ERR_UPDATED`, and goes ahead once the caller read the row with `sm1::read_latest` |
| REPEATABLE READ | the begin snapshot | fails with `SM_ERR_CONFLICT`, and aborts |
| SERIALIZABLE | the begin snapshot | as REPEATABLE READ, plus SSI (§7) |

- REPEATABLE READ is snapshot isolation. It has no lost updates,
  no non-repeatable reads and no phantoms, but write skew is
  possible.
- READ COMMITTED writes check against `T.snapshot_ts`, not the
  statement snapshot. PostgreSQL behaves the same at its default
  level, and workloads rely on it. The `wts` key makes this safe
  (§2.2).
- A layer that wants no statement snapshots can require the
  higher levels. That needs no engine support.

### 4.4 Abort

`sm1::abort` writes no status record: no record already means
aborted, before and after a crash. Only a prepared transaction's
abort writes `ABORTED` (§10.3). An abort is O(1) in the size of
the write set, and the versions stay until GC. It skips the
doomed check, so a doomed transaction can always be aborted.
`sm1::closedb` aborts an active transaction.

`discard` does these steps:

1. End a bulk load still open, with its files.
2. Close the cursors and batch iterators, and let go of the lent
   row. This releases their RocksDB snapshots.
3. Free the SSI node: an aborted node constrains nobody.
4. Take the `xid` out of the active table, and wake the writers
   that wait for it.
5. Drop the stores the transaction created, newest first. No
   other transaction can see them. A failure leaves an orphan
   column family for the startup sweep.
6. Take out the temporary sequences it made.
7. Release its locks.
8. Forget the transaction, which releases its snapshot.

### 4.5 `sm1::on_commit`

`sm1::on_commit` keeps a function to run once the transaction has
committed. It runs at the end of the committing call, on that
thread, outside sm1 and outside any transaction. So `commit`
hands the functions to the entry point through a `thread_local`
vector.

- Functions run newest first, once each. A failed commit and an
  abort run none.
- A rollback drops the functions added since its mark.
- A prepared transaction carries them in memory, and whichever
  call commits it runs them. They are not in the record, so a
  commit after a restart runs none.
- The commit is durable before a function runs, and the function
  is not. After a crash the caller must find that work itself.

---

## 5. Statements

A statement is what a host's SQL statement maps to, and it may
span many sm1 calls. A statement has two parts: a statement
snapshot, for READ COMMITTED, and a statement mark, at every
level.

### 5.1 The statement snapshot

Without `sm1::stmt_begin`, a READ COMMITTED transaction takes a
fresh `visible_ts` at each `sm1::read` and each `sm1::scan`. A
cursor keeps its snapshot for its life. The snapshot is local to
the call, and `T.snapshot_ts` stays the begin snapshot.

An index lookup is one scan and N reads. It would see N+1 states,
and could return a mixture that never existed. So
`sm1::stmt_begin` sets a boundary. From the first read after it,
every READ COMMITTED read uses one snapshot, until the next
boundary or the transaction's end. `T.stmt_pinned` says a
boundary was used. `T.stmt_taken` and `T.stmt_snapshot_ts` hold
the snapshot once taken.

The first **read** takes the snapshot, not the boundary call and
not a write. So a statement may block on a writer or a lock, and
still read everything committed while it waited. `xct_read_view`
takes it. `xct_view`, which a write uses to validate a storeid
and a key, only reads it once taken.

### 5.2 The statement mark

**A statement does not see its own writes.** `sm1::stmt_begin`
also sets `T.stmt_seq` to `T.seq`, at every isolation level. From
then on every read sees the transaction's own versions only up to
the mark. This is PostgreSQL's command id. A cursor alone is
safe, as it freezes its limit at open. A read begun later is not:
the inner side of a nested-loop join, a correlated subquery, a
host's lookup by key. That is the Halloween problem.

- **Writes still see the newest own version**, to lock and check
  against it. An update or delete of a key whose newest version
  is the transaction's own, above the mark, fails with
  `SM_ERR_SELFMODIFIED` and writes nothing. It is not an abort.
  The caller skips the row, as PostgreSQL's update skips a row
  "already modified by me", or raises an error, as `INSERT ... ON
  CONFLICT` does.
- **An insert is unchanged.** A key the statement inserted is
  still a duplicate.
- **A lock of a row the statement read** is refused the same way.
  `sm1::lock_row` and `sm1::chain_lock_row` with `as_read`, as
  FOR UPDATE and FOR SHARE lock a row, fail with
  `SM_ERR_SELFMODIFIED`. They take no lock. PostgreSQL's FOR
  UPDATE passes over such a row. A lock without `as_read` reads
  the newest version, own included, and is unchanged: an insert's
  arbiter, an index build (`a18`).
- **A rollback lowers the mark** with `seq` (§6.4), because the
  same statement writes the rewound numbers again.
- **`sm1::stmt_advance` moves the mark to now** and leaves the
  snapshot, as PostgreSQL's `CommandCounterIncrement`. It is how
  a statement uses what it made: `CREATE TABLE AS` makes a table,
  its store and its `_id` sequence, and fills them. A store or a
  sequence is looked up at the mark like a row, so the statement
  sees it only after the advance.
- **A transaction that never calls `sm1::stmt_begin`** sees all
  its own writes.

### 5.3 A statement inside another

A trigger's or a function's commands run inside the statement
that fired them. Each sees the outer's writes so far. When it
returns, the outer resumes with its own snapshot and mark, and
sees neither its own writes nor the inner one's. PostgreSQL's
executor saves and restores its snapshot the same way.

`sm1::stmt_begin(session, outer)` saves the statement fields in a
`stmt_t` and begins the inner statement. `sm1::stmt_end(session,
outer)` puts them back. The mark goes no higher than `seq`, which
a rollback in between may have lowered.

### 5.4 No end call at top level

The next `sm1::stmt_begin` ends the last statement, and the
transaction ends the last of all. The cost is that a transaction
which uses the call once reads that way for the rest of its life.
The gain is one call in place of a pair to keep balanced across
rollback and prepare.

### 5.5 Reading the newest state

DDL that reads every row of a table and writes them again, or
checks them, must see the rows as they are, not as an old
snapshot shows them. PostgreSQL's `ATRewriteTable()` reads at the
latest snapshot for this reason. A rewrite at a REPEATABLE READ
snapshot older than another transaction's commit would lose that
commit's rows (`../dg/WORKNOTE.md` step 394).

`sm1::view_latest(session, true)` makes every read, and every
cursor opened while it is on, read at `visible_ts`, at every
level. The transaction's own writes stay in its view, by its xid
and mark as before. `sm1::view_latest(session, false)` puts the
transaction's view back, and the transaction's end turns it off.
The optional `previous` argument saves the prior setting for
restoration after a nested check. `sm_view_latest()` exposes this
to the host for foreign-key validation.

- **The caller holds table locks that prevent writes** on what
  it reads. A rewrite holds X. A foreign key check holds only
  IS, and is sound because the key, not valid, is enforced on
  every write: no write breaks what the check decided. What is
  newest when a cursor opens stays true.
- **No snapshot is registered for it.** The newest committed
  version of a key is never purged, and a key purged is a
  deleted one, which the newest state does not hold.
- **Writes are not changed.** They still check for conflicts
  against the transaction's snapshot. A rewrite writes a new
  store, where nothing conflicts.

sm3 uses it for a table rewrite, for the check that a table
holds no row before a NOT NULL column with no default is added,
and for the scan that makes a column NOT NULL.

---

## 6. Savepoints

A savepoint is a mark a transaction may roll back to. Setting one
is cheap. Holding one costs memory in proportion to the work done
after it, and a rollback issues one delete for each version
written since the mark.

Two choices force this. Writes are eager, so a version to undo is
already in RocksDB. Visibility is decided by one `xid` for the
whole transaction, so a version left in place would commit with
the rest. PostgreSQL pays nothing here: each savepoint starts a
subtransaction with its own xid, and a rollback marks that xid
aborted. §13.6 is the design that would remove the cost.

### 6.1 The undo list

`T.undo` is an in-memory vector of `(storeid, userkey, wts, seq)`
entries in write order, each flagged if its key is chained. It
holds no payloads.

**A write is listed only while a savepoint is set.** A rollback
goes back to a mark and no further, so a write made with no mark
on the stack can never be undone. A transaction that sets no
savepoint keeps no list. When the last mark is forgotten the list
is freed. Each mark carries its own `seq_hwm`, because the write
before it may not be in the list.

### 6.2 The cap

`savepoint_undo_bytes`, 64 MB by default, bounds a transaction's
list, each entry counted with its key. A write that passes the
cap is not refused. `xct_undo_overflow` frees the list and logs a
warning, and every mark then on the stack is **lost**.
`T.lost_marks` counts them from the bottom of the stack.

- A rollback to a lost mark fails with `SM_ERR_UNDOLIMIT` and
  dooms the transaction (§6.3). The versions to delete are no
  longer known, so only an abort is left, which needs no list.
- Writes go on, and list nothing while every mark is lost.
- A savepoint set afterwards starts a new list, and rolls back as
  usual.
- A lost mark may be forgotten, and is then no longer counted.

The write goes through because a rollback is rare. Nearly every
statement that outgrows the cap succeeds. Only the rare one pays:
its rollback becomes an abort of the whole transaction.

### 6.3 A failed rollback dooms the transaction

A rollback at a lost mark leaves the writes in place. So does one
whose deletes fail (`SM_ERR_IO`), with some perhaps deleted. None
of that may commit. So the rollback sets `T.doomed` to
`SM_ERR_DOOMED` and keeps its own error as the reason.

Every later call then fails with `SM_ERR_DOOMED`: each entry
point, an open cursor's next read or seek, and a sequence draw.
Only `sm1::abort` goes through. A caller with nowhere for the
rollback's error to go, as PostgreSQL's subtransaction abort has
not, finds it at its next call.

A rollback refused by its checks changes nothing and dooms
nothing: a depth with no mark, or a bulk load open.

### 6.4 The calls

`T.savepoints` is a stack, so `T.savepoints[d-1]` is the mark at
depth `d`. A transaction holds at most `sm1::max_savepoints`
(256), which turns runaway nesting into an error. Nothing needs
that particular value.

Savepoints nest because a layer above needs one for each
statement. Index maintenance turns one row write into several. A
unique violation on the third row of a multi-row insert must undo
the statement's partial writes. That layer pushes a mark at a
statement's start and forgets it at the end.

`sm1::savepoint` fails with `SM_ERR_INVALID` if a bulk load is
open or the stack is full. Otherwise it pushes a `mark_t` and
returns the depth. The mark holds `seq_hwm`, the lock list's
position, the latest cursor's id, and the sizes of the lists a
rollback cuts.

`sm1::forget(depth)`, SQL's RELEASE SAVEPOINT, drops that mark
and those inside it. The writes and the locks stay, and now
belong to the enclosing mark or the transaction. If no mark left
can be rolled back to, it frees the undo list. With no mark at
that depth it fails with `SM_ERR_NOSAVEPOINT`.

`sm1::rollback(depth)` does these steps:

1. Fail with `SM_ERR_INVALID` if a bulk load is open, and with
   `SM_ERR_NOSAVEPOINT` if no mark is at that depth.
2. At a lost mark, doom the transaction and fail with
   `SM_ERR_UNDOLIMIT`.
3. Queue a delete for each versioned entry after the mark, newest
   first, in one batch for each RocksDB instance.
4. Queue a `DeleteRange` for each store loaded since the mark and
   not made since it (§6.6).
5. Write each batch. The temporary instance writes no WAL.
6. Put back each chained key written since the mark, under its
   latch, with `chain_restore`. A key written twice finds nothing
   to undo the second time.
7. Close the cursors opened after the mark (§6.5).
8. Let go of the batch iterators and the lent row, and raise
   `write_gen`. No iterator opened before the rollback serves a
   read after it.
9. Drop the stores made since the mark, newest first.
10. Cut `created`, `dropped`, `dropped_seqs`, the temporary
    sequence lists, `on_commit`, `loaded_stores` and `undo` back
    to the mark.
11. Set `seq_hwm` to the mark's, `seq` to `seq_hwm`, and lower
    `stmt_seq` to at most `seq`.
12. Release the locks taken since the mark.
13. Wake the writers that wait for this transaction, as a version
    they wait on may be gone (`rollback_epoch`).
14. Drop the marks inside this one. This mark stays, so it can be
    rolled back to again.

A failure in the deletes or the chained restores dooms the
transaction. Otherwise it stays active. SSI state is not rolled
back (§7.2).

**Why a rollback is exact.** Nothing else can see the versions it
deletes, so no visibility window exists. `seq` is in the version
key, so a rewrite of a key did not clobber the version from
before the mark. A chained key has no version of its own to
delete. `chain_restore` puts back the newest version at or below
the mark's `seq_hwm`, or not the transaction's, and unlinks the
entries it passes (ROWSTORE.md).

**Why `seq` is rewound.** `seq_hwm` is the last `seq` still in
effect. Rewinding `T.seq` with it keeps the visible own versions
an exact prefix `1..seq_hwm` of the writes. Without the rewind,
the next write would raise `seq_hwm` past the undone numbers.
§3.3 would then admit undone versions wherever they are still
readable. A cursor opened before the rollback is such a place.
With the rewind, a later write reuses an undone number, and the
delete already removed the version that carried it.

**Why releasing the locks is safe.** The row was untouched before
the mark, so no one observed the transaction's intent on it. A
conversion of a lock held from before the mark is not undone. A
lock's `latest` mark goes with a lock taken since the mark, and
stays on one held from before it (`f10`).

**Depths are reused.** A depth kept from a mark that is gone can
name a different mark later. SQL already obliges the layer above
to drop a savepoint name when its mark goes: `RELEASE`, and a
rollback to an outer mark. So a correct caller cannot form a
stale depth. `sm1/API.md` states the hazard.

### 6.5 Cursors

A cursor holds a RocksDB snapshot and a `seq_hwm` frozen at open.
So a rollback's deletes are invisible to it, and its limit still
admits the undone versions.

- **A cursor opened after the mark is closed.** Left open, it
  would yield rows the transaction undid. PostgreSQL also closes
  cursors opened in a subtransaction that aborts. Closing
  releases its RocksDB snapshot.
- **A cursor opened before the mark is untouched.** Its limit
  predates the undone versions.
- **Each mark records `scan_id`.** Each cursor takes an id from a
  session counter, so "opened after the mark" is one comparison.
  A set of open cursors would misjudge a cursor opened at the
  address of one closed before the mark.
- **Closed, not freed.** Each `sm1::scannxt` and `sm1::scan_seek`
  on a closed cursor fails with `SM_ERR_SCANCLOSED`, until
  `sm1::scanend` or the transaction's end frees it. A caller may
  not know that a rollback reached its cursor. A SQL cursor
  declared before a savepoint may begin a scan again inside it.
  Freeing that scan would leave the next `FETCH` a dangling
  pointer.
- **The code is its own**, not `SM_ERR_INVALID`, so a host can
  tell it from a misuse. The cursor's old view cannot be
  recreated, and digres says so.

### 6.6 Bulk loads

A bulk load lists one entry, not one key a row (`BULKLOAD.md`).

- A load opened while a savepoint is set adds its store to
  `T.loaded_stores`.
- A rollback to a mark before the load empties the store with one
  `DeleteRange`, in the batch of its other deletes. The store was
  empty when the load opened and kept from every other
  transaction since. `store_range` bounds the range below the
  store's pages.
- While a load is open, no savepoint is set and none is rolled
  back to (`SM_ERR_INVALID`). Its keys share one `seq` and wait
  in its files, so they cannot be undone in part.

---

## 7. SSI: serializable snapshot isolation

SERIALIZABLE runs at snapshot isolation, tracks
rw-antidependencies, and fails a transaction at the pivot of a
dangerous structure. This follows Cahill, Röhm and Fekete (SIGMOD
2008), with the refinements of Ports and Grittner (VLDB 2012).

### 7.1 Why

Snapshot isolation stops write-write conflicts but misses
read-write ones. Take two on-call doctors, where at least one
must stay on call:

```
T1: read on_call(alice), read on_call(bob) -> both on call
T2: read on_call(alice), read on_call(bob) -> both on call
T1: write alice = off
T2: write bob   = off
both commit -> nobody on call (write skew)
```

The two wrote different keys, so snapshot isolation sees no
conflict. Each one read something the other wrote.

A history is serializable exactly when its dependency graph
(Adya) has no cycle. An edge T1 -> T2 means that T1 must come
first. A ww edge is an overwrite, and a wr edge reads a version.
An **rw edge** means T1 read x and concurrent T2 wrote a newer
version. Under snapshot isolation only rw edges between
concurrent transactions can close a cycle. Waits for a writer and
first-committer-wins keep concurrent writers apart, and a
snapshot never sees uncommitted data. Fekete et al. (2005) proved
that every such cycle has two consecutive rw edges, a **dangerous
structure**:

```
T1 --rw--> T2 --rw--> T3
```

T2 is the **pivot**. T1 and T3 may be one transaction: a write
skew is the cycle of two. So SSI searches no graph, and watches
for this structure.

The doctors make the cycle of two. T1 read bob and T2 wrote bob,
so T1 --rw--> T2. T2 read alice and T1 wrote alice, so T2 --rw-->
T1. Each is a pivot. The first to commit finds no T3 committed
yet, and commits. The second finds it, and fails with
`SM_ERR_SERIALIZATION`.

### 7.2 SIREAD locks

A **SIREAD lock** is a snapshot isolation read lock. It blocks
nobody, and nobody waits on it. It is a note that says
"transaction T read key k, or range [a, b]". A later write uses
it to see a conflict with an earlier read. `pg_locks` shows
PostgreSQL's as `SIReadLock`.

**A point read** records `(storeid, userkey)`, whether or not a
row was found (`ssi_on_read`). Reading a missing key is a
predicate read, and a later insert of that key must find the
lock. The lock is taken before the iterator exists. Otherwise a
write could land after the iterator's snapshot and before the
lock, and neither side would see the other. For the same reason a
SERIALIZABLE read never seeks an older iterator again (`c36`).

**A scan** records a range, the phantom defence: a later insert
into the range conflicts although no row was there at read time.

1. `sm1::scan` registers the whole requested range, bounds as
   passed, before it takes the RocksDB snapshot
   (`ssi_range_open`).
2. The cursor's close narrows the range (`ssi_range_close`). A
   scan that reached the end keeps it whole. A scan that examined
   keys keeps it through the last key examined, inclusive. A scan
   that examined no key drops it.

The last key examined is the last userkey whose versions the
cursor inspected, whether or not it yielded a row.

Registering only at the close would be wrong. A write while the
cursor is open is not in its RocksDB snapshot, so the reader
never steps past it. The writer would find nothing to conflict
with. The order within `sm1::scan` matters too. A writer puts its
version before it calls `ssi_on_write`, so a version put after
the snapshot is found only if the range exists. Narrowing is
safe, as the reader read nothing past that key.

`sm1::scan_seek` moves forward only, and keeps the keys it
skipped inside the range (`c30`). This over-approximates what was
read. It can only cost an unnecessary failure, and never misses
one.

**Other sources.**

- A store lookup is a point read of the catalog key `"R" |
  be64(storeid)` in store 0 (`c31`). Under SERIALIZABLE, a hit in
  the session's store cache counts only if the same transaction
  made the entry, by `begin_seq` (`c35`).
- `sm1::ssi_read_range` records a range of a B-tree's entries,
  read through its pages, and is not narrowed.
  `sm1::ssi_write_key` is the matching write of a key that is no
  version (BTREEINDEX.md).
- A bulk load calls `ssi_on_write` for each key.
- A temporary store takes no SIREAD lock and makes no edge.

**Lifetime.** A SIREAD lock survives its transaction's commit,
because a committed reader can still be the source of an edge. An
abort, a safe snapshot (§7.6) or the end of every overlap (§7.7)
drops it. A rollback to a savepoint does not (`c29`). Otherwise a
rollback could launder a serialization failure. Edges from writes
that were rolled back stay too. Both are conservative and
correct.

### 7.3 Edges

Each `ssi_node_t` has `in_edges` and `out_edges`, the
transactions at the other ends. An edge is kept on both ends. A
node that goes takes its edges with it, so an aborted transaction
leaves none (`c26`, `c27`).

```
T_writer writes k  ->  for each SIREAD on k, or a range covering k,
                       held by a concurrent T_reader != T_writer:
                           edge  T_reader --rw--> T_writer

T_reader reads k   ->  if it steps past a version of a concurrent
                       T_writer (committed after its snapshot,
                       or still in flight):
                           edge  T_reader --rw--> T_writer
```

- `ssi_on_write` runs after the put. A reader that arrives later
  steps past the version instead.
- A reader is concurrent if it is still running, or ended after
  the writer's snapshot.
- A SERIALIZABLE point read seeks before every version of the
  key, so it meets the concurrent writers' versions
  (ROWSTORE.md).
- A scan owes the same edges. They matter most for versions
  committed between the reader's snapshot and `sm1::scan`. Those
  writers ran `ssi_on_write` before the range existed, so only
  the read side can make the edge.

### 7.4 The abort rule

A cycle always holds a dangerous structure in which T3 commits
first (Ports and Grittner). The edge lists make their rule
possible, in place of Cahill's two flags:

```
dangerous(T1, T3), for a pivot T2 that commits after T3:
    T3 has committed, and
    T1 is T3, or
       T3 committed before T1 did (or T1 is still running), and
       if T1 is read-only (§7.5):
           T3 committed before T1's snapshot

at commit of T:
    if T is doomed, or
       for some committed T3 in T.out_edges and T1 in T.in_edges,
       dangerous(T1, T3):
        abort T with SM_ERR_SERIALIZATION

edge R --rw--> W, made by R, onto a committed W:
    if for some T3 in W.out_edges that committed before W,
       dangerous(R, T3):
        doom R
```

- A committed pivot cannot be told "no". So an edge that makes it
  the middle of a dangerous structure dooms the running T1.
- A structure whose T3 commits after T2 fails no one. T3 then
  follows T2 in the serial order.
- Dooming never fails the call that made the edge. The
  transaction fails at its commit, so reads and writes never
  return `SM_ERR_SERIALIZATION` (`c13`).
- `ssi_precommit` is the check. A writer runs it after it claims
  `commit_ts` and before it writes its record, so the rules judge
  it at its commit time. A failure hands `commit_ts` back unused.
- A node that passes is `committed` at `end_ts`, and can no
  longer fail.
- A read-only transaction takes no `commit_ts`, and counts as
  ending at `visible_ts + 1`.
- A prepared transaction counts as committed, at no known time:
  `end_ts` is `UINT64_MAX`. As T3 it counts as committed first
  (`commit_early`), and as T1 or T2 as committing last
  (`commit_late`). These readings can only fail more.

The rules stay conservative: a structure fails a transaction
whether or not the rest of a cycle exists. False failures are the
accepted cost of never taking a read lock and never deadlocking
on reads. Callers must be able to retry.

### 7.5 Who takes part

Only SERIALIZABLE transactions take part, as in PostgreSQL. Other
levels have no node. So serializability holds among SERIALIZABLE
transactions only, and a mixed pair is as permissive as
REPEATABLE READ (`sm1/mvcc/MVCC.md`, family C).

A transaction is **read-only** to SSI in two ways:

- **Declared** at `sm1::begin`. sm1 refuses its writes, which
  makes the rest sound. The rules treat it as read-only from its
  begin.
- **Undeclared**, committed with `T.xid == 0`. The read-only test
  applies from then on. While active it stays fully tracked: it
  may still write, and then needs its read set as a possible
  pivot. A prepare does not count, even with no write.

PostgreSQL applies the read-only test only to a transaction
declared `READ ONLY`, so sm1 commits some pivots that PostgreSQL
fails (`sm1/mvcc/MVCC.md`, p06).

A read-only transaction is never a pivot, as nothing has an
rw-edge into it. It can be the T1 of a structure, Fekete's
read-only anomaly (`c07`, `c32`).

### 7.6 Safe snapshots

A declared read-only transaction's snapshot is **safe** once two
things hold. Every writer concurrent with the snapshot has
finished. None of them committed with an edge out to a
transaction committed before the snapshot. A writer here is any
SERIALIZABLE transaction not itself read-only. The watch is under
`ssi_mu`.

At the begin of declared `T_ro` with snapshot S
(`watch_writers`):

1. If a restored transaction is in doubt, or committed after S,
   mark `T_ro` unsafe.
2. If a writer that committed after S has an edge out to a
   transaction committed at or before S, mark `T_ro` unsafe.
3. Otherwise watch every writer still running. With none, `T_ro`
   is safe at once.

When a watched writer W ends (`settle`):

1. Stop watching W.
2. If `T_ro` is already unsafe, stop.
3. If W committed with an edge out to a transaction committed at
   or before S, mark `T_ro` unsafe.
4. If W was the last one watched, `T_ro` is safe.

- Deciding at a writer's commit check is enough. An edge out it
  gains later leads to a transaction still running then, which
  commits after S.
- A prepared writer settles at its prepare, where it passes that
  check.
- A safe node drops its SIREAD locks and its edges out, and its
  later reads leave no lock and make no edge. It can no longer be
  a T1.
- An unsafe node stays fully tracked to its end.

`DEFERRABLE`, which waits at begin for a safe snapshot, is not
built.

### 7.7 Indexes and memory

Every write must find each SIREAD lock that covers its key.

- Point locks are a hash on storeid and userkey, `ssi_points`.
- Ranges are in `ssi_ranges`, one list a store, in registration
  order, and `ssi_on_write` walks the store's list.
- One mutex, `ssi_mu`, covers the whole tracker.
- A writer below SERIALIZABLE has no node, and pays nothing.

An interval tree for each store would answer in `O(log n + m)`
for `m` matches. It is the replacement once a benchmark shows
many live ranges on one store, and it is not built.

A committed node goes once the GC watermark passes its `end_ts`
(`ssi_cleanup`). No transaction concurrent with it is then left.
`ssi_cleanup` runs at each SERIALIZABLE transaction's end and in
the GC worker. So a long-running reader makes the tracker grow.

The standard escape is to summarize the oldest committed
transactions, past `ssi_max_entries`, into a coarse "something
committed in this range" entry. That is safe, and costs false
failures. It is not built: `ssi_max_entries` is accepted and
unused.

---

## 8. Commit

### 8.1 What makes a commit

**Warning: the status record is the commit.** Data versions are
in RocksDB before it. Before the commit point they resolve as in
flight, or after a crash as dead. After it they resolve as
committed. No transaction is ever half-visible, and no data
version ever needs redo or undo.

The record goes in `CF:txn` under the `xid` (§11.3). Only a
writing transaction gets one, or a read-only one that prepares.
`durability_sync = 0` skips the sync: it trades a bounded window
of commits for throughput, and the database stays
crash-consistent. DBMGR.md has the WAL and group sync.

### 8.2 The procedure

`commit`, `finish_commit` and `publish_commit` do these steps:

1. If the session's transaction is prepared, resolve it by gid
   (§10.3), and stop.
2. Check the transaction with `xct_check_active`. Refuse it with
   `SM_ERR_INVALID` if a bulk load is open.
3. Close the cursors, and let go of the lent row. This releases
   RocksDB snapshots, and narrows SIREAD ranges before
   validation.
4. With `xid` 0, take the read-only path below, and stop.
5. Claim `commit_ts`. If the lease renewal fails, discard the
   transaction and report the error.
6. Run `ssi_precommit`. On a failure, hand `commit_ts` back with
   `db_commit_done`, abort, and report `SM_ERR_SERIALIZATION`.
7. Write the `COMMITTED` record, unsynced.
8. Sync the WAL past it, unless `durability_sync` is off. This is
   the commit point.
9. Keep the `on_commit` functions for the entry point.
10. If the transaction made or dropped a store, raise
    `last_ddl_ts` to `commit_ts`, so no store cache trusts an
    entry across it.
11. Move the `xid` from the active table to the commit cache
    (`enter_cache`), and wake the writers that wait for it.
12. Mark `commit_ts` done, which advances `visible_ts`.
13. Wait until `visible_ts` reaches `commit_ts` (§8.4).
14. Release the locks.
15. Drop the temporary stores it dropped, queue its dropped
    stores for GC, and forget its dropped sequences' leases.
16. End the SSI node, which stays while others overlap it.
17. Forget the transaction.
18. Back at the entry point, run the `on_commit` functions.

The read-only path runs `ssi_precommit` at `visible_ts + 1`, and
a failure aborts. It then ends the SSI node, releases the locks,
keeps the `on_commit` functions, drops the temporary stores it
dropped, and forgets the transaction. It takes no `commit_ts`,
writes no record, and does not wait. A transaction that never
wrote can still hold locks from `sm1::lock_row` and
`sm1::lock_oid`.

**Why this order.**

- The `xid` stays in the active table until the sync is done, so
  no reader takes an unsynced record for a commit.
- The record comes before the cache, so the cache never says
  "committed" of something the disk does not.
- The cache comes before publication, so no snapshot can include
  the commit while a lookup would miss it.
- A writer takes `commit_ts` before it leaves the active table,
  which §2.2 needs.
- Everything after the commit point is `noexcept`. Otherwise
  `sm1::commit` could report a failure for a committed
  transaction, and a caller's `sm1::abort` would drop the stores
  it created. Running out of memory aborts the process.

### 8.3 A failed status write is fatal

**Warning: any failed write or sync of a status record aborts the
process** (`sm0::fatal()`, `CODE.md`).

The record may or may not have reached the WAL, as a sync can
fail after the bytes are written. So the outcome is unknown until
WAL replay decides it. A second sync proves nothing: after a
failed fsync, Linux may drop the dirty pages and clear the error,
as PostgreSQL found in 2018.

- No commit that the failed write or sync was to cover returns,
  so none is acknowledged.
- The next attach's WAL replay decides each transaction. A record
  that reached the OS survives the abort, and is committed.
- A write that RocksDB refuses before it reaches the WAL aborts
  the process too, for example after a full disk in a flush. The
  status does not tell this refusal from a failure after the
  write. PostgreSQL does the same: a failed WAL write is a PANIC.
  While the disk stays full, each restart aborts again at its
  first commit.
- `PREPARED` and `ABORTED` records follow the same rule (§10), so
  the engine keeps no failed state.

### 8.4 A commit returns once it is visible

Commits are not serialized. Only their two ends take `alloc_mu`,
and between them each writes and syncs its record with no lock.

```sql
-- session A                         -- session B, at once
create temp table uc (f1 int);       ...
insert into uc values (1), (2), (3); commit;  -- claims 99, syncing
select count(*) from uc;  -- 0 now and then; 3 is right
```

A's INSERT commits at 100 while B's commit at 99 writes its
record. With no wait, A's commit would return with `visible_ts`
still 98. A's next statement would take its snapshot at 98, and
miss its own rows. PostgreSQL makes a commit visible before
COMMIT returns: any snapshot taken after, in any session, sees
it.

So after `db_commit_done`, `db_commit_wait` waits on
`visible_cv`, under `alloc_mu`, until `visible_ts >= commit_ts`.
`db_commit_done` signals it whenever `visible_ts` rises.

- It waits only behind older commits in progress, with group sync
  often on the same sync. With none older it does not wait.
- It holds its locks through the wait, as it held them through
  its own sync.
- An older commit's failed write or sync aborts the process, so
  no waiter waits for ever.
- **An interrupt does not cut it short**, unlike every other wait
  in sm. The transaction has committed, and an older commit's
  sync bounds the wait. A cancel would leave a committed
  transaction that its own next statement does not see.
- A commit of a prepared transaction waits alike.
- A validation failure after the claim calls `db_commit_done`
  with nothing published, which raises `visible_ts` for the
  commits behind it.

---

## 9. The commit cache

The commit cache is an in-memory map from `xid` to `commit_ts`,
the fast answer for a transaction that committed recently.

**Warning: it changes speed, never answers.** The status record
says the same thing, and a miss costs one RocksDB read.

### 9.1 What it serves

A version carries its writer's `xid`, not its writer's fate. A
reader resolves the `xid` in this order.

| Step | Where | Answers |
| --- | --- | --- |
| 0 | the version's own header | committed, if compaction stamped the `commit_ts` into it |
| 1 | the active table, in memory | in flight: running or prepared |
| 2 | the commit cache, in memory | committed, at this `commit_ts` |
| 3 | `CF:txn` | committed or dead by its record; a `PREPARED` record is in flight |
| 4 | none of the above | never committed: dead |

Callers do step 0 and their own-`xid` check first. `xct_status`
does the rest, and `xct_resolve` folds its answer to committed or
not. Its callers are the read and write paths (`access.cc`,
`chain.cc`), the compaction filter (`gc.cc`) and recovery's
orphan sweep (`recover.cc`).

- A stamp comes first: once the record is swept, it is the only
  evidence of the commit.
- A `PREPARED` record answers in flight, so a compaction filter
  that runs before recovery rebuilds the active table leaves the
  version alone.
- Step 4 makes crash recovery free of any undo pass. One process
  has the database open, so the active table is exhaustive.
- A live transaction is never misjudged, because it enters the
  active table before its first version, and because of the order
  in §8.2.

Without step 2, every unstamped version of a committed
transaction would read `CF:txn`. Compaction may not visit a new
version for a long time. The cache covers that gap for the newest
transactions, which readers ask about most.

### 9.2 What it is

The cache is a fixed array in `db_t`, under `txn_mu`.

```
cache       2 x cache_sets slots, each { xid, commit_ts }
cache_sets  (commit_cache_entries + 1) / 2
```

- An `xid` hashes to one set (`cache_set`), and its entry is in
  one of the set's two slots or nowhere.
- An empty slot holds `xid` 0, which no transaction has.
- A slot is 16 bytes, so the default 65,536 slots take 1 MB.
  Nothing is allocated after the database opens.
- `commit_cache_entries` is an option at open, at least 1.
- `xid`s are hashed, not used as an index, because they are
  sparse (§2.1). PostgreSQL's ids have a counter of their own.
- `txn_mu` also guards the active table. This is deliberate. A
  committing transaction leaves one and enters the other in one
  step, so no reader finds it in neither and concludes it is
  dead.
- Only committed transactions are in it, and only their
  `commit_ts`. A design that adds to a transaction's status must
  add it here too.

### 9.3 The life of an entry

**It enters at commit** (§8.2), a prepared transaction's
included.

**It enters when a lookup finds it on disk.** A lookup that
misses reads the record. If the record says `COMMITTED`, the
lookup caches it before it answers. A transaction then costs one
disk read, however many of its versions are met. This is safe
because a commit is final and an `xid` is never reused.

**It leaves by eviction.** A new entry takes the slot in its set
that holds the smaller `xid`, an empty one first
(`cache_put_locked`).

- A lookup of an old transaction keeps the newer entry. An old
  commit read back has a small `xid`, so it takes the older slot.
  The set's recent entry survives a scan over old transactions.
- An old transaction being read keeps its place until two newer
  entries land in its set.
- A commit always enters. A long transaction's `xid` may be the
  smallest of its set. It still takes a slot, and is the first
  evicted after.
- Eviction cannot change an answer. The record was written before
  the entry, and is swept only once every version it covers is
  stamped.

**Nothing else removes it.** An entry stays after its versions
are stamped and its record is swept. It is still true, and
nothing asks for it.

**It dies with `db_t`.** A closed database frees it, and an open
starts empty.

### 9.4 After a restart

**Warning: the cache starts empty, and nothing loads it.**
Recovery reads `CF:txn` only for `PREPARED` records.

Answers are right from the first read. An unstamped version of a
commit from before the restart reads `CF:txn` once for its
transaction, then uses the cache. An unstamped version of a
transaction running at the crash reads `CF:txn` every time, finds
nothing, and is dead. So a restart costs one disk read for each
committed transaction a reader meets. An evicted transaction
costs the same at any time.

The cache covers the time until compaction stamps the versions
(GC.md). This is not prompt. A version in a file that nothing
rewrites stays unstamped, and no task stamps on a schedule of its
own. A bulk load's ingested files land at the bottom of the tree.
So their rows lean on the cache, then on `CF:txn`, for a long
time. Below `txn_sweep_records`, one million, no record is swept.

### 9.5 Limits

- **Dead transactions are looked up every time.** Caching "no
  record" would spare those reads. It is sound, as a transaction
  absent from the active table with no record can never commit.
  It is not built.
- **A large transaction is still evicted** once two newer entries
  land in its set. It costs one read to return.
- **One mutex.** Every resolution of an unstamped version takes
  `txn_mu`, as does each writer's first write, commit and abort.
  No benchmark has measured it. At hundreds of thousands of
  transactions a second it is likely to bind before the cache's
  size does. Shards are the answer once a benchmark shows it.
- **The size.** The cache reaches back its size over the commit
  rate: about 6.5 seconds at 10,000 transactions a second, 0.65
  at 100,000, and 0.2 at 320,000, about the single-server TPC-C
  record. It must cover the stamping lag, seconds to minutes
  under load. A miss is cheap, as the memtable or block cache
  answers it, and hot rows stay covered. Cold rows written a few
  seconds earlier are the misses. Nothing has measured the lag,
  and the size should come from it. So size the cache by memory,
  with a larger default: a million slots cover 10 seconds at
  100,000 a second, in 16 MB.

---

## 10. Two-phase commit

`sm1::prepare` makes a transaction durable but not visible. Its
result is a **prepared transaction**, named by a **gid**, in
doubt until a coordinator resolves it.

### 10.1 Prepare

1. Check the transaction with `xct_check_active`, and the gid's
   length (`sm1::max_gid`, 200).
2. Refuse a transaction that made, dropped or wrote a temporary
   store (`SM_ERR_INVALID`). A prepared transaction outlives its
   session, and those stores would not.
3. Refuse a transaction with a bulk load open or done
   (`SM_ERR_INVALID`). The record names explicit locks alone, so
   a load's keys are no longer a reason. No other reason is
   recorded, and the refusal stands.
4. Reserve the gid with a null entry in `db_t::prepared`. A gid
   in use among unresolved transactions fails with
   `SM_ERR_EXISTS`.
5. Close the cursors, and let go of the lent row. An open cursor
   could never be used again, and it would hold back the
   watermark while the transaction is in doubt.
6. Give a transaction with no `xid` one, entering the active
   table.
7. Build the `PREPARED` record (§11.3).
8. Make the prepared object, and copy the transaction's state
   into it (§10.2).
9. Run `ssi_precommit` at `UINT64_MAX`. On a failure, release the
   gid, abort, and report `SM_ERR_SERIALIZATION`. This is the
   last moment the engine may say no.
10. Move the transaction into the prepared object.
11. Write the record, always synced. A failure is fatal (§8.3).
12. Register the object under the gid.

**Every step that can fail comes before `ssi_precommit`.** The
check marks the node committed, and nothing can undo that: a
committed node whose transaction then aborts has no `end_ts`,
and `ssi_cleanup` never frees it. So all allocation is done
first, and the move after the check cannot fail. A failure
before the check leaves the transaction with its session to
abort. After the write nothing may fail, and registering finds the reserved entry and allocates
nothing. Until then the entry is null, so no resolver finds a
transaction whose record is not durable.

**A read-only transaction needs a durable record too**, as
`sm1/API.md` promises that a prepared transaction survives a
crash. It gets an `xid` so its record has a unique key. With
`xid` 0, every read-only prepare would overwrite one record.
Locks from `sm1::lock_row` and `sm1::lock_oid` go in the record:
what makes it read-only is the empty set of versions, not an
empty lock set. One that only read holds nothing after a restart.

**After a failed prepare write**, WAL replay decides. If the
record survived, `sm1::list_prepared` lists the transaction. If
not, it is dead. A coordinator that lost its participant checks
the list and resolves by gid. It never received a "yes" vote, so
abort is always a correct decision.

### 10.2 The prepared object

The transaction moves into a `session_t` the database owns, with
`prepared` set, registered in `db_t::prepared`. `detach_prepared`
moves its state, its snapshot's place in the watermark, its SSI
node and its `on_commit` functions. It points `xid_owner[xid]` at
the object, so a waiting writer waits on it. `lock_transfer`
moves its locks and points their table entries at the new owner.

The session keeps only the gid, and has no transaction of its own
until that gid is resolved. Its data calls fail with
`SM_ERR_PREPARED`.

A prepared transaction's versions sit in their stores, and
readers treat it as in flight. It holds its locks, and it holds
the watermark at its `snapshot_ts`.

`sm1::closedb` of its session leaves it alone. Closing the
database frees the object, and the next open restores it from its
record, as after a crash. So one path serves a transaction
prepared before a crash and one prepared in this process.

### 10.3 Resolving

`sm1::commit_prepared` and `sm1::abort_prepared` resolve by gid
on any session, which is the point: after a restart the resolving
session is never the preparing one. On the preparing session
`sm1::commit` and `sm1::abort` do the same. If another session
resolved it first, the session has no transaction:
`SM_ERR_NOTRANS`.

`resolve` does these steps:

1. Under `prepared_mu`, take the object and hold its entry null.
   A second resolver gets `SM_ERR_NOTFOUND`, and no prepare can
   take the gid.
2. Commit or abort it.
3. Put the object back if it is still in doubt, else drop the
   entry and free the object.

Holding the entry, and not erasing it, lets a failed resolution
put the transaction back with no allocation.

**Commit** is §8.2 from the claim of `commit_ts`.

1. Claim `commit_ts`. If the lease renewal fails, the transaction
   stays in doubt.
2. Give the SSI node its `end_ts`, with no validation.
3. Run `finish_commit`. `COMMITTED` overwrites `PREPARED`, which
   drops the lock list and the gid.

**Abort.**

1. Write `ABORTED`, synced, or a crash would restore the
   transaction in doubt. A failure is fatal: after reopen the
   transaction is aborted, or listed for the coordinator to abort
   again.
2. Free the SSI node and the restored entry.
3. Run `discard`, which drops the stores in `created`.

### 10.4 Why validate at prepare

Once prepared, a transaction cannot be told "no". So the
serialization check must already have passed, and must stay
passed. A transaction that would make a dangerous structure with
a prepared transaction sees it as committed (§7.4), and fails
itself. Two prepared transactions were each live during the
other's validation, so they were checked against each other.

### 10.5 After a restart

**Warning: SIREAD locks and edges are volatile.** §10.4 does not
cover a transaction that begins after a restart. A restored
in-doubt transaction lost them, so a new transaction can write a
key it read, and no edge appears. Unchecked, that admits write
skew:

1. Before the crash, `T_prep` reads `A = a0`, writes `B = b1`,
   and prepares.
2. After the restart, `b1` is in flight again, so a writer of `B`
   waits for `T_prep`, but its SIREAD lock on `A` is gone.
3. `T_new` reads `B`, skips `b1`, and reads `b0`: an edge `T_new
   --rw--> T_prep`.
4. `T_new` writes `A = a1`. No SIREAD lock covers `A`, so no edge
   `T_prep --rw--> T_new` appears, and `T_new` commits.
5. The coordinator commits `T_prep`. Each read what the other
   overwrote: a cycle.

Recovery enters each restored transaction in `ssi_restored`, and
the tracker applies one extra rule:

```
T_reader skips a version written by a restored transaction
    ->  doom T_reader; it fails at prepare or commit
        with SM_ERR_SERIALIZATION
```

This is enough with no replay of the SSI graph. Nothing it wrote
can be read before it commits, and its versions in flight make
any writer of its keys wait. So every edge into it from a later
transaction comes from reading past one of its versions, which
the rule sees. Edges out of it are unknowable, so every
transaction with an edge in is doomed, and no cycle can pass
through it.

The rule holds after the coordinator commits it. A reader whose
snapshot predates that commit still skips its versions and is
doomed. A reader whose snapshot follows it sees them and makes no
edge. So `ssi_prepared_committed` records the `commit_ts`, and
`ssi_cleanup` drops the entry once the watermark passes it. An
abort drops the entry at once, as its versions are dead.

The cost is false failures for SERIALIZABLE readers of in-doubt
keys, which is acceptable for a state that only follows a
restart.

`restore_prepared` in `sm1/src/recover.cc` does these steps for
each `PREPARED` record:

1. Decode it into a new prepared object, marked `restored`, with
   a new `begin_seq`.
2. Put its `xid` in the active table and in `xid_owner`.
3. Take its locks again, each in the mode the record carries.
4. Register its snapshot in the watermark.
5. Enter it in `ssi_restored`, and register it under its gid.

### 10.6 The other calls

- `sm1::list_prepared` is the only way to find what recovery
  restored. Without it such transactions hold their locks and the
  watermark for ever, with no way for an operator to name them.
  It needs no active transaction, because a coordinator calls it
  on a fresh session at startup. It skips a gid being prepared or
  resolved. Each row has the gid and `prepared_at`, durable
  because the listing is mostly needed after a restart.
- `sm1::in_doubt` says whether `sm1::commit` and `sm1::abort` on
  this session would resolve a prepared transaction. The C
  wrapper's catch-all asks it, so it never undoes one.

---

## 11. Implementation

### 11.1 Source map

| File | Holds |
| --- | --- |
| `sm1/src/xct.cc` | begin, commit, abort, statements, savepoints, prepare and resolve, `xct_status`, the commit cache, the status records |
| `sm1/src/ssi.cc` | the SSI tracker |
| `sm1/src/session.h` | `session_t`, `scan_t`, `view_t`, the internal calls between the modules |
| `sm1/src/db.h` | `db_t`, `ssi_node_t`, `ssi_range_t` |
| `sm1/src/db.cc` | `db_alloc_ts`, `db_claim_commit_ts`, `db_commit_done`, `db_commit_wait`, the leases |
| `sm1/src/gc.cc` | `db_take_snapshot`, `db_release_snapshot`, `db_watermark` |
| `sm1/src/access.cc` | `visible`, `concurrent_writer`, `skip_version`, `access_undo_add`, the `SM_ERR_SELFMODIFIED` check |
| `sm1/src/recover.cc` | `restore_prepared` |

### 11.2 Shared state

| Mutex | Guards |
| --- | --- |
| `alloc_mu` | the leases, `committing`, and `visible_cv`'s wait |
| `snap_mu` | `snapshots`, the registered begin snapshots |
| `txn_mu` | `active`, `xid_owner`, the commit cache |
| `ssi_mu` | `ssi_nodes`, `ssi_points`, `ssi_ranges`, `ssi_by_xid`, `ssi_restored` |
| `prepared_mu` | `prepared`, by gid; a null value holds a gid in a prepare or a resolution |

`global_ts`, `ts_limit`, `visible_ts` and `last_ddl_ts` are
atomics. A transaction's own state is in `session_t`, and only
its session's thread changes it. `T.seq` is the last write's
number, and a write past `UINT32_MAX` is refused. `T.write_gen`
is raised by each write and never lowered, as `seq` is: an
iterator opened before it rose may lack own versions.

### 11.3 Status records

The key is the `xid`, 8 bytes big-endian. Every value starts with
17 bytes: a state byte, `snapshot_ts`, `commit_ts`.

| State | Byte | `commit_ts` | Rest |
| --- | --- | --- | --- |
| `PREPARED` | 1 | 0 | as below |
| `COMMITTED` | 2 | the commit's | nothing |
| `ABORTED` | 3 | 0 | nothing |

The rest of a `PREPARED` record, all big-endian:

```
prepared_at   8 bytes, epoch seconds
gid           4-byte length, then the bytes
locks         4-byte count, then 20 bytes each: type byte, the two
              words of the lock id, mode byte, 2-byte kind
created       4-byte count, then 8-byte storeids
dropped       4-byte count, then 8-byte storeids
```

- The record is one value, however many locks it lists.
- A lock goes by its id, mode and kind. So a shared lock is
  restored as shared, and no key is hashed again (LOCKMGR.md).
- `snapshot_ts` restores the transaction's place in the
  watermark.
- `created` lets recovery keep those column families.
- `dropped` lets a commit after a restart queue their removal.

### 11.4 Trade-offs

- **`visible_ts` serializes publication.** A stalled commit holds
  back new snapshots. The WAL sync bounds this, and group sync
  shares one sync across the batch (§3.2). The alternative makes
  snapshots expensive to take and visibility expensive to test
  (§13.1).
- **SSI fails transactions that would have been fine.** That is
  the price of readers that never block and never deadlock
  (§7.4). Callers must be able to retry.
- **A transaction's locks, undo list and read set are in
  memory.** Row values stream to RocksDB, so disk bounds them.
  These three are linear in the keys touched:
  - `T.locks` has an entry for each lock from `sm1::lock_row` and
    `sm1::lock_oid`. A write keeps none (LOCKMGR.md).
  - `T.undo` has an entry for each write made while a savepoint
    is set, with its key inline (§6.1).
  - The SSI read set has an entry for each key or range read,
    with the key inline (§7.2).

  So a transaction that touches tens of millions of keys costs
  gigabytes. This is the price of an O(1) abort and no undo pass.
  It is a real limit on one transaction's size, and not a bug.
- **A savepoint costs memory in proportion to the work after
  it.** It is cheap to set and expensive to hold over a large
  statement. `savepoint_undo_bytes` caps the cost, and past the
  cap a rollback becomes an abort (§6.2). A bulk load lists one
  entry for the whole load (§6.6). §13.6 is the design that
  removes the cost.
- **Every write takes a timestamp.** A version is keyed by a
  `wts`, so the counter moves once for each row written and not
  once for each transaction, and its lease runs out sooner. Each
  renewal is a synchronous `Put` (§2.3). `global_ts` is an
  atomic, so the common case takes no mutex. No throughput
  benchmark measures either cost. A larger `ts_lease` is the
  first lever.
- **The READ COMMITTED protocol is unproven under a race
  detector.** `e06` shows no lost increment. A
  `-fsanitize=thread` build of the scenarios reports races,
  mostly through a RocksDB that is not instrumented. Only some
  were read, and `e03` reports one too. Treat the result as
  inconclusive. sm1 has no sanitizer target of its own. digres's
  `make tsan` links a RocksDB built for the sanitizer
  (`dg/test/rocksdb.sh`), and runs digres, not these scenarios.

---

## 12. Cases

| Case | What happens |
| --- | --- |
| a transaction that wrote only a temporary store commits | it has an `xid`, so it commits as any writer; it cannot prepare |
| a commit behind an older commit in progress | it waits for that commit's sync |
| the lease renewal fails at a commit | `SM_ERR_IO`, no record, the transaction aborts |
| the lease renewal fails at a commit of a prepared transaction | the error is reported, and the transaction stays in doubt |
| a commit with a bulk load open | `SM_ERR_INVALID`, and the transaction stays active |
| a crash with a transaction running | no record, absent from the active table: dead |
| a crash after the record, before the sync returned | WAL replay decides; a record that reached the OS commits |
| a rollback to a mark set before a cursor opened | the cursor's next use gets `SM_ERR_SCANCLOSED` |
| a rollback to a mark set after a cursor opened | the cursor reads on, and never saw the undone versions |
| a rollback past `sm1::mkstore` | the store's column family is dropped |
| a rollback past `sm1::rmstore` | the deletes restore the catalog row; the store stays |
| a read committed statement waits on a writer, then reads | its first read comes after the wait, so it sees that commit |
| a pivot's neighbour aborts | its edge goes with it, and the pivot commits |
| two resolvers of one gid | the second gets `SM_ERR_NOTFOUND` |
| a session with an in-doubt transaction closes | the transaction stays |
| a commit of a transaction prepared before a restart | no `on_commit` function runs; `dropped` is queued from the record |

---

## 13. Rejected

### 13.1 Snapshots and ordering

- **A list of in-flight transactions in each snapshot**,
  PostgreSQL's way. Snapshots become expensive to take and
  visibility expensive to test.
- **A ring of slots indexed by timestamp**, for the sequencer.
  While one slot waits on a slow sync, every `xid` and
  `commit_ts` behind it needs a slot too. The ring must then wrap
  onto unpublished slots or block all allocation.
- **Keying a version by `xid`** (§2.2).

### 13.2 Statements and savepoints

- **The statement snapshot at the boundary call, or at a write.**
  A statement that blocked would hold a snapshot from before the
  wait.
- **A required `sm1::stmt_end` at top level** (§5.4).
- **One savepoint for each transaction.** A statement's mark
  clobbered any user SAVEPOINT. Compensating by hand left a
  tombstone for every write undone.
- **An id for each mark, never reused.** A stale reference would
  be an error, not a silent hit on the wrong mark. SQL's rules
  already forbid a stale depth (§6.4).
- **Refusing a write past the undo cap** (§6.2).
- **Freeing a cursor that a rollback closed** (§6.5).

### 13.3 SSI

- **Cahill's two flags.** The edge lists let an abort withdraw
  its edge (`c26`, `c27`). Under the flags both scenarios failed
  the pivot.
- **Dropping an undeclared transaction's tracking once its
  snapshot is safe, and failing a later write.** That fails every
  read-then-write transaction whose snapshot became safe. On an
  idle database it fails at once, and again on every retry.
- **A linear walk of all predicates under a global lock.** It
  would serialize the write path, the one place SSI must cost
  nothing when no reader is watching.
- **Persisting SIREAD locks in the `PREPARED` record.**
  PostgreSQL persists the locks and assumes both flags on
  recovery. Here that is not enough alone, as the edges are lost
  too. A restored transaction with an edge out is a pivot the
  moment a reader adds an edge in. It also makes the record as
  large as the read set.

### 13.4 Commit

- **A second sync after a failed one** (§8.3).
- **An interrupt that cuts short the wait for visibility**
  (§8.4).

### 13.5 The commit cache

- **A hash map with a queue, first in, first out.** About 64
  bytes an entry, against 16 for a slot.
- **Eviction by `commit_ts`.** It evicted an old commit read back
  from disk the moment it entered a full cache.
- **Eviction by use.** Marking an entry on each hit puts a write
  on the read path, under `txn_mu`. A hot entry that is evicted
  comes back with one disk read.
- **Replacing only an entry with a smaller `xid`.** It protects
  recent entries further. But it locks an old transaction out for
  good once newer ones hold both slots: after a restart, every
  transaction from before it.

### 13.6 A rollback that deletes nothing

This design removes the undo list. It is not built, and the
project leans to keeping the undo list.

- A rollback is rare, and the design saves a cost paid only to
  prepare for one.
- The list exists only while a savepoint is set, and
  `savepoint_undo_bytes` caps it.
- The change is to the core: the visibility rule, the status
  record, the commit cache, the compaction filter, recovery and
  the format.

It is worth another look if drivers that set a savepoint around
every statement turn out to be common for digres.

**The idea.** A rollback records that the `seq` numbers since the
mark are dead, one range however many versions. A version is live
only if its transaction committed and its `seq` is in no dead
range.

- `seq` is never rewound, so a range names exactly the writes
  between two moments.
- `T.dead` is a sorted list of ranges. A new range removes those
  inside it, and extends one that ends at the mark, or else is
  appended.
- `xct_status` takes `(xid, seq)`. A cursor asks the list as it
  stands, so it stops seeing undone writes with no close.
- The list lives in the active table, as a woken writer must step
  past a dead version. It also lives in the `PREPARED` and
  `COMMITTED` records and in the cache entry.
- The compaction filter drops a dead version before any committed
  rule. It never stamps one, or counts one as a newer version.
- A bulk load could roll back with no code of its own. Its
  versions carry a `seq`, and a range covers them in every mode.

**What it gives.** A savepoint costs nothing to hold, and no
transaction keeps an undo list. A rollback writes nothing to
RocksDB. Its time does not grow with the writes since the mark,
apart from the locks it releases. The gain is largest for a large
statement under a driver that sets a savepoint around every
statement, or in a PL/pgSQL block with an exception handler.

**What does not change.** The API, the version's key and value,
and abort. Abort already works this way: it deletes nothing, and
its versions are dead by their status.

**The range list.** A loop that sets a savepoint, writes and
rolls back holds one range, however often it runs. A loop that
also keeps a write each time adds a range of two numbers each
time. No limit is proposed on the list's length. PostgreSQL's
subtransaction ids grow the same way.

**Who asks the list.**

- A transaction's own read admits a version if `V.seq <=
  view.seq_hwm` and `V.seq` is not in `T.dead`. `seq_hwm` keeps
  one job, to freeze what a cursor sees at open. `T.dead` takes
  the other, to hide undone writes.
- A reader that meets a running transaction's version skips it,
  dead or not, and needs no list.
- A rollback wakes the writers that wait for the transaction.
  Such a writer finds the rolled-back version still in RocksDB,
  under an `xid` still running. It resolves the version, finds
  the `seq` dead, and steps to the version below. So other
  threads read the list while its owner adds to it, and a guard
  must cover it.
- A committed transaction answers dead for a `seq` in its ranges.

**Records and compaction.**

- The commit writes the list in its one record. A transaction
  that never rolled back writes an empty list.
- A record stays for as long as it does now. The sweep's argument
  stands (GC.md): the pass drops each dead version and stamps
  each live one, so no version needs the record after it.
- A dead version is never stamped. A reader believes a stamp with
  no lookup, so a stamped dead version would be a committed one.
  The filter has the ranges when it stamps, because the record
  outlives the stamping.
- The new filter rule comes first, so a dead version does not
  count as the newer version that drops an older one.
- A running transaction's dead versions stay until it ends.
  Dropping them sooner is possible, as the active table has the
  list. It is not proposed.

**Prepare and recovery.** Prepare writes the list in the
`PREPARED` record. No write follows a prepare, so the list is
final. Recovery rebuilds it with the transaction, and the commit
copies it to the `COMMITTED` record. A crash of a running
transaction loses the list, which is right: all its versions are
dead.

**What a rollback still does.** It drops the stores made since
the mark, cuts the lists back, releases the locks, and closes the
cursors, though correctness no longer needs that. The catalog
rows of dropped stores and sequences are versions, and are dead
with the rest. SIREAD locks stay. A lock's READ COMMITTED mark
stays spent: the newest version is the other transaction's again,
so the next write needs its read again (ROWSTORE.md).

**The cost.**

- Dead versions stay until compaction. A rollback of a million
  writes leaves a million versions in the way of reads, as an
  aborted transaction does.
- `seq` is 4 bytes and never reused, so it runs out sooner. A
  transaction that writes and rolls back a billion rows four
  times is at the limit. Widening `seq` changes the version key,
  which is a larger format change than this one, and is not
  proposed.
- A status lookup of an unstamped version also tests the list.
  The list is empty for nearly every transaction.
- The record and the cache entry grow by 8 bytes a range.
- The status record gains a field, so the format version rises.

**Other ways considered.**

- An undo list spilled to disk bounds memory, but it is a file
  with a life of its own, and a rollback still issues a delete
  for each write.
- An xid for each savepoint, as PostgreSQL's subtransactions, has
  the same effect: a rolled-back xid gets no record, and its
  versions are dead by the existing rule. But a commit then
  writes a record for each one. The active table, the cache, the
  prepare record and SSI must all learn that one transaction has
  many xids, and a transaction must know its xids to read its own
  writes. The ranges keep one xid and one record.

**To check before any build.**

- Nothing reads a version's status but through `xct_status` and
  the own-write test. Its callers are in `access.cc`, `chain.cc`,
  `gc.cc` and `recover.cc`, and SSI acts on a version it steps
  past.
- A dead version does not count against a reader that skips a
  restored transaction's version.
- Every caller of a cursor's view can reach its transaction's
  list, the prepared object's too.
- The cache's bound still means something once an entry can be
  large.
- The design does not yet cover chained stores. A chained key's
  newest version is one key, written in place, and
  `chain_restore` puts the older one back (§6.4). With no
  restore, a dead version stays the newest, and every read, write
  and repair of the key must pass it. `filter_old()` also
  resolves a displacer's `xid` with no `seq` to test.

**What a build changes.** All of it is in sm1.

| Where | What |
| --- | --- |
| `sm1/src/session.h` | `T.dead`; a mark holds `seq`, and no position in an undo list; the undo list goes |
| `sm1/src/access.cc`, `sm1/src/chain.cc` | a write lists nothing; the own-write test and the status calls pass `seq` |
| `sm1/src/xct.cc` | `sm1::savepoint`, `sm1::forget` and `sm1::rollback`; the range rule; `xct_status` takes a `seq`; the list in the active table, the records, the cache and the prepared object |
| `sm1/src/gc.cc` | the filter passes `seq`, and drops a dead version before any committed rule |
| `sm1/src/recover.cc` | the list restored with a prepared transaction |
| `sm1/src/db.h` | the status record's new field, and the format version |
| `sm1/API.md` | the cost of holding a savepoint, and the `seq` limit |

**The order of work.** Tests come first at each step, in
`sm1/test/test_savepoint.cc` unless the step says otherwise.

1. Do the checks above. Verify: each is written up here.
2. Stop rewinding `seq`, with the undo list still there. Verify:
   the whole suite passes unchanged. This step only finds what
   depended on the rewind.
3. Add `T.dead`, own-write visibility by it, and the range rule.
   Verify: the list after nested and repeated rollbacks, and both
   loops above. A transaction and its open cursors see what they
   saw before.
4. Make the resolver take `seq`, and put the list in the active
   table. Verify, as a scenario in `sm1/mvcc`: another writer
   takes a key whose only newer version was rolled back by a
   transaction still running.
5. Add the list to the commit record and the cache, and raise the
   format version. Verify: another session sees the kept writes
   and none rolled back, with the cache, and after a restart
   without it.
6. Change the filter. Verify, in `sm1/test/test_gc.cc`: a
   rolled-back version is dropped and never stamped. An older
   committed version under a dead one survives. The sweep still
   removes the record.
7. Carry the list through prepare and recovery. Verify, in
   `sm1/test/test_2pc.cc` and `sm1/test/test_crash.cc`: a
   prepared transaction's rolled-back writes stay dead across a
   restart and after its commit.
8. Remove the undo list and the deletes. Verify: a savepoint held
   over a million writes adds nothing to the session's memory,
   and a rollback writes nothing to RocksDB.
9. Rewrite the sections of this document, DBMGR.md, GC.md and
   `BULKLOAD.md` that the change touches.

---

## 14. Tests

**Isolation scenarios** (`sm1/mvcc`, `sm1/mvcc/MVCC.md`) are the
specification. A change that fails one is presumed wrong.

| Scenarios | Cover |
| --- | --- |
| `a08`, `a09`, `a12`, `a13` | own writes, cursor stability, the commit boundary, a long reader |
| `a15`, `a16`, `a18` | statements, and a lock of a row the statement changed |
| `b14`, `b16`, `b20`, `b21` | a retryable failure discards the loser; deadlock victims |
| `b25`, `b31`, `f10`, `h04`, `h07`, `h08` | a rollback and locks |
| `c01` to `c36` | SSI |
| `e01` | SSI state is released once no transaction overlaps it |
| `e02`, `e03`, `e05`, `e06` | invariants under load at each level |
| `f01` to `f11` | read committed writes |
| `p01` to `p10` | PostgreSQL's own isolation tests |

A scenario cannot stall a commit between its claim and its
record, so a unit test does.

**sm1 unit tests** (`sm1/test`):

- `test_xct.cc`: `on_commit`, a stalled commit
  (`test_before_commit`), a failed status write, the commit cache
  at size 1, read back and in eviction.
- `test_savepoint.cc`: values restored, the stack and its bounds,
  cursors closed, DDL undone.
- `test_ssi.cc`: the abort rule's cases, declared read-only, safe
  snapshots, `sm1::ssi_read_range`.
- `test_2pc.cc`: resolving by gid, `sm1::in_doubt`, locks across
  a restart, read-only prepares, a prepared drop, the restored
  rule, a failed prepare or abort write.
- `test_crash.cc`: a killed process leaves a prefix of its
  commits, every acknowledged one included.

The test of the wait in §8.4 runs so. A claims its `commit_ts`
and stalls before its record. B commits after, so allocation goes
on past the stall. A new snapshot sees neither row. B's commit
returns only once A's is done, `visible_ts` catches up, and B's
next transaction sees B's row. With the wait taken out, B's next
transaction sees no row.

**rel** (`rel/test`): `test_2pc.rel`, `test_doomed.rel`,
`test_isolation.rel`, `test_stmt_advance.rel`,
`test_stmt_view.rel`.

**digres** (`dg/test`): `server.sh` runs eight sessions at once,
each inserting a row and counting its rows 50 times. Every count
must be the rows inserted so far. With the wait taken out, 28
counts of 400 missed the session's own last row. `run.sh` runs
`select_cursor.sql`.

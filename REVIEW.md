# Code Review: Group-Sync Fixes

The review read the fixes of the group-sync review on
`wip-review`, `b1ddfcad..f81ea1b8`. It found two correctness
gaps, two robustness gaps, stale documents and four cleanups.

- **Target:** `wip-review` at `f81ea1b8`.
- **Effort:** `high`.
- **Verification:** the findings come from reading the code.
  Findings 1 and 2 were checked against `f81ea1b8`.

## Status

The fixes are on branch `wip-review`.

| Finding | Status | Commit |
| --- | --- | --- |
| 1 | gone: nothing sets a failed state now (finding 6) | `ca3b8bb9` |
| 2 | kept, and documented in `SM1_DESIGN.md` §8.2 | `106da4df` |
| 3 | fixed | `40613bfc` |
| 4 | fixed, in both tests that stall a sync | `5520b20d` |
| 5 | fixed | `ca3b8bb9`, `d74c4102` |
| 6 | fixed | `ca3b8bb9` |
| 7 | fixed | `70480e86` |
| 8 | fixed for `run_oom_child()`; the others stay | `a7aec2fa` |
| 9 | no change | — |

## Findings

### 1. A commit can return 0 after a failed sync

- **Location:** `sm/sm1/src/db.cc`, `db_sync_wal()`.
- **Defect:** `db_sync_wal()` did not check `db->failed`. A
  PREPARE's synced write could fail its fsync and set the flag.
  A commit that waits in `db_sync_wal()` then synced again, and
  returned 0. After a failed fsync, Linux may drop the dirty
  pages, so that record may be lost.
- **Resolution:** finding 6 removed the failed state. Every
  failed status record write now aborts the process.

### 2. Every failed commit `Put` aborts the process

- **Location:** `sm/sm1/src/xct.cc`, `finish_commit()`.
- **Defect:** RocksDB refuses writes after a background error,
  such as a full disk in a flush. The record is then not
  written, but the process aborts.
- **Resolution:** kept. The returned status does not tell a
  refusal from a failure after the write. PostgreSQL also
  PANICs on a failed WAL write. A failed `persist_lease()` is
  not part of this finding: `db_claim_commit_ts()` returns
  `SM_ERR_IO` for it, before the write.

### 3. `fatal_v()` can hang

- **Location:** `sm/sm0/src/log.cc`, `fatal_v()`.
- **Defect:** `fatal_v()` took `g_log_mu` through `log_v()`. A
  hook that calls `fatal()` deadlocks on the mutex its thread
  holds. A hook stuck in another thread stops the abort.
- **Fix:** a thread in the hook skips the hook. Other threads
  wait at most one second for `g_log_mu`, now a
  `std::timed_mutex`. stderr always has the message.

### 4. A failed `REQUIRE` hangs the group-sync tests

- **Location:** `sm/sm1/test/test_groupsync.cc`.
- **Defect:** the destructor of a `std::async` future waits for
  its commit. A failed `REQUIRE` before `release.set_value()`
  left that commit stalled in the sync, so the suite hung.
- **Fix:** a scope guard lets the sync go on every way out.

### 5. Documents describe the removed fail-stop

- **Location:** `db.h`, `sm1.h`, `API.md`, `sm0.h`.
- **Defect:** comments and documents said that a failed write
  stops the database until it is reopened.
- **Fix:** they now say that the process aborts.

### 6. Two failure policies for status records

- **Location:** `sm/sm1/src/xct.cc`, `prepare()` and
  `abort_prepared_txn()`.
- **Defect:** a failed `COMMITTED` record aborted the process.
  A failed `PREPARED` or `ABORTED` record still used the
  failed state.
- **Fix:** every failed status record write aborts the process.
  The failed state, `xct_check_writable()` and limbo are gone.

### 7. Unused includes

- **Location:** `sm/sm1/src/env.cc`.
- **Fix:** `<stdio.h>` and `<stdlib.h>` are removed.

### 8. Child-process helpers duplicate `run_child()`

- **Location:** `sm/sm1/test/util.h` and the test files.
- **Fix:** `run_oom_child()` is replaced by `run_child()`.
- **Kept:** `spawn_child()` and the crash and sequence
  spawners. They kill the child after its report, and
  `run_child()` waits for it to exit. sm0's `read_all()` stays,
  because sm0 cannot include sm1's test helpers.

### 9. `sync_waiters` is read only by tests

- **Location:** `sm/sm1/src/db.h`.
- **Resolution:** no change. The count changes under
  `sync_mu`, which the waiting commit holds. Its cost is two
  integer operations for each wait. A hook could count only
  arrivals, and sleeps made the test depend on timing.

# Code Review: Group Sync

The review found no way to lose or wrongly show committed data
under the default RocksDB options. Two findings are durability
gaps: one needs a non-default option, the other a rare error. The
other findings are about waits, tests, comments and efficiency.

- **Target:** `fa1c683b`, "sm1: a commit's sync outside RocksDB's
  write queue".
- **Effort:** `high`.
- **Verification:** the findings come from reading the code. The
  review ran no tests and no benchmarks. Findings 1, 3, 6 and 7
  were checked against `db.cc:805-850`.
- **RocksDB version:** the build links the system RocksDB 9.11.2.
  The review read the vendored 8.9.1 source in `rocksdb/src`. The
  9.11 header confirms finding 1: `SyncWAL()` "does not imply
  FlushWAL, so `FlushWAL(true)` is recommended if using
  manual_wal_flush=true" (`rocksdb/db.h`). Finding 2 was not
  checked against 9.11.
- **Order:** the most severe finding is first.

## Status

The fixes are on branch `wip-review`.

| Finding | Status | Commit |
| --- | --- | --- |
| 1 | fixed: `FlushWAL(true)` in place of `SyncWAL()` | `ad395ca4` |
| 2 | fixed: a failed commit record write or WAL sync aborts the process, through `sm0::fatal()` (`5aa4551b`) | `eaae2964` |
| 3 | documented as an exception to interrupts | `0f91b03a` |
| 4 | fixed: the test waits on `sync_waiters` | `2ccd9b27` |
| 5 | gone: no commit waits behind a failed sync, as the process aborts | `eaae2964` |
| 6 | fixed | `1bb2ae1b` |
| 7 | fixed | `1bb2ae1b` |
| 8 | dropped: the gain is too small to measure | — |
| 9 | dropped: the gain is too small to measure | — |

**Why 8 and 9 were dropped.** The synced writes they name are
rare:

- `ts_lease` is 1,000,000 by default (`include/sm_options.h`),
  and the sequence lease takes a block of the same size.
- A transaction takes a few timestamps: its xid, its
  commit_ts, and its write timestamps.
- At about 6,500 tps, a lease therefore renews once every 30 to
  60 seconds. PREPARE and ABORTED are rare too.

So a synced write group, and a sync that does not move
`synced_seq`, costs one extra fsync about once a minute. A
benchmark of `ins` and `fk` would not show it. The fix would add
a durable-write helper, and it would make a failed lease sync
abort the process. Look at it again only if `ts_lease` becomes
small.

## Checked and Sound

- The leader and follower protocol in `db_sync_wal()`.
- The active-table guard, which stops a reader from taking an
  unsynced record for a commit.
- `GetLatestSequenceNumber()` covers the caller's record.
- In RocksDB 8.9, `SyncWAL()` calls `IOStatusCheck()` when a file
  sync fails. A failed fsync of a log file therefore stops
  RocksDB, as `GROUPSYNC.md` states.

## Findings

### 1. `SyncWAL()` skips the WAL buffer of `manual_wal_flush`

- **Location:** `sm/sm1/src/db.cc:839`
- **Defect:** `db_sync_wal()` calls `rdb->SyncWAL()`, which syncs
  with `SyncWithoutFlush()`. It does not empty the WAL writer's
  buffer. With `manual_wal_flush=true`, the commit record does
  not become durable.
- **Scenario:**
  1. A user sets `rocksdb_options="manual_wal_flush=true"`.
     `db_options()` parses any DBOptions string.
  2. The commit record goes into the buffer of `log::Writer`.
  3. `SyncWAL()` fsyncs the file without that buffer.
  4. `sm1::commit` returns 0.
  5. A power loss loses the acknowledged commit.
- **Note:** the old `Put(sync=true)` path flushed the buffer
  before the sync.
- **Fix:** call `DBImpl::FlushWAL(true)`. It flushes the buffer,
  then calls `SyncWAL()`, so it is correct in both modes.

### 2. A failed fsync of the WAL directory no longer stops RocksDB

- **Location:** `sm/sm1/src/db.cc:839`
- **Defect:** the sync moved out of the write path. On a failed
  fsync of the WAL directory, the old path called
  `IOStatusCheck()`, which put RocksDB in its background-error
  state. `SyncWAL()` returns the failure without that call.
- **Scenario:**
  1. A log switch occurs.
  2. The next commit's `SyncWAL()` fails at
     `FsyncWithDirOptions`.
  3. sm1 sets `db->failed`, but RocksDB still accepts writes and
     flushes.
  4. Writers that do not check `db->failed` change the database
     after the fail-stop: the GC's CF:txn sweep
     (`xct_drop_committed_records`) and the memtable flushes.
- **Note:** before this change, the same failure in
  `WriteToWAL` (`db_impl_write.cc:1434`, then `IOStatusCheck` at
  line 599) stopped every write.

### 3. The sync wait cannot be interrupted

- **Location:** `sm/sm1/src/db.cc:826`
- **Defect:** `sync_cv.wait(guard)` is a raw wait, and an
  interrupt cannot cut it short. `sm/CODE.md` §7 requires every
  wait made for a caller to be an `sm_wait()` with
  `sm_wait_mask()`. Only `db_commit_wait` has a documented
  exception.
- **Scenario:**
  1. The leader's `SyncWAL()` hangs on a stalled disk.
  2. Every commit behind it blocks in `sync_cv.wait`.
  3. An interrupt or a cancel cannot release the commits.
- **Fix:** use `sm_wait()`, and fail-stop on
  `SM_ERR_INTERRUPTED`, since the outcome is unknown. Or document
  the exception next to the one for `db_commit_wait`.

### 4. The one-sync test depends on fixed sleeps

- **Location:** `sm/sm1/test/test_groupsync.cc:390`
- **Defect:** `CHECK(calls == 2)` depends on sleeps of 50 ms for
  each session, then 300 ms. The sleeps must let all 15 records
  be written before the stalled sync is released.
- **Scenario:**
  1. The machine is loaded, or the test runs under sanitizers.
  2. One of the 15 sessions is still in begin, insert or
     `db_claim_commit_ts` when `release.set_value()` runs.
  3. Its record gets a sequence number above the `upto` of the
     second leader.
  4. It leads a third sync, `calls` becomes 3, and the test fails
     on correct code.

### 5. The failed-sync test can miss its target path

- **Location:** `sm/sm1/test/test_groupsync.cc:331`
- **Defect:** the target path is a follower that waits behind
  the failing leader. If B is slow, the test passes without that
  path: B fails in `xct_check_writable` instead.
- **Scenario:**
  1. B does not reach `db_sync_wal()` within 300 ms.
  2. A's sync fails and sets `db->failed`.
  3. B gets `SM_ERR_IO` from `xct_check_writable()`, in
     `commit()` or in the insert.
  4. The test passes, but the code that wakes waiters with the
     failure does not run.
  5. A regression in that code is not caught.

### 6. A comment says the opposite of its code

- **Location:** `sm/sm1/src/db.cc:822`
- **Defect:** the comment "Not leading: wait for the sync that
  runs." is above `if (!db->syncing) break;`. That branch is
  where the caller becomes the leader.
- **Scenario:** a maintainer reads the comment as a description
  of the break, and inverts the condition or moves the wait. The
  leader election of the protocol becomes harder to review.
- **Fix:** write "No sync runs: lead one" above the break, and
  put the wait comment on the `sync_cv.wait` line.

### 7. Two lock acquisitions have no comment

- **Location:** `sm/sm1/src/db.cc:812` and `db.cc:841`
- **Defect:** `std::unique_lock<std::mutex> guard(db->sync_mu);`
  and `guard.lock();` have no comment that says why.
- **Scenario:** `CLAUDE.md` requires a short comment on each
  significant call, a lock included. `sm/CODE.md` §8 states the
  same rule. Other code in `db.cc` follows it, for example
  "// Hold alloc_mu, the sequencer's mutex."

### 8. Lease writes still lead synced write groups

- **Location:** `sm/sm1/src/db.cc:623`
- **Defect:** the fix changes only the commit record. Other
  writes in the same RocksDB queue still use `sync=true`:
  - the commit_ts lease renewal (`persist_lease`, through
    `cover_ts_locked` in `db_claim_commit_ts`, under `alloc_mu`);
  - the sequence lease, near `db.cc:904`.
- **Scenario:**
  1. The `ins` and `fk` workloads run at 16 clients.
  2. Each lease renewal, and each nextval lease for SERIAL keys,
     leads a synced write group.
  3. That group takes in the insert versions in the queue, and
     they wait for its sync.
  4. The commit_ts lease renewal holds `alloc_mu` during its
     fsync, which blocks every commit_ts claim.
- **Fix:** one durable-write helper for every write that must be
  durable: an unsynced write, then `db_sync_wal()`.

### 9. RocksDB's own syncs do not move `synced_seq`

- **Location:** `sm/sm1/src/db.cc:815`
- **Defect:** `synced_seq` moves only when `db_sync_wal()` syncs.
  Syncs that RocksDB makes for a synced write (PREPARE, ABORTED,
  the leases) do not move it.
- **Scenario:**
  1. A lease renewal or a PREPARE syncs the WAL past sequence S.
  2. A commit has its record at or below S.
  3. The commit finds `synced_seq` below its target.
  4. It leads a full `SyncWAL()`: an extra fsync and a round trip
     on `log_write_mutex_`.
- **Fix:** after a synced write, read the latest sequence number,
  and raise `synced_seq` to it under `sync_mu`.

# Code Review: Scattered Keys

The review found no wrong-row, crash or memory-safety bug. The
top findings are performance and error-handling defects.

- **Target:** `HEAD~2..HEAD`, which is `bfe0128a` and `ef64048b`.
  These two commits are the scattered-keys feature.
- **Why this target:** `main` matches `origin/main`, and the tree
  has no change to tracked files. The review had no pending diff
  to read, so it took the latest feature. The two commits were
  the reviewer's choice, not a request.
- **Effort:** `high`.
- **Verification:** the findings come from reading the code. The
  review ran no tests and no benchmarks. Lines cited in findings
  1, 2, 3, 4, 7, 8 and 9 were checked against `ef64048b`.
- **Order:** the most severe finding is first.

## Findings

### 1. Correlation disables the iterator for clustered rows

- **Location:** `sm/sm3/src/table/table.cc:1687`
- **Defect:** correlation measures global order, not clustering.
  A low value disables the batch iterator for the whole cursor.
  This overrides the runtime check that would keep the iterator.
- **Scenario:**
  1. Load rows tenant by tenant, with hashed tenant ids.
  2. Each tenant's rows are adjacent, but |correlation| is near
     0.
  3. `tree_scattered` returns true.
  4. `read_chained_batch` does not try the steps.
  5. Each 64-row batch of a contiguous range goes to MultiGet.
- **Note:** step 4 of `test_tree.cc` asserts this behavior. Rows
  that are together with correlation -0.1 make no iterator. The
  runtime check of WORKNOTE step 372 alone would keep the
  iterator here.

### 2. A probe's gap carries over to later probes

- **Location:** `sm/sm3/src/table/table.cc:2077`
- **Defect:** `tbl_scan_rebound` resets the cursor's per-probe
  state (`at`, `n`, `batch`, `entries_done`). It does not reset
  the `missed` flag of the kept iterator.
- **Scenario:**
  1. A query with `k = ANY(array)` or an IN list reads a tree.
  2. The query reaches `open_probe` through `next_probe`, which
     aims the same cursor again.
  3. The two rows of one value are far apart, so `missed` is set.
  4. All later probes go to MultiGet, even with adjacent rows.
- **Note:** sm1 has no call that clears `missed` without freeing
  the iterator.

### 3. One pair of keys decides for the whole cursor

- **Location:** `sm/sm1/src/chain.cc:864`
- **Defect:** the fills of a tree cursor are 1, 2 and 4 rows. The
  second fill has `n` = 2, so a single miss makes `done` = 1.
- **Scenario:**
  1. A tree has a correlation of 0.9.
  2. One out-of-place row puts the second and third rows of a
     long range scan more than 4 live keys apart.
  3. `missed` is set.
  4. MultiGet reads all remaining rows, though the steps work.
- **Fix:** take a larger sample before giving up, or count the
  misses.

### 4. `tree_scattered` drops a statistics failure

- **Location:** `sm/sm3/src/table/table.cc:1675`
- **Defect:** `tree_scattered` drops a `cat_get_stats` failure
  and leaves the error in the thread's error state. The cache
  does not keep a failed load, so the load repeats on the read
  path.
- **Scenario:**
  1. A cold load runs `in_own_txn` inside `tree_fill`: opendb,
     begin, catalog scan, commit. The "in memory" note of
     WORKNOTE step 373 does not state this.
  2. The load fails, for example with `SM_ERR_CORRUPT` from
     `decode_stat`, or when opendb cannot open a session.
  3. `cat->stats` gets no entry.
  4. Each new cursor on the table repeats the full load.
  5. This path hides the corruption.
- **Note:** no test covers this path.

### 5. Tests skip the expression branch and ANALYZE

- **Location:** `sm/sm3/test/test_tree.cc:1131`
- **Defect:** the test injects statistics through `cat_put_stats`
  with a column position only. The expression branch at
  `table.cc:1685` and the real ANALYZE path do not run. The
  commits add no dg test.
- **Scenario:** `analyze.cc` writes an expression position that
  does not match `0 == st.position` in `tree_scattered`. All
  tests pass, and the feature does nothing for expression trees.
- **Note:** no tree test in `dg/test/slt` mentions ANALYZE.
  `CLAUDE.md` requires each layer that can express the scenario,
  and a statement of which layers cannot. WORKNOTE steps 372 and
  373 name neither the dg layer nor the rel layer.
  `BATCHREAD.md` §4 is unchanged.

### 6. Non-tree secondary indexes keep the old cost

- **Location:** `sm/sm3/src/table/table.cc:1320`
- **Defect:** `tbl_read_keys` passes no iterator, so the fix
  covers only tree cursors. A non-tree secondary index pays on
  each batch the cost that the two commits removed.
- **Scenario:**
  1. dg's read-ahead for a non-tree secondary index (`ra_rows`,
     through `sm_tbl_read_keys`) sends ascending row keys with
     `bi == nullptr`.
  2. Each call does NewIterator and Seek.
  3. The call misses the second key and falls to MultiGet.
  4. `if (bi && 1 == done)` cannot record the miss.
  5. Nothing calls `tree_scattered` on this path.
- **Note:** this path costs more than a tree cursor did, because
  it builds the iterator again for each batch.
- **Fix:** put the decision on a kept iterator that this path
  also uses, or in sm1.

### 7. MultiGet sorts keys that are already sorted

- **Location:** `sm/sm1/src/chain.cc:885`
- **Defect:** the hot MultiGet call passes `false` for
  `sorted_input`. This occurs when line 815 forces `ascending` to
  false after the keys were found sorted, and when MultiGet reads
  the sorted tail after a miss.
- **Scenario:** on the scattered path of q3,
  `PrepareMultiGetKeys` in RocksDB runs `std::sort` on keys that
  are in order. The commits optimize this exact path.
- **Fix:** keep a separate `sorted` flag, and pass it as
  `sorted_input`. Current keys compare bytewise under
  `version_comparator`, so the claim is valid.

### 8. The new code sits inside a dead condition

- **Location:** `sm/sm1/src/chain.cc:860`
- **Defect:** `++seeks == 1` is true on the first miss, so the
  loop always breaks and never seeks past a gap.
- **Scenario:** `placed && ++seeks == 1` reduces to
  `if (placed)`, so `int seeks` can go. Three comments are stale:
  - lines 804-806: "seeking only past a gap".
  - line 853: "then a seek".
  - `sm/sm3/src/table/pin.h:64`: "kept to the cursor's end",
    though the iterator can now hold none.

### 9. Two flags hold one outcome

- **Location:** `sm/sm1/src/session.h:68`
- **Defect:** `missed` and `scattered` record one outcome. The
  store-scoped flag serves a case that no production caller has.
- **Scenario:** the only production caller, `tree_fill`, keeps
  one iterator for each cursor on one table, which is one store.
  This leaves the `storeid` comparison at `chain.cc:814`, the
  reset at line 845 and a second field to maintain.
- **Fix:** use one flag, set by `batch_iter_new` or by a miss.
  Only sm1's tests can reach the difference.

### 10. A procedure comment breaks the steps rule

- **Location:** `sm/sm1/src/chain.cc:742`
- **Defect:** step 3 of the procedure comment holds several
  actions, and its continuation lines lost their indent.
- **Scenario:** step 3 checks the keys, picks or refreshes the
  iterator, seeks, steps, stops, marks `bi` missed and releases
  the iterator. `CLAUDE.md` requires one action for each step.
  Lines 744-747 sit at column 3, not under the step text.
- **Fix:** split it into numbered steps.

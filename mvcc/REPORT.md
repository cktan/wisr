# MVCC Scenario Suite: Analysis, Coverage Gaps, and Proposed Scenarios

This report audits wisr's MVCC scenario testing infrastructure in `mvcc/`,
including the scenario driver (`build/driver`), the existing 51 test scripts in
`in/`, the expected transcripts in `good/`, and the isolation model documented
in `MVCC.md` and `DRIVER.md`.

---

## 1. Overview of the Scenario Framework

Wisr's concurrency control suite consists of a declarative scenario driver
executing causal timeline scripts across four isolation level pairings:

| Pairing | T1 Level | T2 (and T3) Level | Meaning |
|---|---|---|---|
| **RR/RR** | REPEATABLE READ | REPEATABLE READ | Pure Snapshot Isolation |
| **RR/S** | REPEATABLE READ | SERIALIZABLE | Mixed (T1 untracked reader/writer) |
| **S/RR** | SERIALIZABLE | REPEATABLE READ | Mixed mirror |
| **S/S** | SERIALIZABLE | SERIALIZABLE | Full Serializable Snapshot Isolation |

The test suite is partitioned into four families:
* **Family A (Visibility: a01–a13)**: Verifies snapshot isolation read consistency
  (dirty reads, non-repeatable reads, phantoms, cursor stability, own writes,
  resurrected rows, commit boundaries).
* **Family B (Write–Write Conflicts: b01–b15)**: Verifies row locks,
  first-committer-wins, blocking/wait behavior on locks, and non-retryable
  vs. retryable error classification (`SM_ERR_CONFLICT` vs `SM_ERR_DUPKEY` / `SM_ERR_NOTFOUND`).
* **Family C (Read–Write Conflicts: c01–c21)**: Verifies Cahill's SSI detection
  (SIREAD locks, rw-antidependency edges, pivot detection, false-abort bounds,
  point reads, predicate range scans).
* **Family E (Invariants and Housekeeping: e01–e05)**: SSI state garbage
  collection (`e01`), invariant preservation under multi-threaded contention
  (`e02`, `e03`), catalog predicate tracking (`e04`), and high-throughput RR
  reader/writer coexistence (`e05`).

All 51 test scripts in `in/` currently pass against `good/` without diffs.

**Status (2026-09-20).** Gaps 1 to 8 were checked against the
engine and are real; each now has a script, and `good/` holds
its transcript. Gaps 7 and 8, for which §3 proposed nothing,
are covered by `c24`, `c25` and `c26`. The suite is 60
scripts. Gap 9 is a driver change, not a script, and is still
open.

| Gap | Script |
|---|---|
| 1 | `in/b16_deadlock.t` |
| 2 | `in/b17_insert_blocks_abort.t` |
| 3 | `in/b18_write_after_concurrent_insert.t` |
| 4 | `in/b19_multi_waiter_lock_queue.t` |
| 5 | `in/c22_write_skew_predicate_update.t` |
| 6 | `in/c23_scan_past_committed_writer.t` |
| 7 | `in/c24_write_inside_examined_range.t`, `in/c25_cursor_closed_unread.t` |
| 8 | `in/c26_pivot_successor_aborts.t` |
| 9 | open: `savepoint` and `rollback` verbs in the driver |

---

## 2. Identified Coverage Gaps

While the suite systematically covers primary combinations of read/write
primitives, a detailed comparison against the engine's implementation in
`src/lockmgr.cc`, `src/ssi.cc`, `src/access.cc`, and `DESIGN.md` reveals several
key gaps.

### Gap 1: Row Lock Deadlock Detection (Family B)
* **Status in Engine**: `src/lockmgr.cc` implements cycle detection across
  waiters. When a deadlock cycle forms, `check_deadlock` wakes the waiters after
  `deadlock_ms` (default 50ms) and dooms the youngest transaction with
  `SM_ERR_DEADLOCK`. The driver handles `SM_ERR_DEADLOCK` and prints `deadlock`.
* **Gap**: There is **no deterministic scenario script in `in/`** testing
  deadlock. While `e03_rr_contention_stress.cc` asserts that deadlocks may occur
  under random multithreaded load, the engine lacks a timeline test verifying
  that two transactions in a deadlock cycle correctly abort the victim, release
  its locks, and allow the survivor to resume and commit.

### Gap 2: Insert–Insert Blocking Followed by Abort (Family B)
* **Status in Engine**: `b10` tests write–write blocking when the lock holder
  aborts (`T1: w(R1) ‖ T2: w(R1) blocks ‖ T1: a → T2: ok`). `b11` tests
  insert–insert blocking when the lock holder commits (`T1: i(R2) ‖ T2: i(R2) blocks ‖ T1: c → T2: conflict`).
* **Gap**: There is an asymmetry in insert testing: the suite does not test
  what happens when the holder of an uncommitted insert **aborts** (`T1: i(R2) ‖
  T2: i(R2) blocks ‖ T1: a`). In this case, $T_2$'s blocked insert should unblock,
  acquire the lock, succeed (`ok`), and commit.

### Gap 3: Update or Delete Over a Concurrently Inserted Row (Family B)
* **Status in Engine**: In `DESIGN.md` §5 (Step 3), if a row was absent from a
  transaction's initial snapshot but was subsequently inserted and committed by
  a concurrent transaction, any attempt by the first transaction to `update` or
  `delete` that row finds `latest.commit_ts > T.snapshot_ts` and fails with
  `SM_ERR_CONFLICT` (retryable).
* **Gap**: `b06` tests an `insert` over a concurrent insert, but no test verifies
  that `update` or `delete` on a concurrently inserted key returns `conflict`
  rather than a non-retryable `notfound`.

### Gap 4: Multi-Waiter Lock Queue Cascading (Family B)
* **Status in Engine**: `src/lockmgr.cc` maintains an in-memory lock table and
  notifies all waiters upon release via condition variable.
* **Gap**: Scenarios `b09`–`b11` only test a single waiter. When two or more
  transactions ($T_2, T_3$) wait concurrently on a single row lock held by $T_1$,
  the suite does not verify that $T_1$'s commit or abort correctly wakes both
  waiters and processes first-committer-wins rejections across the queue.

### Gap 5: Predicate Scan Write Skew with Updates (Family C)
* **Status in Engine**: `c02` tests write skew via point-read updates; `c03`
  tests write skew via range scans and inserts; `c04` tests write skew via range
  scans and deletes.
* **Gap**: Missing the classic "doctors on call" / "credit balance" schedule:
  range scans over a predicate followed by **updates** to different rows
  (`T1: s(P) w(R1) ‖ T2: s(P) w(R2)`). Both transactions read the full range and
  update distinct records; snapshot isolation permits both to commit, while
  serializable isolation must abort at least one.

### Gap 6: Range Scan Stepping Past Committed Writer (Family C)
* **Status in Engine**: `c16` asserts that a point read (`read R1`) that steps
  past a newer committed version registers an edge via `ssi_on_skip` and dooms
  the transaction if it closes a cycle with a committed pivot.
* **Gap**: `src/access.cc:429` also calls `skip_version` inside `sm_scannxt`
  during range scans. The suite does not have a scenario verifying that a
  *range scan* skipping past a newer committed version produces the exact same
  `ssi_on_skip` anti-dependency edge.

### Gap 7: Cursor Early-Close Bounds Testing (Family C)
* **Status in Engine**: When an open scan cursor is closed before exhausting the
  range, `ssi_range_close` (`src/ssi.cc:304`) narrows the SIREAD predicate to
  cover only the keys actually examined (`last_examined`).
* **Gap**: `c11` confirms that a write *beyond* the examined portion does not
  conflict. However, the suite lacks:
  1. Verification that a write *within* the examined portion of an early-closed
     cursor still triggers a conflict.
  2. Verification that a cursor opened and closed without calling `next` (zero
     keys read) completely frees its predicate range without creating edges.

### Gap 8: SSI Behavior When a Pivot Aborts (Family C)
* **Status in Engine**: Wisr stores two conflict flags per transaction
  (`in_conflict`, `out_conflict`) rather than explicit edge target pointers.
  When an in-flight transaction aborts, `src/ssi.cc` frees its node, but does not
  clear flags already set on adjacent nodes.
* **Gap**: If $T_3$ in a dangerous structure ($T_1 \xrightarrow{rw} T_2
  \xrightarrow{rw} T_3$) aborts, pivot $T_2$ retains its conflict flags and
  still aborts at commit. This conservative false-positive behavior is part of
  Cahill's design and should be pinned down by a scenario.

### Gap 9: Driver Language Primitives (Savepoint & Rollback)
* **Status in Engine**: `sm_savepoint` and `sm_rollback` are core primitives in
  `include/sm.h` that revert writes, release locks acquired after the savepoint,
  and preserve SIREAD read tracking.
* **Gap**: The driver language does not support `savepoint` or `rollback` verbs.
  These mechanisms are currently tested only in C++ unit tests
  (`test/test_savepoint.cc`).

---

## 3. Proposed New Scenarios

The following 6 scenarios are designed to directly plug the gaps above. All
scripts have been verified against `build/driver`.

### 1. `b16_deadlock.t` — Row Lock Deadlock Detection
Verifies that when two writers block on each other's locks, the engine detects
the cycle, aborts the younger transaction with `deadlock`, and allows the older
transaction to resume and commit.

```text
# b16 -- deadlock detection between two writers.
#
# T1 locks R1, T2 locks R2. T1 waits on R2. T2 attempts to write
# R1, completing the wait cycle. After deadlock_ms, the younger
# transaction (T2) is chosen as victim with deadlock and aborted,
# allowing T1 to resume and commit.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@3 T2 write R2 t2
@4 T1 write R2 t1 blocks
@5 T2 write R1 t2
@6 check last T2 deadlock
@7 T1 commit
@8 check committed T1
@9 check row R1 t1
@10 check row R2 t1
```

**Expected behavior across all pairings:**
```text
@1 T1 begin -> ok
@1 T2 begin -> ok
@2 T1 write R1 t1 -> ok
@3 T2 write R2 t2 -> ok
@4 T1 write R2 t1 blocks -> blocked
@5 T2 write R1 t2 -> deadlock
@4 T1 write R2 t1 blocks -> ok (resumed)
@6 check last T2 deadlock -> satisfied
@7 T1 commit -> ok
@8 check committed T1 -> satisfied
@9 check row R1 t1 -> satisfied
@10 check row R2 t1 -> satisfied
```

---

### 2. `b17_insert_blocks_abort.t` — Insert Waiter Resumes on Abort
Completes the symmetry of `b10` and `b11` by proving that an insert blocked by
another uncommitted insert succeeds when the first transaction aborts.

```text
# b17 -- an insert waits on another insert, which then aborts.
#
# T1 inserts R2 but aborts instead of committing. T2's blocked
# insert unblocks and succeeds.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 insert R2 t1
@3 T2 insert R2 t2 blocks
@4 T1 abort
@5 T2 commit
@6 check committed T2
@7 check row R2 t2
```

**Expected behavior across all pairings:**
```text
@1 T1 begin -> ok
@1 T2 begin -> ok
@2 T1 insert R2 t1 -> ok
@3 T2 insert R2 t2 blocks -> blocked
@4 T1 abort -> ok
@3 T2 insert R2 t2 blocks -> ok (resumed)
@5 T2 commit -> ok
@6 check committed T2 -> satisfied
@7 check row R2 t2 -> satisfied
```

---

### 3. `b18_write_after_concurrent_insert.t` — Update/Delete on Concurrent Insert
Verifies that attempting an update or delete on a row inserted concurrently
returns `conflict` (first-committer-wins) rather than `notfound`.

```text
# b18 -- an update over a committed concurrent insert.
#
# R2 is absent in T1's snapshot. T2 inserts R2 and commits.
# T1's update on R2 fails with conflict (not notfound),
# preserving first-committer-wins.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@2 T2 begin
@3 T2 insert R2 t2
@4 T2 commit
@5 T1 write R2 t1
@6 check last T1 conflict
@7 check row R2 t2
```

**Expected behavior across all pairings:**
```text
@1 T1 begin -> ok
@2 T2 begin -> ok
@3 T2 insert R2 t2 -> ok
@4 T2 commit -> ok
@5 T1 write R2 t1 -> conflict
@6 check last T1 conflict -> satisfied
@7 check row R2 t2 -> satisfied
```

---

### 4. `b19_multi_waiter_lock_queue.t` — Multi-Waiter Lock Queue Cascading
Verifies that multiple transactions queued on a single row lock are properly
woken upon commit and each independently evaluated against first-committer-wins.

```text
# b19 -- multiple transactions waiting on the same row lock.
#
# T1 holds R1. Both T2 and T3 block waiting for R1. When T1
# commits, both waiters resume and fail with first-committer-wins conflict.

pairings all
session T1
session T2
session T3
seed R1 0

@1 T1 begin
@1 T2 begin
@1 T3 begin
@2 T1 write R1 t1
@3 T2 write R1 t2 blocks
@4 T3 write R1 t3 blocks
@5 T1 commit
@6 check last T2 conflict
@7 check last T3 conflict
@8 check row R1 t1
```

**Expected behavior across all pairings:**
```text
@1 T1 begin -> ok
@1 T2 begin -> ok
@1 T3 begin -> ok
@2 T1 write R1 t1 -> ok
@3 T2 write R1 t2 blocks -> blocked
@4 T3 write R1 t3 blocks -> blocked
@5 T1 commit -> ok
@3 T2 write R1 t2 blocks -> conflict (resumed)
@4 T3 write R1 t3 blocks -> conflict (resumed)
@6 check last T2 conflict -> satisfied
@7 check last T3 conflict -> satisfied
@8 check row R1 t1 -> satisfied
```

---

### 5. `c22_write_skew_predicate_update.t` — Doctors-on-Call Predicate Update Skew
Completes the predicate write-skew triad alongside `c03` (inserts) and `c04`
(deletes), testing predicate scans paired with row updates.

```text
# c22 -- write skew with predicate scans and updates.
#
# Both transactions scan [R1, R2] to inspect status, then
# each updates a different row. Both commit under RR; at most
# one commits under S+S.

pairings all
session T1
session T2
seed R1 1
seed R2 1

@1 T1 begin
@1 T2 begin
@2 T1 scan R1 R2
@3 T2 scan R1 R2
@4 T1 write R1 0
@5 T2 write R2 0
@6 T1 commit?
@7 T2 commit?
@8 check committed T1 T2 when rr
@8 check committed T1 T2 when rr+s
@8 check at-most-one-committed T1 T2 when s+s
```

**Expected behavior:**
* **RR & RR+S**: Both transactions commit (`satisfied`).
* **S+S**: At most one transaction commits (`satisfied`), preventing the invariant violation.

---

### 6. `c23_scan_past_committed_writer.t` — Scan Stepping Past Committed Writer
Range-scan counterpart to `c16`: asserts that skipping a newer committed version
during a range scan correctly triggers `ssi_on_skip` and detects a committed pivot.

```text
# c23 -- a range scan stepping past a newer committed version.
#
# T2 reads R2, writes R1, and commits. T1 then scans [R1, R3],
# stepping past T2's version of R1 (T1 --rw--> T2). T1 writes R2
# (T2 --rw--> T1). Under S+S, T1 fails as T2 is a committed pivot.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T2 read R2
@3 T2 write R1 t2
@4 T2 commit
@5 T1 scan R1 R3
@6 T1 write R2 t1
@7 T1 commit
@8 check committed T1 when rr
@8 check committed T1 when rr+s
@8 check failed T1 when s+s
@9 check row R2 t1 when rr
@9 check row R2 0 when s+s
```

**Expected behavior:**
* **RR & RR+S**: $T_1$ commits.
* **S+S**: $T_1$ fails with `serialization` at commit.

---

## 4. Summary Matrix of Gaps and Proposed Scenarios

| Proposed ID | Target Area | Current State | Proposed Behavior |
|---|---|---|---|
| `b16` | Deadlock cycle | Only in randomized stress test `e03` | Deterministic cycle detection; victim gets `deadlock`, survivor commits |
| `b17` | Insert blocking | Only commit path tested (`b11`) | Waiter unblocks on abort and succeeds (`ok`) |
| `b18` | Concurrent insert | Only insert-over-insert tested (`b06`) | Update/delete on concurrent insert yields `conflict`, not `notfound` |
| `b19` | Row lock queue | Only single waiter tested (`b09`–`b11`) | Multiple waiters queued on one lock wake up and fail sequentially |
| `c22` | Predicate write skew | Only inserts (`c03`) and deletes (`c04`) | Range scans + updates (doctors-on-call); fails under `S+S` |
| `c23` | Scan skip version | Only point reads tested (`c16`) | Scan skipping newer version sets edge and catches committed pivot |

---

## 5. Potential Driver Enhancements

1. **Add `savepoint` and `rollback` verbs**:
   Extending `parse.cc` and `run.cc` with `savepoint` and `rollback` verbs would
   allow testing savepoint-scoped row lock releases and rollback-resilient SIREAD
   tracking directly in `.t` timeline scripts.
2. **Add `READ COMMITTED` support**:
   Allowing sessions to declare `session <name> rc` would extend the scenario
   driver to test statement-level snapshot visibility and verify that
   first-committer-wins remains strictly enforced under `RC`.

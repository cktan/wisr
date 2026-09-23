# MVCC: what wisr detects, and what it does not

This directory is an argument, in two parts. This document
states the rules wisr's concurrency control follows and
enumerates every scenario those rules decide. Beside it, one
file per scenario runs that scenario against the engine, in
each pairing of isolation levels, and asserts the outcome
this document claims.

Nothing here is a proof in the formal-methods sense: these
are finite schedules, driven call by call, not a model
checker exploring every interleaving. What it does give is
coverage that can be read and audited — every pair of
operations on a row, every read-write shape known to produce
an anomaly under snapshot isolation, and, for each one, the
answer at every level pairing.

`SM1_DESIGN.md` §5 to §7 defines the mechanisms; `SIREAD.md`
explains the serializable theory they implement. This
document is about behaviour: given a history, which calls
fail and which commit.

## 1. The model

**Versions and snapshots.** A row is a chain of versions.
A transaction takes a snapshot when it begins (REPEATABLE
READ and SERIALIZABLE) and sees a version if the version's
writer committed at or before that snapshot, or is the
transaction itself. Nothing a transaction does changes what
it sees afterwards, except its own writes.

**Reads never block.** A read takes no lock and waits for
nobody. It cannot fail with a conflict.

**Writes take a row lock.** A write takes the exclusive lock
on its row for the rest of the transaction, so a second
writer of the same row waits, rather than producing a
version nobody asked for. This is what makes a dirty write
impossible. Two writers that wait on each other's rows
would wait forever, so the engine breaks the cycle itself:
the youngest transaction on it fails with
`SM_ERR_DEADLOCK`, and the others go on.

**First-committer-wins.** Once the lock is granted, the
writer compares the row's newest committed version against
its own snapshot. If that version committed after the
snapshot, the write fails with `SM_ERR_CONFLICT`, which is
retryable and aborts the transaction. This applies at
REPEATABLE READ and SERIALIZABLE, to every pair of writes —
update, delete, and insert alike. READ COMMITTED does not
abort the loser: family F.

**SIREAD and rw-edges (SERIALIZABLE only).** A SERIALIZABLE
transaction records what it read: a point read records the
row whether or not one was found, and a scan records the
range. When another transaction writes into something
recorded, that is a read-write conflict, an edge
`Tr --rw--> Tw` from the reader to the writer.

**The dangerous structure.** A cycle in the serialization
graph always contains a transaction with an incoming and an
outgoing rw-edge — the pivot — where the transaction the
outgoing edge points at commits first. wisr tracks the two
flags per transaction and fails a transaction that would
complete such a structure, at commit, with
`SM_ERR_SERIALIZATION`. This is Cahill's rule, as
PostgreSQL implements it: the check is conservative, so a
failure does not prove a cycle existed, but no cycle escapes.

**What each level promises.**

| Level | Promise |
|---|---|
| `SM_XCT_REPEATABLE_READ` | Snapshot isolation: no dirty read, no dirty write, no non-repeatable read, no phantom within the snapshot, no lost update. Write skew and the read-only anomaly are allowed. Never fails with `SM_ERR_SERIALIZATION`. |
| `SM_XCT_SERIALIZABLE` | Everything above, plus: every committed set of transactions is equivalent to some serial order. Pays for it with `SM_ERR_SERIALIZATION` failures at commit. |

## 2. Notation

Transactions are **T1**, **T2**, **T3**. Rows are **R1**
through **R5**, each holding `"0"` unless a scenario says
otherwise. **P** is a predicate: the key range of a scan.

| Operation | Meaning |
|---|---|
| `r(R)` | point read of a row; `-` means no row is visible |
| `s(P)` | scan of a range, read to the end unless said otherwise |
| `w(R)` | update an existing row |
| `i(R)` | insert a row that is not there |
| `d(R)` | delete a row |
| `c` | commit |
| `a` | abort |

| Outcome | Meaning |
|---|---|
| `OK` | the call succeeds; for `c`, the transaction commits |
| `CONFLICT` | `SM_ERR_CONFLICT` at the write: first-committer-wins. Retryable, and it aborts the transaction |
| `UPDATED` | `SM_ERR_UPDATED` at a READ COMMITTED write: the row changed after the transaction began. Not retryable, and the transaction stays active; `readlatest` reads the newest version |
| `SERIALIZATION` | `SM_ERR_SERIALIZATION` at commit: the dangerous structure. Retryable |
| `BLOCK` | the call waits on a row lock until the holder ends |
| `DEADLOCK` | `SM_ERR_DEADLOCK` at the write: the transaction was the youngest on a cycle of waits. Retryable, and it aborts the transaction |
| `DUPKEY` | `SM_ERR_DUPKEY`: the row exists in this transaction's own snapshot. Not retryable |
| `NOTFOUND` | `SM_ERR_NOTFOUND`: the row does not exist in this transaction's own snapshot. Not retryable |

**Pairings.** Every scenario runs at each combination of
levels, because SSI tracks only SERIALIZABLE transactions
and a mixed pair therefore behaves like neither pure case:

| Pairing | T1 | T2 (and T3) |
|---|---|---|
| **RR** | REPEATABLE READ | REPEATABLE READ |
| **RR+S** | REPEATABLE READ | SERIALIZABLE |
| **RR+S** (mirror) | SERIALIZABLE | REPEATABLE READ |
| **S+S** | SERIALIZABLE | SERIALIZABLE |

A scenario's table below gives one column per pairing group;
the two mirrors of RR+S always agree, and each scenario file
runs both.

## 3. How a scenario is written and run

Each scenario is a script in `in/`, written as a timeline.
The driver runs it once per pairing and prints a transcript;
`good/` holds the transcript each script must produce, and
`make test` diffs the two. `DRIVER.md` is the reference for
the command and the language;
`in/b09_dirty_write_blocks_commit.t` reads:

```
@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@3 T2 write R1 t2 blocks
@4 T1 commit
@5 check last T2 conflict
@6 check row R1 t1
```

and produces, for each pairing:

```
@1 T1 begin -> ok
@1 T2 begin -> ok
@2 T1 write R1 t1 -> ok
@3 T2 write R1 t2 blocks -> blocked
@4 T1 commit -> ok
@3 T2 write R1 t2 blocks -> conflict (resumed)
@5 check last T2 conflict -> satisfied
@6 check row R1 t1 -> satisfied
```

A blocked step is printed where it blocked and again where it
resumed, so the transcript reads causally: the line between
the two is the step that released the lock. Blocking is
claimed by the script rather than measured by the driver --
the trailing `blocks` -- so a slow machine cannot turn an
ordinary step into a "blocked" one and change the
transcript.

```
make test                     # everything, scenarios included
make scenarios                # just the scripts, with diffs
make accept                   # take the current transcripts as
                              #   expected -- a deliberate act,
                              #   never run by make test
build/driver mvcc/in/c02_write_skew_point_reads.t
```

Comments in a script are echoed into the transcript, so the
expected output says what it is proving and not only what
happened. Where an outcome may differ from run to run -- at
S+S either side of a write skew may be the one doomed -- the
script writes `commit?`, which records the result without
printing it, and states what must hold with a `check`.

Three scenarios are not timelines and stay in C++, built into
`build/mvcc_tests`: `e01` reaches into the database object to
prove SSI state is released, and `e02` and `e03` run four
threads for fifty rounds each, with retries. Everything else
-- families A, B and C, the blocking scenarios included --
is a script.

## 4. Family A — visibility

What a transaction sees. No conflict is detected in any of
these; the question is only which version each read returns.
The answers are identical at every pairing, because
visibility is a function of the snapshot alone.

| # | History | RR | RR+S | S+S | File |
|---|---|---|---|---|---|
| a01 | `T1: w(R1)` ‖ `T2: r(R1)` | old value | old value | old value | `in/a01_dirty_read_update.t` |
| a02 | `T1: i(R3) d(R2)` ‖ `T2: s(P)` | unchanged | unchanged | unchanged | `in/a02_dirty_read_scan.t` |
| a03 | `T1: w(R1) d(R2) i(R3) a` ‖ `T2: r/w the same rows` | all OK | all OK | all OK | `in/a03_aborted_writes.t` |
| a04 | `T1: r(R1)` ‖ `T2: w(R1) c` ‖ `T1: r(R1)` | same value | same value | same value | `in/a04_nonrepeatable_read_update.t` |
| a05 | `T1: r(R1)` ‖ `T2: d(R1) c` ‖ `T1: r(R1), s(P)` | still there | still there | still there | `in/a05_nonrepeatable_read_delete.t` |
| a06 | `T1: s(P)` ‖ `T2: i(R2) c` ‖ `T1: s(P)` | no phantom | no phantom | no phantom | `in/a06_phantom_insert.t` |
| a07 | `T1: s(P)` ‖ `T2: d(R2) c` ‖ `T1: s(P)` | unchanged | unchanged | unchanged | `in/a07_phantom_delete.t` |
| a08 | `T1: i(R3) w(R1) d(R2)` then `T1: r/s` | own writes | own writes | own writes | `in/a08_own_writes.t` |
| a09 | `T1: open cursor, w(each row), i(R4)` | cursor stable | cursor stable | cursor stable | `in/a09_cursor_stability.t` |
| a10 | `T1: d(R1) i(R1) c` ‖ older `T2: r(R1)` | old value | old value | old value | `in/a10_resurrected_row.t` |
| a11 | `T1: r(R2) -> -` ‖ `T2: i(R2) c` ‖ `T1: r(R2)` | `-` | `-` | `-` | `in/a11_read_of_later_insert.t` |
| a12 | `T1: w(R1) i(R2)` ‖ `T2` before `c`, `T3` after | T2 no, T3 yes | same | same | `in/a12_commit_boundary.t` |
| a13 | `T1: s(all)` ‖ ten committed transactions ‖ `T1: s(all)` | unchanged | unchanged | unchanged | `in/a13_long_reader.t` |
| a14 | `T1: open C, next, seek(R5), next`, own writes in between | skips, and keeps the open view | same | same | `in/a14_cursor_seek_forward.t` |
| a15 | `T1 (RC): r(R1) s(P)` ‖ `W: w(R1) i(R2) c` ‖ `T1: r(R1) s(P)`, and T2 at RR alongside | RC sees both, RR neither | — | — | `in/a15_read_committed_statements.t` |
| a16 | `T1 (RC): stmt r(R1)` ‖ `W: w(R1) c` ‖ `T1: r(R1)` ‖ `T1: stmt r(R1)` | pinned until the next `stmt` | — | — | `in/a16_statement_boundary.t` |
| a17 | `T1: lookup(R1)` ‖ `T2: rmstore c` ‖ `T1: lookup(R1)`, then a fresh T3 | T1 reads on, T3 gets NOTFOUND | same | same | `in/a17_read_of_dropped_relation.t` |

Note what a06 does **not** say: T1 may not go on to *write*
on the strength of that scan and still be serializable. That
is family C.

## 5. Family B — write-write conflicts

Two transactions writing one row. Detection here is
level-independent between REPEATABLE READ and SERIALIZABLE:
the row lock and first-committer-wins run at both, so the RR,
RR+S and S+S columns agree throughout. READ COMMITTED has its
own family, F. This is the part of the design that needs no
SSI.

| # | History | RR | RR+S | S+S | File |
|---|---|---|---|---|---|
| b01 | `T1: r(R1)` ‖ `T2: w(R1) c` ‖ `T1: w(R1)` | CONFLICT | CONFLICT | CONFLICT | `in/b01_lost_update.t` |
| b02 | `T1: (snapshot)` ‖ `T2: w(R1) c` ‖ `T1: w(R1)` | CONFLICT | CONFLICT | CONFLICT | `in/b02_blind_write_write.t` |
| b03 | `T2: d(R1) c` ‖ `T1: w(R1)` | CONFLICT | CONFLICT | CONFLICT | `in/b03_update_after_delete.t` |
| b04 | `T2: w(R1) c` ‖ `T1: d(R1)` | CONFLICT | CONFLICT | CONFLICT | `in/b04_delete_after_update.t` |
| b05 | `T2: d(R1) c` ‖ `T1: d(R1)` | CONFLICT | CONFLICT | CONFLICT | `in/b05_delete_after_delete.t` |
| b06 | `T2: i(R2) c` ‖ `T1: i(R2)` (absent at T1's snapshot) | CONFLICT | CONFLICT | CONFLICT | `in/b06_insert_after_insert.t` |
| b07 | `T2: d(R1) c` ‖ `T1: i(R1)` (present at T1's snapshot) | CONFLICT | CONFLICT | CONFLICT | `in/b07_insert_after_delete.t` |
| b08 | `T1: w(R1) d(R3) i(R4)` ‖ `T2: w(R2) d(R5)` | OK | OK | OK | `in/b08_writes_to_different_rows.t` |
| b09 | `T1: w(R1)` ‖ `T2: w(R1)` ‖ `T1: c` | BLOCK → CONFLICT | same | same | `in/b09_dirty_write_blocks_commit.t` |
| b10 | `T1: w(R1)` ‖ `T2: w(R1)` ‖ `T1: a` | BLOCK → OK | same | same | `in/b10_dirty_write_blocks_abort.t` |
| b11 | `T1: i(R2)` ‖ `T2: i(R2)` ‖ `T1: c` | BLOCK → CONFLICT | same | same | `in/b11_insert_insert_blocks.t` |
| b12 | `T1: i(R1)`, R1 committed long ago | DUPKEY | DUPKEY | DUPKEY | `in/b12_duplicate_key.t` |
| b13 | `T1: w(R2)`, `T1: d(R2)`, R2 never existed | NOTFOUND | NOTFOUND | NOTFOUND | `in/b13_missing_row.t` |
| b14 | `T1: w(R1) w(R2)` ‖ `T2: w(R3) c` ‖ `T1: w(R3)` | CONFLICT, R1/R2 discarded | same | same | `in/b14_conflict_discards_the_loser.t` |
| b15 | `T1: r(R1) c` ‖ `T2: w(R1) c` | OK | OK | OK | `in/b15_write_after_reader_commits.t` |
| b16 | `T1: w(R1) w(R2)` ‖ `T2: w(R2) w(R1)` | DEADLOCK for T2, T1 commits | same | same | `in/b16_deadlock.t` |
| b17 | `T1: i(R2)` ‖ `T2: i(R2)` ‖ `T1: a` | BLOCK → OK | same | same | `in/b17_insert_blocks_abort.t` |
| b18 | `T2: i(R2) i(R3) c` ‖ `T1: w(R2)`, `T3: d(R3)` (both absent at their snapshots) | CONFLICT | CONFLICT | CONFLICT | `in/b18_write_after_concurrent_insert.t` |
| b19 | `T1: w(R1)` ‖ `T2: w(R1)`, `T3: w(R1)` ‖ `T1: c` | BLOCK → CONFLICT, both waiters | same | same | `in/b19_multi_waiter_lock_queue.t` |
| b20 | `T2: w(R2)` ‖ `T2: w(R1)` blocked ‖ `T1: w(R2)` closes the cycle | DEADLOCK for the waiting T2, T1 proceeds | same | same | `in/b20_deadlock_reverse_victim.t` |
| b21 | `T1: w(R1) w(R2)` ‖ `T2: w(R2) w(R3)` ‖ `T3: w(R3) w(R1)` | DEADLOCK for T3, then the queue drains | same | same | `in/b21_deadlock_three_transactions.t` |
| b22 | `T1: d(R1)` ‖ `T2: d(R1)` ‖ `T1: c` | BLOCK → CONFLICT | same | same | `in/b22_delete_blocks_commit.t` |
| b23 | `T1: d(R1)` ‖ `T2: w(R1)` ‖ `T1: a` | BLOCK → OK | same | same | `in/b23_delete_blocks_abort.t` |
| b24 | `T1: i(R2)` ‖ `T2: w(R2)` ‖ `T1: a` | BLOCK → NOTFOUND | same | same | `in/b24_insert_update_blocks_abort.t` |
| b25 | `T1: w(R1) sp w(R2)` ‖ `T2: w(R2)` ‖ `T1: rollback` | BLOCK → OK on R2, R1 still locked | same | same | `in/b25_savepoint_releases_lock.t` |
| b26 | `T1: lock-s(R1)`, `T2: lock-s(R1)` ‖ `T3: w(R1)` | both share, T3 BLOCKs for both | same | same | `in/b26_shared_locks_coexist.t` |
| b27 | `T1: lock-s(R1)` ‖ `T2: w(R1)`; `T3: lock-x(R2)` ‖ `T1: lock-s(R2)` | BLOCK both ways → OK | same | same | `in/b27_lock_blocks_both_ways.t` |
| b28 | `T1: lock-s(R1)`, `T2: lock-s(R1)` ‖ both `w(R1)` | DEADLOCK for T2, T1 upgrades | same | same | `in/b28_lock_upgrade_deadlock.t` |
| b29 | `T1: r(R1)`, `T3: (snapshot)` ‖ `T2: w(R1) c` ‖ `T1: lock-s(R1)`, `T3: lock-x(R1)` | CONFLICT both modes | same | same | `in/b29_lock_of_committed_row.t` |
| b30 | `T1: lock-s(R1)`, `T2: lock-s(R1)` ‖ `T1: w(R1)` ‖ `T2: c` | BLOCK → OK, then exclusive | same | same | `in/b30_upgrade_after_holder_commits.t` |
| b31 | `T1: lock-s(R1) sp lock-s(R2) w(R1)` ‖ `T1: rollback` | R2 freed, R1 still exclusive | same | same | `in/b31_savepoint_and_explicit_locks.t` |
| b32 | `T1: rmstore` ‖ `T2: rmstore` ‖ `T1: c` | BLOCK → CONFLICT | same | same | `in/b32_concurrent_drop_of_one_relation.t` |

Three distinctions this family pins down, because getting
them wrong would be invisible in ordinary use:

- **CONFLICT, not NOTFOUND** (b03, b05, b18): the row
  exists in the loser's snapshot, or was created after it.
  Reporting "no such row" would tell the caller a falsehood
  and lose the retry.
- **CONFLICT, not DUPKEY** (b06 vs b12): the row is absent
  in the loser's snapshot, so the failure is about
  concurrency, and is retryable.
- **Blocked, then judged** (b09, b10, b11, b17, b19, b22,
  b23, b24): waiting is not the answer, only the wait. What
  decides is what the holder did — commit or abort — and
  every waiter in the queue is judged on its own. An aborted
  holder can even change which error the waiter gets: b23's
  waiter finds its row and succeeds, b24's finds the
  uncommitted insert undone and gets NOTFOUND.
- **The victim need not be the transaction that noticed**
  (b16, b20, b21, b28): b16 dooms the transaction whose own
  step closes the cycle, b20 dooms one that is already
  blocked and must be woken to hear it, b21's cycle of three
  is longer than a single waits-for hop, and b28's cycle sits
  on one row, which only a shared lock makes possible.
- **A lock is a lock, whoever takes it** (b26 to b29):
  `sm_lock` shares the row locks writes take, so a shared
  holder stops a writer, a writer stops a shared request, and
  a lock taken over a row committed since the snapshot fails
  with CONFLICT rather than handing back something stale.
- **A wait that cannot end** (b16): two writers holding what
  the other wants would wait forever, so the engine picks
  the youngest and fails it. This is the one write-write
  outcome no amount of patience produces.

## 6. Family C — read-write conflicts

Two transactions where one writes something the other read.
This is the only family where the pairings disagree, and it
is the reason SERIALIZABLE exists.

The **RR** and **RR+S** columns are the same throughout, and
that is not an accident: an rw-edge is recorded only when the
*reader* is SERIALIZABLE, and the structure needs two
adjacent edges. In RR+S one of the two transactions is not
tracked, so at most one of the two edges exists, and one edge
is never a cycle. **A mixed pair is therefore exactly as
permissive as RR.** A caller who wants serializability must
run *every* transaction in the group at SERIALIZABLE;
nothing enforces that, and c02 in its mixed pairings is what
that costs.

| # | History | RR | RR+S | S+S | File |
|---|---|---|---|---|---|
| c01 | `T1: r(R1)` ‖ `T2: w(R1) c` ‖ `T1: w(R2) c` | OK | OK | OK | `in/c01_single_rw_edge.t` |
| c02 | `T1: r(R1) r(R2) w(R1)` ‖ `T2: r(R1) r(R2) w(R2)` | both commit | both commit | ≤ 1 commits | `in/c02_write_skew_point_reads.t` |
| c03 | `T1: s(P) i(R1)` ‖ `T2: s(P) i(R2)` | both commit | both commit | ≤ 1 commits | `in/c03_write_skew_predicate_insert.t` |
| c04 | `T1: s(P) d(R1)` ‖ `T2: s(P) d(R2)` | both commit | both commit | ≤ 1 commits | `in/c04_write_skew_predicate_delete.t` |
| c05 | `T1: r(R2) i(R1)` ‖ `T2: r(R1) i(R2)` | both commit | both commit | ≤ 1 commits | `in/c05_read_absent_then_insert.t` |
| c06 | `T1: r(R1)` ‖ `T2: r(R2) w(R1)` ‖ `T3: w(R2) c` ‖ `T2: c` | all commit | all commit | T2 (pivot) fails | `in/c06_rw_chain_of_three.t` |
| c07 | Fekete: `T2: r(R1) r(R2)` ‖ `T1: w(R1) c` ‖ `T3: r(R1) r(R2) c` ‖ `T2: w(R2) c` | all commit, anomaly visible | all commit | not all commit | `in/c07_read_only_anomaly.t` |
| c08 | `T1: s(R1..R2)` ‖ `T2: i(R4) r(R3)` ‖ `T1: w(R3)` | OK | OK | OK | `in/c08_insert_outside_predicate.t` |
| c09 | `T1: r(R1) w(R2)` ‖ `T2: r(R3) w(R4)` | OK | OK | OK | `in/c09_disjoint_read_and_write.t` |
| c10 | `T1: r(R1) w(R2) c` then `T2: r(R2) w(R1) c` | OK | OK | OK | `in/c10_reader_commits_first.t` |
| c11 | `T1: scan, stop at R1, close` ‖ `T2: i(R4) r(R2)` ‖ `T1: w(R2)` | OK | OK | OK | `in/c11_cursor_closed_early.t` |
| c12 | `T1: r(R1) r(R2)` ‖ `T2: w(R1) c` ‖ `T1: r(R1) c` | OK | OK | OK | `in/c12_read_only_after_writers.t` |
| c13 | c02, asking *where* the failure appears | OK | OK | at `c`, retry then OK | `in/c13_failure_arrives_at_commit.t` |
| c14 | `T1: w(R1)` ‖ `T2: w(R2)`, no reads at all | OK | OK | OK | `in/c14_blind_writes_only.t` |
| c15 | `T1: s(P) w(R1) i(R3)` — its own range | OK | OK | OK | `in/c15_scan_then_write_own_range.t` |
| c16 | `T2: r(R2) w(R1) c` ‖ `T1: r(R1) w(R2) c` | OK | OK | T1 fails | `in/c16_read_past_committed_writer.t` |
| c17 | `T3: r(R1)` ‖ `T2: r(R2) w(R1) c` ‖ `T1: w(R2) c` | OK | OK | T1 fails | `in/c17_edge_onto_committed_pivot.t` |
| c18 | `T1: cursor open over P` ‖ `T2: r(R3) i(R2 in P) c` ‖ `T1: w(R3) c` | OK | OK | T1 fails | `in/c18_phantom_while_cursor_open.t` |
| c19 | `T1: r(R1)` ‖ `T2: w(R2) c` ‖ `T1: w(R1) c` | OK | OK | OK | `in/c19_safe_snapshot.t` |
| c20 | three concurrent readers, then one writes | OK | OK | OK | `in/c20_concurrent_readers.t` |
| c21 | `T1: s(P) w(R5)` ‖ `T2: r(R5) d(R1 in P)` | both commit | both commit | ≤ 1 commits | `in/c21_delete_inside_scanned_range.t` |
| c22 | `T1: s(P) w(R1)` ‖ `T2: s(P) w(R2)`, both rows in P | both commit | both commit | ≤ 1 commits | `in/c22_write_skew_predicate_update.t` |
| c23 | `T2: r(R2) w(R1) c` ‖ `T1: s(P over R1) w(R2) c` | OK | OK | T1 fails | `in/c23_scan_past_committed_writer.t` |
| c24 | `T1: scan, stop at R1, close` ‖ `T2: w(R1) r(R2)` ‖ `T1: w(R2)` | both commit | both commit | ≤ 1 commits | `in/c24_write_inside_examined_range.t` |
| c25 | `T1: cursor over P, closed unread` ‖ `T2: w(R1 in P) r(R2)` ‖ `T1: w(R2)` | OK | OK | OK | `in/c25_cursor_closed_unread.t` |
| c26 | c06 with `T3: a` in place of `T3: c` | all commit | all commit | T2 (pivot) fails anyway | `in/c26_pivot_successor_aborts.t` |
| c27 | c06 with `T1: a` in place of `T1: c` | all commit | all commit | T2 (pivot) fails anyway | `in/c27_pivot_predecessor_aborts.t` |
| c28 | `T1: r(R1,R2) w(R1)` ‖ `T2: r(R2,R3) w(R2)` ‖ `T3: r(R3,R1) w(R3)` | all commit | all commit | at least one fails | `in/c28_write_skew_three_transactions.t` |
| c29 | c02 with T1's read taken before a savepoint it rolls back to | all commit | all commit | at most one commits | `in/c29_rollback_keeps_read_set.t` |
| c30 | `T1: open C, next, seek past R5, next, close` ‖ `T2: w(R5) r(R2)` ‖ `T1: w(R2)` | all commit | all commit | at most one commits | `in/c30_seek_skips_inside_predicate.t` |
| c31 | `T1: r(R1) rmstore` ‖ `T2: lookup(R1) w(R1)` | all commit | all commit | at most one commits | `in/c31_write_skew_across_a_drop.t` |

Half of this family — c01, c08 through c12, c14, c15, c19,
c20, c25 — exists to bound the other half. A level that failed
every transaction would satisfy c02 to c07 and be useless;
these scenarios are the ones that would catch it.

## 7. Family E — invariants under load, and housekeeping

| # | What it asserts | Level | File |
|---|---|---|---|
| e01 | SSI state (nodes, SIREAD entries, ranges) is released once no transaction overlaps it | S | `e01_ssi_node_lifetime.cc` |
| e02 | Four contending SERIALIZABLE threads enforcing "at least one on call" never break the invariant; a checker thread never sees it broken | S+S | `e02_serializable_invariant_stress.cc` |
| e03 | Four contending RR threads see `CONFLICT` / `DEADLOCK` and **never** `SERIALIZATION`; no update is lost | RR | `e03_rr_contention_stress.cc` |
| e04 | `sm_lsstore` and `sm_mkstore` follow the same predicate rules as rows | all | `in/e04_catalog_predicate.t` |
| e05 | Four RR transactions that each read everything and write one row all commit | RR | `in/e05_rr_many_readers_writers.t` |
| e06 | Four READ COMMITTED threads incrementing three hot counters through the `updated` / `readlatest` exchange, with compaction running, lose no increment | RC | `e06_rc_contention_stress.cc` |

e02 and e03 are the pair that states the trade in one line:
the same contention that SERIALIZABLE resolves by failing
and retrying transactions (e02), REPEATABLE READ resolves by
letting them all through (e03, e05) — and c02 to c07 are the
price of that.

## 7a. Family F — READ COMMITTED writes proceed

Every script here pins its sessions with `rc`, since `pairings
all` runs only RR and SERIALIZABLE. A write that meets a row
committed after the transaction began fails with `updated`,
which does not abort; the caller reads the newest version with
`readlatest` and goes on (`SM1_DESIGN.md` §5.4).

| # | Schedule | Outcome | File |
|---|---|---|---|
| f01 | `T1: w(R1)` ‖ `T2: w(R1)` ‖ `T1: c` | BLOCK → UPDATED, readlatest 1, write, both commit | `in/f01_lost_update_avoided.t` |
| f02 | as f01, T2's `WHERE` clause fails on the newest version | UPDATED, readlatest 1, T2 writes nothing | `in/f02_predicate_no_longer_holds.t` |
| f03 | `T1: d(R1) c` while T2 waits to write | UPDATED, readlatest reports absent | `in/f03_row_deleted_meanwhile.t` |
| f04 | T2 writes again without readlatest; `T3: w(R1)` queued | UPDATED again, T2 still active and holding the lock | `in/f04_write_without_the_reread.t` |
| f05 | `T1: d(R1) c` while T2 waits to insert R1 | BLOCK → OK, no readlatest | `in/f05_insert_after_concurrent_delete.t` |
| f06 | the f01 timeline at RR and SERIALIZABLE | BLOCK → CONFLICT, T2 aborted | `in/f06_higher_levels_unchanged.t` |
| f07 | T1 writes R2, T2 commits R1, T1 writes past it | later reader sees the last commit | `in/f07_version_order.t` |
| f08 | `T1`, `T2`, `T3` in turn on R1 | each wakes with UPDATED, reads, writes; R1 ends at the last | `in/f08_chained_contention.t` |
| f09 | `T1: w(R1) c` ‖ `T2: d(R1)` | UPDATED, readlatest 1, delete skipped | `in/f09_delete_meets_concurrent_update.t` |
| f10 | savepoint, failed write, readlatest, rollback | a lock taken after the savepoint drops its mark; one taken before keeps it | `in/f10_savepoint_rollback_around_readlatest.t` |

## 8. What wisr does not detect, by design

At REPEATABLE READ, wisr **does not** detect, and must not
fail:

- **Write skew** — two transactions reading a common set and
  each writing a different part of it (c02, c04).
- **Predicate write skew / phantom skew** — "insert it if
  the range is empty", concurrently (c03, c05).
- **The read-only anomaly** — a transaction that only reads
  observing a state no serial order produces (c07).

At every level, wisr **does not** detect:

- **Read-read** anything. Readers neither block nor conflict
  (c20).
- **Writes to different rows.** Conflict detection is per
  row, never per store or per range (b08, c09).
- **A transaction's own** reads and writes conflicting with
  each other (a08, a09, c15).

It also does not detect anything across **processes**: one
process attaches a home directory (`SM1_DESIGN.md` §3.5), so all
of the above is within-process concurrency.

## 9. What this coverage is, and what it is not

**The enumeration is complete over these axes.** For two
transactions on one row, the write-write matrix is every
ordered pair from {update, delete, insert} where the loser's
operation is legal in its own snapshot — b01 through b07
and b18, plus the blocked variants b09 to b11, b17 and b22
to b24, the queue of waiters b19, the cycles of waiters b16,
b20, b21 and b28, and the two non-conflict answers b12 and
b13. Explicit locking crosses the same matrix in b26 to b29,
and b25 covers the locks a savepoint gives back. For read-write, it is
every way one transaction can read (point read of a present
row, point read of an absent row, scan of a range, open
cursor, cursor closed early, cursor moved on by a seek, a
storeid resolved in the catalog) crossed with every way
another can write into that read (update, insert, delete,
and a store dropped) — c02 to c06, c16 to c18, c21 to
c24, c30, c31 — plus the shapes that must not fail.

**It is not a model check.** Each scenario is one schedule,
chosen because it is the one that distinguishes the levels.
Interleavings other than the ones written here are not
explored, and the twenty blocking scenarios (b09 to b11,
b16, b17, b19 to b32) and two stress scenarios (e02, e03)
are the only places where real concurrency, rather than a
scripted order, is involved. They are also where it belongs:
`test/test_lock.cc` and `test/test_savepoint.cc` used to
play the same timelines out with threads and sleeps, and
keep only what a script cannot say — error codes, argument
handling, engine state, contention under load, and the
index-build protocol, which needs a write to a second
store the language has no verb for.

**Read committed is covered by two scenarios, not a
pairing.** a15 and a16 declare `session <name> rc` and run
once, because the level is what those scenarios are about;
every other script runs at repeatable read and above, where
the four pairings say something. What a15 and a16 pin is the
level's definition — a fresh snapshot per call, and one
snapshot per statement between `stmt` boundaries — not its
interaction with SSI, which §7.4 settles by making
non-serializable transactions untracked.

**It assumes two stores and a single process.** Rows
live in the first; the second exists to be dropped and
looked up, which is all a17 and c31 need of it. Crash
recovery, two-phase commit, garbage collection and the API
surface are covered by `test/`, not here. Where those
interact with visibility — a prepared transaction's writes,
the reclamation a dropped store waits for — the scenarios
live there.

**Every claim in the tables above is executed.** If a table
cell and the engine disagree, `make test` fails. A cell with
no scenario behind it does not exist in this document, and a
scenario with no row in the tables would be an omission worth
fixing.

**The scenarios were checked against a broken engine, not
only a working one.** A suite that asserts nothing passes
just as quietly as one that asserts everything, so three
deliberate breakages were run against it: removing the SIREAD
record a point read leaves fails 14 scenarios; removing the
first-committer-wins check fails 11; breaking snapshot
visibility fails 5 of the 6 it should. Those counts are worth
re-measuring after a change to the engine's concurrency
control -- if a breakage stops failing, a scenario has
stopped asserting.

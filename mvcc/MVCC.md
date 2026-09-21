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

`DESIGN.md` §5 to §7 defines the mechanisms; `SIREAD.md`
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
impossible.

**First-committer-wins.** Once the lock is granted, the
writer compares the row's newest committed version against
its own snapshot. If that version committed after the
snapshot, the write fails with `SM_ERR_CONFLICT`, which is
retryable and aborts the transaction. This applies at every
isolation level, to every pair of writes — update, delete,
and insert alike.

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
| `SERIALIZATION` | `SM_ERR_SERIALIZATION` at commit: the dangerous structure. Retryable |
| `BLOCK` | the call waits on a row lock until the holder ends |
| `DUPKEY` | `SM_ERR_DUPKEY`: the row exists in this transaction's own snapshot. Not retryable |
| `NOTFOUND` | `SM_ERR_NOTFOUND`: the row does not exist in this transaction's own snapshot. Not retryable |

**Pairings.** Every scenario runs at each combination of
levels, because SSI tracks only SERIALIZABLE transactions
and a mixed pair therefore behaves like neither pure case:

| Pairing | T1 | T2 (and T3) |
|---|---|---|
| **RR** | REPEATABLE READ | REPEATABLE READ |
| **RR+V** | REPEATABLE READ | SERIALIZABLE |
| **RR+V** (mirror) | SERIALIZABLE | REPEATABLE READ |
| **V+V** | SERIALIZABLE | SERIALIZABLE |

A scenario's table below gives one column per pairing group;
the two mirrors of RR+V always agree, and each scenario file
runs both.

## 3. Running the scenarios

```
make test                       # the whole suite, mvcc included
build/mvcc_tests                # every scenario
build/mvcc_tests -tc="c0*"      # one family
build/mvcc_tests -ltc           # list them
```

Each file holds exactly one `TEST_CASE`, named for its
scenario id, and opens with the history in the notation
above. A failure prints the pairing it failed in.

## 4. Family A — visibility

What a transaction sees. No conflict is detected in any of
these; the question is only which version each read returns.
The answers are identical at every pairing, because
visibility is a function of the snapshot alone.

| # | History | RR | RR+V | V+V | File |
|---|---|---|---|---|---|
| a01 | `T1: w(R1)` ‖ `T2: r(R1)` | old value | old value | old value | `a01_dirty_read_update.cc` |
| a02 | `T1: i(R3) d(R2)` ‖ `T2: s(P)` | unchanged | unchanged | unchanged | `a02_dirty_read_scan.cc` |
| a03 | `T1: w(R1) d(R2) i(R3) a` ‖ `T2: r/w the same rows` | all OK | all OK | all OK | `a03_aborted_writes.cc` |
| a04 | `T1: r(R1)` ‖ `T2: w(R1) c` ‖ `T1: r(R1)` | same value | same value | same value | `a04_nonrepeatable_read_update.cc` |
| a05 | `T1: r(R1)` ‖ `T2: d(R1) c` ‖ `T1: r(R1), s(P)` | still there | still there | still there | `a05_nonrepeatable_read_delete.cc` |
| a06 | `T1: s(P)` ‖ `T2: i(R2) c` ‖ `T1: s(P)` | no phantom | no phantom | no phantom | `a06_phantom_insert.cc` |
| a07 | `T1: s(P)` ‖ `T2: d(R2) c` ‖ `T1: s(P)` | unchanged | unchanged | unchanged | `a07_phantom_delete.cc` |
| a08 | `T1: i(R3) w(R1) d(R2)` then `T1: r/s` | own writes | own writes | own writes | `a08_own_writes.cc` |
| a09 | `T1: open cursor, w(each row), i(R4)` | cursor stable | cursor stable | cursor stable | `a09_cursor_stability.cc` |
| a10 | `T1: d(R1) i(R1) c` ‖ older `T2: r(R1)` | old value | old value | old value | `a10_resurrected_row.cc` |
| a11 | `T1: r(R2) -> -` ‖ `T2: i(R2) c` ‖ `T1: r(R2)` | `-` | `-` | `-` | `a11_read_of_later_insert.cc` |
| a12 | `T1: w(R1) i(R2)` ‖ `T2` before `c`, `T3` after | T2 no, T3 yes | same | same | `a12_commit_boundary.cc` |
| a13 | `T1: s(all)` ‖ ten committed transactions ‖ `T1: s(all)` | unchanged | unchanged | unchanged | `a13_long_reader.cc` |

Note what a06 does **not** say: T1 may not go on to *write*
on the strength of that scan and still be serializable. That
is family C.

## 5. Family B — write-write conflicts

Two transactions writing one row. Detection here is
level-independent: the row lock and first-committer-wins run
at every level, so the RR, RR+V and V+V columns agree
throughout. This is the part of the design that needs no
SSI.

| # | History | RR | RR+V | V+V | File |
|---|---|---|---|---|---|
| b01 | `T1: r(R1)` ‖ `T2: w(R1) c` ‖ `T1: w(R1)` | CONFLICT | CONFLICT | CONFLICT | `b01_lost_update.cc` |
| b02 | `T1: (snapshot)` ‖ `T2: w(R1) c` ‖ `T1: w(R1)` | CONFLICT | CONFLICT | CONFLICT | `b02_blind_write_write.cc` |
| b03 | `T2: d(R1) c` ‖ `T1: w(R1)` | CONFLICT | CONFLICT | CONFLICT | `b03_update_after_delete.cc` |
| b04 | `T2: w(R1) c` ‖ `T1: d(R1)` | CONFLICT | CONFLICT | CONFLICT | `b04_delete_after_update.cc` |
| b05 | `T2: d(R1) c` ‖ `T1: d(R1)` | CONFLICT | CONFLICT | CONFLICT | `b05_delete_after_delete.cc` |
| b06 | `T2: i(R2) c` ‖ `T1: i(R2)` (absent at T1's snapshot) | CONFLICT | CONFLICT | CONFLICT | `b06_insert_after_insert.cc` |
| b07 | `T2: d(R1) c` ‖ `T1: i(R1)` (present at T1's snapshot) | CONFLICT | CONFLICT | CONFLICT | `b07_insert_after_delete.cc` |
| b08 | `T1: w(R1) d(R3) i(R4)` ‖ `T2: w(R2) d(R5)` | OK | OK | OK | `b08_writes_to_different_rows.cc` |
| b09 | `T1: w(R1)` ‖ `T2: w(R1)` ‖ `T1: c` | BLOCK → CONFLICT | same | same | `b09_dirty_write_blocks_commit.cc` |
| b10 | `T1: w(R1)` ‖ `T2: w(R1)` ‖ `T1: a` | BLOCK → OK | same | same | `b10_dirty_write_blocks_abort.cc` |
| b11 | `T1: i(R2)` ‖ `T2: i(R2)` ‖ `T1: c` | BLOCK → CONFLICT | same | same | `b11_insert_insert_blocks.cc` |
| b12 | `T1: i(R1)`, R1 committed long ago | DUPKEY | DUPKEY | DUPKEY | `b12_duplicate_key.cc` |
| b13 | `T1: w(R2)`, `T1: d(R2)`, R2 never existed | NOTFOUND | NOTFOUND | NOTFOUND | `b13_missing_row.cc` |
| b14 | `T1: w(R1) w(R2)` ‖ `T2: w(R3) c` ‖ `T1: w(R3)` | CONFLICT, R1/R2 discarded | same | same | `b14_conflict_discards_the_loser.cc` |
| b15 | `T1: r(R1) c` ‖ `T2: w(R1) c` | OK | OK | OK | `b15_write_after_reader_commits.cc` |

Three distinctions this family pins down, because getting
them wrong would be invisible in ordinary use:

- **CONFLICT, not NOTFOUND** (b03, b05): the row exists in
  the loser's snapshot. Reporting "no such row" would tell
  the caller a falsehood and lose the retry.
- **CONFLICT, not DUPKEY** (b06 vs b12): the row is absent
  in the loser's snapshot, so the failure is about
  concurrency, and is retryable.
- **Blocked, then judged** (b09, b10, b11): waiting is not
  the answer, only the wait. What decides is what the holder
  did — commit or abort.

## 6. Family C — read-write conflicts

Two transactions where one writes something the other read.
This is the only family where the pairings disagree, and it
is the reason SERIALIZABLE exists.

The **RR** and **RR+V** columns are the same throughout, and
that is not an accident: an rw-edge is recorded only when the
*reader* is SERIALIZABLE, and the structure needs two
adjacent edges. In RR+V one of the two transactions is not
tracked, so at most one of the two edges exists, and one edge
is never a cycle. **A mixed pair is therefore exactly as
permissive as RR.** A caller who wants serializability must
run *every* transaction in the group at SERIALIZABLE;
nothing enforces that, and c02 in its mixed pairings is what
that costs.

| # | History | RR | RR+V | V+V | File |
|---|---|---|---|---|---|
| c01 | `T1: r(R1)` ‖ `T2: w(R1) c` ‖ `T1: w(R2) c` | OK | OK | OK | `c01_single_rw_edge.cc` |
| c02 | `T1: r(R1) r(R2) w(R1)` ‖ `T2: r(R1) r(R2) w(R2)` | both commit | both commit | ≤ 1 commits | `c02_write_skew_point_reads.cc` |
| c03 | `T1: s(P) i(R1)` ‖ `T2: s(P) i(R2)` | both commit | both commit | ≤ 1 commits | `c03_write_skew_predicate_insert.cc` |
| c04 | `T1: s(P) d(R1)` ‖ `T2: s(P) d(R2)` | both commit | both commit | ≤ 1 commits | `c04_write_skew_predicate_delete.cc` |
| c05 | `T1: r(R2) i(R1)` ‖ `T2: r(R1) i(R2)` | both commit | both commit | ≤ 1 commits | `c05_read_absent_then_insert.cc` |
| c06 | `T1: r(R1)` ‖ `T2: r(R2) w(R1)` ‖ `T3: w(R2) c` ‖ `T2: c` | all commit | all commit | T2 (pivot) fails | `c06_rw_chain_of_three.cc` |
| c07 | Fekete: `T2: r(R1) r(R2)` ‖ `T1: w(R1) c` ‖ `T3: r(R1) r(R2) c` ‖ `T2: w(R2) c` | all commit, anomaly visible | all commit | not all commit | `c07_read_only_anomaly.cc` |
| c08 | `T1: s(R1..R2)` ‖ `T2: i(R4) r(R3)` ‖ `T1: w(R3)` | OK | OK | OK | `c08_insert_outside_predicate.cc` |
| c09 | `T1: r(R1) w(R2)` ‖ `T2: r(R3) w(R4)` | OK | OK | OK | `c09_disjoint_read_and_write.cc` |
| c10 | `T1: r(R1) w(R2) c` then `T2: r(R2) w(R1) c` | OK | OK | OK | `c10_reader_commits_first.cc` |
| c11 | `T1: scan, stop at R1, close` ‖ `T2: i(R4) r(R2)` ‖ `T1: w(R2)` | OK | OK | OK | `c11_cursor_closed_early.cc` |
| c12 | `T1: r(R1) r(R2)` ‖ `T2: w(R1) c` ‖ `T1: r(R1) c` | OK | OK | OK | `c12_read_only_after_writers.cc` |
| c13 | c02, asking *where* the failure appears | OK | OK | at `c`, retry then OK | `c13_failure_arrives_at_commit.cc` |
| c14 | `T1: w(R1)` ‖ `T2: w(R2)`, no reads at all | OK | OK | OK | `c14_blind_writes_only.cc` |
| c15 | `T1: s(P) w(R1) i(R3)` — its own range | OK | OK | OK | `c15_scan_then_write_own_range.cc` |
| c16 | `T2: r(R2) w(R1) c` ‖ `T1: r(R1) w(R2) c` | OK | OK | T1 fails | `c16_read_past_committed_writer.cc` |
| c17 | `T3: r(R1)` ‖ `T2: r(R2) w(R1) c` ‖ `T1: w(R2) c` | OK | OK | T1 fails | `c17_edge_onto_committed_pivot.cc` |
| c18 | `T1: cursor open over P` ‖ `T2: r(R3) i(R2 in P) c` ‖ `T1: w(R3) c` | OK | OK | T1 fails | `c18_phantom_while_cursor_open.cc` |
| c19 | `T1: r(R1)` ‖ `T2: w(R2) c` ‖ `T1: w(R1) c` | OK | OK | OK | `c19_safe_snapshot.cc` |
| c20 | three concurrent readers, then one writes | OK | OK | OK | `c20_concurrent_readers.cc` |
| c21 | `T1: s(P) w(R5)` ‖ `T2: r(R5) d(R1 in P)` | both commit | both commit | ≤ 1 commits | `c21_delete_inside_scanned_range.cc` |

Half of this family — c01, c08 through c12, c14, c15, c19,
c20 — exists to bound the other half. A level that failed
every transaction would satisfy c02 to c07 and be useless;
these scenarios are the ones that would catch it.

## 7. Family E — invariants under load, and housekeeping

| # | What it asserts | Level | File |
|---|---|---|---|
| e01 | SSI state (nodes, SIREAD entries, ranges) is released once no transaction overlaps it | V | `e01_ssi_node_lifetime.cc` |
| e02 | Four contending SERIALIZABLE threads enforcing "at least one on call" never break the invariant; a checker thread never sees it broken | V+V | `e02_serializable_invariant_stress.cc` |
| e03 | Four contending RR threads see `CONFLICT` / `DEADLOCK` and **never** `SERIALIZATION`; no update is lost | RR | `e03_rr_contention_stress.cc` |
| e04 | `sm_lsrel` and `sm_mkrel` follow the same predicate rules as rows | all | `e04_catalog_predicate.cc` |
| e05 | Four RR transactions that each read everything and write one row all commit | RR | `e05_rr_many_readers_writers.cc` |

e02 and e03 are the pair that states the trade in one line:
the same contention that SERIALIZABLE resolves by failing
and retrying transactions (e02), REPEATABLE READ resolves by
letting them all through (e03, e05) — and c02 to c07 are the
price of that.

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
  row, never per relation or per range (b08, c09).
- **A transaction's own** reads and writes conflicting with
  each other (a08, a09, c15).

It also does not detect anything across **processes**: one
process attaches a home directory (`DESIGN.md` §3.5), so all
of the above is within-process concurrency.

## 9. What this coverage is, and what it is not

**The enumeration is complete over these axes.** For two
transactions on one row, the write-write matrix is every
ordered pair from {update, delete, insert} where the loser's
operation is legal in its own snapshot — b01 through b07,
plus the blocked variants b09 to b11 and the two
non-conflict answers b12 and b13. For read-write, it is
every way one transaction can read (point read of a present
row, point read of an absent row, scan of a range, open
cursor) crossed with every way another can write into that
read (update, insert, delete) — c02 to c06, c16 to c18, c21
— plus the shapes that must not fail.

**It is not a model check.** Each scenario is one schedule,
chosen because it is the one that distinguishes the levels.
Interleavings other than the ones written here are not
explored, and the three threaded scenarios (b09 to b11) and
two stress scenarios (e02, e03) are the only places where
real concurrency, rather than a scripted order, is involved.

**It assumes a single relation and a single process.**
Crash recovery, two-phase commit, garbage collection and the
API surface are covered by `test/`, not here. Where those
interact with visibility — a prepared transaction's writes,
a dropped relation's rows — the scenarios live there.

**Every claim in the tables above is executed.** If a table
cell and the engine disagree, `make test` fails. A cell with
no test file behind it does not exist in this document, and
a scenario file with no row in the tables would be an
omission worth fixing.

# MVCC scenario suite — verified behaviours and the open gaps

This is what an audit of wisr's scenario testing infrastructure
in `mvcc/` left standing: three behaviours that look wrong at
first reading and are not, and five driver capability gaps,
all of which are now closed.

**Status (2026-09-21).** Gaps 1 to 8 are closed, each with a
script and a transcript in `good/`; `MVCC.md` documents what
every script pins. The defects the audit found in the scripts
themselves — checks that named the wrong pairing group, and
assertions left to the transcript diff — are fixed. The six
scenario gaps section 8 listed are closed too, as seven
scripts, `b20` to `b24` and `c27`, `c28`. Gaps 9 and 11
followed, as three new verbs and six more scripts, `b25` to
`b29` and `c29`; then gaps 12 and 13, as two more verbs, the
`rc` level, and `a14` to `a16` and `c30`; then gap 10, as a
second store reached by two more verbs, `a17` and `c31`.
The suite is 79 scripts and `make test` is green.

All five driver gaps are closed. The driver has
`savepoint`, `rollback`, `lock`, `seek`, `stmt`, `lookup`
and `rmstore`, plus the `rc` level and a second store, and
twelve scripts use them. Gap 10 was taken in a narrower
shape than proposed — two stores rather than nameable
ones — which §4 records along with what that forecloses.

---

## 1. The framework

Scripts are declarative timelines, run once per pairing of
isolation levels:

| Pairing | T1 | T2 (and T3) | What it exercises |
|---|---|---|---|
| **RR/RR** | REPEATABLE READ | REPEATABLE READ | pure snapshot isolation |
| **RR/S** | REPEATABLE READ | SERIALIZABLE | mixed: T1 untracked |
| **S/RR** | SERIALIZABLE | REPEATABLE READ | the mirror of it |
| **S/S** | SERIALIZABLE | SERIALIZABLE | full SSI |

The suite is in four families:

* **A — visibility (`a01`–`a17`).** Dirty reads,
  non-repeatable reads, phantoms, cursor stability, own
  writes, resurrected rows, commit boundaries, cursor
  seeking, read committed's statement boundaries, and a
  dropped store an older snapshot still reads.
* **B — write-write conflicts (`b01`–`b29`).** Row locks,
  first-committer-wins, blocking and the `blocks` protocol,
  deadlock detection, and retryable against non-retryable
  errors (`SM1_ERR_CONFLICT`, `SM1_ERR_DEADLOCK`,
  `SM1_ERR_DUPKEY`, `SM1_ERR_NOTFOUND`).
* **C — read-write conflicts (`c01`–`c31`).** Cahill's SSI:
  SIREAD locks, rw-antidependency edges, pivots, false-abort
  bounds, point reads, predicate ranges, early cursor close.
* **E — invariants and housekeeping (`e01`–`e05`).** SSI
  state cleanup (`e01`), invariants under load (`e02`,
  `e03`), catalog predicates (`e04`), RR reader/writer
  throughput (`e05`).

---

## 2. Three behaviours the audit verified

Each of these reads as a bug in the suite and is not. They
are recorded so the next reader does not re-open them.

1. **Why `at-most-one-committed`, not `exactly-one`, under
   S/S.** `c02`–`c05`, `c21`, `c22` and `c24` assert write
   skew that way. `add_edge()` in `src/ssi.cc` keeps two
   flags per transaction, not edge targets, so in a symmetric
   cycle both sides become pivots and `ssi_precommit()` fails
   whichever reaches commit with both flags set — one or
   both, depending on the interleaving. `SM1_DESIGN.md` §7.3 says
   the same. At most one is the sound assertion.

2. **`c17` leaves T3 open on purpose.** T3 reads R1 to make
   an edge into T2 and never ends. `~run_t()` in
   `driver/run.cc` calls `sm1_closedb` on every session, and
   `sm1_closedb` aborts an active transaction
   (`src/session.cc`), so the script is clean as written. An
   explicit `@8 T3 abort` would only make the lifetime
   visible to a reader.

3. **Fekete's anomaly in the mixed pairings (`c07`).** All
   three transactions commit under RR/S, and that is correct
   rather than a missed detection: `ssi_on_write()` returns
   at once for a writer that is not SERIALIZABLE, so no edge
   forms into T1 and the cycle never closes. Mixed pairings
   degrade to snapshot isolation by design (`SM1_DESIGN.md`
   §7.4).

---

## 3. Gap 9 — CLOSED: `savepoint` and `rollback` in the driver

**In the engine.** `sm1_savepoint` and `sm1_rollback`
(`include/sm1.h`) mark the undo list and unwind to it:
versions written after the mark are deleted, `seq_hwm` is
rewound, cursors opened after the mark are closed, locks
taken after the mark are released, and the SSI read set is
deliberately **not** rolled back (`SM1_DESIGN.md` §6.5).

**The gap.** The driver has no `savepoint` or `rollback`
verb, so no scenario can reach any of it. Two consequences:

* The cross-session half is written as C++ with threads and
  sleeps. `test/test_savepoint.cc`'s lock test spawns a
  thread, sleeps 150ms, and asserts the write has not
  returned — which is what `blocks` says in one word, and
  what `DRIVER.md` §0 gives as the reason the language
  exists.
* The isolation half is untested. `test/test_savepoint.cc`
  loops over the three levels for value restoration only; no
  test anywhere pairs a rollback with an rw-antidependency.
  The claim that a rolled-back read still constrains the
  transaction — conservative, and the reason rollback cannot
  launder a serialization failure — has no coverage at all.

**What it cost.** Two entries in `STEP_VERBS`
(`driver/parse.cc`) and two cases in the step dispatch
(`driver/run.cc`), then the two scripts it predicted:
`b25_savepoint_releases_lock.t` for the locks a rollback
gives back, and `c29_rollback_keeps_read_set.t` for the read
set it keeps.

One correction to the estimate: `rollback` takes an argument
now. Savepoints nest, so `savepoint` prints the depth it made
rather than `ok`, and `rollback <depth>` names the mark. The
`sm1_forget` half of the stack has no verb yet; nothing needed
one, and it is the same shape when something does.

`c29` is the claim that had no coverage anywhere: a read
taken before a savepoint still constrains the transaction
after a rollback past it. Were it otherwise, both sides of
that write skew would commit at S/S.

---

## 4. Gap 10 — CLOSED: a read of a store against a drop of it

**In the engine.** `sm1_rmstore` is transactional, and it does
not reclaim space at commit: a transaction holding an older
snapshot must still find the store and read its rows, and
the CF survives until `gc_watermark` passes the drop
(`include/sm1.h`, `SM1_DESIGN.md` §11 and §8.3). Under
SERIALIZABLE more is claimed: resolving a storeid is a point
read of the catalog key `"R" | be64(storeid)` in `store.0`, taken
whether or not the storeid is found, so a concurrent
`sm1_rmstore` of that storeid is an rw-edge. That one is built —
`catalog_lookup()` in `src/catalog.cc` reads through
`access_read(s, 0, ...)`, which takes the SIREAD — and
`SM1_DESIGN.md` §11 gives the write skew it exists to stop:

```
T1: sm1_read(S, x)             T2: sm1_read(R, k)    # looks up R
T1: sm1_rmstore(R)             T2: sm1_update(S, x)
```

**What is covered.** `test/test_access.cc` takes DDL through
commit and abort in one session. `test/test_lock.cc` pairs a
drop against a *drop* — blocked, then `SM1_ERR_CONFLICT`.
`test/test_gc.cc` has the older snapshot reading a dropped
store's rows, at REPEATABLE READ, incidentally, as a
watermark test.

**The gap.** No test anywhere pairs SERIALIZABLE with
`sm1_rmstore`, so both halves are asserted by the design and
checked by nothing: the catalog SIREAD a lookup takes, and
the write skew it is there to stop. (§11's lookup cache is
the third rule in that paragraph and needs nothing yet: it
is not implemented, and every lookup reads `CF:store.0`.)

The scenario suite cannot reach any of it either. `rmstore` is
not a verb, `mkstore` does not print the storeid it made, and
rows live in the single store the driver creates
(`DRIVER.md` §5), so no script can drop a store, let
alone read one while another drops it. `e04` covers the
neighbouring case — `lsstore` against `mkstore`, the catalog as
a predicate — and there is no point-lookup mirror of it.

**What it cost, and the narrower shape taken.** Naming
stores was the expensive route: a name on `mkstore` and an
optional store argument on `read`, `scan` and every write
verb — an operand on eight verbs, for two scenarios. Instead
every run now creates a **second store** holding the same
seeded rows, reachable only by two verbs:

* `rmstore` — drop it.
* `lookup <row>` — read a row of it, which resolves its
  storeid on the way, and that resolution is the tracked
  catalog read the scenario is really about.

Two scripts, as predicted:

* `a17_read_of_dropped_relation.t` — a snapshot older than
  the drop reads the store and its rows unchanged, while
  a transaction begun after it gets `notfound`. That is the
  only place the suite states §11's rule that a drop is not
  a deletion.
* `c31_write_skew_across_a_drop.t` — §11's write skew.
  Removing T2's `lookup` leaves one edge and both commit, so
  the detection is the catalog SIREAD and nothing else; that
  control was run before the script was accepted.

**The ceiling is real.** There are two stores, never
three, and they cannot be named or made mid-script. A
scenario wanting more needs the grammar change this avoided.
Nothing in `MVCC.md`'s families looks like it will.

**One transcript moved.** `e04` counts stores with
`lsstore`, so its output went from "1 stores" to "2". The
scenario is unchanged and its checks still pass; only the
count differs.

---

## 5. Gap 11 — CLOSED: explicit row locking (`sm1_lock`) in the driver

**In the engine.** `sm1_lock` (`include/sm1.h`, `src/lockmgr.cc`)
takes a row lock in `SM1_LOCK_SHARED` or `SM1_LOCK_EXCLUSIVE`
mode without writing: SQL's `SELECT ... FOR SHARE` and `FOR
UPDATE`. Multiple shared holders coexist; exclusive requests
block. Upgrading from shared to exclusive waits for other shared
holders; two transactions upgrading the same key deadlock. After
the lock is held, `access_check_current` validates that the key
was not committed after the transaction's begin snapshot, failing
with `SM1_ERR_CONFLICT` otherwise.

**What it cost.** A `lock <row> <shared|exclusive>` verb in
`driver/parse.cc` and `driver/run.cc`, then the four
scenarios it listed, one each:

* `b26_shared_locks_coexist.t` — two shared holders, and a
  writer that waits for the last of them.
* `b27_lock_blocks_both_ways.t` — shared stops a writer,
  exclusive stops a shared request, in one timeline where a
  transaction is holder and waiter at once.
* `b28_lock_upgrade_deadlock.t` — a deadlock cycle on ONE
  row, which exclusive-only locking cannot produce.
* `b29_lock_of_committed_row.t` — first-committer-wins under
  the lock, in both modes.

The C++ tests in `test/test_lock.cc` that these mirror keep
their assertions about error codes and argument handling;
what the scripts retire is the thread-and-sleep timeline
around them.

---

## 6. Gap 12 — CLOSED: cursor seeking (`sm1_scan_seek`)

**In the engine.** `sm1_scan_seek` (`include/sm1.h`,
`src/access.cc`) repositions an open scan forward within its
bounds. Under `SM1_XCT_SERIALIZABLE`, keys skipped over still
count as read: the predicate covers start through the furthest
key reached.

**What it cost.** A `seek <cursor> <key>` verb in
`driver/parse.cc` and `driver/run.cc`, and the two scripts it
named:

* `a14_cursor_seek_forward.t` — the cursor skips rows, keeps
  the view it opened with, and refuses to go backwards.
* `c30_seek_skips_inside_predicate.t` — a write into the gap
  a seek jumped over still makes the edge, so the cycle is
  caught. `c11` is the contrast: a write past the last key
  examined is outside the narrowed predicate and let through.

The verb seeks inclusively. An exclusive seek has no scenario
asking for it, so the verb takes no mode; `test/` covers that
argument.

---

## 7. Gap 13 — CLOSED: Read Committed level and statement boundaries

**In the engine.** `SM1_XCT_READ_COMMITTED` takes a fresh
snapshot per statement. `sm1_stmt_begin` (`include/sm1.h`,
`src/xct.cc`) pins that snapshot across multiple calls within one
statement.

**What it cost.** `session <name> rc` and a `stmt` verb, and
two scripts:

* `a15_read_committed_statements.t` — the non-repeatable read
  and the phantom that define the level, with a repeatable
  read session taking the same steps beside it and seeing
  neither.
* `a16_statement_boundary.t` — `stmt` pins the snapshot until
  the next one, and the snapshot is taken by the statement's
  first read rather than by the boundary call.

**No RC pairing.** The pairings exist to say what changes
between repeatable read and serializable; read committed is
never tracked by SSI (§7.4), so a third axis would multiply
every script's run time to say "same" four more times. The
two scripts declare the level instead, as `e05` does, and
run once. That is a deliberate narrowing of what the report
asked for.

---

## 8. Immediate scenario gaps — CLOSED

Six gaps needing no driver change, now seven scripts. Each
was checked against the suite before it was written, and each
transcript was read against what `SM1_DESIGN.md` claims before it
was accepted.

1. **`b20_deadlock_reverse_victim.t`** — asynchronous victim
   dooming. `b16` dooms the transaction whose own step closes
   the cycle; here the younger T2 blocks first and the older
   T1 closes it, so T1's step returns `ok` and the waiting T2
   resumes with `deadlock (resumed)`. It behaves as predicted.

2. **`b21_deadlock_three_transactions.t`** — the cycle
   `T1 -> T2 -> T3 -> T1`, longer than one waits-for hop, so
   `find_cycle()` must walk it. Written first with T2
   committing after the victim died, which turned out to hand
   T1 a first-committer-wins `conflict` — b19's point, not
   this one's — so T2 aborts instead and the queue drains
   cleanly: T3 doomed, T2 through, T1 through.

3. **`b22_delete_blocks_commit.t`, `b23_delete_blocks_abort.t`**
   — the family had no `delete ... blocks` at all. A delete
   takes the same exclusive lock a write does, so the waiter
   blocks and is then judged by what the holder did:
   `conflict` after a commit, `ok` after an abort.

4. **`b24_insert_update_blocks_abort.t`** — an update waiting
   on an uncommitted insert that aborts gets `notfound`, not
   the `conflict` of `b18` where the insert committed. The
   two errors differ in whether the caller should retry, and
   only the holder's outcome decides which arrives.

5. **`c27_pivot_predecessor_aborts.t`** — the mirror of
   `c26`. With `T1 --rw--> T2 --rw--> T3`, an aborting T1
   leaves T2 failing at S/S all the same: the flags record
   that T2 has an incoming edge, not whose it was. Confirmed
   by running it, not by assuming it.

6. **`c28_write_skew_three_transactions.t`** — write skew
   around a cycle of three. Nothing blocks, every level below
   S/S allows it, and at S/S at least one of the three fails.

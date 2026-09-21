# MVCC scenario suite — verified behaviours and the open gaps

This is what an audit of wisr's scenario testing infrastructure
in `mvcc/` left standing: three behaviours that look wrong at
first reading and are not, and five driver capability gaps.

**Status (2026-09-21).** Gaps 1 to 8 are closed, each with a
script and a transcript in `good/`; `MVCC.md` documents what
every script pins. The defects the audit found in the scripts
themselves — checks that named the wrong pairing group, and
assertions left to the transcript diff — are fixed. The six
scenario gaps section 8 listed are closed too, as seven
scripts, `b20` to `b24` and `c27`, `c28`. The suite is 67
scripts and `make test` is green.

Gaps 9 to 13 are driver capability gaps: engine features
implemented in `include/sm.h` and `src/` that the driver cannot
yet express. They are what is left.

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

* **A — visibility (`a01`–`a13`).** Dirty reads,
  non-repeatable reads, phantoms, cursor stability, own
  writes, resurrected rows, commit boundaries.
* **B — write-write conflicts (`b01`–`b24`).** Row locks,
  first-committer-wins, blocking and the `blocks` protocol,
  deadlock detection, and retryable against non-retryable
  errors (`SM_ERR_CONFLICT`, `SM_ERR_DEADLOCK`,
  `SM_ERR_DUPKEY`, `SM_ERR_NOTFOUND`).
* **C — read-write conflicts (`c01`–`c28`).** Cahill's SSI:
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
   both, depending on the interleaving. `DESIGN.md` §7.3 says
   the same. At most one is the sound assertion.

2. **`c17` leaves T3 open on purpose.** T3 reads R1 to make
   an edge into T2 and never ends. `~run_t()` in
   `driver/run.cc` calls `sm_closedb` on every session, and
   `sm_closedb` aborts an active transaction
   (`src/session.cc`), so the script is clean as written. An
   explicit `@8 T3 abort` would only make the lifetime
   visible to a reader.

3. **Fekete's anomaly in the mixed pairings (`c07`).** All
   three transactions commit under RR/S, and that is correct
   rather than a missed detection: `ssi_on_write()` returns
   at once for a writer that is not SERIALIZABLE, so no edge
   forms into T1 and the cycle never closes. Mixed pairings
   degrade to snapshot isolation by design (`DESIGN.md`
   §7.4).

---

## 3. Gap 9, open: `savepoint` and `rollback` in the driver

**In the engine.** `sm_savepoint` and `sm_rollback`
(`include/sm.h`) mark the undo list and unwind to it:
versions written after the mark are deleted, `seq_hwm` is
rewound, cursors opened after the mark are closed, locks
taken after the mark are released, and the SSI read set is
deliberately **not** rolled back (`DESIGN.md` §6.5).

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

**What it costs.** Two entries in `STEP_VERBS`
(`driver/parse.cc`) and two cases in the step dispatch
(`driver/run.cc`), both taking no arguments, then the
scripts: a B-family one for the lock release, and a C-family
one for the retained read set. It is a driver change first,
which is why no script closes it.

---

## 4. Gap 10, open: a read of a relation against a drop of it

**In the engine.** `sm_rmrel` is transactional, and it does
not reclaim space at commit: a transaction holding an older
snapshot must still find the relation and read its rows, and
the CF survives until `gc_watermark` passes the drop
(`include/sm.h`, `DESIGN.md` §11 and §8.3). Under
SERIALIZABLE more is claimed: resolving a relid is a point
read of the catalog key `"R" | be64(relid)` in `rel.0`, taken
whether or not the relid is found, so a concurrent `sm_rmrel`
of that relid is an rw-edge. That one is built —
`catalog_lookup()` in `src/catalog.cc` reads through
`access_read(s, 0, ...)`, which takes the SIREAD — and
`DESIGN.md` §11 gives the write skew it exists to stop:

```
T1: sm_read(S, x)             T2: sm_read(R, k)    # looks up R
T1: sm_rmrel(R)               T2: sm_update(S, x)
```

**What is covered.** `test/test_access.cc` takes DDL through
commit and abort in one session. `test/test_lock.cc` pairs a
drop against a *drop* — blocked, then `SM_ERR_CONFLICT`.
`test/test_gc.cc` has the older snapshot reading a dropped
relation's rows, at REPEATABLE READ, incidentally, as a
watermark test.

**The gap.** No test anywhere pairs SERIALIZABLE with
`sm_rmrel`, so both halves are asserted by the design and
checked by nothing: the catalog SIREAD a lookup takes, and
the write skew it is there to stop. (§11's lookup cache is
the third rule in that paragraph and needs nothing yet: it
is not implemented, and every lookup reads `CF:rel.0`.)

The scenario suite cannot reach any of it either. `rmrel` is
not a verb, `mkrel` does not print the relid it made, and
rows live in the single relation the driver creates
(`DRIVER.md` §5), so no script can drop a relation, let
alone read one while another drops it. `e04` covers the
neighbouring case — `lsrel` against `mkrel`, the catalog as
a predicate — and there is no point-lookup mirror of it.

**What it costs.** More than gap 9. An `rmrel` verb is the
small part; the shape needs a row read in one relation and a
drop of another, so relations must become nameable. That is
a name on `mkrel`, and an optional relation argument on
`read`, `scan` and the write verbs, defaulting to the
implicit relation so that no existing script changes. Then
two scenarios: an A-family one for the older snapshot that
goes on reading a dropped relation, and a C-family one for
the §11 write skew, which must commit under `rr` and `rr+s`
and fail under `s+s`.

---

## 5. Gap 11, open: explicit row locking (`sm_lock`) in the driver

**In the engine.** `sm_lock` (`include/sm.h`, `src/lockmgr.cc`)
takes a row lock in `SM_LOCK_SHARED` or `SM_LOCK_EXCLUSIVE` mode
without writing: SQL's `SELECT ... FOR SHARE` and `FOR UPDATE`.
Multiple shared holders coexist; exclusive requests block.
Upgrading from shared to exclusive waits for other shared holders;
two transactions upgrading the same key deadlock. After the
lock is held, `access_check_current` validates that the key was
not committed after the transaction's begin snapshot, failing
with `SM_ERR_CONFLICT` otherwise.

**The gap.** The driver has no `lock` verb. Concurrency of
explicit locking, shared-mode coexistence, single-row upgrade
deadlocks, and conflict detection on non-writing locks have
no scenario coverage.

**What it costs.** A `lock <row> <shared|exclusive>` verb in
`driver/parse.cc` and `driver/run.cc`. Then scenarios for:
* Shared lock coexistence (multiple transactions lock shared without blocking).
* Shared blocking exclusive, and exclusive blocking shared.
* Single-row upgrade deadlock (two shared holders write or upgrade R1).
* Lock conflict on concurrently committed rows.

---

## 6. Gap 12, open: cursor seeking (`sm_scan_seek`) and SSI gaps

**In the engine.** `sm_scan_seek` (`include/sm.h`, `src/access.cc`)
repositions an open scan forward within its bounds. Under
`SM_XCT_SERIALIZABLE`, keys skipped over still count as read:
the predicate covers start through the furthest key reached.

**The gap.** The driver has no `seek` verb. The over-approximation
rule (that keys skipped during a seek trigger serialization
conflicts if concurrently modified) is untested in scenarios.

**What it costs.** A `seek <cursor> <key>` verb in `driver/parse.cc`
and `driver/run.cc`, plus:
* An A-family test for forward positioning without seeing new writes.
* A C-family test verifying that concurrent writes into skipped keys
  still generate rw-edges and trigger serialization failure at commit.

---

## 7. Gap 13, open: Read Committed level and statement boundaries

**In the engine.** `SM_XCT_READ_COMMITTED` takes a fresh snapshot
per statement. `sm_stmt_begin` (`include/sm.h`, `src/xct.cc`)
pins that snapshot across multiple calls within one statement.

**The gap.** The driver only parses `rr` and `ser`. The ANSI SQL
anomalies that define Read Committed (non-repeatable reads and
phantoms permitted between statements) and statement boundary
stability across calls have no declarative scenario scripts.

**What it costs.** Allowing `session <name> rc`, an optional RC
pairing, and a `stmt_begin` verb.

---

## 8. Immediate scenario gaps — CLOSED

Six gaps needing no driver change, now seven scripts. Each
was checked against the suite before it was written, and each
transcript was read against what `DESIGN.md` claims before it
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

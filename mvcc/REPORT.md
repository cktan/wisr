# MVCC scenario suite — verified behaviours and the open gaps

This is what an audit of wisr's scenario testing infrastructure
in `mvcc/` left standing: three behaviours that look wrong at
first reading and are not, and two gaps that are still open.

**Status (2026-09-21).** Gaps 1 to 8 are closed, each with a
script and a transcript in `good/`; `MVCC.md` documents what
every script pins. The defects the audit found in the scripts
themselves — checks that named the wrong pairing group, and
assertions left to the transcript diff — are fixed. The suite
is 60 scripts and `make test` is green. Gaps 9 and 10 both
need a driver change before any script can reach them, and
both are untouched.

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
* **B — write-write conflicts (`b01`–`b19`).** Row locks,
  first-committer-wins, blocking and the `blocks` protocol,
  deadlock detection, and retryable against non-retryable
  errors (`SM_ERR_CONFLICT`, `SM_ERR_DEADLOCK`,
  `SM_ERR_DUPKEY`, `SM_ERR_NOTFOUND`).
* **C — read-write conflicts (`c01`–`c26`).** Cahill's SSI:
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

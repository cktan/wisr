# MVCC scenario suite — behaviours the audit verified

This is what an audit of wisr's scenario testing infrastructure
in `mvcc/` left standing: three behaviours that look wrong at
first reading and are not. Every gap the audit found in the
driver and the scenarios is closed; `MVCC.md` documents what
every script pins, and `DRIVER.md` the verbs they use.

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
  errors (`SM_ERR_CONFLICT`, `SM_ERR_DEADLOCK`,
  `SM_ERR_DUPKEY`, `SM_ERR_NOTFOUND`).
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
   `driver/run.cc` calls `sm1::closedb` on every session, and
   `sm1::closedb` aborts an active transaction
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

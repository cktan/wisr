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

The suite is in six families:

* **A — visibility (`a01`–`a17`).** Dirty reads,
  non-repeatable reads, phantoms, cursor stability, own
  writes, resurrected rows, commit boundaries, cursor
  seeking, read committed's statement boundaries, and a
  dropped store an older snapshot still reads.
* **B — write-write conflicts (`b01`–`b36`).** Row locks,
  first-committer-wins, blocking and the `blocks` protocol,
  deadlock detection, explicit locking and the locks a
  savepoint gives back, and retryable against non-retryable
  errors (`SM_ERR_CONFLICT`, `SM_ERR_DEADLOCK`,
  `SM_ERR_DUPKEY`, `SM_ERR_NOTFOUND`).
* **C — read-write conflicts (`c01`–`c34`).** SSI: SIREAD
  locks, rw-antidependency edges, pivots, false-abort
  bounds, point reads, predicate ranges, early cursor close,
  and the `READ ONLY` declaration (`c32`–`c34`).
* **E — invariants and housekeeping (`e01`–`e06`).** SSI
  state cleanup (`e01`), invariants under load (`e02`,
  `e03`), catalog predicates (`e04`), RR reader/writer
  throughput (`e05`), RC contention (`e06`).
* **F — read committed (`f01`–`f10`).** The `updated` /
  `readlatest` exchange: a write that meets a row committed
  since the transaction began is refused without an abort,
  and the caller re-reads and goes on.
* **P — PostgreSQL's isolation tests (`p01`–`p10`).** Ten of
  its specs, run as permutation scripts against the outcome
  its expected output gives.

These counts are as of the audit, 109 scripts (`AUDIT.md`
dates it). The suite has 114 now, in seven families: C runs
to `c36`, F to `f11`, and family G (`g01`, `g02`) holds a
key's versions (`MVCC.md` §7c).

---

## 2. Three behaviours the audit verified

Each of these reads as a bug in the suite and is not. They
are recorded so the next reader does not re-open them.

1. **Why `at-most-one-committed`, not `exactly-one`, under
   S/S.** `c02`–`c05`, `c13`, `c21`, `c22`, `c24`,
   `c29`–`c31` and `e04` assert write skew that way. `ssi_precommit()` in `src/ssi.cc` now
   fails only the second to commit of a plain write skew
   (`SM1_DESIGN.md` §7.3), but serializability asks only that
   one not commit; a more conservative rule would be as
   sound. At most one is the assertion of the property, not
   of the rule.

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

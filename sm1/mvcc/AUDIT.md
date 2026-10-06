# The expected transcripts, audited against the rules

`REPORT.md` audits the scenario *infrastructure* — the driver
and the shape of the scripts. This document audits the
*answers*: for every script in `in/`, whether the transcript
in `good/` is the outcome the isolation rules require, at each
level pairing it runs in.

The question is not whether the engine reproduces `good/`;
`make test` asks that. It is the prior question, the one a
diff cannot ask — whether the recorded behaviour is the
correct behaviour. `DRIVER.md` §0 names the hazard this
addresses: a wrong change to the engine plus a regenerated
transcript looks like progress.

**Audited at** commit `2ae8668`, which introduced Ports and
Grittner's commit-order and read-only refinements, on
2026-10-06.

**Method.** Every expected outcome was re-derived from
`MVCC.md` §1 and `SM1_DESIGN.md` §7.3 by hand — the edge set
of each history, then the abort rule applied to it — and
compared with the transcript. The derivation was done on
paper, deliberately independent of what the engine does:
§1 to §7 would read the same against an engine that was
wrong. §8 records what the engine in fact does, measured
afterwards.

**Verdict: no incorrect expected output was found.** All 106
scripts and their transcripts are correct per the documented
rules. Two observations that are not defects are in §7.

---

## 1. What was covered

| Family | Scripts | What the audit re-derived |
|---|---|---|
| A — visibility | 17 | the version each read must return |
| B — write-write | 36 | lock, first-committer-wins, deadlock victim, error choice |
| C — read-write | 31 | the edge set, then §7.3's abort rule |
| E — housekeeping | 2 | catalog predicates, RR under many readers |
| F — read committed | 10 | the `updated` / `readlatest` exchange |
| P — PostgreSQL ports | 10 files, 390 permutations | each permutation's outcome against PostgreSQL's |

`e01`, `e02`, `e03` and `e06` are C++ rather than scripts and
assert invariants rather than transcripts; they are outside
this audit.

## 2. Family A — visibility

Correct throughout, and correctly identical at all four
pairings: visibility is a function of the snapshot alone, so a
pairing cannot change it.

Three worth naming. `a10`'s resurrected row resolves to the
old value through a tombstone *and* a newer version, and
appears exactly once in a scan. `a14`'s cursor keeps its
open-time view even of the session's own later writes — the
same rule as `a09`, which is what makes it consistent rather
than surprising — and a backward `seek` answers `invalid`
without killing the cursor, which the following `next` proves.
`a15` and `a16` pin READ COMMITTED's definition: a fresh
snapshot per call, and one snapshot per statement between
`stmt` boundaries.

## 3. Family B — write-write

Correct throughout. The three error-choice distinctions
`MVCC.md` §5 claims all hold in the transcripts:

- **CONFLICT, not NOTFOUND** (`b03`, `b05`, `b18`) — the row
  exists in the loser's snapshot, or was created after it.
- **CONFLICT, not DUPKEY** (`b06`, `b11` against `b12`) — the
  row is absent in the loser's snapshot, so the failure is
  about concurrency and is retryable.
- **NOTFOUND only when the holder's insert was undone**
  (`b24`), against OK when a delete was undone (`b23`). The
  same wait, two different answers, decided by what the
  holder did.

All four deadlock shapes name the right victim: `b16` dooms
the transaction whose own step closes the cycle, `b20` one
already blocked and woken to hear it, `b21` the youngest on a
three-hop cycle, `b28` one on a cycle that sits on a single
row, which only a shared lock makes possible. `b26` correctly
makes the writer wait for *both* shared holders — it resumes
after the second commit, not the first. `b35`'s nine blocks
group by T2's level alone, as its comment claims.

## 4. Family C — the SSI core

Correct throughout, including the three scenarios the
refinements changed. Each was re-derived from the edge set:

**`c17` — all commit, at every pairing.** The structure is
`T3 ──rw──▶ T2 ──rw──▶ T1`, pivot T2. The successor T1 commits
*last*, after the pivot, so T3, T2, T1 is a serial order and
nothing is dangerous. The previous expectation, T1 fails, was
Cahill's over-approximation.

No symmetric dooming rule is missing here, which is the
natural worry. A committed pivot can gain an out-edge only
from a writer performing the write that makes it, and such a
writer is by definition still running — so that successor
always commits after the pivot, and the structure can never
become dangerous after the fact.

**`c26` and `c27` — all commit.** An abort takes its edges
with it (§7.1), leaving T1, T2 and T2, T3 respectively as
serial orders. Under Cahill's two flags both failed T2;
these two now pin that the refinement withdraws an edge.

**`c07` — still correctly failed at S/S.** The read-only
refinement does *not* rescue Fekete's anomaly, and should not:
the test asks whether T3 committed before the read-only
transaction's *snapshot*, and here T1 committed after it. That
single condition is what separates a real anomaly from a safe
structure, and it is also the discriminator between `c17`
(safe) and `c16` / `c23` (a genuine cycle of two, caught by
the `T1 is T3` clause).

The bounding half of the family — `c01`, `c08` to `c12`,
`c14`, `c15`, `c19`, `c20`, `c25` — all commit, as they must
for the other half to mean anything. The cursor-predicate
cases separate cleanly: `c11` lets through a write past the
last key examined, `c24` catches one inside it, `c25` records
nothing for a cursor closed unread, and `c30` keeps the gap a
`seek` jumped over.

## 5. Families E and F

`e04` holds the catalog to the same predicate rules as rows,
and its failure is correctly a serialization failure rather
than a conflict: two `mkstore` calls make distinct storeids,
so no row conflict exists to find.

Family F is correct throughout. `f04` is the one worth
naming: a second write without a re-read answers `updated`
again, and the transcript *proves* the transaction kept its
lock rather than asserting it — T3 stays blocked until T2
aborts. `f10`'s asymmetry is right too: the mark taken before
the savepoint survives the rollback and the one taken after
does not, which is why `@9` is refused and `@10` is not.

## 6. Family P — the PostgreSQL ports

Every permutation was cross-checked mechanically: the
permutation orders align between script and transcript, each
`as PostgreSQL` outcome equals its script's `expect` line,
each `differs` outcome equals its `sm1` override and its
parenthetical equals `expect`, and no override is stale. The
totals match `MVCC.md` §7b exactly — 390 permutations, 359 as
PostgreSQL, 31 differing, 0 failures.

The 31 differences divide into two kinds, and only one of them
needs an argument.

**sm1 fails more (p02's 24, p10's 1).** Safe by construction:
a conservative rule costs a retry, never correctness. p02's
cause is that PostgreSQL knows s3 is `READ ONLY` from its
`BEGIN` while sm1 learns it only at commit; permutations 22
and 23 were traced step by step, and they differ only in
whether the edge completes before or after s1 commits, which
is exactly what decides whether s1 or s3 is the one refused.
p10's cause is granularity: sm1's SIREAD entries are on keys,
so the extra `s1 ──▶ s3` edge makes `s1 → s3 → s4 → s1` a
real cycle in sm1's graph, and failing s3 follows.

**sm1 commits more (p06's 6).** This is the direction that
could hide an anomaly, so it carries the audit's closest
check. All six share the commit order `c1 c3 c2` with every
operation preceding every commit, so all three transactions
are mutually concurrent and the edges are identical in each:
`s3 ──▶ s2 ──▶ s1`, pivot s2, successor s1 committing first.
The read-only test spares s2 because s1 committed *after*
s3's snapshot. That is sound: the serial order **s3, s2, s1**
reproduces every value read, ending D1=2 and D2=2, so the
history is serializable and PostgreSQL is merely conservative
in failing s2. A scan of all 390 permutations confirmed these
six are the only places sm1 commits a transaction PostgreSQL
fails.

Applying the read-only test to any transaction that committed
having written nothing — rather than only to a declared
`READ ONLY` one — is sound for the same reason: by the time
the test is applied the transaction has demonstrably written
nothing, so the property holds in retrospect. sm1 defers the
decision and therefore has more information than PostgreSQL
does, which is why it can commit where PostgreSQL cannot.

`p03` is at REPEATABLE READ, where the read-only anomaly is
permitted, and all three commit with the anomaly visible —
the same shape as `c07` and the opposite outcome, correctly.
`p04` maps the two error classes as PostgreSQL does: a
concurrent duplicate insert is 40001, a non-concurrent one a
unique violation, matching `b06` and `b12`.

## 7. Two observations, neither a defect

1. **`c03`, `c04` and `c22` assert final row state only at
   RR/RR.** Their `@9 check rows when rr` is scoped to that
   one pairing, so the mixed pairings assert that both
   transactions commit but never what the rows ended up
   holding. The difference between the RR/RR and RR/S blocks
   in those three transcripts is precisely that one line, and
   nothing behavioural. Widening the scope to `rr+s` would
   close a small gap at no cost; it changes expected output,
   so it is recorded here rather than done.

2. **`REPORT.md`'s family ranges are stale.** It names family
   B as `b01`–`b29` and family E as `e01`–`e05`; they are now
   `b01`–`b36` and `e01`–`e06`. The three behaviours that
   document records are all still accurate, `c17`'s open T3
   included.

## 8. The engine, and whether the suite still asserts

Two measurements were taken after the audit above, both at
`2ae8668`.

**`make -j4 test` passes.** All 106 scripts match `good/`,
the eleven transcripts the refinement regenerated included;
163 unit cases and 21,041 assertions pass, `test_ssi.cc`
among them, as do the four stress scenarios and the
examples. So the engine does produce these transcripts, and
the audit's verdict and the engine's behaviour agree.

**The suite did not lose assertive power.** `MVCC.md` §9's
three deliberate breakages were re-run, since the refinement
made three scenarios stop failing (`c17`, `c26`, `c27`) and
that is exactly the shape of a suite going quiet. None of the
three breakages escapes: they are caught by 17, 18 and 17
scenarios. Breakage A is the one whose old count is
recoverable, and it is unchanged — family C still catches it
14 times, the three extra being P scripts added since. The
counts for B and C are not comparable to the old 11 and
"5 of the 6", because those were recorded without saying what
the edit was; §9 now names all three edits so the numbers can
be reproduced.

## 9. What this audit does not establish

It does not re-derive the C++ scenarios (`e01` to `e03`,
`e06`), and it inherits `MVCC.md` §9's limits: these are
finite schedules, not a model check, so an interleaving not
written here is not covered by either document.

Where this audit and the engine disagree, neither is
automatically right: the disagreement is the finding, and
`MVCC.md`'s rules are what settle it.

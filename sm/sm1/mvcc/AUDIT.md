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
2026-10-06; brought forward to `44bd134`, which added the
`READ ONLY` declaration, on 2026-10-07. §6 and §8 carry the
later figures, and §4a the three scenarios that arrived with
it.

The counts below are as of those dates, 109 scripts. There are
122 now: `a18`, `a19`, `c35`, `c36`, `f11`, `g01`, `g02` and
`h01` to `h06` arrived since, and this audit has not
re-derived them.

**Method.** Every expected outcome was re-derived from
`MVCC.md` §1 and `../XCTMGR.md` §7.4 by hand — the edge set
of each history, then the abort rule applied to it — and
compared with the transcript. The derivation was done on
paper, deliberately independent of what the engine does:
§1 to §7 would read the same against an engine that was
wrong. §8 records what the engine in fact does, measured
afterwards.

**Verdict: no incorrect expected output was found.** All 109
scripts and their transcripts give the outcome the rules
require. One defect in the *driver* turned up on the way, in
`c34`'s transcript; it is §7's first item, and is fixed.

---

## 1. What was covered

| Family | Scripts | What the audit re-derived |
|---|---|---|
| A — visibility | 17 | the version each read must return |
| B — write-write | 36 | lock, first-committer-wins, deadlock victim, error choice |
| C — read-write | 34 | the edge set, then §7.3's abort rule |
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

## 4a. The read-only declaration — c32, c33, c34

These three arrived with `44bd134` and were audited the same
way. All correct.

**`c33` is the one that earns the declaration.** `T3 ──rw──▶
T2 ──rw──▶ T1`, pivot T2, successor T1 committing first — the
shape that fails T2 everywhere else. T1 commits *after* T3's
snapshot, and T3 is declared read-only, so the read-only test
spares the pivot and all three commit. The serial order
T3, T2, T1 confirms it: T3 reads R2 before T2 writes it, T2
reads R1 before T1 writes it, and T3 and T1 touch different
rows. Undeclared, T3 might still write, so T2 would have to
fail — which is exactly p02's old behaviour, making `c33` and
p02 the two ends of one change.

**`c32` is the control that keeps it honest.** It is `c07`
with T3 declared, and T2 still fails at S+S, because T3's
snapshot is taken *after* T1's commit: the test's condition is
that T3 committed before the read-only transaction's
snapshot, and here it did. A declaration that excused this
would re-admit Fekete's anomaly, so `c32` is what stops the
refinement from being over-applied. Its RR and mixed pairings
commit, as they must.

**`c34` pins soundness at the source.** A declared
transaction's write is refused, at every level, and the
refusal does not abort it — T1 reads on and commits, and the
row is unchanged. This is what makes the other two sound: the
read-only test is only valid for a transaction that cannot
write. The behaviour is right; the one thing wrong is how the
refusal *prints*, which is §7's first item.

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
totals match `MVCC.md` §7b exactly — 390 permutations, 383 as
PostgreSQL, 7 differing, 0 failures.

The 7 differences divide into two kinds, and only one of them
needs an argument.

**sm1 fails more (p10's 1).** Safe by construction: a
conservative rule costs a retry, never correctness. p10's
cause is granularity: sm1's SIREAD entries are on keys,
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

Applying the read-only test to an *undeclared* transaction —
one merely observed to have committed having written nothing
— is sound for the same reason: by the time the test is
applied it has demonstrably written nothing, so the property
holds in retrospect. That is still how p06's s3 qualifies,
and why those six remain the one place sm1 commits more.

p02's 24 differences were of this family until `44bd134`,
which gave `sm1::begin` a `READ ONLY` declaration and the
driver a `ro` session modifier. p02's s3 is declared in
PostgreSQL's setup and is now declared here too, so the test
applies from its begin rather than from its commit and all
210 permutations end as PostgreSQL's do. The audit's earlier
reading of those 24 — that sm1 learned too late, and which of
s1 or s3 was refused turned on whether the edge completed
before s1's commit — was the diagnosis; the declaration is
the fix, and `c33` is the scenario that pins it.

`p03` is at REPEATABLE READ, where the read-only anomaly is
permitted, and all three commit with the anomaly visible —
the same shape as `c07` and the opposite outcome, correctly.
`p04` maps the two error classes as PostgreSQL does: a
concurrent duplicate insert is 40001, a non-concurrent one a
unique violation, matching `b06` and `b12`.

## 7. What the audit found

1. **A defect, since fixed: `c34`'s transcript printed an
   engine message, not an error name.** The step read
   `@2 T1 write R1 t1 -> error:the transaction was declared
   read-only`. `DRIVER.md` §2.2 says error names are
   "symbolic, never numbers or messages"; §4 says the driver
   "prints nothing the engine generates". The cause was a gap
   in `error_name()` in `mvcc/driver/run.cc`, which had a
   case for each of the nine names and none for the new
   `SM_ERR_READONLY`, so `c34` fell to the `default:` branch
   and emitted `"error:" + sm1::errmsg()`. The behaviour
   under test was right; only its rendering was wrong — but
   it left `c34`'s transcript hostage to the wording of a
   message, the coupling §4 exists to prevent.

   Fixed by naming the error, with `readonly` added to
   §2.2's list and `c34`'s transcript regenerated — the one
   scenario the change touches.

   `error_class()` in the same file was the more interesting
   half. It sorts a failure into PostgreSQL's outcome classes
   for the permutation scripts, and it caught a read-only
   failure only by the `error:` prefix, which a symbolic name
   no longer matches. Naming the error without telling that
   function would have made it return `""` — which
   `execute_step` reads as *success*. No permutation reaches
   it today, p02's declared s3 never writing, but one that
   did would have recorded a committed transaction where sm1
   refused the write, and no test would have said so.

   The two functions kept parallel lists of the same ten
   names, which is what allowed a name to exist without a
   class. They now share one table of
   `{code, name, class}`, so an error cannot be given a name
   without also being given a class. The refactor leaves all
   109 transcripts byte-identical.

2. **Fixed: `c03`, `c04` and `c22` asserted final row state
   only at RR/RR.** Their `@9 check rows when rr` was scoped
   to that one pairing, so the mixed pairings established
   that both transactions commit but never what the rows
   ended up holding — the sole difference between their
   RR/RR and RR/S blocks, and nothing behavioural. Each now
   carries a second `@9 check rows when rr+s`, the idiom
   those scripts already use at `@8`, since the language has
   no group for "every pairing but S+S". The three
   transcripts gain that one line per mixed pairing and
   nothing else, and it records the same rows RR/RR gives,
   which is the point: all four pairings but S+S end in the
   same state, and now all three say so.

3. **Fixed: `REPORT.md` had gone stale in its lists, though
   not in its findings.** It named family B as `b01`–`b29`,
   C as `c01`–`c31` and E as `e01`–`e05`, now `b01`–`b36`,
   `c01`–`c34` and `e01`–`e06`; it opened "the suite is in
   four families" when F and P had since made six, and
   neither had a bullet; its §2 listed seven scenarios
   asserting write skew at S/S where twelve do. Its C bullet
   also still called the mechanism "Cahill's SSI", which
   `2ae8668` made only half true. All corrected.

   Its three verified behaviours needed no change, and each
   was re-checked rather than assumed: the
   `at-most-one-committed` reasoning still holds,
   `ssi_precommit`, `ssi_on_write` and the `closedb` abort
   all still exist where it says, and `c17` still leaves T3
   open, `2ae8668` having rewritten the script but not that.

## 8. The engine, and whether the suite still asserts

Two measurements were taken after the audit above, re-run at
`44bd134`.

**`make -j4 test` passes.** All 109 scripts match `good/`,
the transcripts `2ae8668` and `44bd134` regenerated included;
169 unit cases pass, `test_ssi.cc` among them, as do the four
stress scenarios and the examples. So the engine does produce
these transcripts, and the audit's verdict and the engine's
behaviour agree.

The case count is the figure to compare; the assertion total
is not. Two runs of identical sources gave 21,266 and 21,262,
so some test's assertion count depends on contention or
timing. A number recorded here would read as a regression on
the next run that disagreed with it.

**The suite did not lose assertive power.** `MVCC.md` §9's
three deliberate breakages were re-run, because `2ae8668`
made three scenarios stop failing (`c17`, `c26`, `c27`) and
that is exactly the shape of a suite going quiet. None of the
three escapes: they are caught by 18, 18 and 17 scenarios.

Across the two commits the counts moved once, upward:
breakage A went from 17 to 18, `c32` being a fourteenth
family-C scenario that catches it — fifteenth, counting from
the family. B and C did not move, which is expected: the
read-only work touched neither the write path's
first-committer-wins nor snapshot visibility.

Only A's pre-audit count is recoverable, and it is unchanged
where it can be compared: the 14 family-C scenarios that
caught it before still do. B and C are not comparable to the
old 11 and "5 of the 6", because those were recorded without
saying what the edit was; §9 now names all three edits so the
numbers can be reproduced.

## 9. What this audit does not establish

It does not re-derive the C++ scenarios (`e01` to `e03`,
`e06`), and it inherits `MVCC.md` §9's limits: these are
finite schedules, not a model check, so an interleaving not
written here is not covered by either document.

Where this audit and the engine disagree, neither is
automatically right: the disagreement is the finding, and
`MVCC.md`'s rules are what settle it.

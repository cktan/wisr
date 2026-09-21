# Plan: a script-driven MVCC scenario driver

The 54 scenario files in `mvcc/` say the right things, but
each one spends twenty lines of C++ on what is really a
timeline of six steps. This plan replaces that C++ with a
small language: one script per scenario, a driver that runs
it and prints what happened, and a committed copy of that
output to diff against.

The idea is not new -- PostgreSQL's `isolationtester` runs
`.spec` files this way, and its blocked-step protocol is the
one adopted below. What follows is the design, the risks,
and the order of work.

## 1. What the shape buys, and what it costs

**Buys.** A scenario reads as a timeline instead of as
assertions. Adding one costs a script, not a file. The
output is a transcript, so a regression shows up as a diff
naming the step that changed rather than as
`CHECK(c1) failed`. Blocking, which needs threads and
sleeps in C++, becomes one word in a script.

**Costs, and what this plan does about each.**

- *A diff is not an intention.* A golden file records what
  the engine did, and a wrong change to the engine plus a
  regenerated golden file looks like progress. Mitigations:
  comments are echoed into the transcript, so the file says
  what it is proving; `check` steps state expectations in
  the transcript itself; and `make accept` is a separate,
  explicit target that is never run by `make test`.
- *Nondeterminism kills golden files.* Timestamps, relids,
  xids and thread order must never reach the output, and a
  symmetric write skew may doom either side. §4 covers this;
  it is the main design risk.
- *A language is a thing to maintain.* It stays small on
  purpose: one verb per API call, no expressions, no
  variables, no control flow. If a scenario needs more, it
  stays in C++ (§7).

## 2. Layout

```
mvcc/
  MVCC.md        the rules and the scenario tables (unchanged)
  PLAN.md        this file
  driver/        the driver's C++ sources
  in/            scripts, one per scenario:  c02_write_skew.t
  good/          committed expected transcripts: c02_write_skew.out
  out/           transcripts from the last run (gitignored)
  *.cc           the scenarios that stay in C++ (§7)
build/driver     the executable
```

`make test` runs every script in `in/`, writes `out/`, and
diffs each against `good/`. Any difference fails the build
and prints the diff.

## 3. The language

One step per line. A line is a comment, a directive, or a
step. Tokens are whitespace-separated; a value that needs
spaces is quoted.

```
# comments start with '#' and are echoed to the transcript.

pairings all              # run the whole script once per level pairing
session T1                # declare a session; levels come from the pairing
session T2
seed R1 0                 # rows exist before any transaction begins
seed R2 0

@1  T1 begin              # @<time> <session> <verb> [args]
@1  T2 begin              #   same time: run in the order written
@2  T1 read R1
@2  T2 read R1
@3  T1 write R1 t1
@3  T2 write R2 t2
@4  T1 commit
@4  T2 commit
@5  check rows            # dump every row, as a fresh transaction sees it
```

**Directives.**

| Directive | Meaning |
|---|---|
| `pairings all` | run the script four times: RR/RR, RR/V, V/RR, V/V |
| `pairings rr` / `ser` / `rr+v` | run only that pairing |
| `session <name> [rr\|ser]` | declare a session; an explicit level overrides the pairing |
| `seed <row> <value>` | create the row before the scenario starts |

**Steps.** `@<time> <session> <verb> [args]`

| Verb | Maps to |
|---|---|
| `begin` | `sm_begin` at the session's level for this pairing |
| `read <row>` | `sm_read`; prints the value or `-` |
| `scan <lo> <hi>` | `sm_scan` to the end of the range; prints the keys |
| `scan <lo> <hi> limit <n>` | scan, take `n` rows, close: the early-close case |
| `write <row> <value>` | `sm_update` |
| `insert <row> <value>` | `sm_insert` |
| `delete <row>` | `sm_delete` |
| `commit` / `abort` | `sm_commit` / `sm_abort` |
| `mkrel` / `lsrel` | the catalog scenarios (e04) |

**Checks.** `@<time> check <what>` — assertions whose output
is canonical, so nondeterminism never reaches the golden
file.

| Check | Passes when |
|---|---|
| `check rows` | always; prints every row and value |
| `check row <row> <value>` | the row has that value now |
| `check committed T1 T2 ...` | all named sessions committed |
| `check failed T1` | that session's commit failed |
| `check exactly-one-committed T1 T2` | one of the two committed -- for symmetric skew |
| `check at-most-one-committed T1 T2` | zero or one committed |

**Blocking is claimed, not observed.** A step that waits on a
row lock is written with a trailing `blocks`:

```
@3 T2 write R1 t2 blocks
```

Only such a step may be reported as blocked. Every other step
is waited for however slow the machine is, so a transcript
never depends on timing -- an early version reported a slow
catalog commit as "blocked" under a sanitizer build, which is
exactly the flakiness golden files cannot survive. A step
marked `blocks` that finishes at once is a failure
(`DID NOT BLOCK`), and one not marked that never finishes is
a failure too (`HUNG`).

**Time.** `@<n>` is a label, not a delay. Steps run in
ascending order, and within one time in the order written.
Its only jobs are to make the timeline readable and to
appear in the transcript.

## 4. Determinism

The transcript must be byte-identical across runs and
machines. Rules the driver follows:

- **Nothing generated is printed.** No timestamps, xids,
  relids, durations or paths. Rows are named by the script;
  the single relation is implicit.
- **Errors print as symbolic names**: `conflict`,
  `serialization`, `dupkey`, `notfound`, `notrans`,
  `prepared`, `invalid`.
- **Scan output is key order**, which the engine already
  guarantees.
- **Blocked steps** follow the protocol in §5, which orders
  the transcript by causality rather than by thread timing.
- **Symmetric outcomes are never printed raw.** In c02 at
  V+V either transaction may be the one doomed, so its
  commits are written `commit?`, which records the outcome
  and prints only `recorded`. What must hold is then stated
  by a check -- `check at-most-one-committed T1 T2` -- whose
  own output is the same either way. A commit whose outcome
  is forced is written `commit` and prints it.
- **One process, one relation, fresh database per run**, so
  no state leaks between scripts.

## 5. Blocking

A step that waits on a row lock is the point of half the
write-write scenarios, so the driver runs each session in
its own thread and dispatches one step at a time:

1. Send the step to its session's thread.
2. Wait: `--block-ms` (default 250) for a step the script
   marked `blocks`, and up to a minute for any other, since
   only a hang can exhaust that.
3. If it finished, print its line and go on -- with
   `DID NOT BLOCK` against it, as a failure, if the script
   claimed it would wait.
4. If a `blocks` step did not finish, print `... blocked` and
   go on to the next step; it stays pending. If any other
   step did not finish, print `HUNG` and count a failure.
5. Before printing any later step, drain pending steps that
   have since completed, printing each as
   `<session> <verb> ... -> <result> (resumed)`. That drain
   waits only briefly: a step still blocked is meant to be,
   and the next step is what releases it.
6. At end of script, wait properly for every pending step;
   one still blocked is a failure, printed as
   `STILL BLOCKED`.

This is `isolationtester`'s protocol. It makes the
transcript causal: a blocked write appears where it blocked
and again where it resumed, and the step that released it is
the line in between.

## 6. Worked example

`in/b09_dirty_write.t`:

```
# b09 -- a write waits for the row lock, then conflicts.
#
# T1 holds the row lock on R1. T2's write of the same row
# must wait, and once T1 commits, T2 must lose
# first-committer-wins rather than overwrite it.

pairings all
session T1
session T2
seed R1 0

@1 T1 begin
@1 T2 begin
@2 T1 write R1 t1
@3 T2 write R1 t2
@4 T1 commit
@5 check failed-write T2
@6 check row R1 t1
```

`good/b09_dirty_write.out`:

```
== pairing RR/RR ==
# b09 -- a write waits for the row lock, then conflicts.
#
# T1 holds the row lock on R1. T2's write of the same row
# must wait, and once T1 commits, T2 must lose
# first-committer-wins rather than overwrite it.
@1 T1 begin -> ok
@1 T2 begin -> ok
@2 T1 write R1 t1 -> ok
@3 T2 write R1 t2 -> blocked
@4 T1 commit -> ok
@3 T2 write R1 t2 -> conflict (resumed)
@5 check failed-write T2 -> satisfied
@6 check row R1 t1 -> satisfied
== pairing RR/V ==
...
```

## 7. What stays in C++

Three of the current scenarios are not timelines and should
not be forced into one:

- `e01_ssi_node_lifetime.cc` — reaches into `db_t` to prove
  SSI state is released.
- `e02_serializable_invariant_stress.cc` — four threads,
  fifty rounds each, with retries.
- `e03_rr_contention_stress.cc` — same shape at RR.

`e05` (four RR transactions reading everything) is a
timeline and converts. Everything in families a, b and c
converts, `b09`–`b11` included, via §5.

## 8. Order of work

Each phase ends with a check that can fail.

1. **Driver skeleton.** Parse a script, one session, no
   threads: directives, `begin/read/write/commit`, the
   transcript format, `check rows`.
   *Verify:* `in/a01` converted; transcript matches a
   hand-written `good/a01_*.out`; running twice gives
   identical bytes.
2. **Pairings.** Run each script once per pairing with the
   `== pairing ... ==` banner.
   *Verify:* a01's transcript has four sections; a script
   with an explicit level overrides the pairing.
3. **Full verb set and checks.** `scan`, `insert`, `delete`,
   `abort`, `scan ... limit`, `mkrel`/`lsrel`, every `check`.
   *Verify:* families a and c convert (34 scripts); each
   transcript agrees with the outcome MVCC.md's table claims
   for that scenario and pairing.
4. **Threads and blocking.** §5.
   *Verify:* b09, b10, b11 convert; their transcripts show
   `blocked` and `(resumed)`; running the suite 20 times
   gives identical output every time.
5. **Make integration.** `build/driver`; `make test` runs
   `in/*.t` into `out/` and diffs `good/`; `make accept`
   copies `out/` over `good/` and is never invoked by
   `make test`; `mvcc/out/` in `.gitignore`.
   *Verify:* a deliberate one-character change to `good/`
   fails `make test` with a readable diff; `make accept`
   restores it.
6. **Migration and deletion.** Convert the remaining b
   scenarios, delete each converted `.cc`, and point
   MVCC.md's tables at `in/*.t` instead of `*.cc`.
   *Verify:* the doc/directory cross-check still finds every
   table row backed by a file and no file missing from the
   tables.
7. **Prove the suite still bites.** Re-run the mutations
   that the C++ suite caught: disable point-read SIREAD
   tracking, disable first-committer-wins, break snapshot
   visibility.
   *Verify:* each mutation produces failing diffs in at
   least as many scenarios as before (14, 11, and 5
   respectively). A mutation that now passes means the
   conversion lost an assertion.

## 9. Decisions, as settled

Checks stay in the scripts; one golden file per script, with
a section per pairing; everything convertible was converted
and its C++ deleted; `in/`, `good/` and `out/` live under
`mvcc/`.

The original list of options follows, for the record.

## 10. The options as they were put

1. **Where do checks live?** The plan puts expectations in
   the script (`check ...`) *and* in the golden file. The
   alternative is a pure transcript with no `check` verb,
   where the golden file is the only expectation. Pure
   transcripts are less to write; scripts with checks
   survive an accidental `make accept`. I recommend keeping
   `check`.
2. **One golden file per script, or per pairing?** One file
   with four banner-separated sections keeps a scenario in
   one place; four files make a diff smaller and let a
   single pairing be accepted alone. I recommend one file.
3. **How far to migrate?** Convert everything convertible
   and delete the C++ (this plan), or keep the C++ scenarios
   as they are and add scripts only for new ones. Keeping
   both doubles the maintenance for the same coverage; I
   recommend converting.
4. **`in/` and `good/` under `mvcc/`, or at the repo root?**
   Your message said `in/` and `out/`; I have them under
   `mvcc/` so the isolation material stays together. Say the
   word if you meant the repository root.

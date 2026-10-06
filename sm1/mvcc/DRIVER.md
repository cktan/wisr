# The scenario driver

`build/driver` runs one scenario script and prints a
transcript of what the engine did. The scripts live in `in/`,
the transcript each must produce lives in `good/`, and a run
writes `out/`, which is transient. `make test` runs every
script and diffs the two; a difference fails the build.

This file is the reference for the command and the language.
`MVCC.md` is the isolation argument the scripts serve.

## 0. Why a script and a transcript

A scenario is a timeline, and a timeline written as C++
assertions spends twenty lines on six steps. As a script it
stays a timeline, adding one costs a file of ten lines rather
than a hundred, and a regression arrives as a diff naming the
step that changed instead of `CHECK(c1) failed`. Blocking,
which needs threads and sleeps in C++, is one word here.
PostgreSQL's `isolationtester` runs its `.spec` files this
way, and the blocked-step protocol in §2.4 is its.

The shape has one real hazard: **a diff is not an
intention.** A transcript records what the engine did, so a
wrong change to the engine plus a regenerated transcript
looks like progress. Three things push back on that, and they
are the reason the language has the features it has:

- comments are echoed into the transcript, so the expected
  output says what it is proving, not only what happened;
- `check` steps put the expectations in the script, where
  they survive a careless `make accept`;
- `make accept` is a separate target, never run by
  `make test`.

The language stays small for the same reason: one verb per
API call, no expressions, no variables, no control flow. A
scenario that needs more belongs in C++ beside `e01` to
`e03`.

## 1. Usage

```
driver [--block-ms N] script.t
```

| | |
|---|---|
| `script.t` | the script to run; exactly one is required |
| `--block-ms N` | how long a step marked `blocks` is given before it is reported blocked (default 250) |

The transcript goes to **stdout**. Errors -- a script that
does not parse, a database that will not open, a count of
failed checks -- go to **stderr**.

| Exit | Meaning |
|---|---|
| 0 | the script ran and every check was satisfied |
| 1 | the script ran and at least one check failed |
| 2 | the script could not be run: bad arguments, a malformed script, or a database that would not open |

The count on stderr totals the failures across every pairing
the script ran in, so one bad step in a script that runs all
four pairings is reported as four.

A malformed script names the line it died on:

```
$ build/driver mvcc/in/broken.t
mvcc/in/broken.t:7: unknown verb: frobnicate
```

Unknown verbs, unknown checks, and a verb given the wrong
number of arguments are all caught before anything runs, so a
typo cannot reach a transcript and be mistaken for a result.

### Through make

```
make scenarios     # run every script in in/, diff against good/
make test          # the whole suite, scenarios included
make accept        # take the current transcripts as expected
```

`make accept` rewrites `good/`. It is never run by
`make test`, because accepting a transcript is accepting a
behaviour: read the diff first.

### Writing a new scenario

1. Write `in/<id>_<name>.t`, opening with a comment that says
   what the scenario proves.
2. Run `build/driver in/<id>_<name>.t` and read the
   transcript. It is the scenario's behaviour: check it
   against what the design claims before believing it.
3. `make accept` to record it, then commit the script and its
   `good/` transcript together.
4. Add a row to the tables in `MVCC.md`, which are
   cross-checked against the directory.

## 2. The language

A script is a list of lines. A line is blank, a comment, a
directive, or a step. Comments start with `#` and are echoed
into the transcript, so the expected output says what it is
proving.

Tokens are separated by whitespace, and **no token may
contain a space**: row names and values are single words.

```
# c02 -- write skew on two rows.

pairings all
session T1
session T2
seed R1 0
seed R2 0

@1 T1 begin
@1 T2 begin
@2 T1 read R1
@3 T2 read R1
@4 T1 write R1 t1
@5 T2 write R2 t2
@6 T1 commit?
@7 T2 commit?
@8 check at-most-one-committed T1 T2 when s+s
```

### 2.1 Directives

Directives set the run up. They are not echoed.

| Directive | Meaning |
|---|---|
| `pairings all` | run the script once per pairing: RR/RR, RR/S, S/RR, S/S (the default) |
| `pairings rr` | only RR/RR |
| `pairings rr+s` | only the two mixed pairings |
| `pairings s+s` | only S/S |
| `pairings levels` | every pairing of READ COMMITTED, REPEATABLE READ and SERIALIZABLE: RC/RC, RC/RR, RC/S, RR/RC, RR/RR, RR/S, S/RC, S/RR, S/S |
| `session <name>` | declare a session; its level comes from the pairing |
| `session <name> rc` | declare a session pinned to READ COMMITTED |
| `session <name> rr` | declare a session pinned to REPEATABLE READ |
| `session <name> ser` | declare a session pinned to SERIALIZABLE |
| `session <name> [<level>] ro` | as above, each transaction begun read-only (`sm1::begin`'s `read_only`) |
| `seed <row> <value>` | create the row, committed, before the scenario starts |

**How a pairing assigns levels.** The first session declared
takes T1's level; every other session takes T2's. A session
declared with an explicit level keeps it in every pairing --
useful when a scenario is about one level only, as `e05` is.

Every script runs against a fresh database with two
stores, both holding the seeded rows, so nothing leaks
between scripts or between pairings.

### 2.2 Steps

```
@<time> <session> <verb> [args] [blocks] [when <group>]
```

`@<time>` is a label, not a delay: steps run in the order
written, and the time appears in the transcript so a timeline
can be read at a glance. Several steps may share a time.

| Verb | Arguments | Prints |
|---|---|---|
| `begin` | — | `ok`, or an error name |
| `read` | `<row>` | the value, or `-` if no row is visible |
| `scan` | `<lo> <hi>` | the keys in the closed range: `{R1, R2}` |
| `scan` | `<lo> <hi> limit <n>` | the first `n` keys, then closes the cursor |
| `write` | `<row> <value>` | `ok`, or an error name |
| `insert` | `<row> <value>` | `ok`, or an error name |
| `delete` | `<row>` | `ok`, or an error name |
| `update-where` | `<lo> <hi> <old> <new>` | SQL's `UPDATE ... SET v = new WHERE v = old` over rows `lo..hi`: a scan, then a write of each row holding `old`; prints the rows written, `{R1, R2}` |
| `commit` | — | `ok`, or an error name |
| `commit?` | — | `recorded`: the outcome is kept but not printed |
| `abort` | — | `ok` |
| `open` | `<cursor> <lo> <hi>` | `ok`: a cursor that outlives the step |
| `next` | `<cursor>` | `<key>=<value>`, or `end` |
| `close` | `<cursor>` | `ok` |
| `savepoint` | — | the new mark's depth: `1`, `2`, … |
| `rollback` | `<depth>` | `ok`, or an error name |
| `lock` | `<row> <is\|ix\|shared\|six\|exclusive>` | `ok`, or an error name |
| `seek` | `<cursor> <key>` | `ok`, or an error name |
| `stmt` | — | `ok`: starts a statement, for `rc` |
| `readlatest` | `<row>` | the newest committed value, or `-`; at `rc`, under a lock the session holds |
| `lookup` | `<row>` | the value in the SECOND store, or `-` |
| `rmstore` | — | `ok`: drops the second store |
| `mkstore` | — | `ok`: creates a store, whose storeid is not printed |
| `lsstore` | — | `<n> stores` |

Error names are symbolic, never numbers or messages:
`conflict`, `serialization`, `dupkey`, `notfound`, `notrans`,
`deadlock`, `prepared`, `invalid`, `updated`, `readonly`.

**`commit?`** is for a commit whose outcome may differ from
run to run -- at S+S either side of a write skew may be the
one doomed. It records the result for the checks below and
prints only `recorded`, so the transcript stays stable. A
commit whose outcome is forced uses plain `commit` and prints
it.

**`savepoint`** prints the depth it made rather than `ok`,
because the depth is the mark's name: a later `rollback` names
the number this step reported. Marks nest, so a second
`savepoint` prints `2`, and rolling back to a depth keeps that
mark and drops the ones inside it. `sm1::forget` has no verb:
no scenario has needed one, and it would take the same shape.

**`lock`** takes the row lock without writing the row, in one
of Gray's modes: `is`, `ix`, `shared`, `six` or `exclusive`.
Compatible holders coexist, as `sm1.h`'s table for `lockmode_t`
says; anything else waits, so a `lock` step takes `blocks` like
a write does. A transaction already holding the row and asking
for a stronger mode converts to the weakest mode covering both,
which is how two transactions deadlock on one row (`b28`). Once held, the lock is checked against the
transaction's snapshot, so `lock` can also answer `conflict`.

**`seek`** moves an open cursor forward to a key, that key
included, keeping the snapshot and bounds it opened with. It
moves one way only: a key before the last one the cursor
returned, or below the scan's start key, answers `invalid`
(`a14`). The keys it jumps over still count as read under
SERIALIZABLE, which is what `c30` pins. It takes no mode: no
scenario asks for an exclusive seek, and `test/` covers that
argument.

**`stmt`** starts a statement. It matters only at READ
COMMITTED, where every read after it uses one snapshot until
the next `stmt`; above that level the transaction already has
one snapshot and the step is accepted and does nothing. There
is no pairing for READ COMMITTED: a script that wants it
declares `session <name> rc`, as `a15` and `a16` do, because
the level is the scenario rather than one axis of it.

**Two stores.** Every run creates a second store
holding the same seeded rows as the first. Every verb above
works in the first one; `lookup` and `rmstore` are the only ways
to reach the second, and they exist so that a script can drop
a store while reading another — `a17` for the older
snapshot that goes on reading a dropped store, `c31` for
§11's write skew, where the only thing joining two
transactions is that one resolved the store the other
dropped. `lookup` reads a row like `read` does; what the
scenario is usually after is the storeid resolution it takes on
the way, which is a tracked catalog read.

The ceiling is deliberate: there are two stores, not n, and
they cannot be named or made mid-script. Stores became
reachable at all without giving every verb a store operand,
which is what naming them would have cost. A scenario needing
a third store needs the grammar change this avoided.

**`blocks`** says the step must wait on a row lock. Only a
step marked this way may be reported blocked; every other
step is waited for however slow the machine is, so a
transcript never depends on timing. A marked step that
finishes immediately fails with `DID NOT BLOCK`; an unmarked
step that never finishes fails with `HUNG`, after a minute.

**`when <group>`** limits a step to some pairings: `rr`,
`rr+s`, `s+s`, or `all`; or by one side's level, `t1=` or
`t2=` and levels joined by `+`, as `t2=rc` or `t2=rr+ser`,
which is how a script under `pairings levels` says what READ
COMMITTED does differently (`b35`). It is how a scenario
states an outcome that differs by level, as `c06` does:

```
@9 check committed T1 T2 T3 when rr
@9 check failed T2 when s+s
```

Both modifiers may appear together, in either order:
`... blocks when s+s`.

### 2.3 Checks

```
@<time> check <what> [args] [when <group>]
```

A check runs in the driver's own session, in a fresh
REPEATABLE READ transaction where it needs one, and prints
`satisfied` or `FAILED (...)`. A failed check makes the
driver exit 1, on top of the diff against `good/`.

| Check | Satisfied when |
|---|---|
| `rows` | always; prints every row as `{R1=t1, R2=0}` |
| `row <row> <value>` | the row has that value now; `-` means it is gone |
| `committed <session>...` | every named session committed |
| `failed <session>...` | every named session's commit failed |
| `exactly-one-committed <a> <b>` | exactly one of them committed |
| `at-most-one-committed <a> <b>` | one or neither committed |
| `not-all-committed <a> <b> ...` | at least one did not commit |
| `last <session> <result>` | that session's last step printed `<result>` |

`check last T2 conflict` is how a scenario states the outcome
of a step that failed -- including a step that was blocked
and then resumed.

### 2.4 How blocking is detected

Each session runs on its own thread, and the driver
dispatches one step at a time:

1. Send the step to its session's thread.
2. Wait: `--block-ms` (default 250) for a step the script
   marked `blocks`, and up to a minute for any other, since
   only a hang can exhaust that.
3. If it finished, print its line and go on -- with
   `DID NOT BLOCK` against it, as a failure, if the script
   claimed it would wait.
4. If a `blocks` step did not finish, print `blocked` and go
   on to the next step; it stays pending. If any other step
   did not finish, print `HUNG` and count a failure.
5. Before printing any later step, print any pending step
   that has since finished, as `... -> <result> (resumed)`.
   That wait is short: a step still blocked is meant to be,
   and the next step is what releases it.
6. At the end of the script, wait properly for every pending
   step. One still blocked is a failure, printed as
   `STILL BLOCKED`.

The transcript is therefore causal rather than timed: a
blocked write appears where it blocked and again where it
resumed, and the step between the two is what released it.

## 3. The transcript

```
# a02 -- dirty read through a scan, for an insert and a delete.
#
# T1 inserts R3 and deletes R2 without committing. T2's scan
# must show neither, and must not change when T1 commits.


== pairing RR/RR == T1=RR T2=RR
@1 T1 begin -> ok
@1 T2 begin -> ok
@2 T1 insert R3 t1 -> ok
@2 T1 delete R2 -> ok
@3 T2 scan R1 R3 -> {R1, R2}
@4 T1 commit -> ok
@5 T2 scan R1 R3 -> {R1, R2}
@6 T2 commit -> ok
@7 check rows -> {R1=0, R3=t1}
== pairing RR/S == T1=RR T2=SER
...
```

- The comments a script **opens** with head the transcript
  once; comments further down are echoed in each pairing,
  where they stand.
- Each pairing gets a banner naming the level of every
  session in it.
- A blocked step appears twice: where it blocked, and where
  it resumed. The step between the two is what released it.

```
@3 T2 write R1 t2 blocks -> blocked
@4 T1 commit -> ok
@3 T2 write R1 t2 blocks -> conflict (resumed)
```

Five markers mean a failure, and all of them make the driver
exit 1:

| Marker | Meaning |
|---|---|
| `FAILED (...)` | a check was not satisfied; the detail says what was found |
| `DID NOT BLOCK` | a step marked `blocks` finished at once |
| `HUNG` | a step not marked `blocks` never finished |
| `STILL BLOCKED` | a blocked step was never released, even at the end of the script |
| `ERROR: ...` | a step was sent to a session that is still blocked |

## 4. What keeps a transcript stable

The driver prints nothing the engine generates: no
timestamps, transaction ids, storeids, durations or paths. Rows
are named by the script, the store is implicit, and errors
print as names. Blocking is claimed rather than measured, and
an outcome that may vary is hidden behind `commit?` and
stated by a check.

The result is that a script's transcript is the same bytes on
a fast machine and under a sanitizer build, which is what
makes a diff against `good/` a test rather than a source of
noise.

## 5. Limits

- **One store per script**, created by the driver. Rows
  are keys in it, and their values are opaque single words.
- **No expressions, variables, or control flow.** A scenario
  that needs them belongs in C++ beside `e01` to `e03`.
- **Sessions are one step at a time.** A session with a
  blocked step cannot be given another; doing so is reported
  as an error rather than queued.
- **Up to eight sessions** may be named in one check.
- **Time labels are not ordering.** The driver runs steps in
  file order and does not sort by `@<time>`; a script whose
  times are out of order will mislead a reader, not the
  driver.

---

## 6. Permutation scripts

A second kind of script, made for porting PostgreSQL's
isolation tests (`POSTGRES.md`). It names its steps once and
lists the orders to run them in, as PostgreSQL's specs do, in
place of one schedule of `@` steps. A script is one kind or the
other.

```
session s1 ser
session s2 ser
seed t05 apple

step s1 rwx1 update-where t t~ pear apple
step s1 c1 commit
step s2 rwx2 update-where t t~ apple pear
step s2 c2 commit

permutation rwx1 rwx2 c1 c2
expect s1=commit s2=serialize
sm1 s1=serialize s2=serialize
```

| Directive | Meaning |
|---|---|
| `session <name> <level>` | as §2.1, but the level is required: there are no pairings |
| `step <session> <name> <verb> [args] [; <verb> [args]]...` | a named step: the session runs the verbs in order, as one SQL statement |
| `permutation <step>...` | one order to run the steps in |
| `expect <session>=<outcome>...` | each session's outcome in PostgreSQL, for the permutation above it; every session is named |
| `sm1 <session>=<outcome>...` | sm1's outcome where it differs on purpose, with the reason in a comment |

An outcome is `commit`, `serialize` (a serialization failure:
sm1's `serialization` or `conflict`, both PostgreSQL's 40001),
`unique`, `deadlock`, or `abort` (any other error, or no
commit).

**How a permutation runs.**

- **Each permutation starts from the seeds.** The driver keeps
  one database for the script, and before each permutation
  deletes every row and inserts the seeds again, so no
  permutation sees another's rows. Making new stores each time
  was the alternative; a column family each made a script of
  two hundred permutations take half a minute. The database
  does not sync each commit, which nothing here needs.
- **A session's transaction begins at its first step,** which
  is when PostgreSQL takes the snapshot, not at its `BEGIN`.
- **Any error ends the transaction.** The driver aborts it, as
  PostgreSQL does, and the session's later steps print
  `skipped`. The session's outcome is the class of that error.
- **Waiting is found, not declared.** A step is reported as
  `waiting` once sm1 shows its session waiting on a lock or on
  another transaction, which is how PostgreSQL's tester tells
  too; it is printed again, `(resumed)`, when it finishes. No
  `blocks` annotation is used.
- **The outcome line** compares each session's outcome with
  `expect`: `as PostgreSQL`, or `differs from PostgreSQL (...)
  as noted` when it matches the `sm1` line, or `FAILED`.

```
== permutation 2: rwx1 rwx2 c1 c2 ==
rwx1 s1 -> {t07}
rwx2 s2 -> {t05}
c1 s1 -> serialization
c2 s2 -> serialization
outcome s1=serialize s2=serialize -> differs from PostgreSQL (s1=commit s2=serialize) as noted
```

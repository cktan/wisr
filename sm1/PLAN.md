# PLAN — sm1: READ COMMITTED writes proceed

**Status: implemented (steps 0 to 6). Suite: 125 unit tests, 92
scenarios, 4 stress tests. Still open: the write-throughput
benchmark of step 2, and a thread-sanitizer run of step 5 that
is not conclusive (see step 5).**

`DESIGN.md` §6.2 records the decision: at READ COMMITTED, a
write that meets a row committed after the transaction's begin
snapshot waits for the row lock and then goes ahead against the
newest committed version, instead of failing with
`SM_ERR_CONFLICT`. `PART3.md` §5a says why: PostgreSQL's default
level does this, and its workloads rely on it. This file is the
order of work, and the argument for that order.

## The scenario

**A. A hot row at PostgreSQL's default level.** Two sessions,
both at READ COMMITTED, each add one to a counter row `R1`
that holds 0. A shared counter, a hot account balance, a
`next_order_id`: the commonest write pattern in OLTP.

```
T1 begin                      T2 begin
T1 UPDATE R1 SET n = n + 1
                              T2 UPDATE R1 SET n = n + 1
                                (T1 holds the row lock: waits)
T1 commit                     (R1 = 1)
                              T2 wakes: R1 changed after T2 began
```

| | T2's update | R1 afterwards |
| --- | --- | --- |
| PostgreSQL | re-reads R1 (1), re-checks the `WHERE` clause, writes 2 | 2 |
| sm1 today | fails with `SM_ERR_CONFLICT`, and T2 is aborted | 1, until T2 retries |
| this plan | fails with `SM_ERR_UPDATED`; T2 calls `sm_read_latest`, sees 1, re-checks, writes 2 | 2 |

A client written for PostgreSQL sees no error here and has no
retry loop, so sm1 as it stands cannot back it.

The variant matters as much. If the statement is
`UPDATE R1 SET n = 1 WHERE n = 0` — claim this row once — T2's
re-check must fail, and T2 must write nothing. Writing blindly
against the newest version would change a row that no longer
qualifies. So letting the write through is not enough: the
caller has to look at the newest version first, and sm1 has to
make it.

**B. What goes wrong underneath if the write simply proceeds.**
Suppose sm1 stopped failing and let T1's write through, and
nothing else changed. T1 has already written something else
(`R2`), so it already has an `xid`. The numbers are timestamps
from the one counter.

```
 10  R1 = 0 committed (seed)
 20  T1 begins at READ COMMITTED (snapshot 20)
 50  T1 writes R2                  T1's xid is 50
 55  T2 begins
 60  T2 writes R1 = 1              T2's xid is 60
 70  T2 commits                    commit_ts 70
 75  T1 updates R1 = 2             version key R1|~50
 80  T1 commits                    commit_ts 80
 85  a reader begins (snapshot 85) and reads R1
```

R1's versions sort by `xid`, newest first: `R1|~60` (value 1,
committed at 70), then `R1|~50` (value 2, committed at 80). T1
committed last, so the answer is 2. The reader seeks to the
newest version by `xid`, finds `~60`, sees it is visible
(70 <= 85), and returns 1. Worse, when compaction reaches R1 its
rule — a newer version already seen, with `commit_ts` at or
below the watermark, means drop — sees `~60` first and deletes
`~50`. T1's committed update is gone for good, and no reader
ever sees it.

With this plan T1's write is keyed by a timestamp taken after
it holds the lock, say 75, so `R1|~75` sorts first: the reader
returns 2, and compaction keeps `~75` and drops `~60` as the
superseded one.

A is what the user sees, and it needs the protocol. B is what
breaks if only A's protocol is built, and it needs the
ordering. Step 3's `f01`, `f02` and `f07` are these scenarios
written as tests.

## The two changes

Two changes make it possible, and the plan keeps them apart:

1. **Order versions by write timestamp, not by `xid`.** Today a
   key's versions sort by the writer's `xid`, which is fixed at
   its first write, and everything that reads a version chain
   assumes that order is commit order. A read committed write
   that passes a newer commit breaks the assumption (§6.2's
   example). Keying each version by a timestamp taken *after*
   the row lock is held makes the assumption true by
   construction. This step changes no behavior.
2. **A protocol for the re-check.** PostgreSQL's executor must
   re-evaluate the row's `WHERE` clause against the newest
   version before it writes. sm1 enforces that exchange, so a
   caller cannot skip it and lose a predicate.

## Decisions

| | Decision | Status |
| --- | --- | --- |
| 1 | Key versions by a write timestamp; `xid` moves to the value | settled |
| 2 | sm1 enforces the read-latest step; the caller does not pass a flag | settled |
| 3 | The format version is bumped; databases written before it are refused | assumed, not confirmed |
| 4 | `global_ts` is atomic: `xid` and `wts` are a bare `fetch_add`; only `commit_ts` and lease renewal take `alloc_mu` (step 2) | settled |
| 5 | An insert at READ COMMITTED goes ahead against the newest committed version, with no re-check | assumed, see step 4 |
| 6 | `sm_lock` at READ COMMITTED no longer fails with `SM_ERR_CONFLICT` | assumed, see step 4 |
| 7 | Names: `SM_ERR_UPDATED` and `sm_read_latest` | provisional |
| 8 | The read-latest mark lives in the session's lock list, not the shared lock table, and a successful write clears it | assumed, see step 4 |
| 9 | `sm_read_latest` is allowed under a shared lock too, and sets no mark there | assumed, see step 4 |

## Step 0. Verify the starting state

- `git status` is clean, and the branch is `main`.
- The RocksDB development headers are installed
  (`librocksdb-dev`); without `rocksdb/db.h` the build cannot
  run.
- `make -C sm1 -j4 test`, output saved to a log. Record the
  number of unit tests and of scenarios, so later steps can be
  compared against them.
- Record a write-throughput baseline. If no benchmark exists,
  step 2 writes one; take the baseline then, from this commit.

**Verify:** everything passes, and the log is kept.

## Step 1. Order versions by write timestamp

A behavior-preserving change, and the only one that touches the
on-disk format. Every existing test must pass unchanged.

**Key.** The tail stays twelve bytes, `~wts | ~seq`. `wts` is a
timestamp from the same counter as `xid` and `commit_ts`,
allocated for every write. Width and comparator are unchanged
(`wisr.version.v1`); only what the eight bytes mean changes.
`seq` stays: it is what own-write visibility and savepoint
rollback use (§6.5).

**Value.** `version.h` today is `flags | commit_ts (stamped
only) | payload`. It becomes `flags | xid | payload` while
unstamped, and `flags | commit_ts | payload` once the
compaction filter stamps it. A stamped version is no bigger
than before; a fresh one is eight bytes bigger until it is
compacted. The `xid` is only needed while the writer may still
be in flight, which is exactly when a version is unstamped.

**Write path (`access_write`).** After `lock_acquire`, after the
conflict check, after the `xid` is allocated on a first write,
allocate `wts`, then `Put`. The order is the whole point: a
writer takes its `commit_ts` before it releases the lock, and
the next writer allocates `wts` after it acquires it, so
`wts_B > commit_ts_A > wts_A` for two writers of one key. Add a
debug assertion of exactly that against the version
`latest_version` returns.

**Readers.**

- `visible`: the shortcut "written after our snapshot, so
  committed after it" compares `wts`, not `xid`. It still holds,
  because `commit_ts` is allocated after every `wts` of its
  transaction.
- Own-write tests (`visible`, `concurrent_writer`,
  `latest_version`) compare the `xid` decoded from the value.
- The seek bound in `access_read`, `max(xid, snapshot_ts)`,
  becomes `max(last own wts, snapshot_ts)`; the session keeps
  its largest `wts`. A larger bound only starts earlier in the
  chain, and the own-write filter (`seq <= seq_hwm`) decides
  visibility as before.
- `ssi_on_skip` and the status lookups take the `xid` from the
  value. Only unstamped versions reach them.

**Savepoint rollback.** The undo entry becomes
`(relid, userkey, wts, seq)`, because the exact key to delete
now needs `wts`. That is eight more bytes per write in
`T.undo`, which §5.1 already lists as linear in keys written.

**GC and recovery.** The compaction filter's stamp replaces the
`xid` field with `commit_ts` instead of inserting one. Its
"newer version already seen" rule is unchanged, since key order
is now commit order at every level. `recover.cc` reads `xid`
from the value.

**Format.** Bump `WISR_META_VERSION` (`db.cc`), so an older
database is refused instead of misread.

**Audit before starting.** Every use of `version_t::xid` and of
`version_decode_key` decides which of the two it means. From
grep: `access.cc` 18 sites, `ssi.cc` 22, `gc.cc` 2,
`recover.cc` 8, `xct.cc` 54 (mostly the status table, which
keeps `xid` and does not change).

**Verify:**
- Unit tests in `test_version.cc`: encode and decode round trip
  for both value forms, key order by `wts`, and golden bytes for
  each.
- A test that interleaves committed writers of one key and shows
  the key's versions sort in commit order.
- The full suite passes with no expectation changed.

## Step 2. Make the per-write allocation lock-free

Done. `db_alloc_ts` was taking `alloc_mu`, the commit
sequencer's mutex, so every write took a global mutex.
`global_ts` and `ts_limit` are now atomics:

- `db_alloc_ts` is a bare `fetch_add`. Only a value at or past
  `ts_limit` takes `alloc_mu`, to renew the lease with a
  synchronous `Put` to `CF:meta` (`cover_ts_locked`); the value
  is handed out only after the lease covers it, and a failed
  renewal only burns it.
- `db_claim_commit_ts` still takes `alloc_mu`, so its
  `fetch_add` and its insert into `committing` are one step to
  the advancer, and `db_commit_done` reads `global_ts` under
  it. §4.3's argument stands, because `xid` and `wts` never
  enter the set (DESIGN.md §4.3).
- Every row write now takes a timestamp, so the lease
  (`ts_lease`, default 1M) is used up faster than when only a
  transaction's first write took one, and each renewal is a
  synchronous `Put` that stalls allocation while it runs.

**Verify:** the suite passes. Still to do: a write-throughput
benchmark, to take numbers against the step 0 commit and to
count lease renewals per second; enlarge the default
`ts_lease` if they matter.

## Step 3. Tests first

Write what step 4 must make pass, and watch it fail.

**Driver** (`mvcc/driver`, `DRIVER.md`): a verb `readlatest
<row>` that prints the value or its absence, and `updated` as an
error name a `check last` can match.

**Scenarios,** pinned with `session T rc`, since `pairings all`
runs only RR and SERIALIZABLE. New series `f`:

| Scenario | Shows |
| --- | --- |
| f01 lost update avoided | two RC sessions increment one row; the second waits, re-reads the newest value, and writes it plus one |
| f02 predicate no longer holds | the second session's re-check fails its `WHERE` clause, so it skips the row and writes nothing |
| f03 row deleted meanwhile | `readlatest` reports the row absent; the update is skipped |
| f04 write without the re-read | the write fails with `updated` and the transaction stays active, holding the lock |
| f05 insert after a concurrent delete | the insert goes ahead, judged against the newest committed version |
| f06 repeatable read and serializable unchanged | the same timeline still ends in `conflict`, aborting the loser |
| f07 version order | a writer whose `xid` predates a newer commit writes past it; a later read returns the writer's value, and after compaction the newer commit is not dropped as superseded |

| f08 chained contention | three RC sessions queue on one hot counter; each in turn wakes with `updated`, calls `readlatest`, writes and commits, so the counter ends at three |
| f09 delete meets a concurrent update | the first session commits `n = 1`; the second's `DELETE WHERE n = 0` gets `updated`, `readlatest` shows `n = 1`, and the delete is skipped |
| f10 savepoint rollback around `readlatest` | a session takes a savepoint, its update fails with `updated`, it calls `readlatest`, then rolls back to the savepoint; the mark and the lock state are what step 4 says they are |

f07 is the §6.2 example, and the one that fails on today's
ordering. It needs a way to force compaction; `test_gc.cc`
already does.

**Verify:** the new scenarios and unit tests fail for the
expected reason, and only they fail.

## Step 4. The protocol

- **`SM_ERR_UPDATED`.** Returned by `sm_update` and `sm_delete`
  at READ COMMITTED when the newest committed version has a
  `commit_ts` after the begin snapshot and this transaction has
  not read it. Unlike `SM_ERR_CONFLICT` it does not abort: the
  transaction stays active and keeps the lock, and
  `sm_retryable()` is 0.
- **`sm_read_latest`.** Takes the same arguments as `sm_read`
  and returns as it does (0 found, 1 no such key). It requires
  READ COMMITTED and a lock this transaction already holds on
  the key, taken by the failed write or by `sm_lock`;
  otherwise `SM_ERR_INVALID`. It returns the newest committed
  version, ignoring the snapshot. Because the lock is held, that
  version cannot change before commit.
  - Under an **exclusive** lock it also sets the mark that lets
    the write through.
  - Under a **shared** lock it sets no mark, since a shared
    holder cannot write without upgrading. It is allowed
    because `sm_lock` no longer fails at READ COMMITTED: a
    caller that waited for `SHARED` (a writer locking a schema
    row, `SELECT FOR SHARE`) must be able to see the row that
    committed while it waited, and `sm_read` may still return
    the pre-lock version if the statement snapshot was already
    pinned.
- **The write.** With the mark set, the check in `access_write`
  passes and the write applies to the newest version. The old
  row `sm_update` hands back (§5.3) is that version's image.
- **Insert.** Judged against the newest committed version
  (`SM_ERR_DUPKEY` if it is live), with no re-check, because an
  insert has no predicate to re-evaluate. This is how
  PostgreSQL's unique check behaves.
- **`sm_lock`.** At READ COMMITTED it no longer fails with
  `SM_ERR_CONFLICT`; the caller reads the row with
  `sm_read_latest`. At the higher levels it is unchanged.
- **Unchanged:** REPEATABLE READ and SERIALIZABLE, SSI (READ
  COMMITTED never takes part, §7.4), 2PC, the GC watermark (a
  read committed transaction already pins its begin snapshot,
  and the newest committed version is never collected).

**The mark.** One bit per held lock, kept in the session's lock
list (`sm_session_t::lockref_t`, `session.h`), not in the shared
`db_t::lock_ent_t`. Only the exclusive holder reads or sets it,
so `access_write` and `sm_read_latest` need not take
`db->lock_mu`. It goes away with the lock.

- A successful write **clears** it. Left set, a later write to
  the key, after a savepoint rollback or much later in a long
  transaction, would slip past the conflict check without a
  fresh read.
- A rollback to a savepoint taken after `sm_read_latest` and
  before the write leaves the mark set, since the lock is still
  held. If the lock was acquired after the savepoint, rolling
  back releases it, and the mark with it. f10 covers both.

**Existing tests that encode the old rule at READ COMMITTED,**
found by grep and to be read before they are changed:

| Test | Why it changes |
| --- | --- |
| `test_lock.cc` "…conflict at every isolation level" (line 72) | its READ COMMITTED iteration expected `SM_ERR_CONFLICT` |
| `test_lock.cc` "read committed sees a newer commit but cannot overwrite it" (line 94) | the title is the old rule; it becomes the read-latest exchange |
| `test_lock.cc` lines 333–363 | READ COMMITTED sessions around `sm_lock` |
| `test_gc.cc` "an idle read-committed transaction holds back the tombstone purge" (line 262) | expects the insert to fail with `SM_ERR_CONFLICT` |
| `test_2pc.cc` lines 385, 468 | to be checked for the level in use |
| `sm.h` and `API.md` | say "at every isolation level — including READ COMMITTED" |

**Verify:** the step 3 tests pass; the tests above are rewritten
to the new rule, each with a note of why; the full suite passes.

## Step 5. A stress test

Modelled on `mvcc/e03_rr_contention_stress.cc`: many READ
COMMITTED sessions increment a few hot counters through the
full exchange, with compaction running. The check is the sum:
every increment lands exactly once, so no update is lost and
none is applied twice.

**Done** as `mvcc/e06_rc_contention_stress.cc`: four threads,
three counters, a compaction thread running throughout. The sum
was exact in every run tried (about 100 of the 160 increments
took the `updated` path). The build has no sanitizer target. A
one-off `-fsanitize=thread` build reports 44 races. The two I
read sit on heap buffers handed between threads through
RocksDB, which is not instrumented, and the existing `e03`
reports one too; the other 42 were not read. Treat it as
inconclusive rather than clean.

**Verify:** the sum is exact over many runs.

## Step 6. Documents

`DESIGN.md` §3.1 (key and value), §4.1 (the invariant, restated
over `wts`), §5 (write path, and a new subsection for the
protocol), §5.2, §6.2 (fold the decision in, and delete "not yet
implemented"), §7.4 if READ COMMITTED's part in SSI needs a
sentence; `API.md`; `mvcc/MVCC.md`'s scenario table; and
`sm2/DESIGN.md` §5, whose online-build steps say blocked writers
"fail their conflict check". At READ COMMITTED they no longer
do; the schema-row lock (`sm2/DESIGN.md` §4) is what keeps them
out, and that text has to say so.

**Verify:** no document still states the old rule. `grep` for
"including READ COMMITTED" and "first-committer" and read each
hit.

## Risks

- **A use of key order that the audit misses.** Anything that
  assumed the order of a key's versions was the order of their
  `xid`s. Step 1's tests are the net; the debug assertion
  catches an out-of-order write at the source.
- **`visible_ts` and the new allocations.** `wts` values, like
  `xid`s, never enter the commit sequencer's set, so they cannot
  hold back `visible_ts` (§4.3). The argument is short, but it is
  the one place a subtle error would be silent; step 1's review
  should check it, not assume it.
- **The mutex** (step 2).
- **Format refusal.** Any database written before step 1 is
  unreadable afterwards (decision 3).

## What is not here

- The executor's side: the PostgreSQL fork calls this protocol
  from its update path. That belongs to the fork, not sm1.
- Effort estimates. None were made.
- sm2: it inherits the behavior, and the `DESIGN.md` §5 text
  above is its only change for now.

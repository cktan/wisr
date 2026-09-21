# PLAN — sm1: READ COMMITTED writes proceed

**Status: proposed. Nothing here is started; this plan waits
for a go-ahead.**

`DESIGN.md` §6.2 records the decision: at READ COMMITTED, a
write that meets a row committed after the transaction's begin
snapshot waits for the row lock and then goes ahead against the
newest committed version, instead of failing with
`SM_ERR_CONFLICT`. `PART3.md` §5a says why: PostgreSQL's default
level does this, and its workloads rely on it. This file is the
order of work, and the argument for that order.

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
| 4 | The first cut allocates the timestamp under `alloc_mu`; the mutex is measured before it is removed (step 2) | assumed, not confirmed |
| 5 | An insert at READ COMMITTED goes ahead against the newest committed version, with no re-check | assumed, see step 4 |
| 6 | `sm_lock` at READ COMMITTED no longer fails with `SM_ERR_CONFLICT` | assumed, see step 4 |
| 7 | Names: `SM_ERR_UPDATED` and `sm_read_latest` | provisional |

## Step 0. Verify the starting state

- `git status` is clean, and the branch is `main`.
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

## Step 2. Measure the per-write allocation

`db_alloc_ts` takes `alloc_mu`, which is also the commit
sequencer's mutex, so every write now takes a global mutex.

- Take the baseline from step 0 and a measurement from step 1,
  with many sessions writing different keys.
- If the cost is small, leave it and record the numbers. If it
  is not, `global_ts` becomes atomic and `db_claim_commit_ts`
  keeps the set insert atomic with the allocation another way.
  That is its own change, with its own review, because §4.3's
  argument that `visible_ts` never passes a commit in progress
  depends on how claim and advance share the mutex.

**Verify:** the numbers are in this file, and the decision.

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
  READ COMMITTED and an exclusive lock this transaction already
  holds on the key, taken by the failed write or by
  `sm_lock`; otherwise `SM_ERR_INVALID`. It returns the newest
  committed version, ignoring the snapshot, and marks the lock
  entry. Because the lock is held, that version cannot change
  before commit.
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

The lock entry gains one bit per holder, in `lockmgr.cc`; it is
released with the lock.

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

**Verify:** the sum is exact over many runs, and the run is
clean under a thread sanitizer if the build has one.

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

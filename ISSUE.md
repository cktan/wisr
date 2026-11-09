# Issues

Open items found in the reviews of 2026-11-04 and 2026-11-05.
Each states what the code does now, and what closes it.

## 1. Bugs found by reading the code

Found 2026-11-05, while the module documents were written.

**Not reproduced, from reading alone:**

None open.

**Closed 2026-11-06:**

1. **A failed prepare leaked an SSI node.** `prepare` now does
   every step that can fail before `ssi_precommit`
   ([`XCTMGR.md`](sm/XCTMGR.md#101-prepare) §10.1). A test fails
   the prepare by a hook.
2. **The `CF:txn` sweep had no back-off**
   ([`GC.md`](sm/GC.md#86-the-pass-under-a-production-load-open)
   §8.6). A
   sweep now sets `sweep_floor` to the records it left, and the
   next starts only past that plus `txn_sweep_records`.

**Thread sanitizer, closed 2026-11-06.** Found by the dg review
(review at `bc43915c`);
reproduced with `digres-tsan` (`server.sh`) and a program that
grows the session set. `server.sh` and `run.sh` now pass under it
with no report.

3. **`sm_detach()` raced `sm_closedb()`.** It read
   `env->sessions` without `env->mu`, which a session's thread
   holds to erase its own. It now reads a copy taken under the
   mutex.
4. **`start_builder()` raced the builder's threads.** A builder
   thread read `cat->btree_reaper` while `btree_reaper_attach()` set it,
   and the btree reaper's fields were set after the pointer. The
   btree reaper is now attached first, before any builder thread
   exists. Not in that review.
5. **`sm1::lock_status()` took sessions' `fp_mu` in the order of
   an `unordered_set`.** A rehash changes that order, so two
   calls took two in opposite orders: a cycle for the sanitizer,
   not a deadlock today, as the environment's `mu` serializes the
   calls and no other path holds two. It now takes them in
   address order ([`LOCKMGR.md`](sm/LOCKMGR.md#311-mutex-order)
   §3.11). The test lists the locks of
   a set that grows and shrinks; only the sanitizer sees the
   order.

**Closed 2026-11-08:** `resolve_own()` cleared `prepared_gid`
before it resolved. After a lease failure the transaction stayed
in doubt, and only its gid could resolve it. The session now
clears the gid only once the transaction is resolved, or gone. A
test fails the lease by a hook, then commits again.

**Closed 2026-11-08:** `drop_owned_sequences()` could miss an
`own_sequence` committed after the drop's snapshot, and leave a
Sequence that named a dropped table, which nothing could remove.
A test reproduced it at REPEATABLE READ. A commit that dropped a
table now sweeps the table's Namespace for such orphans, and
opening a database sweeps all Namespaces, for a crash between the
commit and the sweep
([`sm/SEQUENCE.md`](sm/SEQUENCE.md#35-orphan-sequences) §3.5).
The sweep costs a
read of each Sequence record in the Namespace; keeping the owners
in memory would remove that cost (§3.5, "Improvement").

**Closed 2026-11-08:** a write that conflicted at REPEATABLE
READ or above ran `xct_fail_retryable`, and so `discard`, with
the key's write latch held, in versioned and in chained stores.
`discard` takes no key latch, so it could not deadlock, but it
drops stores, and [`LOCKMGR.md`](sm/LOCKMGR.md#35-write-latches)
§3.5 says the latch is held only
to check a key and put a version. Another writer of any key that
hashed to the same latch waited for the drop. The latch is now
released first. A hook shows the latch free during the abort, in
a test for each kind of store. Only the error path changed.

**Closed 2026-11-08:** a Sequence's lease renewal held
`alloc_mu` across a synced write, once a block. `alloc_mu` is no
longer the commit sequencer's mutex (`commit_mu` is), but a
commit that renews the timestamp lease takes it under
`commit_mu`, so it waited for the Sequence's write. Sequences now
have a mutex of their own, `seq_mu`. A renewal still holds it
across the write, so draws on other Sequences wait; the mutex
is never held with `alloc_mu`. A hook shows `alloc_mu` free
during a renewal. No test at the mvcc, rel or dg layer can
express it: each needs the hook inside a renewal.

**Closed 2026-11-08:** a prepare after a bulk load was refused
for a reason that no longer held: the PREPARED record lists
explicit locks alone, not the keys written. A closed load now
prepares; an open one still does not (`XCTMGR.md` §10.1). A test
loads into two stores, as one batch and as files, prepares,
restarts, shows a writer of a loaded key blocked, and commits or
aborts by gid; `rel` prepares and aborts a load. The session's
`loaded` flag, which only barred the prepare, is gone. digres
refuses `PREPARE TRANSACTION`, so `dg` cannot reach it.

**Closed 2026-11-08:** a prepared `sm1::rmseq` leaked its lease
row in `CF:meta`: the prepared transaction did not carry its
dropped seqids, so its commit never deleted the leases. It
carries them now, and the PREPARED record lists them after
`dropped`, only if there are any, so an older record reads as
none (`sm/DBMGR.md` §6.5). A test for each case, in one process
and across a restart. Only sm1 can reach it: sm3 does not prepare
DDL, and `rel` and `dg` cannot read `CF:meta`.

**Closed 2026-11-08:** a cursor ended after its transaction was a
double free: the end of a transaction freed sm1's cursor, and
`sm_rel_scanend` and `sm_idx_scanend` then freed it again. The
end of a transaction now closes its cursors and frees none. A read
on one fails with `SM_ERR_SCANCLOSED`; the caller's end frees it,
and the session's next begin or its close frees one nobody ended.
So a cursor ended between the end and the next begin is safe, for
sm2, sm3 and raw handles alike; one ended after the next begin is
still a double free. The cost is that a cursor nobody ends lives
until the next begin. A test in sm1 and one in sm3 crashed with
SIGSEGV before the change (`sm/XCTMGR.md`, `sm/include/sm.h`).

**Closed:** `find_now()` at READ COMMITTED, with a row moved
since the statement's snapshot, gives the conflict with both
kinds of index. A test showed no failure. A move in the narrower
gap between its lookup and its read was not forced.

### Bulk-load fallback ignores `lock_timeout`

**Closed 2026-11-08** by `09b80676`, reproduced the same day.
Found on branch
`wip-insert-select-bulk`, at `62e7a3f2`. An
`INSERT ... SELECT` into a table with rows uses the loader's
ordinary insert fallback. A conflicting uncommitted key makes
it wait, but `lock_timeout` does not end that wait.

**Reproduce in two sessions:**

1. Create a nonempty table, then hold an uncommitted key in
   session A. Keep its transaction open.

   ```sql
   CREATE TABLE timeout_test (id int PRIMARY KEY);
   INSERT INTO timeout_test VALUES (1);
   BEGIN;
   INSERT INTO timeout_test VALUES (2);
   ```

2. Run the query insert in session B.

   ```sql
   SET lock_timeout = '100ms';
   SET statement_timeout = '900ms';
   INSERT INTO timeout_test
   SELECT i FROM generate_series(2, 2) i;
   ```

3. Observe `canceling statement due to statement timeout`
   (SQLSTATE `57014`). The expected error is
   `canceling statement due to lock timeout`
   (SQLSTATE `55P03`), after about 100 ms.
4. Run `ROLLBACK` in session A to release the key.

**Cause.** `smstore_load()` in
`dg/src/backend/access/sm/smstore.c` calls
`waiting(..., false)` for `sm_tbl_load_add()`. This disables
the lock timer even when `tbl_load_add()` falls back to
`tbl_insert()` in `sm/sm3/src/table/table.cc`.

The timer is disabled to keep sorting and file writes from
triggering a lock timeout. Arming it around every load call
would also count time spent doing that work, without a lock
wait. The fix must distinguish lock waits from load work.

**Regression test.** `dg/test/server.sh` now holds a key in
one session and checks that the query insert gets `55P03`
before its statement timeout. Both conflicts now pass: an
uncommitted key and an explicit row lock.

**Fix.** sm now reports lock waits to digres through
`sm_set_lock_wait_hook()`. The hook runs outside the wait mutex.
It arms a timer only when no caller armed one already. An
unexpired timer is disarmed when the wait ends. An expired
timer stays armed until the alarm handler reports it.

The tests also release a holder before the timeout, then run a
later query to confirm that the timer was disarmed. A bulk load
of 50,000 rows with a one millisecond lock timeout passes alone
and in eight sessions. The separate data-modifying CTE fallback
remains in place.

Smaller points:


### A bulk load's sort was interrupted by a stale timer

**Closed 2026-11-09.** `dg/test/server.sh` failed once in a full
`make test` on `main` at `57991774`: a `COPY` into an empty keyed
table, a bulk load, failed with

```text
ERROR:  could not end a bulk load: sort: interrupted
```

**Cause.** A session's timeouts share one sm timer
(`timeout.c`), which raises `INTERRUPT_ALARM` on the session's
thread. PostgreSQL leaves that timer armed when a timeout is
disabled, as a SIGALRM that finds nothing due is harmless
(`schedule_alarm()`). In digres the stale ALARM still lands, and
a load's final sort (`check_interrupt()`, `sm/sm0/src/sort.cc`)
stops on any bit in the wait mask and cannot be retried.
`server.sh` starts its server with `authentication_timeout=1`:
each session's authentication timeouts, disabled at once, rang a
second later, in a COPY that had just started.

1. A session disables its last active timeout, or one of several;
   the timer stays set for the one disabled.
2. The timer fires with nothing due, during the load's final sort.
3. The sort fails with `SM_ERR_INTERRUPTED`, and the COPY with it.

Reproduced by `statement_timeout` set around a `SELECT 1`, then
cleared, then a COPY of 100,000 rows: 11 of 31 delays from 300
to 600 ms failed, every run.

**Fix.** `timeout.c` cancels the timer and takes its ALARM bit when
a timeout is disabled (`cancel_alarm()`), then arms it again for
the earliest timeout left, if any. No timer stays set for a
timeout that is gone.

**Test.** `dg/test/copy_alarm.sh`, sourced by `server.sh`, times a
COPY, then runs 16 COPYs with a stale `statement_timeout` across
the last half of that time: alone, and inside a transaction whose
`transaction_timeout` stays active. Without the fix both fail;
with only the cancel of the last timeout, the second fails.

## 2. Costs to profile

Found 2026-11-06 by `bench/privcheck.sh`, not by a review.

### 2.1 A statement that reads nothing costs digres 12% more

**Closed 2026-11-06: profiled, not fixed.** The superuser's
prepared `select a from e where false`, on an empty table, costs
digres 2,854 more instructions than PostgreSQL 18. The cause is
sm's work for each transaction. The extra cost is in the session,
not in a background thread: an idle digres runs about 12,000
instructions a second.

**By owner.** Exact instructions a statement, at 4abf45ee, on the
aarch64 WSL2 box. `t` is the statement above, `pt` a warm point
read of `narrow`.

| Owner | `t` digres | `t` PostgreSQL | `t` gap | `pt` gap |
| --- | --- | --- | --- | --- |
| PostgreSQL's code | 16,586 | 20,713 | -4,127 | -15,954 |
| sm | 3,329 | 0 | 3,329 | 5,216 |
| RocksDB | 4 | 0 | 4 | 7,271 |
| libc malloc and free | 1,504 | 0 | 1,504 | 1,522 |
| libstdc++ new and delete | 658 | 0 | 658 | 828 |
| digres's adapter, `access/sm` | 859 | 0 | 859 | 1,930 |
| aa | 161 | 0 | 161 | 177 |
| libc, the rest | 2,652 | 2,103 | 549 | 1,016 |
| the loader and libgcc | -143 | -59 | -84 | 0 |
| **Total** | **25,611** | **22,757** | **2,854** | **2,006** |

**What digres adds.** Five calls make a fixed cost, 7,522
instructions a statement in `t`. They include code of each owner
above. `pt` pays the same within 36 instructions. Each runs once
a transaction:

| Call | Instructions | What it does |
| --- | --- | --- |
| `sm_tbl_open` | 2,748 | opens the table's handle, under `LockRelationOid` |
| `sm_commit` | 1,748 | commits sm's transaction |
| `sm_tbl_close` | 1,193 | closes the handle, in `close_tables()` |
| `pg_global_post_commit` | 1,015 | digres's work after a commit |
| `sm_begin` | 818 | begins sm's transaction |

The open and the close, 3,941 together, are more than the whole
gap. The table's lock is inside the open.
[`SM_API.md`](sm/SM_API.md) makes this
the contract: an `sm_tbl_t` lasts one transaction. So the cost
comes from the design and is not a bug. digres makes 10 calls of
`operator new` a statement, some of them in the open, where
PostgreSQL's `t` calls no `malloc`.

**What digres saves.** PostgreSQL's code runs 4,127 fewer
instructions. Its lock manager and light locks are the most of
it: `LockAcquireExtended`, `LWLockAcquire`, `LWLockRelease`,
`LockReleaseAll`, `FastPathUnGrantRelationLock`, 1,755 together.
Its hash walks and lookups are 948 more (`hash_seq_search`,
`hash_bytes`, `hash_search_with_hash_value`). The catalog cache
is 355, and `pgstat_report_activity` 239.

**Why the point read's gap is smaller.** The fixed cost is the
same. The read itself costs digres less: `pt` less `t` is 19,218
on digres and 20,065 on PostgreSQL, 847 fewer.

**How to repeat it.**

1. Run each server under `valgrind --tool=callgrind`, one server
   a run, on data already loaded.
2. Run prepared pgbench for two lengths, as 2,000 and 12,000 for
   `t`. For `pt`, run 150,000 and 170,000, so that the diff holds
   only warm statements.
3. Take each function's count, the second run less the first,
   over the difference in statements.
4. For PostgreSQL, read the backend's dump, the largest of them.

The totals agree with `perf stat`: 25,611 against 25,600, and
22,757 against 22,763.

**Warnings for a profile on this box.**

- Do not use `perf record` for symbols. A statement repeats the
  same instructions, so a fixed period aliases: `aa_commit` read
  454 in one run and 58 in the next. The totals agree, so it does
  not show. The sample rate is also capped at 2,250 a second.
- Trust callgrind's self costs. Its inclusive costs near the top
  of the stack are wrong, as aarch64 tail calls leave frames that
  never return: `PortalStart` read 179 million. Below that, read
  the raw call edges, and use a function only if its callers'
  charge equals its self cost and its children's. A child's name
  can still be wrong.
- `perf stat -p` counts a forked child. A child, probably
  autovacuum, raised PostgreSQL's `t` to about 44,900 in two
  profile runs. Count the backend alone, or set
  `autovacuum = off`.

**Not done: a fix.** A handle kept across transactions might
save part of the open and the close; the saving is not measured,
and the lock must still be taken. It changes
[`SM_API.md`](sm/SM_API.md)'s
contract, and a catalog change would have to reach a kept handle.
It is not designed, and no case has been checked.

### 2.2 `sm_tbl_open` is too heavy

**Closed 2026-11-08: built, and at parity.** A statement that
reads nothing costs digres 1% less than PostgreSQL, and the same
with 16 secondary indexes as with none. It was 12% more, and 3.5%
more on 2026-11-06. The target is parity on `WHERE false`.

**Measured again 2026-11-08** by `bench/idxopen.sh`, at
`823d0e85`, on the release build, aarch64. Instructions a
statement, prepared, superuser, by index count. The text and
tables below this one are the measure of 2026-11-06, kept as
history.

| Statement | Index kind | System | 0 | 1 | 4 | 16 |
| --- | --- | --- | --- | --- | --- | --- |
| `WHERE false` | lsm | digres | 22,288 | 22,285 | 22,285 | 22,286 |
| | btree | digres | 22,290 | 22,306 | 22,321 | 22,316 |
| | btree | PostgreSQL | 22,405 | 22,405 | 22,410 | 22,513 |
| point read | lsm | digres | 40,724 | 42,653 | 44,758 | 43,694 |
| | btree | digres | 40,706 | 42,053 | 41,876 | 41,874 |
| | btree | PostgreSQL | 39,932 | 39,976 | 40,185 | 40,325 |

- `WHERE false` is 0.990 to 0.996 of PostgreSQL's. The spread of
  PostgreSQL's cell at 16 indexes is 15.7%; digres's are 0.05%.
- A point read is 1.02 to 1.11 of PostgreSQL's: 1.04 to 1.05 with
  btree indexes, 1.07 to 1.11 with lsm indexes at 1 to 16.
  How a point read's cost moves with the data is §2.3.

**What it does now.**

1. `sm_tbl_close()` (`sm/src/sm.cc`) parks the handle on the
   session.
2. `sm_tbl_open()` takes a parked handle back, and calls
   `sm3::tbl_reopen()`. This takes the table-oid lock and reads
   the definition, in each transaction.
3. The same definition keeps the rel and the indexes. A new one
   makes them again. The index states are read again only if
   `cat_epoch()` has moved.
4. An index opens at its first use (`use_index()` in
   `sm/sm3/src/table/table.cc`).
5. A parked handle's age starts at 128. It halves at the end of
   each transaction that opened a table, and at 0 the handle is
   freed. A session keeps at most 1,024.

[`SM_API.md`](sm/SM_API.md)'s contract stays: a handle is good
for one
transaction. digres does not change. The reopen must read the
definition under the catalog's mutex: a commit of DDL releases
the table-oid lock before it publishes, so a check without the
mutex could keep the old definition.

**Measured** by `bench/idxopen.sh`, on the data each run loaded.
Instructions a statement, prepared, superuser, on 2026-11-06.

| Statement | Index kind | System | 0 | 1 | 4 | 16 |
| --- | --- | --- | --- | --- | --- | --- |
| `WHERE false` | lsm | digres, before | 25,653 | 26,675 | 29,243 | 39,647 |
| | lsm | digres, now | 23,563 | 23,559 | 23,559 | 23,563 |
| | btree | digres, before | 25,649 | 26,793 | 29,594 | 39,842 |
| | btree | digres, now | 23,558 | 23,604 | 23,595 | 23,599 |
| | btree | PostgreSQL | 22,794 | 22,761 | 22,773 | 22,871 |
| point read | lsm | digres, before | 42,315 | 43,505 | 46,520 | 66,076 |
| | lsm | digres, now | 40,299 | 41,274 | 41,995 | 48,676 |
| | btree | digres, before | 42,848 | 47,887 | 50,548 | 63,248 |
| | btree | digres, now | 40,370 | 44,634 | 44,775 | 44,828 |
| | btree | PostgreSQL | 41,174 | 41,214 | 41,421 | 41,519 |

The saving at no secondary index is about 2,090 a statement. The
gap left on `WHERE false` is about 800.

**Not measured:** what opening on demand saves. It helps only a
session's first open of a table, and a first open after DDL, and
a slope leaves both out. `sm3/test/test_lock.cc` shows which
indexes open.

**Where an open's instructions go now.** Exact counts by
callgrind, by the method in §2.1, on `WHERE false`. Edges are
approximate near tail calls: callgrind charges `sm_tbl_open`
1,388, of which 321 is digres's insert into `open_tables()`.

| Path | Part | Instructions |
| --- | --- | --- |
| Reopen, 0 or 16 indexes | all of it | about 1,070 |
| | the table-oid lock | about 570 |
| | the definition's read | about 400 |
| | the rest | about 100 |
| First open, no index | all of it | about 3,700 |
| First open, 16 indexes | all of it | about 7,900 |

- A reopen reads no storage: RocksDB's code runs 4 instructions a
  statement, and the index states are not read.
- `sm_tbl_close` costs 99, and aging 33 at a commit.
- A first open grows by about 262 an index, all in
  `load_states()`. It takes the catalog's mutex once for each
  index, used or not. Taking it once for the loop would save part
  of this, on first opens only.
- The definition's read must stay under the mutex. A check of the
  epoch under the mutex, in place of the lookup, might save about
  200. This is an estimate.

**A plan's sizes are read once a session.** A session's first
plan of a table calls `approxsize` for the table and for each
index: 17 calls and about 61,700 instructions with 16 indexes, in
RocksDB's `GetApproximateSizes`. `kept_estimate()` in `smstore.c`
keeps the answers by epoch, so later plans make no such call.

Each statement planned, by the simple protocol, on the same
tables, median of 4 runs:

| Table | digres | PostgreSQL |
| --- | --- | --- |
| no secondary index | 55,956 | 57,303 |
| 16 indexes | 93,632 | 110,006 |

**Not explained:** a new connection's statement costs about
630,000 more on the table of 16 indexes than on the table of
none. The size calls are about a tenth of that.

**Tests:** `sm/rel/test/test_reopen.rel`,
`sm/sm3/test/test_lock.cc`, `dg/test/parked_tables.sh`. An
isolation scenario cannot express this: the change is above sm1.

### 2.3 A point read's cost depends on the state of the data

**Open.** The point-read rows of §2.2 are not a clean effect of
the index count. They move with the data's state, and the cause
is not known.

On one data directory, with the server restarted, a build that
opens every index at the open and the build that opens on demand
agree to 0.4%:

| Table | Open on demand | Open all | The same table before the restart |
| --- | --- | --- | --- |
| lsm, 0 indexes | 42,700 | 42,850 | 40,299 |
| lsm, 4 indexes | 42,990 | 42,990 | 41,995 |
| lsm, 16 indexes | 44,560 | 44,565 | 48,676 |
| btree, 1 index | 43,450 | 43,450 | 44,634 |

So opening on demand does not cost a point read. But:

- After the restart, digres's read of a table with no secondary
  index costs more than PostgreSQL's 41,174. Before it, less.
  Do not quote digres as cheaper than PostgreSQL on a point read.
- Runs that load the data separately differ: lsm with 16 indexes
  gave 45,771 on one build and 48,676 on the next, though the
  rounds of each agree to 1%. Why a load's outcome differs is not
  known.

**First to do:** load once, then compare a table's count before
and after a flush, and after a compaction, with `rocksdb` level
counts beside each. An lsm index's entries share the table's
store, so a read may search more files. That is a guess.

**Warning:** this box's clock can step backwards. After a
sabotage, delete the object file: `touch` may not rebuild it.

### 2.4 A `COPY` into a table with a unique B-tree Index is slow

**Closed 2026-11-07** by
[`sm/BTREELOAD_PLAN.md`](sm/BTREELOAD_PLAN.md): a load builds
its trees from the bottom up
([`sm/BTREEINDEX.md`](sm/BTREEINDEX.md#67-a-bulk-load-builds-the-tree-whole)
§6.7).

`bench/sortload.sh` with `KIND=btree`: a `COPY` of 5 million
rows, in no key order, into an empty table:

```sql
CREATE TABLE t (id int8 PRIMARY KEY, email text, n int8);
CREATE UNIQUE INDEX t_email ON t USING btree (email);
CREATE INDEX t_n ON t USING btree (n);
```

| System | Instructions a row | Time, median of 3 |
| --- | --- | --- |
| digres, rows inserted | 494,733 | 825 s |
| digres, trees built | 20,819 | 12.0 s |
| PostgreSQL 18 | 43,534 | 59.4 s |

Inserted, digres was 11 times PostgreSQL's instructions. The
rounds agree to 1% in instructions. The inserted times spread
from 749 to 871 s, as builds ran on the box at the same time.
With LSM Indexes, the same load costs digres 27,195
instructions a row and 14 s.

**The cause.** A load into an empty table writes its rows to
sorted files ([`sm/BULKLOAD.md`](sm/BULKLOAD.md#5-file-mode) §5).
A unique tree stops this:

1. `tbl_load_open()` (`sm/sm3/src/table/table.cc`) sends a table
   with a unique tree to row-by-row inserts, as the tree's check
   reads rows, and a file's rows are in no store yet.
2. So each row goes through RocksDB's WAL and memtable, and the
   primary key is checked.
3. Each tree's entry latches its leaf and writes the whole 4 KB
   page: about 40 GB of page writes for the load, then flushed
   and compacted.

A non-unique tree alone keeps the files for the rows, but still
writes a page for each entry (`idx_load()` in
`sm/sm2/src/rel/index.cc`).

**What closed it.** The load sorts each tree's entries and
builds the tree from the bottom up, one write a page, into the
load's files. A unique tree's duplicate is found by the sort,
with no row read.

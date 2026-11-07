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
   (`XCTMGR.md` §10.1). A test fails the prepare by a hook.
2. **The `CF:txn` sweep had no back-off** (`GC.md` §8.6). A
   sweep now sets `sweep_floor` to the records it left, and the
   next starts only past that plus `txn_sweep_records`.

**Thread sanitizer, closed 2026-11-06.** Found by `DG_REVIEW.md`;
reproduced with `digres-tsan` (`server.sh`) and a program that
grows the session set. `server.sh` and `run.sh` now pass under it
with no report.

3. **`sm_detach()` raced `sm_closedb()`.** It read
   `env->sessions` without `env->mu`, which a session's thread
   holds to erase its own. It now reads a copy taken under the
   mutex.
4. **`start_builder()` raced the builder's threads.** A builder
   thread read `cat->cleaner` while `cleaner_attach()` set it,
   and the cleaner's fields were set after the pointer. The
   cleaner is now attached first, before any builder thread
   exists. Not in `DG_REVIEW.md`.
5. **`sm1::lock_status()` took sessions' `fp_mu` in the order of
   an `unordered_set`.** A rehash changes that order, so two
   calls took two in opposite orders: a cycle for the sanitizer,
   not a deadlock today, as the environment's `mu` serializes the
   calls and no other path holds two. It now takes them in
   address order (`LOCKMGR.md` §3.11). The test lists the locks of
   a set that grows and shrinks; only the sanitizer sees the
   order.

**Closed:** `find_now()` at READ COMMITTED, with a row moved
since the statement's snapshot, gives the conflict with both
kinds of index. A test showed no failure. A move in the narrower
gap between its lookup and its read was not forced.

Smaller points:

- A prepared `sm1::rmseq` leaks its lease row in `CF:meta`. This
  is harmless, as a seqid is not reused.
- A Sequence's lease renewal holds `alloc_mu`, the commit
  sequencer's mutex, across a synced write, once a block.
- `xct_fail_retryable` runs `discard` with the write latch held.
  It takes no key latch, so it looks harmless, but it drops
  stores there, and `LOCKMGR.md` says the latch is held only to
  check a key and put a version.
- `drop_owned_sequences()` can miss an `own_sequence` committed
  after the snapshot, and leave a Sequence that names a dropped
  table. Not tested.
- A cursor ended after its transaction is a double free. The end
  of a transaction frees sm1's cursor, and `sm_rel_scanend` and
  `sm_idx_scanend` then use it: valgrind shows it, and the
  process aborts. digres cannot reach it, as its portals end
  scans first. `sm.h` and `SM_API.md` state the rule. If sm gets
  a second host, sm3 could keep a list of each session's open
  scans, and clear their sm1 cursors at commit and abort: then
  `tbl_scanend` frees only its wrapper, at the cost of a list
  update on every scan.
- `resolve_own()` clears `prepared_gid` before it resolves. After
  a lease failure the transaction stays in doubt, and only its
  gid can resolve it.
- A prepare after a bulk load is refused, but the reason it had
  no longer holds (`XCTMGR.md`).

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
gap. The table's lock is inside the open. `SM_API.md` makes this
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
and the lock must still be taken. It changes `SM_API.md`'s
contract, and a catalog change would have to reach a kept handle.
It is not designed, and no case has been checked.

### 2.2 `sm_tbl_open` is too heavy

**Built, target not agreed.** A statement that reads nothing now
costs digres 3.5% more than PostgreSQL, not 12%, and costs the
same with 16 secondary indexes as with none. No target was set,
so this stays open until one is.

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

`SM_API.md`'s contract stays: a handle is good for one
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

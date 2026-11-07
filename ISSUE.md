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

**Open.** `sm_tbl_open` costs 2,748 instructions a transaction
for each table, and `sm_tbl_close` 1,193. Together they are more
than digres's whole gap of 2,854 to PostgreSQL on a statement
that reads nothing (§2.1). PostgreSQL's two `LockRelationOid`
calls on that statement cost 1,379; digres's cost 4,682, with the
open inside the first. Find what an open can stop doing, then
make it lighter.

**What it does now.** Each transaction that touches a table, for
each table:

1. `sm_tbl_open()` (`sm/src/sm.cc`) allocates an `sm_tbl_t`,
   which holds an `unordered_map` and an `optional`.
2. `open_table()` (`sm/sm3/src/table/table.cc`) allocates a
   `table_t`.
3. `cat_lock_table()` takes the table-oid lock and returns the
   definition.
4. `load_states()` reads the indexes' states.
5. `open_rels()` builds the `rel`, then every index of the table,
   used or not.
6. `close_tables()` in `dg/src/backend/access/sm/smstore.c` frees
   all of it at commit.

The lock must happen in each transaction. Whether the rest must
is the question. The definition is shared, and the layouts are
made once with it, yet the objects built from them are built
again each time.

**Where the instructions go.** Exact counts by callgrind, at
4abf45ee, for a table with a primary key and no other index. The
method and its limits are in §2.1. The split is approximate, as
aarch64 tail calls blur the edges below `sm3::tbl_open`.

| Part | Instructions |
| --- | --- |
| `sm_tbl_open`, all of it | 2,748 |
| `sm3::tbl_open`, within it | 2,278 |
| `wisr::lock_oid`, the lock | 752 |
| `sm2::open_rel`, the `rel` | 869 |
| `sm2::idx_open_primary_layout`, the primary key | 387 |
| `sm_tbl_t`, the guard and the rest of `sm_tbl_open` | 470 |

The lock is about a quarter of the open, and a kept object could
not save it. `sm_tbl_close` mirrors the build: 1,193
instructions, the most of it closing the `rel` and the index.

**Not measured, and first to do: a table with more indexes.**
`open_rels()` loops over every index, and each secondary index
goes through `sm2::idx_open_layout`. So an open should grow with
the index count, even for a read by the primary key. Measure
`t` and `pt` on a table of 0, 1, 4 and 16 secondary indexes, by
the method in §2.1, with `bench/privcheck.sh`'s statements.

**In particular: open an index on demand.** This is decided. An
open builds the `rel` and takes the lock, and an index is opened
when it is first used, not in `open_rels()`. A read by the
primary key then opens no secondary index. A write opens the
indexes it maintains. This keeps `SM_API.md`'s contract. To
settle:

- Every reader of `table->indexes` must go through the open on
  demand, as the vector is filled at the open today. `table.cc`
  reads it in many places, by position, so an entry not yet
  opened must be a null that each one handles. `close_rels()`
  must skip those.
- A first use that fails must report its error where the caller
  can take it, as the open's failure does now.

**Other candidates.** These are not designed, and neither is
recommended. Either can go with the open on demand:

- **Reuse the objects within a session**: a free list of
  `sm_tbl_t`, `table_t`, `rel` and index objects. This removes
  allocations, and the contract stays.
- **Keep a handle across transactions**, and take only the lock
  at each open. This saves the most, and changes `SM_API.md`'s
  contract: an `sm_tbl_t` lasts one transaction.

**Before building,** answer these for each change:

- A definition that changes between two transactions: ALTER,
  DROP INDEX, CREATE INDEX pending, TRUNCATE, a rewrite that
  moves the table to a new store.
- `load_states()`: an index's state read late, or kept too long.
- Abort, savepoint rollback and a handle opened in a
  subtransaction (`smstore.c` closes those at the rollback).
- `fresh`, a table this transaction made, and temporary tables.
- The cleaner's note set on a tree index in `open_rels()`.
- What `rel` and an index hold of the transaction: the session,
  a snapshot, a cursor, a pin. Read `sm/sm2/src/rel/rel.cc` and
  `index.cc` to settle this first.

**Rules for the work.** Test at each layer the change reaches
(`sm/sm3/test`, `sm/rel/test`, `dg/test`), and add an isolation
scenario if a change moves what a transaction sees. Compare with
PostgreSQL, in instructions.

**Done when:** a target is agreed, the design is chosen, and `t`
and `pt` measure under it. No target is set yet.

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

**The question.** The superuser's prepared `select a from e
where false`, on an empty table, costs digres 2,837 more
instructions than PostgreSQL 18. No row is read, and the
superuser skips every privilege check. Find what the 2,837
instructions are. This item asks for a profile, not a fix.

**The numbers.** User instructions a statement, superuser,
prepared, the median of 5 rounds, at 3623ce73, on the aarch64
WSL2 box. `dg/WORKNOTE.md` step 400 has the role's numbers.

| Statement | digres | PostgreSQL | digres over |
| --- | --- | --- | --- |
| `t`: `select a from e where false` | 25,600 | 22,763 | 2,837 |
| `fn`: `t` with `length(c)` | 26,358 | 23,765 | 2,593 |
| `fn16`: `t` with 16 `length(c)` | 49,709 | 56,583 | -6,874 |
| `pt`: a point read of `narrow` | 45,089 | 43,168 | 1,921 |

**What is known.**

- Compiler flags do not explain it. digres and PostgreSQL 18 are
  both `-O2`; `pg_config --cflags` and `digres-release --build`
  show it. sm is `-O3`. Neither build has assertions.
- The cost looks fixed per statement, not per expression. A
  function adds about 1,507 instructions to digres and 2,114 to
  PostgreSQL (`fn16` less `t`, over 16).
- A real row narrows the gap: `pt` is 1,921 over, not 2,837.
- The simple protocol shows almost no gap: `t` was 55,405 and
  55,111, a gap of 294. That run's rounds differ by about 2%,
  so the gap is uncertain. digres's parser is `sq`, so a cheaper
  parse may hide the same cost there. Not checked.

**Not known: a count may include other threads.** `perf stat -p`
counts every thread of the server. The 0.00% spread does not rule
out a background thread, as one that runs at a steady rate adds a
steady count. PostgreSQL's background processes count too, by
inheritance. Measure this first.

**Steps.**

1. Build the release binary: `make -C dg release -j4`.
2. Count only the session. Use `perf stat --per-thread`, or
   count an idle server for the same time. Correct the 2,837 if
   it changes.
3. Start each server as `bench/privcheck.sh` does: pinned by
   `taskset`, `shared_buffers = 1GB`, the tables of its
   `prepare()`.
4. Run `pgbench -n -M prepared -c 1 -t 200000 -f t.sql` under
   `perf record -e instructions:u -p <pid>`, once for each
   server. The script leaves `t.sql` in
   `bench/build/privcheck/`.
5. Compare the two flat profiles by symbol, with `perf report
   --stdio --sort symbol`. Sampling works on this box.
6. Group digres's symbols by owner: PostgreSQL's code, `dg/`'s
   changes, `aa/`, `sm/`, RocksDB, libc. Add call graphs
   (`--call-graph dwarf`) only for the symbols that differ.
7. Repeat for `pt`, to explain why its gap is smaller.

**Where to look first.** These are guesses, not findings:
sm's transaction begin and commit for each statement, a table's
lock through sm1, the checks at each lock that the catalog has
not moved (`smstore_catalog_moved()`, `sm_cat_publishes()`,
`aa_objects_stale()`), and thread-local access in a threaded
server.

**Rules for the work.**

- Compare digres with PostgreSQL, not with older digres commits.
- Compare instructions. Timing on this box swings with the host's
  clock.
- `bench/privcheck.sh` deletes `bench/build/privcheck` when it
  starts. Keep data and logs under `bench/build/`, and sockets
  in a short directory such as `dg/build/<short>`.
- A later fix may change only code that digres added or changed,
  not code digres shares with PostgreSQL, and may use no trick
  PostgreSQL lacks.

**Done when:** the 2,837, as corrected by the count of the
session alone, is accounted for by owner and by symbol, to within
about 10%. Record the result here, with any fix proposed as a new
item in `sm/NEXT.md`.

# Issues

Open items found in the reviews of 2026-11-04 and 2026-11-05.
Each states what the code does now, and what closes it.

## 1. Bugs found by reading the code

Found 2026-11-05, while the module documents were written.

**Not reproduced, from reading alone:**

1. **A failed prepare leaks an SSI node.** A prepare that fails
   after `ssi_precommit` passed leaves the node committed with
   `end_ts` at `UINT64_MAX`. `ssi_cleanup` never frees it, and it
   counts as a first committer for ever, which can cause false
   failures. It needs a failed lease write, and sm1 has no way to
   inject one.
**Closed 2026-11-06:** the `CF:txn` sweep had no back-off
(`GC.md` §8.6). A sweep now sets `sweep_floor` to the records it
left, and the next starts only past that plus `txn_sweep_records`.

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

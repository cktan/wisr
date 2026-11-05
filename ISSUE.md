# Issues

Open items found in the reviews of 2026-11-04 and 2026-11-05.
Each states what the code does now, and what closes it.

## 1. Possible bugs found by reading the code

Found 2026-11-05, while the module documents were written. Each
comes from reading the code. None was reproduced, and none has a
test. Each needs a test first: a test that does not fail closes
the item.

1. **A row moved during an index build fails the build.**
   `tbl_index_backfill()` reads a row by `rel_read_latest()`, which
   gives `SM_ERR_MOVED` for a row moved since the batch's scan. That
   is no retryable error, so the index ends failed. The comment
   there says such a row is simply gone from its key, and its mover
   made its entry.
2. **`find_now()` has the same shape.** At READ COMMITTED, a moved
   row would fail an insert with `SM_ERR_MOVED`, where the arbiter
   should treat the key as free.
3. **`nextval` on an old snapshot restarts a dropped Sequence.**
   `nextval()` takes no lock. After a drop commits, a transaction
   on an older snapshot finds the `"S"` row, with its state and lease
   gone, and restarts from `start`.
4. **A Sequence committed after a REPEATABLE READ snapshot.**
   `cat_get_sequence()` finds it, but `nextval()` gives
   `SM_ERR_NOTFOUND`.
5. **`drop_owned_sequences()` reads at the snapshot.** `drop_table()`
   can fail with `SM_ERR_NOTFOUND` when a Sequence committed in the
   namespace after the snapshot. It can also miss an `own_sequence`
   committed after the snapshot, and leave a Sequence that names a
   dropped table.
6. **An exhausted Sequence gives its last value again after a
   reopen.** `exhausted` is in memory only, and the lease holds
   `UINT64_MAX`, so the first draw gives that value again.
7. **A failed prepare leaks an SSI node.** A prepare that fails after
   `ssi_precommit` passed leaves the node committed with `end_ts` at
   `UINT64_MAX`. `ssi_cleanup` never frees it, and it counts as a
   first committer for ever, which can cause false failures.
8. **The `CF:txn` sweep has no back-off** (`GC.md`). An abandoned
   pass, or a snapshot that keeps `W0` low, leaves the count above
   the threshold, so the next run starts another full pass.
9. **`sample_by_key` may overestimate `live_rows`.** It multiplies
   by `tbl_size`, which counts older versions, and divides by
   `tbl_range_size`, which does not. So `live_rows` may run high on
   a table with many old versions, and the planner's row counts
   with it.

Smaller points:

- A prepared `sm1::rmseq` leaks its lease row in `CF:meta`. This is
  harmless, as a seqid is not reused.
- A Sequence's lease renewal holds `alloc_mu`, the commit
  sequencer's mutex, across a synced write, once a block.
- `xct_fail_retryable` runs `discard` with the write latch held. It
  takes no key latch, so it looks harmless, but it drops stores
  there, and `LOCKMGR.md` says the latch is held only to check a key
  and put a version.

# Find a memory leak: reduce the SQL to rel, then run valgrind

A soak run says that the server's memory grows. It does not say
where. This method finds the allocation that causes it. It works
because `rel` drives sm without SQL, the planner or the executor,
and valgrind can run `rel` to the end.

## 1. When to use it

- The soak (`SOAK.md`) shows a PSS slope over its limit.
- A growth that follows writes, not reads, is the best case:
  the workload is easy to reduce.

## 2. Method

1. Find the smallest workload that still grows.
   - Change one part of the mix at a time. Here reads were flat
     and updates alone gave the full curve (`MIX=updateonly` in
     `soak.sh`).
   - Do this on the digres server first, with the sampler's
     `smaps` log (`maps.log`) to see which mappings grow.
2. Write the same workload as a `rel` script.
   - Same tables, same keys, same indexes
     (`index-create ... :tree true` for digres's `using btree`).
   - Same sessions: open and close one for a transaction to
     imitate `pgbench -C`.
   - Print the PSS after each round, from
     `/proc/self/smaps_rollup`.
   - See `leakrel.rel` (one table) and `leakrel2.rel` (the school
     of the soak).
3. Check that `rel` also grows. If it does not, the cause is
   above sm: the SQL layer, not the storage stack.
4. Run `rel` under valgrind to the end, and detach cleanly.
   - End the script with `(env-detach :force true)`, so sm and
     RocksDB free what they own before the process exits.
   - Run:

         valgrind --leak-check=full \
           --show-leak-kinds=definite,indirect \
           --num-callers=24 --log-file=memcheck.log \
           ../sm/build/release/rel leakrel2.rel

   - Keep the run short: 10,000 to 20,000 transactions. Valgrind
     is slow, and a leak shows from the first transactions.
5. Read the leak summary.
   - After a clean detach, nothing should be lost: definitely,
     indirectly and possibly lost must all be 0 bytes.
   - A block "lost" has a stack in the log: that is the leak.
6. If nothing is lost but the PSS still grows, the memory is
   reachable at exit or held by the allocator. Run `massif`:

       valgrind --tool=massif --depth=30 \
         --massif-out-file=massif.out ../sm/build/release/rel x.rel
       ms_print massif.out

   - A live heap that rises with the work is a structure that
     grows. `ms_print` gives the allocation sites.
   - A live heap that stays flat while the PSS rises is memory
     the allocator holds, or memory outside malloc.

## 3. What a clean exit looks like

- Definitely, indirectly and possibly lost: 0 bytes.
- Still reachable: about 29 KB in 582 blocks, the same after 900
  and after 12,000 transactions. They come from static
  initialisers that run at load time: RocksDB's option registry
  and gflags. They are not a leak.

## 4. Tips

- Use the release build of `rel`: the debug build in
  `sm/build/rel` is `-O0` and may be stale. Build:

      make -C sm -j4 BUILD=build/release \
        OPTFLAGS='-O3 -DNDEBUG' build/release/libsm.a
      make -C sm/rel -j4 BUILD=../build/release

- `rel` links the system `librocksdb`, not the static copy of
  digres. A difference in allocator behaviour is possible.
- `rel` has one thread, so its sessions take turns. It cannot
  show a leak that needs two sessions at the same time.
- Put the home under `bench/build/`, not `/tmp` (`REL_TMP`).
- A sawtooth in the PSS is not a leak. Memory that rises, stays,
  then falls in one step is held until a flush or a reaper pass.
  Run long enough to see two drops, and compare the floors: a
  floor that rises from drop to drop is a leak.

## 5. Result of the first use (20261110)

- `leakrel.rel`, one table, 600,000 updates: PSS rose 4.5 MB per
  20,000 updates, then fell 40 MB in one step.
- `leakrel2.rel`, two tree indexes and 9 sessions, 2.4 M
  updates: PSS rose from 83 to 207 MB in steps, no drop.
- With 1 KB rows (`PAD=1000`): drops at 1 M updates; the second
  floor was 40 MB over the first, not settled.
- Valgrind memcheck, 12,000 updates: 0 bytes lost at exit.
- Massif, 12,000 updates: the live heap stayed at 143 to 145 MB.
- Massif, 120,000 updates: the live heap rose from 142 to 172 MB,
  the same 4.9 MB per 20,000 updates as the PSS. By allocation
  site, at the start, the middle and the end:

  | site | MB, start | MB, middle | MB, end |
  | --- | --: | --: | --: |
  | `bufpool_t` (shared buffers / 4) | 67 | 67 | 67 |
  | `hint_queue_t::push` (one buffer) | 34 | 34 | 34 |
  | `rocksdb::Arena::AllocateNewBlock` | 11.5 | 24.1 | 32.5 |
  | Janet, btree reaper, the rest | 38 | 38 | 38 |

  Only the memtable arena of RocksDB grows. Of its 32.5 MB at
  the end, 24 MB came from `chain_write` (the row chains) and
  6 MB from `finish_commit` (the commit records in CF:txn). So
  the growth is live data in memtables, not a lost block. It is
  bounded by `write_buffer_size` (64 MB) twice for each column
  family, until a flush frees it.

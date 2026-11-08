# NEXT — what to build next, and in what order

This file holds only what is still to do, across levels. What
is built or settled is in the design documents: `DESIGN.md`
§3 has where each level stands, and `sm3/SM3_DESIGN.md` §4
has sm3's own order of work.

---

## 1. Next

The spike `PART4.md` §7 aimed the stack at is long past:
digres runs on it (`PART4.md` §10). Statistics for a planner
are built, the stack's part and digres's
(`../dg/STATISTICS.md`, `../dg/WORKNOTE.md` steps 74 to 78),
and so are the catalog tables psql reads
(`../dg/SYSCATALOG.md` §6).

1. **digres's key-column ALTERs** (`CLUSTER.md` §11, step 3):
   built. ADD and DROP PRIMARY KEY, and of a unique constraint
   (`../dg/PKDDL.md`), and `ALTER COLUMN TYPE` of any column, a
   key column's among them (`../dg/ALTERTYPE.md`,
   `../dg/WORKNOTE.md` step 378). Left: the dependents
   PostgreSQL makes again, an expression or a partial index, a
   foreign key, an identity column's sequence, which digres
   refuses (`ALTERTYPE.md` §8). This item is done.
3. **A secondary index's scan, within 1.5 times PostgreSQL's**
   (`BTREEINDEX.md`, `../PARITY.md`, `../PERF.md`). An index
   scan may cost digres no more than 1.5 times what it costs
   PostgreSQL; a sequential scan may, as OLTP runs none. The
   measure is three queries of `../bench/run.sh` on
   `usertable`, 10,000 rows, with btree indexes. Instructions a
   statement, from `../PARITY.md` §3.2:

   | Query | What it reads | digres | PostgreSQL | Ratio |
   | --- | --- | --- | --- | --- |
   | q1 | 8 entries of the `lastname` index, a count | 63,071 | 60,170 | 1.05 |
   | q2 | 183 entries of the `state` index, a count | 241,411 | 148,062 | 1.63 |
   | q3 | 8 rows, found by the `lastname` index | 176,393 | 86,009 | 2.05 |

   Since then, `7f381103` made q2 4.3% cheaper. Latency is 1.19,
   1.29 and 1.74 times PostgreSQL's (`../PERF.md`). The fixed
   cost of a statement is at parity, so the gap is the cost of
   an entry and of a row.

   - **q1 passes.** The gap is its 8 entries and one descent.
   - **q2 fails by the cost of an entry**: about 1,000
     instructions, against PostgreSQL's 530. The tree cursor
     takes about half of it, the executor the rest. In order:
     1. **Copy an entry once**: built. sm3's batch views the
        cursor's copy of one leaf, and ends with it
        (`BTREEINDEX.md` §5.3). q2 went from 245.5k to 230.0k
        instructions, 1.52 times PostgreSQL's 150.8k; q1 and q3
        about 1% fewer (aarch64 under WSL2, 2026-11-07).
     2. **Hand out a leaf's entries from the pinned leaf**:
        dropped (2026-11-07). PostgreSQL copies each entry too:
        `_bt_saveitem()` copies an index-only scan's tuple, then
        unlocks the page. A pin is a latch (`BUFMGR.md` §1.3),
        so a pin held across calls would make writers wait on
        the reader. A leaf id kept as a hint, pinned only if in
        the pool, was measured: q2 0.2% fewer instructions, q1
        1.2% and q3 0.3% more, as a hit saves only page 0's
        search and a random probe's miss costs a pin.
     What is left of q2's gap is the cursor's per-entry work and
     the executor. Parity needs a count in sm, one tuple a leaf,
     which helps `count(*)` alone.
   - **q3 fails by the cost of a row**: about 13,200
     instructions beyond its entry, against PostgreSQL's 3,200.
     RocksDB's `MultiGet` takes 9,000 of it, and RocksDB's
     settings are already tuned. With no cost in RocksDB at all,
     q3 is still 1.2 times PostgreSQL. Two large changes close
     the gap:
     - **A row cache in sm's buffer pool**, keyed by the row's
       key and version: about 1.35 times PostgreSQL. It needs a
       design for the version in the key and for invalidation.
     - **A table's rows in sm's own pages**, as a tree keeps an
       index's entries: parity, and a redesign of the row store.

   For lsm indexes, the default, q1 to q3 are 1.41, 2.96 and
   2.40 times PostgreSQL's latency (`../PERF.md`). What is left
   for them (`LSMINDEX.md` §8): one cursor a scan, moved from
   probe to probe, not one
   opened a probe (`open_probe()` in
   `../dg/src/backend/access/sm/smstore.c`), with each
   probe's range an SIREAD of its own, about 10k a probe; sm2
   decoding an entry's value once (`idx_scannxt()` and
   `entry_row()` in `sm2/src/rel/index.cc`), about 540 an
   entry; and an entry that is all key, as MyRocks's.
4. **sm1's lock table, a hash table of its own**
   (`LOCKMGR.md` §3.2). `../bench.old/roles.sh`'s t16,
   sixteen tables a statement, scaled 2.8× from 1 to 16
   clients, every table lock taken and released under the one
   `lock_mu`. Keyed by two words (`../dg/WORKNOTE.md` step 337)
   it went from 38.7 to 72–76 thousand tps at 16 clients; with
   the fast path, a table's IS and IX in slots of the session's
   own (step 338), to 125 thousand, 8.6×, and no time is spent
   waiting for `lock_mu` in t16. The table was a
   `std::unordered_map`, and a table that contended should not
   be the language's:

   - **An allocation an entry, under the mutex**: a node, and
     the holders' `std::vector` in it; both freed when the last
     holder goes. Row locks are mostly of distinct keys, so a
     row lock pays two allocations and two frees under
     `lock_mu`. Step 337 measured what allocations under it
     cost: taking the string keys' out nearly doubled t16.
   - **A rehash, under the mutex**: past its load factor the
     table rehashes every entry in one call, a transaction
     locking a million rows several times over, every other
     locker waiting; the bucket array never shrinks.
   - **Its layout and its policy are not ours**: bucket array,
     node, then the holders' array, a likely miss each; and
     when it grows, how full it runs, and its allocator, which
     we do not choose.

   PostgreSQL keeps its own for the same reasons: `dynahash`,
   sized once, entries from a free list, in 16 partitions. Ours
   follows it, below:

   - **What it is measured on**: after the fast path, t16 does
     not reach the table. What does: row locks, `SELECT ...
     FOR UPDATE` and `FOR SHARE`, the foreign-key check's FOR
     KEY SHARE of the parent row at every child insert, index
     entries and catalog keys; and the strong table locks.

   **Measured without digres or sm**: built. The table is a
   module of its own, `sm1/src/locktab.h` and `.cc`, holding
   today's `std::unordered_map` unchanged, with its own unit
   tests (`sm1/test/test_locktab.cc`); `lockmgr.cc` calls it
   (`LOCKMGR.md` §3.2). Its benchmark,
   `sm1/bench/locktab_bench.cc`, runs threads calling it
   directly, a transaction taking N locks, then releasing them
   all as commit does: `distinct`, 100 row keys at random, X;
   `hot`, 10 of 8 keys, S; `big`, one thread's million locks
   beside `distinct`; `mixed`, 16 table locks of 64, one in 50
   strong. Locks a second and allocations are counted in a run
   that reads no clock; latencies in a second run, as a clock
   read on this box costs 500 to 700 ns, a system call, so a
   lock's p50 is under the clock's noise. The baseline, on
   2026-10-31, `-O3`, one run of a second:

   | Workload | Threads | Locks/s | p99 lock | Max lock | Allocs a lock |
   | --- | --- | --- | --- | --- | --- |
   | distinct | 1 | 10.7M | 0.8 µs | 3.2 ms | 2.00 |
   | distinct | 16 | 1.76M | 254 µs | 16.8 ms | 2.00 |
   | distinct | 32 | 1.61M | 410 µs | 16.7 ms | 2.00 |
   | hot | 1 | 22.1M | 0.8 µs | 3.7 ms | 1.18 |
   | hot | 16 | 2.74M | 115 µs | 9.8 ms | 0.00 |
   | big | 16 | 1.35M | 193 µs | 62.8 ms | 2.00 |
   | mixed | 1 | 12.8M | 0.7 µs | 2.2 ms | 1.78 |
   | mixed | 16 | 2.08M | 131 µs | 6.2 ms | 0.45 |

   One mutex: `distinct` falls from 10.7 million locks a second
   at 1 thread to 1.6 at 32. A row lock of a key held by none
   makes two allocations, the node and its holders' vector,
   under the mutex. And `big`'s rehash shows: a lock call of
   63 to 72 ms at every thread count, every other locker waiting
   behind it.

   **Then digres**: a row-lock workload in `../bench/` (clients
   inserting children of a few parent rows; `SELECT ... FOR
   UPDATE` of keys at random), before and after, to show the
   module's gain reaches a client. Built as
   `../bench.old/rowlocks.sh` (`../dg/WORKNOTE.md` step 344), each
   client locking rows of its own, so none waits. Its baseline,
   at 8612f68b: a lock adds 3.3 µs of a client's time to a
   statement of 100 at 1 client and 28 at 16, and 100 rows locked
   a statement scale 2.4× from 1 client to 16, as 100 read scale
   8.7×. Its `fk` measures commit batching, not a lock (item 5).
   Run against PostgreSQL 18 on 2026-11-01, on the hand-written
   partition (below), digres's tps over PostgreSQL's at 1 and 16
   clients: one row FOR UPDATE 12.7 and 12.4, ten 9.8 and 6.5, a
   hundred 3.3 and 1.8. PostgreSQL's FOR UPDATE writes the row
   and flushes WAL at its commit, about 800 tps at one client, as
   an insert does; sm1's lock writes nothing, so its transaction
   has none to flush. The lock table is not what digres trails
   PostgreSQL by here: an insert is, 0.96 and 0.47, and an insert
   under the foreign key 0.95 and 0.41 (item 5); and reads of 1
   to 100 rows at 16 clients, 0.71 to 0.82.

   The id's hash is `fmix64` over the two words
   (`sm1/src/locktab.h`), so a table's fast-path count is no
   longer its `oid % 1024`; it costs about 38 instructions a lock
   in the map, of about 810. The hash stays cached in each node:
   without it, made by a `noexcept` hash, a lookup hashes every
   node of a bucket it walks, and a lock costs 12.6% more, not
   4.7%.

   **Partitions**: built. The table is in 16, as PostgreSQL's,
   each with a mutex and a condition of its own
   (`LOCKMGR.md` §3.2). A session keeps a lock list for
   each partition, as PostgreSQL's `myProcLocks`, and a release
   walks each under its partition's mutex, taken once, as
   `LockReleaseAll` does. A wait for a transaction sleeps under
   `xact_mu`, and the deadlock search runs as PostgreSQL's does,
   once a waiter has slept `deadlock_ms`, holding every
   partition's mutex and `xact_mu`. Measured as above, against
   the baseline:

   | Workload | Threads | Baseline | 16 partitions | Max lock |
   | --- | --- | --- | --- | --- |
   | distinct | 1 | 10.7M | 9.2M | 1.8 ms |
   | distinct | 16 | 1.76M | 4.97M | 5.8 ms |
   | distinct | 32 | 1.61M | 4.87M | 4.4 ms |
   | hot | 1 | 22.1M | 17.5M | 0.5 ms |
   | hot | 2 | 11.7M | 9.4M | 1.4 ms |
   | hot | 16 | 2.74M | 4.87M | 5.1 ms |
   | big | 16 | 1.35M | 2.43M | 7.5 ms |
   | mixed | 1 | 12.8M | 10.6M | 3.9 ms |
   | mixed | 2 | 8.5M | 6.7M | 3.0 ms |
   | mixed | 16 | 2.08M | 5.67M | 6.3 ms |

   From 8 threads on, 1.4 to 3 times the baseline, and the rehash
   a million locks makes stalls a locker 7 to 15 ms, not 63 to
   72: a partition rehashes its sixteenth. At 1 thread, 14 to 21%
   less, and `hot` and `mixed` about 20% less at 2. Of that, the
   partitions are 0 to 3% over `fmix64`'s table with one mutex,
   timed; the rest is `fmix64`'s, timed, though only 4 to 7% in
   instructions, so this box's swings are some of it. Counted in
   instructions on one thread, a lock and its release cost 556 to
   941 with `fmix64` and one mutex, and 29 to 64 more in
   partitions, a release taking a mutex for each partition it
   holds locks in where it took one. A first try released by a
   pass over the whole list for each partition, 76 to 135 more. A
   transaction of sm's mostly locks in few partitions: a table's
   IS and IX are in fast-path slots, and a child's insert locks
   one parent row.

   **The hand-written partition**: built (`LOCKMGR.md` §3.2). A
   power-of-two bucket array indexed by the hash's low bits; an
   entry with its id, its hash and three holders inline; dropped
   entries kept on a free list, up to 1,024 a partition; the
   array doubled or quartered into a new one a few buckets at
   each add and drop. Counted in instructions on one thread, a
   lock and its release cost 383 to 453 in `hot`, `mixed` and
   `distinct`, against 615 to 970 in the map's partitions and 528
   to 903 in the baseline; `big`, a million locks, 918, as most
   of its entries are past the free list and allocated. Timed:

   | Workload | Threads | Baseline | Map partitions | Own partitions | Allocs a lock |
   | --- | --- | --- | --- | --- | --- |
   | distinct | 1 | 10.7M | 9.2M | 28.5M | 0.00 |
   | distinct | 16 | 1.76M | 4.97M | 7.14M | 0.00 |
   | distinct | 32 | 1.61M | 4.87M | 7.28M | 0.00 |
   | hot | 1 | 22.1M | 17.5M | 24.9M | 0.00 |
   | hot | 2 | 11.7M | 9.4M | 9.7M | 0.00 |
   | hot | 16 | 2.74M | 4.87M | 4.69M | 0.00 |
   | big | 16 | 1.35M | 2.43M | 5.00M | 0.15 |
   | mixed | 1 | 12.8M | 10.6M | 23.2M | 0.00 |
   | mixed | 16 | 2.08M | 5.67M | 7.24M | 0.00 |

   Above the baseline at every thread count but `hot` at 2, 17%
   under, and at 4, even: its 8 keys are in 8 partitions at most,
   whose mutexes and entries two threads pass between them, an
   entry of two holders at most. An entry of many holders, as a
   foreign-key parent under many child inserts, was walked twice
   a lock, for the asker's own hold and for the conflicts;
   `locktab_t::probe` walks it once, gathering the others' modes
   as bits and checking them against the asked mode's conflicts
   in one AND, and names the holders in conflict only when there
   are some. On entries of about 16 holders a lock and its
   release cost 502 instructions, not 903, and `hot` at 16 and 32
   threads ran 7 and 16% faster; with one holder, 286 against
   289. The count of holders by mode that PostgreSQL keeps
   (`LOCK.granted[]`) is not needed for that, and would have to
   follow every conversion `lockmgr.cc` makes in place; it is not
   built. This item is done.
5. **A write transaction's two trips through RocksDB's write
   queue** (`../dg/WORKNOTE.md` step 344). An insert writes its
   uncommitted version at once (`sm1/src/access.cc`,
   unsynced), and the commit its status record
   (`sm1/src/xct.cc`, synced), each a write of its own through
   the one queue. RocksDB syncs the WAL once a group of the
   writers waiting, and an unsynced write may join a group a
   commit syncs, so the insert waits for a sync it does not
   need: at 16 clients about 85% of a session's samples wait
   in the queue, near half under each write. And a group is as
   large as the writers waiting when it forms, so anything a
   transaction does outside the queue costs syncs: 100 µs of
   `pg_sleep` an insert takes `../bench.old/rowlocks.sh`'s `ins`
   from about 5,600 tps to 4,100, and its `fk`, a foreign key's
   check of about 70 µs, to 3,950. A plain insert scales 5.1×
   from 1 client to 16. To weigh, each measured on `ins` and
   `fk`:

   - **RocksDB's own**: `enable_pipelined_write`, the WAL written
     apart from the memtables, measured on 2026-11-01
     (`../bench.old/rowlocks.sh`, digres, the hand-written lock
     partition): at 16 clients `ins` 4,602 tps with it and 4,753
     without, `fk` 4,551 and 4,293, within a run's swing, against
     PostgreSQL's 10,073 and 10,523. It does not help: the cost
     is the trip through the queue, not the order of a group's
     two stages. `two_write_queues` was not tried: RocksDB's
     second queue takes only writes that skip the memtable, as
     `WritePrepared` transactions make, and sm1 makes none.
   - **The version writes kept out of a synced group**: a
     transaction's versions written by a queue, or a batch, of
     their own, so only the commit waits for a sync. No option of
     RocksDB's does it: a group led by a synced write takes in
     the unsynced writes queued behind it, while one led by an
     unsynced write leaves the synced ones out
     (`WriteThread::EnterAsBatchGroupLeader`,
     `db/write_thread.cc`, 8.9.1), and `manual_wal_flush` keeps
     that grouping, putting off only the writes to the file. A
     version may not skip the WAL either: a commit's synced
     record makes durable what the log holds before it, the
     transaction's versions with it. A design change; a note
     first.
   - **sm1's own group sync**: built (`DBMGR.md`,
     `../dg/WORKNOTE.md` step 374). The commit record is written
     unsynced, and one commit syncs the WAL for every commit
     waiting, outside the queue, so no version write waits for a
     sync. At 16 clients `ins` 3,412 tps to 6,591 and `fk` 3,190
     to 6,545, 0.67 and 0.62 of PostgreSQL's 9,862 and 10,637; at
     1 client 0.96 and 0.94.
   - **What is left at 16 clients, profiled**
     (`../dg/WORKNOTE.md` step 376): neither system is
     CPU-bound. The WAL sync of digres is idle 27% of the time,
     PostgreSQL's almost never, so its groups are smaller (6.3
     commits a sync, against 7.6). The cause is the handoff
     between syncs: `notify_all()` wakes every waiter before
     one leads the next sync. A commit also waits 11% of its
     time in `db_commit_wait()` for an older commit that missed
     its sync.
   - **The handoff wakes one leader**: built (step 377). The
     sync is busy 90% to 95% of the time.
   - **A visible commit wakes only the commits it makes
     visible**: built (`../dg/WORKNOTE.md` step 405). A sync
     covers about 6.7 commits, from 5.9, against PostgreSQL's
     7.5; with the disk steady, `ins` and `fk` at 16 clients run
     about even with PostgreSQL, as a sync of digres is shorter.
   - **Left: the wait for an older commit's sync**
     (`XCTMGR.md` §8.5). Two commits in three wait about 220 µs,
     0.4 of a sync, as records reach the WAL out of `commit_ts`
     order. The remedy, records written in `commit_ts` order by
     the sync's leader, changes the commit protocol: a design
     note first. Also left: twice PostgreSQL's CPU a
     transaction.
6. **An lsm index's entry as a key alone**: dropped
   (2026-11-04). An entry's value must keep its version's
   flags and xid or `commit_ts`: the compaction filter stamps
   a `commit_ts` over the xid, and can change a value, never a
   key. What else the value holds saves about 15 bytes an
   entry and a decode, not worth a change of format.
7. **A GUC for the frames of sm1's buffer pool** (`bufpool_t`,
   `BUFMGR.md`). Today nothing sets the count: sm1 gives
   the pool a quarter of `block_cache_bytes`, in frames of
   4 KB, at least 64 and at least 6 a session
   (`sm1/src/env.cc`, `attach()`). digres sets
   `block_cache_bytes` to `shared_buffers`
   (`../dg/src/backend/utils/init/datadir.c`), so 128 MB gives
   8,192 frames. To decide:
   - whether the frames come out of `shared_buffers` or are
     added to it, so the block cache's share is known;
   - the floor: the GUC cannot go below the pins' floor;
   - an option in `sm_options_t` that digres's GUC sets, as
     `block_cache_bytes` is set now.
8. **A list of the btree leaves that need vacuum**: built.
   VACUUM cleans the leaves on the cleaner's queue alone, and
   misses some (`BTREEINDEX.md` §9.2 gives the cases and why
   that is enough). A temporary table's trees are still walked.
9. **Smaller btree leaf entries** (`BTREEINDEX.md` §4.3, §12).
   A leaf entry is its key, its value and 6 bytes of lengths
   and slot (`sm2/src/tree/page.h`, `entry_size()`). An index on
   `status` with `'open'` in many rows, keyed by an `int8`
   primary key: a key of 18 bytes (relid 2, `'open'` 7, the
   primary key 9), a value of 1, so 25 bytes an entry and about
   160 entries a 4 KB leaf. About 16 of the 18 key bytes equal
   those of the entry before. In order:
   1. **The prefix the whole page shares**, from its low and
      high keys, stored once: about 10 bytes an entry, 2.5
      times the entries. A binary search over the slots still
      works.
   2. **Less fixed overhead**: lengths of 1 byte, and the cut
      flag as a bit, not a value: about 6 to 7 bytes an entry.
   3. **Deduplication**: an equal key stored once a leaf, the
      row keys after it as small deltas: 1 to 2 bytes a row,
      10 times the entries or more on a column of few values.
   Prefix compression alone gains little on keys that are
   mostly distinct, such as an email, about 20%. Fewer leaves
   mean fewer pages a range scan reads (item 3) and fewer
   splits; an insert still writes its whole page. Each step
   changes the page's format.
10. **A bulk load builds a B-tree Index from the bottom up**:
    built (`BTREEINDEX.md` §6.7, `BTREELOAD_PLAN.md`). A load
    into an empty table sorts each tree's entries, unique trees
    too, and ingests the pages with the rows. Still open:
    whether `CREATE INDEX` can share the builder, as it runs
    beside writers.

Low priority:

1. **Bulk load, the rest of it.** File mode into an empty
   table is built, and digres's `COPY` uses it (`BULKLOAD.md`
   §13). Not built: `INSERT ... SELECT` into an empty table,
   which digres inserts a row at a time
   (`smtable_tuple_insert()` in
   `../dg/src/backend/access/sm/smtable.c`); a load from
   files into a table that has rows, which inserts a row at a
   time today; rows stored out of line, which end a load's
   files; and batch mode and mini-batch, which matter less
   now that no write keeps a row lock.
2. **digres's `xact.c`, until it is empty.** sm keeps the
   transaction; `xact.c` keeps only what a DG session needs
   around it (`../dg/DESIGN.md` §4), and what it no longer needs
   is deleted as it is found. Nothing is moved out of it just
   so the file goes (`../dg/WORKNOTE.md` steps 232 to 236).
   It is 2,893 lines.
3. **GUC** replace it with something simpler.

### Feature gaps against PostgreSQL

What a PostgreSQL user misses most in digres, largest first by
the statements of PostgreSQL's regression files taken out for
each (`../dg/UNSUPPORTED.md`, which has the full list and the
steps behind each entry).

| Gap | pgregress | Why it matters | Effort |
| --- | --- | --- | --- |
| Users, authentication, authorization: roles, owners, GRANT and REVOKE are built (`../aa/ROLE.md`); a column's privileges, a function's, ALTER DEFAULT PRIVILEGES and row-level security fail; peer on the socket and SCRAM over TCP only, no `pg_hba.conf`, no TLS, no GSSAPI | policies about 90 | encryption on the wire; compliance | medium: TLS by one setting (`../aa/ROLE.md` §8); row-level security, now that roles exist; `pg_hba` is not planned |
| Code in the server: no PL/pgSQL, no `sql` functions, no `DO`, so no triggers | 820, triggers 48, rules 132 | stored procedures, audit and `updated_at` triggers | large: waits for the scripting language SPI is kept for |
| CREATE TABLE beyond columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY: CHECK, generated columns, `OF type`, UNLOGGED | 604, column-level 113 | CHECK is in most schemas | medium: constraints stored in sm's catalog, checked in the executor as PostgreSQL does |
| ALTER TABLE: constraints added later, `SET`/`DROP NOT NULL`, `SET DEFAULT` | 264 | schema migrations | medium a subcommand; PRIMARY KEY and ALTER COLUMN TYPE are built (item 1 above) |
| Partitioning | refused | large time-series tables | large |

- **Largest for adoption:** users and the scripting language;
  a production deployment needs both. Of the first, roles
  with GRANT and REVOKE on tables are built; TLS is next, and
  row-level security last.

---

## 2. Loose ends

Owed, and blocking nothing above:

- **A table's size, taken again after 10 s, is at times the
  old one.** dg's `test/server.sh` failed once in four runs
  (2026-10-28, debug build) at "a size taken again after 10
  s": t33's estimates were 20,000, 20,000, 40,000, 40,000 and
  40,000, where the last, after 20,000 more rows and 10.5 s,
  should be 60,000 within 10%. `kept_estimate()` in
  `../dg/src/backend/access/sm/smstore.c` lets an estimate go
  after `size_cache_age`; whether it was made again from sizes
  RocksDB had not yet changed (`sm1`'s approxsize, kept per
  version of the tree), or a flush between the two read as no
  growth, is not known. Under the thread sanitizer, once in
  three runs (2026-10-28), "a transaction's own analyze"
  estimated 107,256 rows right after the analyze's 80,000:
  the table's bytes moved between the two, as a flush or a
  compaction would move them. Next: repeat those checks
  alone, and print the table's bytes in files and memtables
  beside each estimate.
- **The commit cache's size** (`XCTMGR.md` §9.5).
  65,536 entries reaches back under a second at 100,000
  transactions a second, while a version needs a lookup until
  compaction stamps it. Size the cache by memory with a larger
  default, 16 MB for a million slots; and measure how
  long stamping takes under load, which is what the size
  should be set from.

- **A parent's other columns wait for its children.** sm1 has
  no pair of modes like PostgreSQL's `FOR KEY SHARE` and `FOR
  NO KEY UPDATE`, so an update of a parent row that leaves its
  key alone still waits for a child's transaction, which holds
  the row shared.
- **`lock_timeout`, and the rest of no-wait.** A lock that
  does not wait is built for a row (`sm1::trylock_row`,
  `sm_tbl_trylock`) and for a table's table-oid lock
  (`sm_tbl_tryopen`), and digres's NOWAIT, SKIP LOCKED and
  `LOCK TABLE ... NOWAIT` use them, and `lock_timeout` ends a
  wait in sm by a timer's cancel (`../dg/WORKNOTE.md` step
  168), counted from the call, as sm does not say when a call
  begins to wait. Not built: a write that does not wait, which
  nothing in SQL asks for. sm1's `set_qa_nowait` is still for
  QA only (`LOCKMGR.md` §3.2).
- **The evaluator's cost is unmeasured**: what a call a row
  adds to a write on a table with a partial or an expression
  index.
- **The index build is unmeasured.** Four workers is a guess:
  builds lock rows in sm1's lock table, in 16 partitions since
  §1 item 4, and contend on RocksDB's write path. Two indexes
  built on one table walk its rows twice where one pass could
  fill both.
- **Nothing reads a range at its newest version.** An
  over-long key in a unique index is refused instead
  (`sm3/SM3_DESIGN.md` §6). The primitive is built when a
  workload asks.

- **A key scan and a DELETE's RETURNING copy their rows**
  (`ROWSTORE.md`). Every other scan hands digres's slot a pin
  ref, deformed as far as it is read. The key scan, which
  replaced the TID scan (`ROWSTORE.md` §10.3), and RETURNING read
  a lent copy by the row's key, their first columns up to the
  last the plan names (`smstore_scan_natts()`,
  `smstore_expr_natts()`); they would read into a pin of their
  own.

- **Views, the rest of them** (`VIEW.md` §9). CREATE, OR
  REPLACE, DROP and the cascades are built, sm3's and digres's
  (`../dg/WORKNOTE.md` step 238), and of SQLite's sample digres
  now fails nothing PostgreSQL 18 passes; psql's `\dv` and
  `\d+` and `pg_get_viewdef()` read them (step 239), and
  ALTER VIEW renames one and sets its options, `WITH CHECK
  OPTION` among them (step 240), and a temporary view is
  a session's (`VIEW.md` §12, step 245). Not built: ALTER
  VIEW's `OWNER TO`, `SET SCHEMA` and a column's DEFAULT; and
  `information_schema` (`../dg/SYSCATALOG.md` §7).
- **What foreign keys left** (`FOREIGNKEY.md`,
  `../dg/WORKNOTE.md` step 177): a DEFERRABLE one is refused,
  as a row trigger on a table in sm is given its rows from a
  store that lasts only the statement.
- **What PostgreSQL's isolation specs found**
  (`../dg/test/isolation_specs.txt`, `../dg/WORKNOTE.md` step
  169), beyond the key-share modes above: sm1
  has no wait queue to reorder, so a soft deadlock PostgreSQL
  untangles ends a transaction (`deadlock-soft`).
- **digres's OID is 32 bits; sm's is 64.** sm gives every
  table, index, view, sequence and namespace a `uint64_t` oid,
  and digres narrows each to PostgreSQL's 32-bit `Oid`
  (`(Oid)oid` in `../dg/src/backend/access/sm/smstore.c`, 57
  places), so a database past 2^32 oids would see two objects
  as one. Extend `Oid` in digres to 64 bits, matching sm:
  `Oid`, its `Datum` macros, the `oid` type and `regclass` and
  its kin, and what lays an oid out in 4 bytes.

- **A bulk load's duplicate at its close is sm's.** A unique
  build and every write say PostgreSQL's key now
  (`../dg/WORKNOTE.md` steps 260 and 261). A COPY in file mode
  finds a duplicate when the load closes, in sm1's sort, which
  knows a stream, not the index, and has no row to read: it
  says `key already exists`.

---

## 3. Not blocking

- **Explore dropping a chained store's OLD entries at
  startup.** After a restart no snapshot is older than the
  start, so an old entry (class `0x02`, `ROWSTORE.md` §3.2) whose
  displacer committed is garbage then; today each waits for
  the compaction filter to pass it (`GC.md` §4.2). To find
  out: whether the whole `0x02` range of a store can go at
  once, by `DeleteRange` (allowed: stores have no
  `row_cache`, `DBMGR.md` §10), or must be swept
  entry by entry; what a prepared transaction kept across the
  restart needs, as its version in CURR is in flight and the
  one it displaced, in OLD, is what readers see; what it
  costs at startup against what it saves in compaction and in
  reads that follow a chain; and when it runs, before the
  first session or in the background.
- **Edges from digres in sm1's wait-for graph.** digres keeps
  PostgreSQL's advisory locks (`pg_advisory_lock()`,
  `pg_advisory_xact_lock()` and the rest) in a table of its
  own, `../dg/src/backend/storage/lmgr/advisory_lock.c`, with
  a deadlock detector for cycles among them (`../dg/DESIGN.md`
  §4). A cycle through an advisory lock and an sm1 lock is
  seen by neither, and waits until `lock_timeout` or a cancel. sm1 could take
  an edge from its caller -- "session A waits for session B",
  set while A waits on the advisory lock and cleared after --
  and follow it in `find_cycle()` as its own, so the one
  detector finds every cycle.
- **A snapshot handed to another session.**
  `pg_export_snapshot()` and `SET TRANSACTION SNAPSHOT` share
  PostgreSQL's snapshot, which is not what decides what a
  session sees in digres, so digres refuses both
  (`../dg/WORKNOTE.md` step 149). `pg_dump -j` needs them: a
  session would take an sm1 snapshot another has, at its
  timestamp, for a transaction of its own.
- **What temporary tables left for later** (`TEMPTABLE.md`
  §9). They are built, in digres too. Not built: skipping the
  sync of a commit that wrote only temporary stores,
  statistics of a temporary table, and `CREATE TEMPORARY
  SEQUENCE`.
- **Dependency checks**, such as refusing `DROP COLLATION` in
  use and `DROP SCHEMA … CASCADE`: low priority. They wait
  for a dependency index (`CATALOG.md` §11), and
  nothing is waiting for them.
- **The unmeasured startup cost for thousands of tables** —
  open in `CATALOG.md` §11.
- **A table in a namespace other than `public`, from `rel`.**
  `rel` creates and drops namespaces but has no way to name
  one for a table.
- **Savepoints without an undo list** (`XCTMGR.md`):
  a rollback would mark the `seq` range it undid as dead and
  delete nothing. Designed, and not to be built for now: we
  lean towards the capped undo list, for the reasons at the
  top of that file.

---

## 4. What this file is not

Not a replacement for `sm3/SM3_DESIGN.md` §4, which holds
sm3's order of work and the reasoning behind it, nor for the
design documents, where a protocol is argued before it is
built and recorded once it is. A decision settled here moves
into one of those and leaves this file.

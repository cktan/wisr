# NEXT — what to build next, and in what order

This file holds only what is still to do, across levels. What
is built or settled is in the design documents:
[`DESIGN.md`](DESIGN.md)
§3 has where each level stands, and
[`sm3/SM3_DESIGN.md`](sm3/SM3_DESIGN.md#4-structure) §4
has sm3's own order of work.

---

## 1. Next

The spike [`PART4.md`](PART4.md#7-recommended-sequencing) §7
aimed the stack at is long past:
digres runs on it
([`PART4.md`](PART4.md#10-digres-how-the-fork-is-to-be-made)
§10). Statistics for a planner
are built, the stack's part and digres's
([`../dg/STATISTICS.md`](../dg/STATISTICS.md),
[`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) steps 74 to 78),
and so are the catalog tables psql reads
([`../dg/SYSCATALOG.md`](../dg/SYSCATALOG.md#6-status) §6).

1. **digres's key-column ALTERs**
   ([`CLUSTER.md`](CLUSTER.md#11-what-is-left) §11, step 3):
   built. ADD and DROP PRIMARY KEY, and of a unique constraint
   ([`../dg/PKDDL.md`](../dg/PKDDL.md)), and `ALTER COLUMN TYPE`
   of any column, a
   key column's among them
   ([`../dg/ALTERTYPE.md`](../dg/ALTERTYPE.md),
   [`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 378). Left: the
   dependents
   PostgreSQL makes again, an expression or a partial index, a
   foreign key, an identity column's sequence, which digres
   refuses ([`ALTERTYPE.md`](../dg/ALTERTYPE.md#8-decided) §8).
   This item is done.
3. **A secondary index's scan, within 1.5 times PostgreSQL's**
   ([`BTREEINDEX.md`](BTREEINDEX.md),
   [`../PARITY.md`](../PARITY.md), [`../PERF.md`](../PERF.md)).
   An index
   scan may cost digres no more than 1.5 times what it costs
   PostgreSQL; a sequential scan may, as OLTP runs none. The
   measure is three queries of `../bench/run.sh` on
   `usertable`, 10,000 rows, with btree indexes. Instructions a
   statement, the slope of 5,000 and 25,000 prepared statements
   under `perf stat`, the server pinned
   ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md)
   step 407):

   | Query | What it reads | digres | PostgreSQL | Ratio |
   | --- | --- | --- | --- | --- |
   | q1 | 8 entries of the `lastname` index, a count | 66,100 | 62,100 | 1.06 |
   | q2 | 183 entries of the `state` index, a count | 216,900 | 150,800 | 1.44 |
   | q3 | 8 rows, found by the `lastname` index | 178,000 | 89,100 | 2.00 |

   The fixed cost of a statement is at parity, so the gap is the
   cost of an entry and of a row.

   - **q1 passes.** The gap is its 8 entries and one descent.
   - **q2 passes.** An entry costs about 360 instructions more
     than PostgreSQL's, half of it the executor. Built:
     1. **Copy an entry once**: sm3's batch views the cursor's
        copy of one leaf, and ends with it ([`BTREEINDEX.md`](BTREEINDEX.md)
        §5.3). q2 245.5k to 230.0k.
     2. **A tree's row key viewed, not copied, and its columns'
        decodability asked once a batch**: q2 230.0k to 216.9k.
     Dropped (2026-11-07): **handing out a leaf's entries from
     the pinned leaf**. PostgreSQL copies each entry too:
     `_bt_saveitem()` copies an index-only scan's tuple, then
     unlocks the page. A pin is a latch ([`BUFMGR.md`](BUFMGR.md#13-pins-are-latches) §1.3), so a
     pin held across calls would make writers wait on the
     reader. A leaf id kept as a hint, pinned only if in the
     pool, was measured: q2 0.2% fewer instructions, q1 1.2% and
     q3 0.3% more. Parity needs a count in sm, one tuple a leaf,
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
   2.40 times PostgreSQL's latency ([`../PERF.md`](../PERF.md)).
   What is left
   for them ([`LSMINDEX.md`](LSMINDEX.md#8-the-kind-of-an-index)
   §8): one cursor a scan, moved from
   probe to probe, not one
   opened a probe (`open_probe()` in
   `../dg/src/backend/access/sm/smstore.c`), with each
   probe's range an SIREAD of its own, about 10k a probe; sm2
   decoding an entry's value once (`idx_scannxt()` and
   `entry_row()` in `sm2/src/rel/index.cc`), about 540 an
   entry; and an entry that is all key, as MyRocks's.
5. **A write transaction's two trips through RocksDB's write
   queue** ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 344).
   An insert writes its
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
   - **sm1's own group sync**: built ([`DBMGR.md`](DBMGR.md),
     [`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 374). The commit record is written
     unsynced, and one commit syncs the WAL for every commit
     waiting, outside the queue, so no version write waits for a
     sync. At 16 clients `ins` 3,412 tps to 6,591 and `fk` 3,190
     to 6,545, 0.67 and 0.62 of PostgreSQL's 9,862 and 10,637; at
     1 client 0.96 and 0.94.
   - **What is left at 16 clients, profiled**
     ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 376): neither system is
     CPU-bound. The WAL sync of digres is idle 27% of the time,
     PostgreSQL's almost never, so its groups are smaller (6.3
     commits a sync, against 7.6). The cause is the handoff
     between syncs: `notify_all()` wakes every waiter before
     one leads the next sync. A commit also waits 11% of its
     time for an older commit that missed its sync.
   - **The handoff wakes one leader**: built (step 377). The
     sync is busy 90% to 95% of the time.
   - **A visible commit wakes only the commits it makes
     visible**: built ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 405). A sync
     covers about 6.7 commits, from 5.9, against PostgreSQL's
     7.5; with the disk steady, `ins` and `fk` at 16 clients run
     about even with PostgreSQL, as a sync of digres is shorter.
   - **Records in `commit_ts` order**: built
     ([`COMMITORDER_PLAN.md`](COMMITORDER_PLAN.md), [`XCTMGR.md`](XCTMGR.md#85-records-in-commit_ts-order) §8.5,
     [`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 406). The sync's leader writes the
     queued commits' records in one batch, in `commit_ts` order,
     so no commit waits for an older commit's sync. A sync covers
     7.6 to 7.9 commits, PostgreSQL's 7.3 to 7.8 in the same
     runs.
   - **Left: twice PostgreSQL's CPU a transaction.**
6. **An lsm index's entry as a key alone**: dropped
   (2026-11-04). An entry's value must keep its version's
   flags and xid or `commit_ts`: the compaction filter stamps
   a `commit_ts` over the xid, and can change a value, never a
   key. What else the value holds saves about 15 bytes an
   entry and a decode, not worth a change of format.
7. **A GUC for the frames of sm1's buffer pool** (`bufpool_t`,
   [`BUFMGR.md`](BUFMGR.md)). Today nothing sets the count: sm1
   gives
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
   misses some ([`BTREEINDEX.md`](BTREEINDEX.md#92-vacuum) §9.2
   gives the cases and why
   that is enough). A temporary table's trees are still walked.
9. **Smaller btree leaf entries**
   ([`BTREEINDEX.md`](BTREEINDEX.md#43-entries) §4.3, §12).
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
    built ([`BTREEINDEX.md`](BTREEINDEX.md#67-a-bulk-load-builds-the-tree-whole) §6.7, [`BTREELOAD_PLAN.md`](BTREELOAD_PLAN.md)). A load
    into an empty table sorts each tree's entries, unique trees
    too, and ingests the pages with the rows. Still open:
    whether `CREATE INDEX` can share the builder, as it runs
    beside writers.

Low priority:

1. **Bulk load, the rest of it.** File mode into an empty
   table is built, and digres's `COPY` uses it
   ([`BULKLOAD.md`](BULKLOAD.md)
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
   around it
   ([`../dg/DESIGN.md`](../dg/DESIGN.md#4-decisions-of-record)
   §4), and what it no longer needs
   is deleted as it is found. Nothing is moved out of it just
   so the file goes ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md)
   steps 232 to 236).
   It is 2,893 lines.
3. **GUC** replace it with something simpler.

### Feature gaps against PostgreSQL

What a PostgreSQL user misses most in digres, largest first by
the statements of PostgreSQL's regression files taken out for
each ([`../dg/UNSUPPORTED.md`](../dg/UNSUPPORTED.md), which has
the full list and the
steps behind each entry).

| Gap | pgregress | Why it matters | Effort |
| --- | --- | --- | --- |
| Users, authentication, authorization: roles, owners, GRANT and REVOKE are built ([`../aa/ROLE.md`](../aa/ROLE.md)); a column's privileges, a function's, ALTER DEFAULT PRIVILEGES and row-level security fail; peer on the socket and SCRAM over TCP only, no `pg_hba.conf`, no TLS, no GSSAPI | policies about 90 | encryption on the wire; compliance | medium: TLS by one setting ([`../aa/ROLE.md`](../aa/ROLE.md#8-decisions) §8); row-level security, now that roles exist; `pg_hba` is not planned |
| Code in the server: no PL/pgSQL, no `sql` functions, no `DO`, so no triggers | 820, triggers 48, rules 132 | stored procedures, audit and `updated_at` triggers | large: waits for the scripting language SPI is kept for |
| CREATE TABLE beyond columns, NOT NULL, PRIMARY KEY, UNIQUE, CHECK, generated columns and TEMPORARY: `OF type`, UNLOGGED; ADD COLUMN of a generated column, SET and DROP EXPRESSION | not counted yet | computed columns added to a table | small: ADD COLUMN computes each row's value from its own columns; CHECK and generated columns are built ([`CHECK.md`](CHECK.md), [`GENERATED.md`](GENERATED.md)) |
| ALTER TABLE: `SET SCHEMA`, `ADD CONSTRAINT ... USING INDEX`, `SET STATISTICS`, storage, replica identity | 180 | schema migrations | small a subcommand; PRIMARY KEY, CHECK, ALTER COLUMN TYPE, SET/DROP NOT NULL and SET/DROP DEFAULT are built (item 1 above) |
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
- **The commit cache's size** ([`XCTMGR.md`](XCTMGR.md#95-limits)
  §9.5).
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
  wait in sm by a timer's cancel
  ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
  168), counted from the call, as sm does not say when a call
  begins to wait. Not built: a write that does not wait, which
  nothing in SQL asks for. sm1's `set_qa_nowait` is still for
  QA only ([`LOCKMGR.md`](LOCKMGR.md#32-the-lock-table) §3.2).
- **The evaluator's cost is unmeasured**: what a call a row
  adds to a write on a table with a partial or an expression
  index.
- **The index build is unmeasured.** Four workers is a guess:
  builds lock rows in sm1's lock table, in 16 partitions
  ([`LOCKMGR.md`](LOCKMGR.md#32-the-lock-table) §3.2), and
  contend on RocksDB's write path. Two indexes
  built on one table walk its rows twice where one pass could
  fill both.
- **Nothing reads a range at its newest version.** An
  over-long key in a unique index is refused instead
  ([`sm3/SM3_DESIGN.md`](sm3/SM3_DESIGN.md#6-known-trade-offs)
  §6). The primitive is built when a
  workload asks.

- **A key scan and a DELETE's RETURNING copy their rows**
  ([`ROWSTORE.md`](ROWSTORE.md)). Every other scan hands digres's
  slot a pin
  ref, deformed as far as it is read. The key scan, which
  replaced the TID scan
  ([`ROWSTORE.md`](ROWSTORE.md#103-the-executor) §10.3), and
  RETURNING read
  a lent copy by the row's key, their first columns up to the
  last the plan names (`smstore_scan_natts()`,
  `smstore_expr_natts()`); they would read into a pin of their
  own.

- **Views, the rest of them** ([`VIEW.md`](VIEW.md#9-not-here)
  §9). CREATE, OR
  REPLACE, DROP and the cascades are built, sm3's and digres's
  ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 238), and of
  SQLite's sample digres
  now fails nothing PostgreSQL 18 passes; psql's `\dv` and
  `\d+` and `pg_get_viewdef()` read them (step 239), and
  ALTER VIEW renames one and sets its options, `WITH CHECK
  OPTION` among them (step 240), and a temporary view is
  a session's ([`VIEW.md`](VIEW.md#12-temporary-views) §12, step
  245). Not built: ALTER
  VIEW's `OWNER TO`, `SET SCHEMA` and a column's DEFAULT; and
  `information_schema`
  ([`../dg/SYSCATALOG.md`](../dg/SYSCATALOG.md#7-left-out) §7).
- **What foreign keys left** ([`FOREIGNKEY.md`](FOREIGNKEY.md),
  [`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 177): a
  DEFERRABLE one is refused,
  as a row trigger on a table in sm is given its rows from a
  store that lasts only the statement.
- **What PostgreSQL's isolation specs found**
  (`../dg/test/isolation_specs.txt`,
  [`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
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
  ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) steps 260 and 261). A
  COPY in file mode
  finds a duplicate when the load closes, in sm1's sort, which
  knows a stream, not the index, and has no row to read: it
  says `key already exists`.

---

## 3. Not blocking

- **Explore dropping a chained store's OLD entries at
  startup.** After a restart no snapshot is older than the
  start, so an old entry (class `0x02`,
  [`ROWSTORE.md`](ROWSTORE.md#32-the-class-byte-and-the-comparator)
  §3.2) whose
  displacer committed is garbage then; today each waits for
  the compaction filter to pass it
  ([`GC.md`](GC.md#42-chained-keys) §4.2). To find
  out: whether the whole `0x02` range of a store can go at
  once, by `DeleteRange` (allowed: stores have no
  `row_cache`, [`DBMGR.md`](DBMGR.md#10-configuration) §10), or
  must be swept
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
  a deadlock detector for cycles among them
  ([`../dg/DESIGN.md`](../dg/DESIGN.md)
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
  ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 149). `pg_dump
  -j` needs them: a
  session would take an sm1 snapshot another has, at its
  timestamp, for a transaction of its own.
- **What temporary tables left for later**
  ([`TEMPTABLE.md`](TEMPTABLE.md)
  §9). They are built, in digres too. Not built: skipping the
  sync of a commit that wrote only temporary stores,
  statistics of a temporary table, and `CREATE TEMPORARY
  SEQUENCE`.
- **Dependency checks**, such as refusing `DROP COLLATION` in
  use and `DROP SCHEMA … CASCADE`: low priority. They wait
  for a dependency index ([`CATALOG.md`](CATALOG.md#11-not-built)
  §11), and
  nothing is waiting for them.
- **The unmeasured startup cost for thousands of tables** —
  open in [`CATALOG.md`](CATALOG.md#11-not-built) §11.
- **A table in a namespace other than `public`, from `rel`.**
  `rel` creates and drops namespaces but has no way to name
  one for a table.
- **Savepoints without an undo list** ([`XCTMGR.md`](XCTMGR.md)):
  a rollback would mark the `seq` range it undid as dead and
  delete nothing. Designed, and not to be built for now: we
  lean towards the capped undo list, for the reasons at the
  top of that file.

---

## 4. What this file is not

Not a replacement for
[`sm3/SM3_DESIGN.md`](sm3/SM3_DESIGN.md#4-structure) §4, which
holds
sm3's order of work and the reasoning behind it, nor for the
design documents, where a protocol is argued before it is
built and recorded once it is. A decision settled here moves
into one of those and leaves this file.

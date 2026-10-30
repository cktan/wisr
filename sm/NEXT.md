# NEXT — what to build next, and in what order

This file holds only what is still to do, across levels. What
is built or settled is in the design documents: `DESIGN.md`
§3 has where each level stands, and `sm3/SM3_DESIGN.md` §8
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
   ADD and DROP PRIMARY KEY, and of a unique constraint, are
   built (`../dg/PKDDL.md`); a key column's `ALTER COLUMN
   TYPE` waits for that ALTER, which digres refuses for every
   column (`../dg/UNSUPPORTED.md` §3).
2. **What a role pays that a superuser does not**
   (`../aa/ROLE.md`).
   A role that is not a superuser runs every privilege check a
   superuser skips, and pays for it, measured on 2026-10-28 on
   `digres-release`: `narrow`, a million rows; pgbench, 20
   seconds a run, two runs each; the same role granted SELECT
   and UPDATE.

   | Workload | Protocol | Clients | Superuser tps | Role tps | Lost |
   | --- | --- | --- | --- | --- | --- |
   | read by key | simple | 1 | 11,011 | 9,758 | 11% |
   | read by key | simple | 16 | 101,805 | 88,024 | 14% |
   | read by key | prepared | 1 | 14,502 | 12,955 | 11% |
   | read by key | prepared | 16 | 132,451 | 111,254 | 16% |
   | 9 reads, 1 update | prepared | 1 | 6,562 | 6,040 | 8% |
   | 9 reads, 1 update | prepared | 16 | 45,051 | 43,332 | 4% |

   About 8 µs a statement, and more as clients are added, so
   some of it is contention.

   **Profiled, and the most of it gone** (`../dg/WORKNOTE.md`
   step 329): 19% of a role's busy time was the syscache
   missing the table's `pg_class` row at each check, which is
   now read from `pg_global` and the relation cache instead,
   and an ACL's text is read without `sscanf()` (step 330), and
   a role's privileges on an object are kept in its session
   (step 331): an object's check costs a role 0.07 µs more than
   the superuser (`../bench/roles.sh`). What is left of the
   steps below is the syscache for the rows other paths read
   (`catrel_is_cached()`, step 321), and the one mutex every
   owner or ACL looked up takes (step 330): `aa_roles()` and
   `aa_objects()` in `../aa/src/aa.cc` copy a `shared_ptr`
   under `env->mutex`.

   The plan, a step at a time, each measured as above:

   1. **Keep the benchmark**: done, `../bench/roles.sh`.
   2. **Profile first**: done, by gdb (step 329, above).
   3. **Keep sm's catalog rows in the syscache again**, made
      stale correctly: dropped when sm's catalog epoch moves
      (`sm_cat_epoch()`, checked where the namespaces' epoch
      is, `AcceptInvalidationMessages()`), when `pg_global`'s
      objects epoch moves (an owner or an ACL), and by the
      session's own DDL, which invalidates the catalog it
      changed as a catalog tuple's write would. The check that
      a dropped table's privilege is answered as for no table
      (`server.sh`) must still pass, and be shown to fail with
      the epoch's check left out.
   4. **Read the snapshots without the mutex**:
      `std::atomic<std::shared_ptr<const T>>`, loaded by every
      reader and stored by the one that replaces it; the mutex
      kept only to order the replacements. Run under TSan
      (`make -C ../dg tsan`).
   5. **Then measure**: the gap a step leaves is recorded here
      and in `../dg/WORKNOTE.md`; done when a role's point read
      costs within 3% of a superuser's. What is left, if more,
      is the checks PostgreSQL itself makes, and is PostgreSQL's
      cost too.
3. **A secondary index's scan, within 1.5 times PostgreSQL's**
   (`BTREE.md`, `../dg/WORKNOTE.md` steps 333 to 336). An
   index scan may cost digres no more than 1.5 times what it
   costs PostgreSQL; a sequential scan may, as OLTP runs none.
   Instructions a statement, servers pinned, on 2026-10-29:

   | Scan | Rows | digres | PostgreSQL | Ratio |
   | --- | --- | --- | --- | --- |
   | primary key, a range | 100 | 445k | 381k | 1.17 |
   | Q7, by primary key | 100 | 976k | 802k | 1.22 |
   | secondary, a range of `c2` (Q9's) | 10 | 376k | 249k | 1.51 |
   | | 100 | 1,345k | 377k | 3.57 |
   | secondary, an uncorrelated range | 10 | 432k | 258k | 1.67 |
   | Q10, Q7's rows by `c2` | 100 | 2,946k | 802k | 3.67 |

   Reads by the primary key pass; reads by a secondary index
   fail from about 10 rows. A probe of one costs about 30k
   instructions more than one of the primary key: a RocksDB
   iterator made and destroyed a probe, its `Seek`, and
   sm's wrappers. An entry of a range costs 2,820 against
   PostgreSQL's 695, RocksDB's `Next` alone 1,110 of it.
   In order:

   1. **Secondary indexes as B+trees** (`BTREE.md`): a tree
      per index, beside today's entries, chosen when the index
      is made; inner pages kept in memory, leaves read from
      RocksDB. Built in `sm2/src/tree/` a part at a time, each
      with its tests: the page, the inner-page cache, the
      writers' latch table, the tree; then sm1's class
      `0x04` and SSI on keys not stored, sm3's index kind, and
      digres's way to ask for one (`BTREE.md` §11, question
      4). Estimated: Q10 1.6 to 1.7 times.
   2. **Rows of sorted, close keys read by one cursor**, a
      seek and then the next, not `MultiGet` (`BATCHREAD.md`):
      about 6,000 a row of Q9, which a tree alone leaves at
      2.9 times. A design change; a note first.
   3. **Then measure** the grid again, on both kinds of index
      (`BTREE.md` §12): what is left past 1.5 is recorded
      here, and the kind that loses every workload goes.

   For today's entries, only if they stay (`BTREE.md` §12):
   one cursor a scan, moved from probe to probe, not one
   opened a probe (`open_probe()` in
   `../dg/src/backend/access/sm/smstore.c`), with each
   probe's range an SIREAD of its own, about 10k a probe; sm2
   decoding an entry's value once (`idx_scannxt()` and
   `entry_row()` in `sm2/src/rel/index.cc`), about 540 an
   entry; and an entry that is all key, as MyRocks's.
4. **sm1's lock table, a hash table of its own**
   (`sm1/SM1_DESIGN.md` §5.1). `../bench/roles.sh`'s t16,
   sixteen tables a statement, scaled 2.8× from 1 to 16
   clients, every table lock taken and released under the one
   `lock_mu`. Keyed by two words (`../dg/WORKNOTE.md` step 337)
   it went from 38.7 to 72–76 thousand tps at 16 clients; with
   the fast path, a table's IS and IX in slots of the session's
   own (step 338), to 125 thousand, 8.6×, and no time is spent
   waiting for `lock_mu` in t16. The table is still a
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
   - **The hash it is given**: `lockid_hash_t`, `lo ^ (hi *
     K)`. libstdc++ takes `hash % bucket_count`, the count a
     prime (`_Mod_range_hashing`, `_Prime_rehash_policy` in
     `bits/hashtable_policy.h`), which hides weak low bits, at
     a 64-bit division a lookup. A power of two does not hide
     them: `fp_strong` takes the hash modulo 1,024, and the low
     bits of `hi * K` depend only on `hi`'s low bits, so a
     table lock's count is its `oid % 1024`, the type bit and a
     storeid's high bits never reaching it. Tables 1,024 oids
     apart share a count, and one's DDL sends the other's
     readers to the slow path; correct, only slower. The fix is
     a finalizer, MurmurHash3's `fmix64`, after the combine, so
     every bit of both words reaches every bit of the hash; an
     OID lock's id stays exact, `lo` one-to-one for a fixed
     `hi`. `operator()` should be `noexcept`, or libstdc++ keeps
     the hash in each node. Lock ids are never persisted (a
     prepared transaction's record keeps the names), so the
     hash can change at any time.

   PostgreSQL keeps its own for the same reasons: `dynahash`,
   sized once, entries from a free list, in 16 partitions. What
   to build:

   - **Partitions**, by the id's hash, each with its own mutex;
     the deadlock search takes them all, in order, as
     PostgreSQL's does.
   - **In a partition**: a power-of-two bucket array, indexed
     by `fmix64`, no division; entries from a pool kept by a
     free list, no allocation under the mutex; a few holders
     inline, one to four, the rest in an overflow; a count of
     the holders granted each mode in each entry, so a conflict
     check is O(1), not a walk of the holders, as PostgreSQL's
     `LOCK.granted[]`; growth by adding a block to the pool,
     never a rehash of the whole partition under its mutex.
   - **What it is measured on**: after the fast path, t16 does
     not reach the table. What does: row locks, `SELECT ...
     FOR UPDATE` and `FOR SHARE`, the foreign-key check's FOR
     KEY SHARE of the parent row at every child insert, index
     entries and catalog keys; and the strong table locks.
   - **The lock id, not the key.** A row lock keeps its whole
     key in the lock list, the `PREPARED` record and
     `sm1::lock_status`, and should keep only its `lockid_t`.
     The plan, its cases, open decision and order of work are
     in `sm1/LOCKID.md`; it is this item's first step.

   **Measured without digres or sm.** The table is a module of
   its own, `sm1/src/locktab.h` and `.cc`, that depends on
   nothing but sm0: no RocksDB, no `db_t`, no `session_t`. A
   holder is an opaque owner; the module finds, adds and drops
   entries and their holders, and answers whether a mode
   conflicts and with whom. Waiting, the waits-for graph, the
   deadlock search, the fast path and the session's lock list
   stay in `lockmgr.cc`, which calls it. Then:

   - **A benchmark of the module alone**, `sm1/bench/
     locktab_bench`, threads calling it directly: a
     transaction takes N locks, then releases them all, as
     commit does. Its workloads: distinct row keys at random
     (`FOR UPDATE`); a few hot keys, shared (foreign-key
     parents); one transaction of a million locks, then its
     release, beside others (a rehash's pause); strong and weak
     locks mixed. Each at 1 to 32 threads, reporting locks a
     second, the latency of a lock and of a release at p50,
     p99 and the maximum, and the allocations a lock makes
     (counted by `operator new`).
   - **Both tables behind the one interface**: today's
     `std::unordered_map`, moved there first unchanged, as the
     baseline the benchmark records; then the hand-written one,
     the first's numbers to beat.
   - **Its own unit tests** in `sm1/test`, the module's alone,
     beside `test_lock.cc`, which keeps testing the lock
     manager through `sm1.h`; and the isolation scenarios,
     unchanged, the claim that nothing above it moved.
   - **Then digres**: a row-lock workload in `../bench/`
     (clients inserting children of a few parent rows; `SELECT
     ... FOR UPDATE` of keys at random), before and after, to
     show the module's gain reaches a client. Built as
     `../bench/rowlocks.sh` (`../dg/WORKNOTE.md` step 344),
     each client locking rows of its own, so none waits. Its
     baseline, at 8612f68b: a lock adds 3.3 µs of a client's
     time to a statement of 100 at 1 client and 28 at 16, and
     100 rows locked a statement scale 2.4× from 1 client to
     16, as 100 read scale 8.7×. Its `fk` measures commit
     batching, not a lock (item 5).

   The order: the module with today's table, and its
   benchmark's baseline; `fmix64`; the partitions; the
   hand-written partition; each a step, measured.
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
   `pg_sleep` an insert takes `../bench/rowlocks.sh`'s `ins`
   from about 5,600 tps to 4,100, and its `fk`, a foreign key's
   check of about 70 µs, to 3,950. A plain insert scales 5.1×
   from 1 client to 16. To weigh, each measured on `ins` and
   `fk`:

   - **RocksDB's own**: `enable_pipelined_write`, the WAL
     written apart from the memtables; `two_write_queues`, for
     writes that need no memtable order.
   - **The version writes kept out of a synced group**: a
     transaction's versions written by a queue, or a batch, of
     their own, so only the commit waits for a sync.

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
   around it (`../dg/PLAN.md` §7), and what it no longer needs
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
| ALTER TABLE: constraints added later, `ALTER COLUMN TYPE`, `SET`/`DROP NOT NULL`, `SET DEFAULT` | 264 | schema migrations | medium a subcommand; PRIMARY KEY is item 1 above |
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
- **The commit cache's size** (`sm1/COMMITCACHE.md` §7).
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
  QA only (`sm1/SM1_DESIGN.md` §5.1).
- **The evaluator's cost is unmeasured**: what a call a row
  adds to a write on a table with a partial or an expression
  index.
- **The index build is unmeasured.** Four workers is a guess:
  builds contend on sm1's lock table, one mutex, and on
  RocksDB's write path. Two indexes built on one table walk
  its rows twice where one pass could fill both.
- **Nothing reads a range at its newest version.** An
  over-long key in a unique index is refused instead
  (`sm3/SM3_DESIGN.md` §7). The primitive is built when a
  workload asks.

- **A key scan and a DELETE's RETURNING copy their rows**
  (`ROW.md`). Every other scan hands digres's slot a pin ref,
  deformed as far as it is read. The key scan, which replaced
  the TID scan (`ROWKEY.md` §3.3), and RETURNING read a lent
  copy by the row's key, their first columns up to the last
  the plan names (`smstore_scan_natts()`,
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
  start, so an old entry (class `0x02`, `VCHAIN.md` §2) whose
  displacer committed is garbage then; today each waits for
  the compaction filter to pass it (`VCHAIN.md` §7). To find
  out: whether the whole `0x02` range of a store can go at
  once, by `DeleteRange` (allowed: stores have no
  `row_cache`, `sm1/SM1_DESIGN.md` §12.2), or must be swept
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
  a deadlock detector for cycles among them (`../dg/PLAN.md`
  §7). A cycle through an advisory lock and an sm1 lock is
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
  for a dependency index (`sm3/CATALOG_INMEM.md` §4), and
  nothing is waiting for them.
- **The unmeasured startup cost for thousands of tables** —
  open in `sm3/CATALOG_INMEM.md` §4.
- **A table in a namespace other than `public`, from `rel`.**
  `rel` creates and drops namespaces but has no way to name
  one for a table.
- **Savepoints without an undo list** (`sm1/SAVEPOINT.md`):
  a rollback would mark the `seq` range it undid as dead and
  delete nothing. Designed, and not to be built for now: we
  lean towards the capped undo list, for the reasons at the
  top of that file.

---

## 4. What this file is not

Not a replacement for `sm3/SM3_DESIGN.md` §8, which holds
sm3's order of work and the reasoning behind it, nor for the
design documents, where a protocol is argued before it is
built and recorded once it is. A decision settled here moves
into one of those and leaves this file.

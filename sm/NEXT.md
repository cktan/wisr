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
   some of it is contention. Where it is likely to go, not yet
   profiled (`perf` does not run on this WSL kernel):

   - **No row of `pg_class` or `pg_namespace` is kept by the
     syscache** (`catrel_is_cached()`, `../dg/WORKNOTE.md`
     step 321): each check makes the table's row again from
     sm's definition, its columns and indexes with it, and the
     search path's schema check every row of `pg_namespace`.
   - **`pg_global`'s snapshots are read under one mutex**
     (`shared_objects()`, `shared_roles()` in
     `../dg/src/backend/utils/init/pg_global.c`): each owner
     or ACL looked up copies a `shared_ptr` under the lock
     every session takes.

   **Profiled, and the most of it gone** (`../dg/WORKNOTE.md`
   step 329): 19% of a role's busy time was the syscache
   missing the table's `pg_class` row at each check, which is
   now read from `pg_global` and the relation cache instead. A
   read by key loses 4 to 7%; a statement that reads no row 2
   to 3 µs, was 7 to 10. The steps below are for what is left.

   The plan, a step at a time, each measured as above:

   1. **Keep the benchmark**: the script that made the table,
      as `../bench/roles.sh`, so each step below is measured
      the same way, and the gap can be checked again later.
   2. **Profile first**, to confirm where the 8 µs go before
      changing anything: a sampling profile where `perf` runs,
      or the counters of the step 3 caches' misses.
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
   transaction; `xact.c` keeps only what a session needs
   around it (`../dg/PLAN.md` §7), and what it no longer needs
   is deleted as it is found. Nothing is moved out of it just
   so the file goes (`../dg/WORKNOTE.md` steps 232 to 236).
   It is 2,874 lines.

### Feature gaps against PostgreSQL

What a PostgreSQL user misses most in digres, largest first by
the statements of PostgreSQL's regression files taken out for
each (`../dg/UNSUPPORTED.md`, which has the full list and the
steps behind each entry).

| Gap | pgregress | Why it matters | Effort |
| --- | --- | --- | --- |
| Users, authentication, authorization: one superuser; CREATE ROLE, SET ROLE, GRANT and REVOKE fail; no row-level security; peer on the socket and SCRAM over TCP only, no `pg_hba.conf`, no TLS, no GSSAPI | about 1,400, policies about 90 | separate application, read-only and admin roles; encryption on the wire; compliance | large: roles and object ACLs in sm's catalog, and every privilege check reading them; TLS and `pg_hba` put back what step 92 removed |
| Code in the server: no PL/pgSQL, no `sql` functions, no `DO`, so no triggers | 820, triggers 48, rules 132 | stored procedures, audit and `updated_at` triggers | large: waits for the scripting language SPI is kept for |
| CREATE TABLE beyond columns, NOT NULL, PRIMARY KEY, UNIQUE and TEMPORARY: CHECK, generated columns, `OF type`, UNLOGGED | 604, column-level 113 | CHECK is in most schemas | medium: constraints stored in sm's catalog, checked in the executor as PostgreSQL does |
| ALTER TABLE: constraints added later, `ALTER COLUMN TYPE`, `SET`/`DROP NOT NULL`, `SET DEFAULT` | 264 | schema migrations | medium a subcommand; PRIMARY KEY is item 1 above |
| Partitioning | refused | large time-series tables | large |

- **Largest for adoption:** users and the scripting language;
  a production deployment needs both. Of the first, roles
  with GRANT and REVOKE on tables come first, TLS second, and
  row-level security last, as it needs roles.

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
  does not wait is built for a row (`sm1::trylock`,
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

- **Views, the rest of them** (`VIEW.md` §10). CREATE, OR
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

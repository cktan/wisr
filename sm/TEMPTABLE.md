# TEMPTABLE — temporary tables

A temporary table belongs to one session: no other session
sees it, it is gone when the session ends, and nothing of it
survives a crash. This file is the design of temporary tables
across the levels: where their rows live, what a write to one
skips, and what it keeps.

It is built, at every level and in digres: §11 has the steps,
and §9 what was left for later. It replaced the mechanism
first proposed in `sm1/SM1_DESIGN.md` §11.3, which put a
temporary store's column family in the database's own
RocksDB. §10 is the list drawn up before the work, of what to
check first, kept as it was written.

---

## 0. The proposal in brief

- **One RocksDB instance for every temporary row**, called
  **X** here, at `{homedir}/base/pgsql_tmp/sm`. It belongs to
  the environment, not to a database: every database's
  temporary stores are column families of X.
- **X is never logged and never recovered.** Every write to
  it sets `disableWAL`, and `sm1::attach` destroys whatever
  it finds there and makes X empty.
- **A temporary table is a temporary store**, as decided
  (`DESIGN.md` §5): one column family of X for its heap, its
  indexes and its TOAST data.
- **Its definition is in the session's memory**, in a
  temporary catalog of its own, never in store 0
  (`sm3/CATALOG.md` §7).
- **Its transactions are the database's.** A row version in
  X carries the `xid` and timestamps of the session's
  transaction in its own database, and is resolved against
  that database's `CF:txn`. So commit, abort, savepoints and
  a statement's view of its own writes work as they do for
  any table, by the same code.
- **It has no concurrency control.** One session reaches the
  store, so nothing is locked, nothing waits, and no conflict
  is looked for.

| | A table | A temporary table |
| --- | --- | --- |
| rows | `base/{dbid}`, `CF:store.<id>` | X, `CF:<dbid>.<id>` |
| logged | yes | no |
| definition | store 0 | the session's memory |
| seen by | every session | its session |
| versions, snapshots | yes | yes, the same |
| abort, savepoints | yes | yes, the same |
| locks, conflicts, SSI | yes | none |
| after a crash | recovered | gone |
| index build | in the background | in the session |

---

## 1. Why not in the database's RocksDB

`sm1/SM1_DESIGN.md` §11.3 proposed a column family in the
database's own RocksDB, written with `disableWAL`. Two things
decided against it.

- **A database is its directory** (`../dg/CODE.md`, the
  overriding objective): `base/{dbid}` is copied to another
  machine and imported whole. What makes that package is what
  is transactionally consistent, a database's data and its
  WAL. Temporary rows are no part of it, so they are not to
  sit in its files, its MANIFEST or its column families.
- **RocksDB has no unlogged column family.** `disableWAL` is
  an option of one write, covering the whole batch. In a
  database's own RocksDB, unlogged would be a discipline of
  every write path, and one path that forgot would log a
  temporary table's rows. In an instance where every write is
  unlogged there is nothing to forget, and a write that did
  log would cost a little and harm nothing, since X is
  destroyed at the next start.

The price is that sm1 no longer has one RocksDB behind a
database: a store is in the database's instance or in X (§3).

---

## 2. X

- **Where.** `{homedir}/base/pgsql_tmp/sm`. `base/pgsql_tmp`
  is the host's directory of temporary files, shared by every
  database; `sm1::attach` makes it if the host has not, and X
  is the one name in it that is sm's. The sweep of `base` at
  attach already leaves alone an entry that is not a number
  (`sm1/SM1_DESIGN.md` §3.5).
- **Whose.** The environment's: `sm1::attach` opens it and
  `sm1::detach` closes it. It is shared between databases,
  as the host's temporary files are, and for the same reason:
  neither is part of a database's package.
- **Its life.** `sm1::attach` runs `DestroyDB` on the path
  and opens a new, empty instance, before any database is
  recovered. Nothing in X is ever read after a restart, so X
  has no recovery, no format version to check and no sweep.
- **Its column families.** One for each temporary store,
  named `<dbid>.<storeid>`: a `storeid` is a database's own,
  and two databases hand out the same ones. The comparator,
  the prefix extractor and the GC compaction filter are a
  store's, as in a database (`sm1/SM1_DESIGN.md` §3.1).
- **What it does not hold.** No `CF:txn`, no `CF:meta`, no
  catalog. X is a place to put versions and nothing else.
- **Every write is unlogged.** One function writes to a
  store, and it takes the write options from the store's
  instance: `disableWAL` for X.

---

## 3. sm1: temporary stores

### 3.1 A store names its instance

Today a store is a column family of `db->rdb`. A store
becomes a pair: the RocksDB instance and the column family in
it. Every read, write, iterator, snapshot and compaction of a
store goes through the pair.

- **A cursor's RocksDB snapshot** is taken from its store's
  instance. A cursor reads one store, so it needs one.
- **A batch does not cross instances.** A rollback to a
  savepoint deletes the versions it undoes in one batch
  today; it becomes one batch for each instance. No atomicity
  is lost: the deletes of a rollback are not atomic with
  anything, and a version left behind by a failure is dead
  with its transaction.
- **The commit needs no change.** Row versions and the commit
  record were never one write (`sm1/SM1_DESIGN.md` §8.2): the
  versions are in the stores, and the record goes to the
  database's `CF:txn`, logged and synced.

### 3.2 Making and dropping one

- **`sm1::mkstore` is asked for a temporary store** by a
  flag. It takes a `storeid` from the database's allocator,
  creates the column family in X, and records the store in
  the session: no row in store 0.
- **Private to the session.** A session's temporary stores
  are a set in the session. A call that names a temporary
  `storeid` the session does not hold fails with
  `SM_ERR_NOTFOUND`, as for a store that does not exist.
- **Transactional, as any store is.** A temporary store made
  by a transaction that aborts, or rolls back past its
  making, is dropped, by the list that does this for a store
  today. `sm1::rmstore` of one takes it out of the session's
  set at commit, and drops its column family then and there:
  no other snapshot can be reading it. An abort or a rollback
  leaves it in place.
- **`sm1::closedb` drops what the session still holds**,
  after aborting its transaction.
- **`sm1::lsstore` does not list them**: it reads store 0.

### 3.3 What a write keeps

Everything that makes a transaction a transaction, unchanged:

- **The version format** (`sm1/SM1_DESIGN.md` §3.1) and the
  visibility test (§4.4 there).
- **Status resolution** (§3.4 there), against the session's
  database. A version in X written by a transaction with no
  commit record is dead; so abort still writes nothing.
- **Savepoints** (§6.5 there): a write to a temporary store
  while a mark is set is listed in the undo list like any
  other, under the same cap, and a rollback deletes it.
- **A statement does not see its own writes**, and a cursor
  keeps the view it opened with.
- **Garbage collection.** An update or a delete leaves an old
  version, and the compaction filter of the store reclaims it
  below the database's watermark.

### 3.4 What a write skips

One session reaches a temporary store, and it runs one
transaction at a time. So, for a temporary store:

- **No lock.** `sm1::lock` on a key of it succeeds and enters
  nothing in the lock table. A write looks for no explicit
  lock held by another.
- **No wait and no conflict.** The newest version of a key is
  never another transaction's in flight, and never committed
  after the writer's snapshot by another. A write does not
  consult the lock table. It still takes the key's latch and
  reads the newest version, as it must to know whether the
  key exists, and the checks that follow cannot fire.
- **No SSI.** No SIREAD entry and no rw-edge is made for a
  read or a write of it.
- **No log.** §2.

### 3.5 What else differs

- **A read-only transaction may write one**, as PostgreSQL
  allows. It takes an `xid` and writes a commit record like
  any writer.
- **`sm1::prepare` refuses** a transaction that made, dropped
  or wrote a temporary store, with `SM_ERR_INVALID`: the
  transaction would outlive the session, and the store would
  not.
- **A temporary sequence** is asked of `sm1::mkseq` by the
  same flag. It is an entry in the session's memory: no row
  in store 0, no lease in `CF:meta`, nothing written at all.
  Making and dropping one follow the transaction; its values
  do not, as with any sequence. To its session the name
  means the temporary sequence, whatever else has it. A
  temporary table counts its rownos with one (§4).
- **A temporary-only transaction still writes to its
  database**: its commit record. That record need not be
  synced, since nothing it commits survives a crash; the sync
  is left in until it is measured (§9).

---

## 4. sm3: the temporary catalog

- **A second overlay, which commit does not publish.** A
  session has an overlay of its open transaction's DDL
  (`sm3/CATALOG.md` §4). The temporary catalog is the same
  structure with a longer life: definitions of the session's
  temporary tables, kept from commit to commit, and dropped
  when the session closes.
- **A change to it is still transactional.** A temporary
  table made or dropped by the open transaction is in the
  transaction's overlay, tagged with its savepoint depth, as
  now. At commit it moves into the temporary catalog instead
  of the shared one; an abort or a rollback drops it.
- **A call of its own asks for one**: `create_temp_table`,
  which is `create_table` without a namespace.
- **No temporary namespace.** sm3 keeps none: no oid is
  reserved for one and no name. The host names its own, with
  whatever oid it likes, as PostgreSQL does. A temporary
  table's definition has namespace 0 (`sm3::nsp_temp`), which
  no namespace has, and that is what marks it temporary.
- **The lookups by name take namespace 0** to mean the
  session's own temporary tables and their indexes. A session
  finds its own there and no one else's. `create_table` and
  the other creators refuse namespace 0 as one that does not
  exist.
- **Names** of a session's temporary tables and their indexes
  are one name space, checked in memory, since no key in
  store 0 holds them. A temporary name and the same name in a
  namespace do not meet, and two sessions may each have a
  temporary table of the same name.
- **A table's oid** comes from the database's `sm3.oid`
  sequence, as any table's does.
- **A table's rowno counter** is a temporary sequence of sm1
  (§3.5), in the session's memory: an ordinary one is a row
  in store 0 and a lease in `CF:meta`. sm2 draws from it by
  name, as it does for any table.
- **No table-oid lock.** `cat_lock_table` on a temporary
  table returns its definition and takes nothing.
- **Indexes are built in the session**, at once, whenever
  they are made. The builder works in sessions of its own,
  which cannot reach the store or the definition. This is
  the path a table made by its own transaction already took:
  it is chosen by the table's absence from the shared
  catalog, and a temporary table is never in it.
- **TRUNCATE** is a new temporary store and a drop of the
  old, as for a table (`sm3/SM3_DESIGN.md` §4).
- **Not kept yet, and refused**: statistics of a temporary
  table (`tbl_analyze` fails, and `cat_get_stats` gives
  none), and a temporary sequence or collation.
- **A prepare is refused** after any of it, as after any DDL.

---

## 5. The wrapper and the host

- **`include/sm.h`** gains `sm_cat_create_temp_table` and
  `SM_NSP_TEMP`, which is 0. The other calls take a temporary
  table's oid as they take any, and the lookups by name take
  `SM_NSP_TEMP`.
- **`ON COMMIT DELETE ROWS` and `ON COMMIT DROP` are the
  host's**: it truncates or drops the table before it
  commits.
- **The temporary namespace and the search path are the
  host's**: it maps its own temporary namespace to
  `SM_NSP_TEMP`, and looks there first, as PostgreSQL does.
- **`rel`** has `(temp-table-create ...)`, and finds a name
  among the session's temporary tables before `public`.
- **digres** stops refusing `CREATE TEMPORARY TABLE`, and its
  sweep of `base/pgsql_tmp` at startup leaves `sm` alone.

---

## 6. A crash, and an ordinary end

| Event | What happens to temporary rows |
| --- | --- |
| the transaction aborts | its versions are dead; stores it made are dropped |
| the session closes | its stores are dropped |
| the last session on a database closes | nothing is left to do |
| `sm1::rmdb` | no session is open on it, so it has no store in X |
| `sm1::detach` | X is closed |
| a crash | `sm1::attach` destroys X |

A crash leaves commit records in a database's `CF:txn` for
transactions whose only writes were in X. They name versions
that no longer exist, and are swept with the rest.

---

## 7. Hazards

- **The `CF:txn` sweep must cover X.** A `COMMITTED` record
  is deleted only once every version it covers is stamped
  with its `commit_ts` (`sm1/SM1_DESIGN.md` §8.3). The pass
  that stamps them walks the database's stores. If it skipped
  the database's stores in X, a temporary version would lose
  its record and read as aborted. It does not skip them: a
  database's map of stores holds its temporary ones, and the
  pass walks the map.
- **A compaction filter in X belongs to a database.** It
  reads that database's watermark and commit cache. Every
  temporary store of a database is dropped before its last
  session closes, but a compaction of one may still be
  running. A database that ever had a temporary store
  pauses X's background work as it closes, which waits for
  what is running, and lets it go on at once.
- **Many column families.** A session with a hundred
  temporary tables is a hundred column families of X, each
  with a memtable. X gets one write buffer budget for all of
  them, so the count costs handles and not memory.
- **Creating and dropping a column family is a MANIFEST
  write** under X's mutex. Sessions that make and drop
  temporary tables at a high rate meet there.

---

## 8. Alternatives considered

- **A column family in the database's RocksDB**
  (`sm1/SM1_DESIGN.md` §11.3). §1.
- **An instance per database, or per session.** No name
  prefix, and a database's temporary rows go with one
  `DestroyDB`. It costs an open and a close of RocksDB for
  each, and more of everything RocksDB keeps for an
  instance. One instance is opened once.
- **No transactions in X**: a write that is visible at once
  and cannot be rolled back. It is simpler, and wrong: in
  PostgreSQL a temporary table's rows roll back with their
  transaction, and callers reach savepoints without asking
  for them, through an ORM's nested transaction or `psql`'s
  `ON_ERROR_ROLLBACK`.
- **A cheaper rollback for temporary rows.** A savepoint
  costs an undo entry for each write made under it, for
  every table (`sm1/SM1_DESIGN.md` §6.5). Temporary tables
  are treated the same; `sm1/SAVEPOINT.md` is the design
  that removes the cost for both.

---

## 9. Left for later

- **Skipping the sync** of a commit record whose transaction
  wrote only temporary stores.
- **Statistics** of a temporary table: `ANALYZE` of one is
  refused.
- **`CREATE TEMPORARY SEQUENCE`**: sm1 has the counter (§3.5),
  and sm3's sequence catalog is not yet routed to the
  session.
- **Bulk load** into one (`BULKLOAD.md`).

---

## 10. To check, first

1. `disableWAL` in the RocksDB the build links: that it is an
   option of a write, that a flush still writes files, and
   that a column family may be created and dropped at will
   in an instance that logs nothing.
2. Every use of `db->rdb` in `sm1/src`, about thirty: which
   name a store and so must go through the pair of §3.1.
3. The `CF:txn` sweep's pass (§7): where it lists the stores
   it compacts.
4. That nothing but the rollback writes one batch to more
   than one store.
5. What digres's `RemovePgTempFiles()` does with a directory
   in `base/pgsql_tmp` it does not know.

---

## 11. Order of work

Each step lands on its own, with its tests.

1. **X**: `sm1::attach` destroys and opens it, and
   `sm1::detach` closes it. Built.
2. **A store names its instance** (§3.1), with every store
   still in the database's. No behaviour changes. Built.
3. **Temporary stores in sm1** (§3.2 to §3.5). Built, and
   temporary sequences with them.
4. **The temporary catalog in sm3** (§4). Built.
5. **The wrapper, and `rel`** (§5). Built.
6. **digres**: Built (`dg/WORKNOTE.md` step 80).
   `CREATE TEMPORARY TABLE`, `ON COMMIT`, and
   the temporary namespace in the search path.

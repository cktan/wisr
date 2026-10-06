# BULKLOAD — loading many rows in one transaction

`COPY`, `CREATE TABLE AS`, a large `INSERT ... SELECT` and a
restore all put millions of rows into one table in one
transaction. Row at a time through `tbl_insert` that works,
and costs memory in proportion to the rows. This file is the
design of a path that does not.

It is a design and not yet built. What is said here of
RocksDB is from memory of its documentation, and the stack
builds on 8.9.1. §8 lists what is to be checked and measured
before any of it is relied on.

---

## 0. The proposal in brief

**The idea**

- A load writes its rows into sorted files of its own,
  outside the database, and hands them to RocksDB whole:
  external file ingestion. The rows pass through no log and
  no memtable.
- A load holds one lock on the table that keeps other writers
  out. Under it, a lock on each row says nothing new, so none
  is taken.
- The load is still one transaction. Every version in the
  files carries its `xid`, and it commits or aborts whole, by
  the one status record every transaction has.

**How**

- The heap comes out sorted for free: rownos ascend. Its file
  is written as the rows arrive.
- An index's entries are sorted: in memory to a budget, in
  runs on disk past it, merged at the end.
- A duplicate in a unique index shows up as two equal
  neighbours in that merge.
- Everything is ingested at the end, in one atomic step.

**What it keeps**

- Atomic, as `COPY` is. No reader sees part of a load.
- Readers are not blocked by the lock.
- Every check a row gets today: `NOT NULL`, unique indexes,
  the host's evaluator for partial and expression indexes.
- Memory is bounded by the sort budget, not by the load.

**What it gives up**

- Other writers of the table wait until the load's transaction
  ends. A load that must not block them uses `tbl_insert`.
- The transaction cannot read the rows it loaded until the
  load is closed.
- A duplicate key is found at the end, not at its row.
- A rollback to a savepoint cannot undo a load. The
  transaction aborts instead.
- It inserts. There is no bulk update or delete.

**What has to be built**

- sm1: a loader on a store, with the files, the sort, the
  merge and the ingestion; a sequence call that draws a run
  of values; a sweep of leftover files at startup.
- sm2: heap and index inserts that feed it.
- sm3: a loader on an open table.
- `include/sm.h` and `rel`: the calls, and a verb.

**What is not known**

- How much faster it is. Nothing has measured the present
  path, so §8's first step is to.
- Several of RocksDB's behaviours the design leans on (§8).

---

## 1. The problem

`tbl_insert` on a table with `k` indexes writes `1 + k` keys.
For each key sm1 does this (`sm1/SM1_DESIGN.md` §5):

| Step | Cost |
| --- | --- |
| take the key's lock | the lock table's one mutex, and an entry in `T.locks` kept to the end of the transaction |
| take a write timestamp | one use of the global counter and its lease |
| write the version | one RocksDB `Put`: logged, then put in a memtable, flushed, and compacted down the tree |
| remember it for a savepoint, if one is set | an entry in `T.undo`, with the key inline, kept to the end |
| tell SSI | one lookup |

**Memory.** The lock entries are kept for the life of the
transaction, and the undo entries too when a savepoint is
set, so a load of `n` rows holds `n × (1 + k)` entries or
twice that. `sm1/SM1_DESIGN.md` §13 records it: a transaction
writing tens of millions of keys costs gigabytes, and "bulk
loads want batching into many transactions."

Batching into many transactions is what this design avoids.
It would bound the memory and lose the atomicity: a failed
`COPY` would leave half its rows committed, and a reader
would see a table half loaded.

**Writing everything several times.** A row put through the
ordinary path is written to the log, then to a level-0 file,
then rewritten at each level compaction moves it down. A
loaded row is written once, into a file that is already where
it will stay.

---

## 2. What a load must keep

1. **One transaction.** All of it commits or none does, and
   it may share its transaction with other statements.
2. **Readers never wait on a lock.** No reader of the table
   is held up by a load, as none is by a write.
3. **One way to make a row's entries.** The loader calls the
   routines `tbl_insert` calls to check a row and to make its
   index entries. It differs only in how they reach RocksDB.
4. **Bounded memory.** What a load holds does not grow with
   the rows loaded.
5. **Crash safe by what is already there.** No log of its
   own, and no recovery but deleting files left behind.
6. **Tuple at a time across the API** (`DESIGN.md` §5). The
   host hands rows over one by one.

---

## 3. The mechanism

### 3.1 A table lock stands in for the row locks

A writer holds its table's schema lock IX, and a reader IS
(`sm3/SM3_DESIGN.md` §2). A loader takes it **SIX**.

| Held by another | Against the loader's SIX |
| --- | --- |
| IS, a reader | compatible: readers pass |
| IX, a writer or an index build's batch | waits, or is waited for |
| S, SIX, X: another loader or a DDL | waits, or is waited for |

So once the loader holds SIX, it is the only transaction that
can write any key of the table's store, and it stays so to
the end of its transaction. Three things follow.

- **A row lock would exclude no one.** The only transactions
  it could keep out are already kept out. The loader takes
  none, and `T.locks` does not grow.
- **Every other version in the store is settled.** SIX waited
  out every writer in flight, prepared ones among them. What
  the loader finds under a key is committed or dead, never
  another transaction's unfinished write.
- **A reader's row lock does not meet the loader.** A reader
  may lock an existing row shared (`tbl_lock`), as a host
  does to check a foreign key. The loader touches no existing
  row: it adds rows under new rownos and adds index entries.

A table made in the same transaction is held X by its
creator, which covers SIX. It needs no case of its own.

sm1 keeps no lock hierarchy and cannot know that one key
stands for a table. So sm1's loader is told which lock covers
it, and refuses unless the transaction holds that lock SIX or
X. The check is cheap and it is what stops a caller that
forgot the lock from writing unprotected.

While a loader is open on a table, the transaction's ordinary
writes to that table are refused. One at a time keeps the
timestamps of §3.5 in order.

### 3.2 The files

A file is written with RocksDB's `SstFileWriter`, using the
store's own options and comparator, into a directory of the
database kept for loads. A key must be added in the
comparator's order, which is the whole of the difficulty.

What goes into a file is exactly what a write would have put
into RocksDB: a version, keyed `userkey | ~wts | ~seq`, with
the transaction's `xid` in its value
(`sm1/SM1_DESIGN.md` §3.1). A version in an ingested file is
no different from one in a memtable. It is uncommitted until
the transaction's status record says otherwise, so the files
can be handed to RocksDB before the commit.

sm1's loader takes keys in **streams**. A stream is declared
ascending or not, and unique or not.

| Stream | Declared | So sm1 |
| --- | --- | --- |
| the heap | ascending | writes each key straight into a file |
| a non-unique index | unsorted | sorts it (§3.3) |
| a unique index | unsorted, unique | sorts it and checks it (§3.4) |

**The heap is ascending for free.** A row's key is its
`rowno`. The loader draws rownos in runs from the table's
sequence, and no one else draws from it while SIX is held, so
its rownos ascend and are above every `rowno` the table has.
A heap file is started, filled as rows arrive, and closed at
a size limit, and the next one started.

### 3.3 Sorting an index

An index's entries arrive in the heap's order, not the key's.
It is an external sort, in the plain two-phase form.

1. **Runs.** Entries collect in memory up to a budget, a
   knob. When it is full they are sorted and written out as a
   run, and the memory is reused.
2. **Merge.** At the end the runs are merged into the files
   to ingest, which do not overlap each other. A stream that
   never filled its budget is sorted and written once, with
   no run.

Memory is the budget, shared by a table's streams. Disk is,
for a moment, twice the index: the runs and the merged files
together, until the runs are deleted.

**The alternative is to skip the merge**: write each run as a
file and ingest them all. RocksDB accepts files that overlap
each other and its compaction merges them later. That is less
code, and it moves the merge into the database: tens of
overlapping files in level 0, compaction rewriting the index
at least once more, and reads of the index slow until it has.
The merge here is a small routine over sorted streams, and it
is where §3.4 does its work. The merge is kept.

### 3.4 Unique keys

A non-unique entry ends in its row's `rowno`, so it is new by
construction, and the heap key is the `rowno` itself. Only a
unique index's key can collide.

| Collides with | Found by |
| --- | --- |
| another row of the load | the merge: equal keys come out as neighbours |
| a row already in the table, or written earlier by this transaction | a read of the key as it comes out of the merge |

The read is the check `sm1::insert` does today
(`sm1/SM1_DESIGN.md` §5, step 3), with the same outcomes: a
live version is `SM_ERR_DUPKEY`, and a version committed after
the snapshot of a transaction above READ COMMITTED is
`SM_ERR_CONFLICT`. The keys are read in ascending order, which
is kind to the block cache, and a bloom filter answers for a
key that was never there.

**An index with nothing under its prefix needs no reads.**
The loader looks once when it opens. No one else can write
the index while SIX is held, so nothing found then is nothing
to collide with later. That is every index of a table being
restored or made by `CREATE TABLE AS`.

A key too long for a unique index is refused as it is today
(`sm3/SM3_DESIGN.md` §7).

**All of this happens at the end.** A duplicate in the tenth
row of a hundred million is found after the last row is
handed over. PostgreSQL finds it at the tenth row. The work
lost is the price of sorting before checking.

### 3.5 Timestamps and `seq`

A version's key is `userkey | ~wts | ~seq`.

- **`wts` is taken once, when the loader opens**, after the
  covering lock is held. The rule it serves is that a key's
  versions sort in commit order. Every older version of any
  key in the store was written before the loader got its
  lock, and the transaction writes the table by no other path
  while the loader is open (§3.1).
- **`seq` is taken once.** It exists so that a transaction
  writing one key twice makes two versions. A load writes
  each key once: the heap keys are distinct rownos, and §3.4
  refuses a repeated unique key.

One `seq` a load is also what makes a large load possible.
`seq` is four bytes and counts a transaction's writes, so
four billion keys written one `seq` each would exhaust it.

### 3.6 Closing: ingestion

Closing the loader finishes the merges and then ingests every
file of the load in one call, which RocksDB applies
atomically.

- **Files are moved, not copied.** The load directory is on
  the database's filesystem so that RocksDB can link them in.
- **The heap's files overlap nothing**, so RocksDB can place
  them at the bottom of the tree, where no compaction has
  reason to rewrite them.
- **An index's files** overlap nothing if the index was
  empty, and go to the bottom too. Otherwise RocksDB places
  each at the lowest level it fits, level 0 at worst, and
  compaction folds it in.
- **A memtable holding keys in a file's range is flushed
  first.** That can only be an index of a table recently
  written. The heap's new rownos are in no memtable.

From then the transaction sees its loaded rows, by the load's
`seq`, and other transactions do not, by its `xid`.

Before the close nothing of the load is in the database. The
transaction cannot read the rows it has handed over, by the
heap or by any index.

### 3.7 Commit, abort, crash and prepare

- **Commit** is the one status record. Every version of the
  load carries the transaction's `xid` and becomes visible
  with it. A commit with a loader still open is refused.
- **Abort before the close** deletes the load's files. Nothing
  reached the database.
- **Abort after the close** writes nothing, as any abort. The
  versions resolve as dead, and compaction drops them when it
  next visits their files. A table made in the same
  transaction loses its store whole.
- **A crash before the close** leaves files in the load
  directory that no database knows. Opening the database
  empties the directory.
- **A crash after the close and before the commit** leaves no
  status record, which is an abort.
- **Prepare** needs the loader closed, and records the locks
  the transaction holds, with their modes. The SIX schema
  lock is one of them, so after a restart an in-doubt load
  still keeps writers out, as the row locks would have.

**Garbage collection is unchanged, and has one cost here.** A
loaded version carries its `xid` until compaction stamps the
commit timestamp into it (`sm1/SM1_DESIGN.md` §8.3). Files at
the bottom of the tree are not visited by ordinary
compaction, so a load's versions are stamped by the periodic
sweep, which rewrites every store. Until then each read of
one resolves its `xid`, from the commit cache or the status
table. It is correct, and it is the same for any cold file.

Dead versions from a load aborted after its close also wait
for that sweep, if their files went to the bottom. Deleting
the heap's range at once is possible, since the loader knows
it, and is not proposed until the space matters.

### 3.8 Serializable readers

A serializable reader's SIREAD locks are checked by every
write, a key at a time (`sm1/SM1_DESIGN.md` §7.1). The loader
makes the same call for every key it writes. It keeps nothing
in the loader's transaction, so it does not grow, and a store
no serializable reader has touched pays one lookup a key.

A loader that is itself serializable reads nothing but the
unique keys it checks, and those reads are recorded like any
other. A large serializable load into a table with a unique
index and rows therefore still grows a read set. READ
COMMITTED is the level to load at.

### 3.9 Savepoints

A savepoint is a mark into `T.undo`, and a rollback deletes
the versions listed after the mark
(`sm1/SM1_DESIGN.md` §6.5). A load lists none.

With no savepoint set, an ordinary write lists none either,
and the question does not arise. It arises for a load made
while a savepoint is set.

**Decision proposed: a rollback past a load is refused.** The
transaction remembers the load's `seq`. A rollback to a mark
older than that fails, and the caller aborts the transaction.

- A failed load is the common case, and it needs nothing: a
  failed `COPY` aborts its transaction in PostgreSQL too,
  unless it ran inside a savepoint.
- A load inside a savepoint that is then rolled back is rare:
  a `COPY` in a PL/pgSQL block with an exception handler.
- A rollback that drops a table made after the mark still
  works, since the store goes whole.

`sm1/SAVEPOINT.md` removes the question. Once a rollback
marks a range of `seq` dead, a load's versions are covered by
it like any other, and the refusal goes.

### 3.10 A small load

Each ingestion adds files, and a file of a hundred rows at
the bottom of the tree stays a file of a hundred rows: no
compaction has reason to merge it with its neighbours. A
thousand small loads would leave a thousand such files.

The host cannot always know a load's size when it starts; a
`COPY` reads until its input ends.

**Decision proposed: a load that ends small is written the
ordinary way.** The loader keeps its first rows in memory and
starts no file until they pass a threshold. If the load is
closed before that, the rows go through `tbl_insert`'s own
write, with the rownos already drawn. The loader holds SIX
either way.

### 3.11 Indexes

A load maintains every index of the table, each as a stream.
An index that is still building is maintained too, as any
writer maintains it.

A restore does not need that. `pg_dump` loads a table's rows
before it creates the indexes and the primary key, so the
load there writes the heap alone, and the indexes are the
concurrent build's (`sm3/SM3_DESIGN.md` §3). That build goes
a row at a time through the ordinary path, and how fast it is
has not been measured (`NEXT.md` §2). For a restore it is the
larger half of the work, and sorting an index into files is
what §3.3 builds. Giving the build the same streams is the
natural next step and is not part of this design.

A backfill batch holds the schema lock IX. So a load waits
for the batch in flight, and the build waits for the load.

---

## 4. The interface

In sm3, on a table opened SIX or X:

```cpp
// Begin a load into table.
int tbl_load_open(table_t* table, loader_t*& loader);

// Hand over one row. Its rowno is known at once.
int tbl_load_add(loader_t* loader, std::span<const sm2::datum_t> values,
                 std::span<const bool> isnull, uint64_t& rowno);

// Sort, check, and ingest. The transaction is still to commit.
int tbl_load_close(loader_t* loader);
```

`include/sm.h` has the same three as `sm_load_*`, in its C
style. The sort budget and the file size are options of the
database, not arguments.

- **Errors.** `tbl_load_add` fails at once for what it can
  see in the row: a NULL in a `NOT NULL` column, a key too
  long for a unique index, an evaluator that fails. A
  duplicate key fails the close, and the message names the
  index and both rows' rownos. After any failure the
  transaction is to be aborted, which deletes the files.
- **Nothing is readable before the close.** A statement that
  must read the table as it loads, to fire a trigger that
  does, cannot use the loader. PostgreSQL's own `COPY` falls
  back to one row at a time for such a table, and the fork
  does the same with `tbl_insert`.
- **Which path a statement takes is the host's choice.**
  `tbl_insert` where other writers must not wait, or the
  table must be read meanwhile; the loader for the rest.

---

## 5. What it costs

- **Writers wait for the whole transaction**, not for the
  load alone: the lock is held to commit, as every lock is.
  PostgreSQL's `COPY` does not shut writers out. This is the
  design's main price, and the reason `tbl_insert` stays the
  default.
- **A late duplicate wastes the load** (§3.4).
- **Nothing is readable before the close** (§3.6).
- **No rollback to a savepoint** (§3.9).
- **Disk for the sort**: for a moment, twice the indexes.
- **A load is a long transaction.** It holds back garbage
  collection across the database for as long as any long
  transaction does.
- **Ingestion pauses the database's writes for a moment**
  while RocksDB fits the files in, and for longer if it must
  flush a memtable first. How long is to be measured.
- **A second write path in sm1**, and the first code in the
  stack that writes files of its own. It shares the version
  format, the visibility rules and the commit with the
  ordinary path.

---

## 6. Alternatives considered

| Alternative | Why not |
| --- | --- |
| Many small transactions | Bounded memory, but not atomic: a failed load leaves rows behind and readers see a table half loaded |
| Batches through the ordinary write path, under the same table lock | Bounded memory, and rows readable as they go. But every row is still logged, flushed and compacted down the tree, which is most of the cost of a load |
| Keep the row locks and undo, spilled to disk | Atomic and blocks no one, but keeps every per-row cost and adds a spill file with a life of its own |
| Lock escalation: trade row locks for a table lock past a threshold | The same end state as §3.1, reached by a conversion in mid-statement that can deadlock, and sm1 has no hierarchy to escalate along |
| Ingest each index's runs unmerged | §3.3 |
| Skip the log for an ordinary write (`disableWAL`) | Saves the log and keeps the memtable and the compactions. Ingestion saves all three |

Other RocksDB facilities for loading, and why they are not
used:

- **`PrepareForBulkLoad`**, a preset that turns compaction
  off and compacts once at the end. It tunes the memtable
  path, which the load no longer takes.
- **The vector memtable**, fast to fill and slow to read. The
  same.
- **`CreateColumnFamilyWithImport`**, which makes a column
  family from finished files. It would suit a table made and
  loaded in one step, but the store exists before the load
  does (`sm1/SM1_DESIGN.md` §11.2), and plain ingestion into
  an empty store ends in the same place.

---

## 7. What has to be built

| Level | What | For |
| --- | --- | --- |
| sm1 | a loader on a store: the covering-lock check, one `wts` and `seq`, streams, files, the sort and the merge, the unique check, the SSI calls, and ingestion | §3.1 to §3.8 |
| sm1 | the load directory, emptied when the database opens; a loader's files deleted by abort | §3.7 |
| sm1 | commit and prepare refused with a loader open; a rollback refused past a load | §3.7, §3.9 |
| sm1 | a sequence call that draws a run of values | §3.2 |
| sm1 | options: the sort budget, the file size, the small-load threshold | §3.3, §3.10 |
| sm2 | heap and index inserts that feed a stream in place of writing | §3.2 |
| sm3 | `tbl_load_*`: the calls into `tbl_insert`'s own routines, and the small-load path | §3.10, §4 |
| top | `sm_load_*` in `include/sm.h` | §4 |
| `rel` | a `load` verb | testing |
| fork | `COPY` and the other bulk statements choosing a path | §4 |

---

## 8. To check and to measure, first

**Check**, against RocksDB 8.9.1 and the code:

- `SstFileWriter` writes with a custom comparator and the
  column family's options, and its files ingest into that
  column family;
- one ingestion call takes many files and applies them
  atomically, and files that overlap each other or existing
  data are accepted and placed as §3.6 says;
- an ingested file is durable once the call returns, so that
  a commit record written after it never outlives its rows;
- what ingestion blocks, and for how long: writes to the
  store, to the database, and the flush it may force;
- an iterator or snapshot opened before an ingestion does
  not see the file, and a compaction filter sees its keys as
  it sees any other;
- what sm1 does today when a transaction's `seq` runs out;
- that no caller under IS can lock or write a key the loader
  inserts, beyond `tbl_lock` on a row that exists.

**Measure**, each with a small program and no PostgreSQL:

1. **The present path.** Load ten million rows through
   `tbl_insert`, with no index and with three.
   → report: rows a second, peak memory, bytes written to
   disk against bytes loaded.
2. **The heap by ingestion.** The same rows written to files
   and ingested, into an empty store and into one with rows.
   → accept: memory flat in the row count. Report the gain,
   where the files land, and the pause of the ingestion.
3. **An index by ingestion.** Sorted with a budget far below
   its size, merged and ingested, into an empty index and
   into one with entries.
   → report: the gain, the disk used, and the state of the
   tree afterwards.
4. **The small load.** Loads of ten rows to a million, each
   way.
   → pick the threshold of §3.10.

---

## 9. Order of work

1. The checks of §8, and measurement 1.
   → verify: each written up here, and the design changed
   where a check fails.
2. sm1's loader for one ascending stream, tests first.
   → verify: refused without the covering lock; the rows
   invisible to others until commit and gone after an abort;
   `T.locks` and `T.undo` do not grow; files left by a crash
   are deleted at open; a rollback past a load is refused.
3. Sorted streams and the unique check.
   → verify: runs and merge with a budget of a few rows; a
   duplicate inside the load, against a committed row, and
   against the transaction's own, under each isolation level.
4. sm2's and sm3's loader, and the small-load path.
   → verify: after a load the indexes and the heap agree with
   a brute-force scan, with a partial and an expression index
   among them, for a load on each side of the threshold.
5. Two sessions, as `rel` scripts and with `qa_nowait`.
   → verify: a reader passes a load in flight and sees none
   of it; a writer is blocked until it commits.
6. Crash and prepare.
   → verify: a load cut off by a crash leaves nothing; a
   prepared load still blocks writers after a restart, and
   commits.
7. Measurements 2 to 4.

The fork's own work in §7 is outside this repository.

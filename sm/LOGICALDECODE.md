# LOGICALDECODE — a change feed for replication

A database's committed changes, in commit order, as a stream a
replica or any other consumer reads to stay in step with it:
change data capture. `FEATURELIST.md` §7 wants logical
replication and CDC decoding; `PART4.md` §4 and
`sm3/SM3_DESIGN.md` §2 note that sm1 has no commit-ordered
change feed to build it on. This file is the design of one.

**Status: proposed.** Nothing here is built. §12 is what is
to be decided first.

Paths are relative to `sm/`; digres's are under `../dg/`. A
section number alone is this file's.

---

## 1. What it is for

The feed is for replication first: a consumer applies it to
keep a copy of a database, another digres, another
PostgreSQL or a system of another kind, and a replica must be
able to start, stop, fall behind and catch up without the
primary's help. That asks for:

- **Every committed change, once, in commit order**, grouped
  by transaction, and nothing of a transaction that did not
  commit.
- **A position that totally orders the stream**, durable, so
  a consumer resumes where it stopped and applies each
  transaction exactly once.
- **A consistent start**: a copy of the tables at some point,
  and the stream from exactly that point on, with no change
  missed or applied twice.
- **Enough to apply a change elsewhere**: each row's values,
  and for an update or a delete the values that identify the
  row, since a replica's rownos are not the primary's.
- **Schema changes in the stream**, at their place in it, so
  a consumer always knows the shape of what it reads.
- **A bounded cost on the primary.** A consumer that falls
  behind must not make the primary's storage grow without
  bound, nor hold back its garbage collection.

## 2. How PostgreSQL does it

PostgreSQL decodes its write-ahead log.

- **`wal_level = logical`** makes the WAL carry what decoding
  needs beyond physical redo: the old row's key (or the whole
  old row, by `REPLICA IDENTITY`) for an update or a delete,
  and enough about catalog changes to rebuild the catalog as
  it was at any point.
- **A replication slot** names a consumer, and holds two
  horizons for it: `restart_lsn`, the oldest WAL it may still
  need, which the server keeps; and `catalog_xmin`, the oldest
  catalog row versions it may still need, which vacuum keeps.
  `confirmed_flush_lsn` is how far the consumer says it got.
- **The reorder buffer** collects each transaction's changes
  from the interleaved WAL, spilling to disk past
  `logical_decoding_work_mem`, and hands each transaction over
  whole at its commit record, in commit order.
- **Historic snapshots** decode each change with the catalog
  as it was when the change was made: a time-travel view of
  the catalog tables, which is why the slot pins
  `catalog_xmin`.
- **An output plugin** turns changes into messages:
  `pgoutput`, which logical replication and Debezium read,
  filtered by **publications** (tables, and since 15 columns
  and rows); `test_decoding` and `wal2json` are others.
- **A consistent start**: creating a slot finds a point from
  which every later transaction is decoded whole, and exports
  a snapshot at that point for the initial copy.
- **Not decoded**: DDL, sequences (as of 18), large objects.
  A subscriber must be given the schema by hand.

The costs follow from the WAL being the source. Everything
decoding needs must be in the WAL, so it is written twice,
once for redo and again for decoding. A slot whose consumer
stops pins WAL and catalog garbage until the disk fills. And
decoding needs the catalog's history, which is what the
historic snapshots and `catalog_xmin` are for.

## 3. What is different here

Five facts of sm1 and sm3 shape the design:

- **Commits are already totally ordered.** `commit_ts` is
  taken from the one counter, and commits are published in
  `commit_ts` order: `visible_ts` stays below a commit still in
  progress, so nothing at or below it can still appear
  (`sm1/SM1_DESIGN.md` §4.3). A snapshot is one integer, and
  sees exactly the commits at or below it (§4.4 there).
  `commit_ts` is the LSN of this design, with no log behind
  it.
- **The commit point is one synced batch.** A commit is the
  status record written in one `WriteBatch` to `CF:txn`,
  synced and group-committed (`sm1/SM1_DESIGN.md` §8.2).
  Anything else written in that batch commits with it, at no
  extra sync.
- **Writes are eager, and undone by key.** A version is in
  RocksDB from the moment it is written, under its writer's
  `xid`, with no record of the transaction's writes kept in
  memory (`sm1/SM1_DESIGN.md` §5, §6.5). A rollback deletes
  the versions written since its mark and rewinds `seq`; an
  abort writes nothing, and a crash leaves versions no status
  record covers, which are dead (§3.4 there).
- **The WAL is RocksDB's, and not all of a write goes
  through it.** A bulk load in file mode ingests sorted files
  past the WAL (`BULKLOAD.md`), a dropped store is a column
  family drop, and a range delete is one record for many
  keys. Decoding the WAL would meet each.
- **The catalog is not versioned, but a column's type never
  changes.** sm3 keeps one definition of a table, replaced
  whole at commit (`sm3/SM3_DESIGN.md` §2); it adds and drops
  columns, and changes no column's type, so a table's newest
  definition reads every tuple the table ever held: a dropped
  column keeps its place, an added one reads as its missing
  value (`sm2/TUPLE.md` §2). A tuple can be read later without
  the catalog's history, provided the definition travels with
  the stream.

## 4. The options

- **A. Decode RocksDB's WAL, as PostgreSQL decodes its own.**
  Tail each database's WAL (`GetUpdatesSince`), collect each
  `xid`'s versions in a reorder buffer, and hand a transaction
  over when its status record goes by. It adds nothing to the
  write path. But the WAL must be kept for the slowest
  consumer; a bulk load in file mode, a store drop and a
  range delete are not in it as rows; the versions are raw
  keys and sm2 tuples, to be decoded against a catalog that
  may have moved on; and the reorder buffer, with its spill to
  disk, is PostgreSQL's whole machinery again.
- **B. Write the transaction's changes at commit.** Keep the
  changes in memory and write them in the commit's batch.
  Simple, and one write a change, but the memory is linear in
  the transaction's writes, which sm1 avoided everywhere
  (`sm1/SM1_DESIGN.md` §6.5), and a bulk load of a billion
  rows has no commit batch that holds them.
- **C. Write a change record beside each write, committed by
  the status record.** sm3 writes one record for each row it
  inserts, updates or deletes, into a column family of its
  own in the same database, keyed by the writer's `xid` and
  `seq`, as eagerly as the version. The commit adds one
  entry, `commit_ts → xid`, to the batch it already writes.
  Nothing is held in memory, the records are as durable as
  the commit, and the key order is the reorder buffer.
- **D. Write the change records to a file, outside RocksDB.**
  An append-only file a transaction writes as it goes,
  reconciled with `CF:txn` after a crash. But it is a second
  thing to make durable at each commit: either a second sync,
  or a window where a commit is durable and its changes are
  not. Two logs that must agree are what sm1 was built
  without (`sm1/SM1_DESIGN.md` §8.1).
- **E. As C, with each record naming its version** instead of
  copying its row, the row read back when the record is
  drained. No row is written twice, but the version must
  outlive the drain: the drain's position becomes a horizon
  that garbage collection and store drops wait for, which is
  PostgreSQL's slot problem in another place, and a TOAST
  datum an update replaced is erased with it
  (`sm2/SM2_DESIGN.md` §4.2).

**Choice: C, drained into files.** The change log in RocksDB
is the truth, committed with the data; it is short-lived, a
staging area of seconds. A drain reads it in commit order and
appends each transaction to a stream of segment files, which
are what consumers read, ship and keep. That is the file
stream D wants, without D's second commit point: a file is
written only from what has committed, and a crash at any
moment leaves either the log entry or the file's copy of it,
never neither. The cost is each logged row written twice, as
PostgreSQL's `wal_level = logical` writes it twice, and only
for the tables a publication names (§6.6). E is the way to
take that cost out later, if it matters, once a horizon is
acceptable.

```
  sm3 write ──▶ version, in the table's store      ┐
            └─▶ change record, CF:changes          │ one transaction,
  sm3 DDL   ──▶ schema record, CF:changes          │ eager, unsynced
                                                   ┘
  sm1 commit ─▶ CF:txn status + CF:changes index   one synced batch

  drain (dg) ─▶ commit_ts order, up to visible_ts
              ─▶ segment files  ─▶ consumers, shipping
              ─▶ trim CF:changes once a segment is synced
```

---

## 5. The change log: sm1

sm1 keeps the records and orders them, and knows nothing of
what they say: a record is bytes, as an index's `eval` is to
sm3. In this section "sm1's §n" is `sm1/SM1_DESIGN.md` §n.

### 5.1 Where it lives

A column family, `CF:changes`, in each database, made with
the database or on first use in one made before it, beside
`CF:txn` and `CF:meta`, with the bytewise comparator. It is
the database's own, so the change log goes where the database
goes. Two kinds of key:

| Key | Value | Written |
| --- | --- | --- |
| `"T" ‖ xid ‖ seq ‖ n` | a record | with the write it describes |
| `"C" ‖ commit_ts` | `xid`, the wall-clock commit time in microseconds, the count of records | in the commit's batch |

`xid`, `seq`, `n` and `commit_ts` are big-endian, so a
transaction's records are together in the order written, and
the commits in `commit_ts` order. `n` is 0 but in a bulk
load, whose rows share one `seq` (`BULKLOAD.md` §3.3) and are
told apart by their place in the load.

### 5.2 Writing

- **`sm1::log_append(session, record)`** writes
  `"T" ‖ xid ‖ seq ‖ 0`, taking `seq` as a write does,
  `T.seq++` and `T.seq_hwm` with it (sm1's §5): a record is
  ordered among the transaction's versions, and undone with
  them. It is an ordinary `Put`, WAL-logged and not synced, as
  a version is. It takes the transaction's `xid` if it has
  none, and sets `T.logged`.
- **Commit** of a transaction with `T.logged` adds
  `"C" ‖ commit_ts` to the batch of sm1's §8.2, step 3, so the
  entry is durable exactly when the status record is.
  `commit_prepared` does the same. A transaction that logged
  nothing writes no entry, so the feed passes over it.
- **A rollback to a savepoint** deletes the records written
  since its mark with one `DeleteRange` from
  `"T" ‖ xid ‖ mark.seq_hwm + 1` to the end of the `xid`, in
  the batch of its other deletes. `seq` is then rewound, and a
  later record reuses a number whose record is already gone
  (sm1's §6.5).
- **An abort** deletes `"T" ‖ xid` whole with one
  `DeleteRange`. A crash leaves the records of a transaction
  that never committed; they have no `"C"` entry, so nothing
  reads them, and §5.4 removes them.
- **A prepare** syncs the records with everything else; the
  `"C"` entry is written by `commit_prepared`, and
  `abort_prepared` deletes them as an abort does.
- **A bulk load** writes its records as its versions: in its
  own batch for a small load, and for file mode into a sorted
  file of `CF:changes` keys, ingested with the store's files in
  one atomic `IngestExternalFiles` across both column
  families (`sm1::load_log`). A rollback past the load deletes
  them with the `DeleteRange` above, as it empties the store.

### 5.3 Reading and trimming

- **`sm1::feed_open(env, dbname, after, feed)`** opens a
  reader of the commits after `commit_ts` `after`.
  **`sm1::feed_next(feed, commit)`** gives the next one, in
  `commit_ts` order, with its `xid`, wall-clock time and
  records; 1 when there is none at or below `visible_ts`.
  Reading no further than `visible_ts` is what makes the order
  safe: an entry above it may be in RocksDB already, written
  by a commit that has not yet been marked done, and one below
  it still to come would be a gap (sm1's §4.3).
- **`sm1::feed_trim(env, dbname, upto)`** deletes the `"C"`
  entries at or below `upto`, and each one's `"T" ‖ xid`
  range, once the drain has what they held.
- The reader takes no lock and no snapshot: the records it
  reads are committed, and nothing changes them but the trim.

### 5.4 Garbage

A rollback and an abort delete their records as they go, so
only a crash leaves records behind: those of the transactions
that were running. A **sweep at open**, after recovery, finds
them. It reads the `"C"` entries, the commits not yet
trimmed, and the prepared transactions recovery restored, and
deletes with one `DeleteRange` each `"T" ‖ xid` that is
neither. `CF:changes` holds seconds of commits when the drain
keeps up, so the sweep reads little; a committed record is
kept until it is trimmed, however long that is, which is why
the drain must keep up (§7.5).

It is not a compaction filter resolving each `xid` by sm1's
§3.4, as the stores' is, because `CF:txn`'s sweep (sm1's
§8.3) drops a `COMMITTED` record once the transaction's
versions are stamped, whether or not its records are
drained, and the `xid` would then resolve dead. The `"C"`
entry is what says a transaction committed here: it carries
the `xid` and the commit time, so neither the drain nor the
sweep at open asks `CF:txn`.

---

## 6. Change records: sm3

sm3 knows which rel a write is to, which table that is, and
what the row holds, so it is sm3 that writes the records.
Each is a header, an sm2 tuple framed as the catalog's parts
are (`sm3/CATALOG_ONDISK.md` §3), so that its schema evolves as a
part's does, then the row's values, each its bytes with their
length. A long value is in it whole (§6.3), so a record is
held to an sm1 value's 64 MB, not to a tuple's 64 KB; a row
past that is split over records of one `seq`, told apart by
`n` (§5.1).

### 6.1 The kinds

| Kind | Holds | Written by |
| --- | --- | --- |
| insert | table oid, rowno, the new row | `tbl_insert`, `tbl_insert_arbiter` when it inserts, a load |
| update | table oid, rowno, the old row's identity, the new row | `tbl_update` |
| delete | table oid, rowno, the old row's identity | `tbl_delete` |
| truncate | table oid | `truncate_table` |
| schema | what changed: the kind and oid of a namespace, table, sequence or view, and its definition as the catalog keeps it, or that it was dropped | the commit of DDL that changed a logged object |
| sequence | sequence oid, the lease's end | a lease renewal of a logged sequence |

- **Only the heap is logged.** A table's index entries and
  TOAST chunks are writes to its store too (relids
  `0x0002`–`0xFFFF`, `sm3/SM3_DESIGN.md` §4), but they follow
  from the row and from the definition, and a consumer has no
  use for them. A row write is logged once, after its index
  entries are written, so a write refused partway logs
  nothing a rollback must undo beyond the versions.
- **The rowno is carried** so that a replica that keeps the
  primary's rownos, as a digres replica could, applies a
  change by it; any other replica applies by identity.

### 6.2 Identity

An update or a delete must say which row it was, by what a
replica can find it by. As in PostgreSQL, a table has a
**replica identity**: by default its primary key, whose
columns the old row's identity holds; or `FULL`, every
column of the old row. A table with neither, published, has
its updates and deletes refused, as PostgreSQL refuses them.
The old row is at hand: `tbl_update` and `tbl_delete` pin it
for the host already (`ROW.md`), and the record takes the
identity's columns from it. An update that changes none of
the key's columns carries no old identity, as PostgreSQL's
does not: the new row's key is it.

### 6.3 Long values

A record carries its row's values whole, with one exception.
A datum the host gave in full is in the record in full, not
as the TOAST pointer the version holds, so a record needs no
chunk to be read. An update that keeps a TOAST pointer
unchanged, which it does for an attribute it does not change
(`sm2/SM2_DESIGN.md` §4.2), marks that column **unchanged**
instead, as PostgreSQL's `pgoutput` does: the value was not
written, and copying it into the log would be the cost of a
long value for each update of a short one. A replica keeps
the value it has. `FULL` identity has the same exception for
the old row.

### 6.4 Schema

The catalog is not versioned (§3), so the stream carries the
definitions itself: a schema record at the commit of each DDL
that changed a logged object, holding the object's record as
the catalog keeps it (`sm3/CATALOG_ONDISK.md` §3). sm3 knows what
changed from the overlay it publishes at commit
(`sm3/CATALOG_INMEM.md` §1), and writes the records just before
`sm1::commit`, in the same transaction. A reader keeps the
definitions the stream has given it, and decodes each row by
its table's newest one (§3); it never asks the live catalog,
which may already have dropped the table. A drop is a schema
record too.

### 6.5 Sequences

`nextval` is not transactional and writes no version, so
there is no change to log. What a replica needs to take over
is a value past any the primary handed out, and sm1 already
writes one: a sequence's lease, the first value it has not
covered (`sm1/SM1_DESIGN.md` §4.2). A lease renewal of a
logged sequence logs a sequence record, in a transaction of
its own, so a replica that sets each sequence to the last
lease it saw never hands out a value the primary did. The
gap is a lease wide, as after a crash.

### 6.6 What is logged: publications

A record is written for a table only if a publication names
it, as `wal_level = logical` decodes every table and a
publication filters, except that here an unpublished table
costs nothing. A **publication** is an sm3 catalog object of
its own, a new key kind, `PUBLICATION`, holding its name, its
tables or all of them, and what it publishes (insert, update,
delete, truncate); a table definition then carries whether
any publication names it and its replica identity. A table's
definition is replaced at commit, so the flag is read under
the table-oid lock every write already holds, and a
transaction that adds a table to a publication logs its rows
from its commit on. A temporary table is never logged: it
has no catalog row and nothing of it outlives its session
(`TEMPTABLE.md`).

---

## 7. The feed: digres

The drain turns committed records into files, and lives in
digres because a consumer wants values, and digres has the
types' output functions. sm stays a store of bytes.

### 7.1 The drain

One thread for each database with a publication, started
when the server starts. It holds a reader
(`sm_feed_open`, §5.3) from the last commit its files hold,
and for each commit:

1. reads the records, keeping each schema record's
   definition;
2. formats the transaction as messages (§7.2);
3. appends them to the open segment (§7.3);
4. after each segment's sync, trims the log up to its last
   commit (`sm_feed_trim`).

A crash between 3 and 4 leaves commits in both the file and
the log. On start the drain reads the last complete commit of
the newest segment, cuts a torn tail at the first record whose
checksum fails, and resumes after it, so a commit is in the
files once.

The drain works after commit, so a large transaction is
written out whole after it commits and holds back the ones
behind it meanwhile, as PostgreSQL's does without streaming.
Its records were on disk all along, so it costs the drain no
memory.

### 7.2 The messages: `pgoutput`'s

The files hold PostgreSQL's logical replication messages, as
`pgoutput` sends them over a replication connection: Begin,
Relation, Insert, Update, Delete, Truncate, Commit, and Message
for what `pgoutput` has no message for. A consumer that reads
`pgoutput` reads them as they are, Debezium's and
PostgreSQL's own among them, and a replication connection can
later stream the same bytes (§10). How the fields map:

- **LSN** is `commit_ts`: the Begin's final LSN, and the
  Commit's commit and end LSN, are the commit's `commit_ts`,
  a 64-bit number that only grows, as an LSN does.
- **The commit timestamp** is the `"C"` entry's wall-clock
  time (§5.1).
- **xid** is the low 32 bits of sm's 64-bit `xid`, which
  `pgoutput` has room for; it is informational there.
- **Relation** gives a table's oid, schema, name, replica
  identity and columns, each column's name, type oid and
  typmod, all PostgreSQL's numbers, as digres's built-in types
  and oids are (`../dg/CODE.md`). It is sent before a
  transaction's first change to a table it has not been sent
  for since the segment began, and again after a schema record
  changes the table.
- **Values** are in text, by each type's output function, or
  in binary on request; an unchanged long value is
  `pgoutput`'s `u` (§6.3), a null `n`.
- **DDL and sequences** are Message, transactional, with the
  prefixes `wisr.schema` and `wisr.sequence`; a consumer that
  does not know them passes them by, as `pgoutput`'s consumers
  do any message they did not ask for. The content is the
  object as a small JSON document: kind, oid, schema, name, and
  for a table its columns, replica identity and primary key,
  or that it was dropped.

### 7.3 Segments

A stream is a directory of segment files, one stream a
database:

```
{feed}/{dbname}/
    0000000000a3f100.seg      the first commit_ts it holds, hex
    0000000000a40e2c.seg
    consumers                 each consumer's name and position
```

- **A segment** is a header — magic, format version, database
  name, first `commit_ts` — then its commits, each a frame of
  length, `commit_ts`, the commit's messages, and a CRC-32C.
  It begins with a Relation for every published table, so a
  consumer can start reading at any segment.
- **Rolled** at `feed_segment_bytes` (64 MB) or
  `feed_segment_ms` (1 s) since its first commit, whichever is
  first, then synced; the open segment is synced at least each
  `feed_sync_ms` too. A consumer reads only up to the last
  sync.
- **Where `{feed}` is**, the stream is not part of the
  database: it is derived, and as large as the history kept.
  It is outside `base/`, so that a database is still its
  directory (`../dg/CODE.md`), and moving a database moves its
  change log, not its history (§12).

### 7.4 Consumers and a consistent start

A consumer reads segments in order, from a file system or
from wherever they are shipped, and keeps its own position:
the last `commit_ts` it applied, stored with what it applied,
in the same transaction on a replica, so it applies each
commit once.

A consistent start needs no slot. A consumer copies the
published tables in one `REPEATABLE READ` transaction, whose
snapshot is one `commit_ts`, `S`: the copy holds exactly the
commits at or below `S` (§3). It then applies the stream from
the first commit above `S`. `sm_snapshot_ts(session)` gives
`S`, a call sm does not have yet, and digres gives it to SQL
as a function, for a copy made by another tool.

### 7.5 Retention

Segments are kept by `feed_retain_bytes` and
`feed_retain_ms`, and while a consumer registered in
`consumers` has not acknowledged them: a registration is a
name and a position, which the consumer advances, and which
can be dropped. A consumer that falls behind retention copies
the tables again. Nothing of this reaches the database: its
garbage collection, its WAL and its stores never wait for a
consumer, only the feed's directory does, and the change log
waits only for the drain, which is in the server and seconds
behind. A drain that stops, its disk full, leaves the log
growing in the database; that is reported, and is the one
way a feed can cost the primary space.

---

## 8. What it guarantees

- **Order.** Commits are in `commit_ts` order, and a
  transaction's changes in the order it made them (`seq`).
  Applied in that order, they leave each row as the primary
  has it: committed versions of a key are in `commit_ts`
  order (`sm1/SM1_DESIGN.md` §4.1), the
  READ COMMITTED writes that pass a newer commit included.
- **Consistency.** The stream up to `commit_ts` `T`, applied
  to an empty copy, or to a copy at snapshot `S` from `S` on,
  gives the published tables as a snapshot at `T` shows them.
- **Completeness.** Every committed write to a published
  table is in it, bulk loads in either mode included; nothing
  rolled back, aborted or crashed is.
- **Durability.** A commit is in the stream if and only if it
  is durable: the log entry and the status record are one
  batch. With `durability_sync = false` a crash may lose the
  last commits, and loses their entries with them.
- **Once.** A commit is in the files once (§7.1), and a
  consumer that stores its position with what it applied
  applies it once.

## 9. Costs

- **Each logged row is written twice**, its version and its
  record, and an update or a delete also writes its old
  identity. That is WAL bytes and memtable bytes; the records
  are trimmed within seconds and most never reach a level
  past the first.
- **A large value is written twice when it is written**, and
  not when an update keeps it (§6.3).
- **A bulk load into a logged table writes its rows twice**,
  both into sorted files, still past the WAL and the
  memtable.
- **Nothing for an unpublished table**: no record, no
  `"C"` entry for a transaction that logged nothing, and one
  flag read under a lock already held.
- **The drain**, a thread a database, and the segments' disk.

---

## 10. Not here

- **Applying a stream.** A digres subscriber, PostgreSQL's
  `CREATE SUBSCRIPTION`, reads segments and applies them by
  identity. It is the next design, on this one.
- **A replication connection.** digres has no walsender
  (`../dg/WORKNOTE.md` steps 2, 61 and 231). Streaming segments
  over `START_REPLICATION ... LOGICAL` is `pgoutput`'s own
  transport and needs nothing of the format changed.
- **Column lists and row filters** of a publication.
- **Origins**, which tag a change with where it came from, so
  that two databases replicating to each other do not send a
  change back.
- **Decoding a prepared transaction at its prepare**, as
  PostgreSQL's two-phase decoding does. Here it is in the
  stream at its commit.
- **A database moved or imported** with its log not yet
  drained (§12).

---

## 11. Order of work

1. **sm1: the change log**, §5: `CF:changes`, `log_append`,
   the `"C"` entry at commit and at `commit_prepared`, the
   deletes at rollback and abort, the sweep at open, the
   reader and the trim → verify: sm1's tests, with a scenario
   of two sessions committing out of `xid` order, a rollback
   to a savepoint, an abort and a crash, each read back in
   `commit_ts` order.
2. **sm1: the bulk load's records**, §5.2 → verify: a load in
   each mode, committed and rolled back past.
3. **sm3: the records and publications**, §6, and `sm.h`'s
   `sm_feed_*` and `sm_snapshot_ts` → verify: sm3's tests,
   each kind written and read back, a table added to a
   publication mid-stream, an update of a row with a long
   value it keeps.
4. **digres: publications in SQL**, `CREATE`, `ALTER` and
   `DROP PUBLICATION`, `REPLICA IDENTITY` → verify: an slt
   file, PostgreSQL 18's answers.
5. **digres: the drain and the segments**, §7 → verify:
   `server.sh` writes, crashes and restarts a server under
   load, and a reader of the segments finds each commit once,
   in order, equal to what a snapshot shows.
6. **A reader for tests and tools**, a `pg_recvlogical`-like
   `build/digres --feed DIR` that prints a stream → verify: its
   output on the step 5 runs.

## 12. For review

- **C over E (§4).** Writing each logged row twice, against a
  drain horizon that holds back garbage collection and store
  drops. Proposed: C, and E only if a profile asks.
- **The drain in digres, the files in `pgoutput`'s format
  (§7.2).** The other way is a format of sm's own, written by
  sm, with sm2 tuples, which a digres replica could apply
  without formatting and which no other tool reads.
  Proposed: `pgoutput`, for every consumer that already
  reads it.
- **DDL as Message (§7.2).** PostgreSQL replicates no DDL; a
  consumer that applies schema changes needs them, and one
  that does not ignores them. Proposed: send them.
- **Where the feed lives (§7.3).** Outside `base/`, a
  directory of its own, against inside the database's
  directory, where it would move with it. Proposed: outside;
  the change log in `CF:changes` moves with the database, and
  an imported database's undrained commits go to a new
  stream on the importer.
- **Sequences by lease (§6.5).** A replica's sequences run a
  lease ahead of the primary's, against logging each
  `nextval`. Proposed: by lease.
- **Retention by consumers (§7.5).** A registered consumer
  holds segments, never the database. Proposed: so, with a
  size cap that wins over a registration, so a consumer gone
  for good cannot fill the disk.

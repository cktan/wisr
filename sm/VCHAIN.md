# VCHAIN — the newest version in place, the older ones chained

A proposal; nothing here is built. sm1 keeps every version of
a key as a key of its own, `userkey ‖ wts ‖ seq`, so a read
seeks an iterator to the newest version it may see. This file
moves a table's rows to two rels: the newest version of each
row under its plain key, where a read is a RocksDB `Get`, and
the older ones in a rel of their own, reached by a chain from
the newest. Indexes lose their versions too. TOAST keeps
today's layout.

Paths are relative to `sm/`; digres's are under `../dg/`.

---

## 0. In brief

- **CURR, relid `0x0001`** (the row rel's, `sm3::heap_relid`),
  holds each row's newest version under its plain key: the key
  of `ROWKEY.md`, with no version fields. Its value carries the
  version's header and an `int64` reference to the version
  before it.
- **OLD, relid `0xFFFE`** (the `_id` index's, `sm2::id_relid`,
  reserved and unused since `ROWKEY.md`), holds the older
  versions: key `be64(id)`, value the version and a reference
  to the one before it. A row's versions are a chain, newest
  first: CURR, then OLD, then OLD.
- **TOAST, relid `0xFFFF`**, is as before: chunks written once,
  erased once, each a versioned sm1 key (`sm2/SM2_DESIGN.md`
  §4.2).
- **An index entry has no version.** A hit reads its row from
  CURR, following the chain if need be, and rechecks the
  entry's columns against the version it sees.
- **The assumption**: most of CURR is readable by most
  readers. A version a reader cannot see is one written by a
  transaction still running or aborted, or committed after the
  reader's snapshot; the reader follows the chain.
- **Aborts are rare, and repaired lazily.** A transaction that
  aborts with its writes in its rollback buffer repairs CURR
  before it ends; any other dead version in CURR is repaired by
  a background worker, from a queue readers feed.
- **OLD is harvested lazily**, once no snapshot needs a
  version.

---

## 1. Why

Q7 of `../bench` reads one row of `wide_mixed` by its primary
key, a query at a time. Its profile, in cycles a transaction,
user space alone:

| | PostgreSQL | digres |
| --- | --- | --- |
| the transaction | 50k | 73k |
| finding and fetching the row | 15k | 17k |

The row is read with `sm1::read`, which seeks an iterator:
building it is about 4% of digres's cycles with its teardown,
and searching the 16 KB data block key by key, as RocksDB does
between restart points, 7.5%. A `Get` builds no iterator, and
with whole-key filtering and a data block hash index it finds
the key in the block without that search. An estimate, not a
measurement: 8k to 10k cycles of the 73k. A `Get` needs the
plain key, which today's layout does not have.

A row with no version fields in its key also makes a scan read
one entry a row, with no versions to step over and a shorter
key to compare.

The rest of the gap is elsewhere: the parser, opening and
locking the table, planning. This file does not touch it.

---

## 2. The layout

### 2.1 CURR

- **Key**: `0x0001 ‖ encode(key)`, as now less the version
  suffix.
- **Value**: the version's header as sm1 has it now (§3.4 of
  `sm1/SM1_DESIGN.md`: the writer's xid, `wts`, `seq`, flags,
  the commit stamp once known), then `prev`, the OLD id of the
  version before it, `0` for none, then the tuple.
- **A delete is a tombstone in CURR**: a version flagged so,
  with no tuple, whose `prev` is the row it deleted, now in
  OLD. Older snapshots still find the row through it. Delete
  and insert again is a chain through the tombstone, as two
  updates are.

### 2.2 OLD

- **Key**: `0xFFFE ‖ be64(id)`. Ids are drawn from a counter of
  the store's, in memory; at open it starts past the last key
  under `0xFFFE`, read once. Nothing is persisted for it.
- **Value**: the version as it was in CURR, header and tuple,
  its `prev`, and `displaced_by`, the xid of the writer whose
  version replaced it.
- **Ids carry no order a reader relies on.** A hop is a `Get`
  by id.

### 2.3 TOAST

As `sm2/SM2_DESIGN.md` §4.2: a datum's chunks are written once
and erased once, by the update that replaces it or the delete
of its row, as versioned sm1 keys. Versions of one row share a
datum while it is unchanged; its chunks outlive every version
that points to them, since their tombstone is purged only once
no snapshot sees the version that erased them. Moving TOAST to
CURR and OLD would copy each erased 8 KB chunk into OLD.

### 2.4 Indexes

- **An entry is `relid ‖ encode(columns) ‖ encode(key)`**, as
  now, with no version. Its value is as now: the cut flag, the
  INCLUDE columns, the row's key.
- **A transaction's index writes are puts alone.** An update
  that changes an entry's columns puts the new entry and leaves
  the old one; nothing a transaction does deletes an entry. An
  aborted writer's entries stay, and fail every recheck.
- **Every hit is rechecked**: the row is read from CURR, its
  chain followed to the version the reader sees, and the
  entry's columns compared with that version's. A mismatch is
  passed.
- **An index-only scan reads the row too.** With no versions
  in the entry, nothing short of the row says whether the
  reader sees it. A record of what every snapshot sees, as
  PostgreSQL's visibility map is, could restore it; not
  proposed here.
- **A unique insert locks the value.** Today two inserts of
  one value meet on the entry's versions; without versions they
  do not. An insert of a unique value takes an sm1 lock on the
  entry's key, held to the end of the transaction, then checks
  each existing entry of the value by reading its row: one no
  live version of the row holds is stale, and no conflict.
- **The index is smaller and its search faster**: one entry a
  value, a shorter key, and a unique index's equality lookup a
  `Get`.

---

## 3. Reads

- **A read `Get`s the key from CURR**, then checks the version
  as now: committed at or before the snapshot, or the reader's
  own at or before the statement's mark (`sm1/SM1_DESIGN.md`
  §6.2). Seen: done. Else it `Get`s `prev` from OLD and checks
  again, until it finds one it sees, or `prev` is `0`, or the
  id is gone: no row for this reader.
- **A missing id is "no older version"**, never an error: OLD
  is harvested only of versions no live snapshot needs (§6), so
  a reader that reaches one has passed every version it could
  see.
- **A scan iterates CURR**, one entry a row, and follows the
  chain of a row whose newest version it does not see.
- **SERIALIZABLE is as now.** `ssi_on_read` takes the CURR key,
  found or not; a scan registers its range over CURR keys; a
  version passed over owes its writer an edge (`skip_version`).
- **A dead version in CURR is reported**, not repaired, by the
  reader that finds it: §5.2.

---

## 4. Writes

- **An insert** puts CURR, `prev` `0`; or, over a tombstone,
  as an update.
- **An update or a delete**, under the key's latch, having
  read CURR's version for its checks as now: draws an OLD id,
  then one WriteBatch puts that version to OLD, with
  `displaced_by` its own xid, and the new version, or a
  tombstone, to CURR, `prev` the id.
- **Each write of a transaction chains**, its own earlier
  versions included: a statement's mark, a savepoint and an
  open cursor may each need one of them.
- **An update writes the old tuple again**, to OLD: twice
  today's bytes. A delete copies the row likewise. Storing only
  the columns that changed would cut it, at the cost of
  rebuilding an old version from its successor; not proposed
  here.
- **The commit stamp** is written into CURR's version by the
  compaction filter, as now.

---

## 5. Aborts

A dead version in CURR is one whose writer aborted, crashed, or
rolled back to a savepoint set before it. Readers pass it by
following the chain, so it costs a hop, never a wrong answer.
It is removed by putting back the version before it.

### 5.1 The rollback buffer

- **sm1's undo list (`session_t::undo`) records every write**,
  not only while a savepoint is set, up to `undo_max_bytes`.
- **On overflow with no savepoint set**, the list is dropped
  and recording stops; an abort then repairs nothing itself
  (§5.2 does). A savepoint set after it records again from its
  mark, as now.
- **On overflow with a savepoint set**, the savepoints are lost,
  as now (`sm1/SM1_DESIGN.md` §6.5).
- **An abort repairs from the list, then ends**: for each key,
  under its latch, it puts back the newest version in the chain
  not its own, and deletes the OLD entries it passed. Between
  the two, readers see a version in progress and follow the
  chain, and writers wait for it, as now. A transaction that
  fit its list never leaves a dead version visible.
- **A rollback to a savepoint** does the same for the keys
  written since the mark, putting back the newest version at
  or below the mark's `seq_hwm` or committed. It must: a
  version written under a rolled-back savepoint is dead while
  its transaction lives, which no status lookup tells.
- **A crash or an in-doubt transaction rolled back after a
  restart** has no list; §5.2 repairs after it.

### 5.2 The repair queue

- **A reader that meets a dead version in CURR queues its key**
  (storeid, CURR key) for a background worker, and follows the
  chain.
- **A bit filter keeps a key out of the queue twice**: one bit
  a key, by one hash of storeid and key, in a bitmap of the
  database's, say 64K bits. A reader sets the bit with an
  atomic fetch-or and queues the key only if the bit was 0. A
  key whose bit is already set is skipped, whether it is
  queued already or collides with one that is; the collision
  lasts until that key is dequeued. A set bit therefore
  belongs to exactly one queued key.
- **The worker clears the bit once the key is repaired.**
  Cleared at dequeue instead, a reader meeting the version
  before the repair would queue it again.
- **The worker wakes on a push**, not on a timer alone, so a
  hot key's readers follow the chain for as short a time as
  can be. It takes a batch, sorts it, and for each key, under
  its latch, checks that CURR still holds a dead version, then
  in one WriteBatch puts back the version before it and
  deletes that version's OLD entry.
- **The queue is volatile.** A key lost to a crash, or not
  queued because the queue is full, is queued again by the next
  reader that meets it. A repeated key finds nothing to do.
- **A repair belongs to no transaction.** It stands whatever
  becomes of the reader that queued it.
- **No recovery step is needed.** A crashed transaction's
  versions are dead by the status lookup, as now; the first
  readers to meet them queue them.

---

## 6. Harvest

- **An OLD version is needed** by a snapshot at or after the
  watermark (`sm1/SM1_DESIGN.md` §8.3) only while the version
  that displaced it committed after the watermark. Once
  `displaced_by` committed at or before it, the version and
  every older one in its chain are no one's.
- **The compaction filter drops it then**, as it drops a
  superseded version now. A version whose displacer aborted is
  kept: CURR's dead version still points to it until the
  repair, which deletes it.
- **What is dropped is a chain's tail.** A reader that sees no
  version newer than a dropped one would need a snapshot older
  than the watermark, which none has.
- **A tombstone in CURR goes** once it committed at or before
  the watermark: the worker deletes the CURR key, under its
  latch, checking it still holds the tombstone. One RocksDB
  `Delete`, where today one `DeleteRange` a key is needed: CURR
  holds no older version of the key for it to resurrect. The
  chain below it is then no one's, and the filter drops it.
- **A stale index entry goes with the last version that held
  it.** The writer that displaces a version whose entries it
  does not keep lists those entries' keys in the OLD version,
  and stamps each entry it puts with its `wts`. When the filter
  drops the version, it queues the listed keys; the worker
  deletes an entry, under the entry's latch, only if its stamp
  is not newer than the displacer's `wts`. A later write that
  gave the row the value again stamped the entry anew, and
  keeps it. The filter needs no schema: the keys are bytes.

---

## 7. What it buys, and what it costs

| | Gains | Costs |
| --- | --- | --- |
| point read | a `Get` on CURR | a hop into OLD a version the reader does not see |
| scan | one entry a row, a shorter key | a hop for each row changed since the snapshot |
| index | one entry a value, smaller, faster | a recheck on every hit; an index-only scan reads the row |
| update | — | the old tuple written again, to OLD |
| delete | the tombstone purge is one `Delete` | the row copied to OLD |
| abort | — | a repair, by the transaction or the worker |
| GC | OLD harvested by the filter, its tails whole | stale index entries until their version goes |

A long snapshot over hot rows pays the most: one random `Get`
a version it passes, as PostgreSQL's reader walks a long HOT
chain and InnoDB's its undo.

---

## 8. Open questions

1. **Where it lives.** Versions, latches, the watermark and the
   filter are sm1's; relids and schemas are sm2's and sm3's.
   sm1 would offer a chained key space beside the versioned
   one. A comparator and a prefix extractor are a column
   family's, so a store becomes two CFs: one bytewise, for
   CURR, OLD and the indexes, and the versioned one, for TOAST;
   one WriteBatch writes both atomically.
2. **The index stamp of §6.** Check it against every order of
   updates that changes a value and sets it back, with the
   harvest between.
3. **The bulk load** (`BULKLOAD.md`) builds SST files of
   versioned keys; it would build CURR's instead, with no
   chain.
4. **Measure first.** Time a `Get` against today's seek on a
   compacted `wide_mixed`, with sm1's options, before building
   any of it; §1's 8k to 10k cycles is an estimate.
5. **The specification stands.** The isolation scenarios in
   `sm1/mvcc` must pass unchanged.

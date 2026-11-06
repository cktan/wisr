# ROWSTORE: the Row Store

The Row Store keeps a table's rows. It names a row by its key,
keeps the row's versions in one RocksDB column family, writes and
finds versions for a transaction, and hands a row to its client
with no copy. Its code is in `sm1/src/access.cc`,
`sm1/src/chain.cc`, `sm1/src/chain.h`, `sm1/src/version.h`,
`sm1/src/pin.cc`, `sm2/src/rel/rel.cc`, `sm3/src/table/pin.cc`,
`sm3/src/table/pin.h` and the row calls of
`sm3/src/table/table.cc`. Its client in digres is
`../dg/src/backend/access/sm/smstore.c`.

Types are in `sm2/TYPE.md`, and a tuple's bytes in
`sm2/TUPLE.md`. Other modules: TOAST.md for values out of line,
LOCKMGR.md for latches and locks, XCTMGR.md for transactions,
visibility, savepoints and SSI, GC.md for the Garbage Collector,
DBMGR.md for column families and options, LSMINDEX.md and
BTREEINDEX.md for index entries.

---

## 1. Overview

### 1.1 In brief

- **A row is named by its key**: the values of its primary key's
  columns. A table without a primary key names a row by `_id`,
  one `int8`. Nothing is a TID.
- **A store is one RocksDB column family.** Every key in it opens
  with a class byte, sm1's own, which no caller sees.
- **A versioned key** keeps each version as a key of its own,
  `userkey ‖ ~wts ‖ ~seq`. Index entries, TOAST and the catalog
  use versioned keys.
- **A chained key** keeps the newest version under its plain key,
  in CURR, and the older ones in a chain behind it, in OLD. Every
  table's rows are chained keys.
- **A write is eager.** It puts a version into RocksDB at once,
  unsynced, under the key's latch. The uncommitted version stands
  for the row's lock.
- **A read of a chained key is a `Get`** of CURR. It follows the
  chain while it does not see the version.
- **An abort repairs nothing.** Readers pass a dead version by
  the chain. The next write of the key, or the Garbage Collector,
  removes it.
- **A pin keeps a row's bytes in memory**, so a client reads them
  where RocksDB has them. A ref with a generation makes a stale
  handle fail and not read freed memory.
- **A client deforms a row only as far as it reads.** sm3's pin
  keeps how far it went.
- **A batch read** reads many rows by key in one call: by one
  iterator when they are close, by `MultiGet` when they are not.
- **A rel** is sm2's codec over a store: a tuple goes in and a
  tuple comes out. It keeps nothing that a rollback must undo.

### 1.2 Terms

| Term | Meaning |
| --- | --- |
| row key | `encode(pk)`, or `be64(_id)` in a keyless table: a row's key past the relid |
| userkey | the bytes sm1's caller gives: `relid ‖ row key` for a row |
| keyless table | a table without a primary key |
| CURR | class `0x01`: a chained key's newest version, under `01 ‖ userkey` |
| OLD | class `0x02`: a chained key's older versions, under `02 ‖ be64(id)` |
| `prev` | the OLD id of the version before this one, 0 for none |
| `displaced` | an OLD version's displacer: the writer that replaced it |
| unlinked | an OLD entry that no chain reaches any more |
| dead | written by a transaction that aborted or that a crash ended |
| raw pin | `sm1::rawpin_t`: sm1's handle on one row's bytes, owned by its caller |
| pin | `sm3::pin_t`: a raw pin, the row's definition, and how far it is deformed |
| ref | `pin_ref_t`: a client's plain value that names a pin and a generation |
| generation | a counter on each pin, increased each time the pin is released |
| slot | digres's `TupleTableSlot` |

An index's entries are **entries**, not rows.

### 1.3 A worked example

```sql
CREATE TABLE acct (id int8 PRIMARY KEY, owner text, bal int8);
CREATE INDEX acct_owner ON acct (owner);
INSERT INTO acct VALUES (42, 'alice', 7);   -- T1, commits
UPDATE acct SET bal = 9 WHERE id = 42;      -- T2, commits
```

The table's store is the column family `chain.<storeid>`. After
the two commits it holds three keys, in this order:

```
class  key                                   value
00     0002 enc('alice') enc(42) ~wts ~seq   flags, xid of T1, entry tuple
01     0001 enc(42)                          flags, xid of T2, wts, seq,
                                             prev = 7, (42, 'alice', 9)
02     be64(7)                               flags, xid of T1, wts, seq,
                                             prev = 0, displaced = T2,
                                             (42, 'alice', 7)
```

- The first key is the index's entry, a versioned key. The UPDATE
  did not change `owner`, so sm3 left the entry alone.
- The second key is the row's newest version, in CURR.
- The third key is the version T2 displaced, in OLD, under the
  fresh id 7. One `WriteBatch` wrote the second and the third.

Each call goes down one level:

```
sm_tbl_update -> sm3::tbl_update -> sm2::rel_update
              -> sm1::chain_update -> chain_write
```

A reader at a snapshot taken after T2 commits makes one `Get`, of
`01 0001 enc(42)`, and sees `bal = 9`. A reader at a snapshot
taken before T2 commits makes that `Get`, does not see the
version, and makes a second `Get`, of OLD id 7. It sees
`bal = 7`.

---

## 2. Row keys

### 2.1 A row's name

- **The key is the primary key's columns** as the tuple holds
  them, not their encoding. `encode` is lossy for collated text,
  so nothing decodes a key, and the values a read hands back come
  from the tuple. sm takes a key as `values`, `isnull` and a
  count, as it takes a row, and encodes it. That costs CPU alone.
- **A keyless table's key is its `_id`**, encoded `be64(_id)`.
- **A key column is never NULL.** A primary key's columns are NOT
  NULL, and `_id` is drawn. `isnull` is in the signature for the
  one shape a row has. A null fails with `SM_ERR_INVALID`.
- **A key change is a new name.** The executor names a row by its
  key as the scan read it. An UPDATE that changes the key writes
  at the old name, and the row ends at the new one.

Why a table is clustered on its key: on an LSM a heap's key is a
key like any other, so a heap pays an index's price on every read
through an index (`CLUSTER.md`).

Why no TID: an insert writes one key less, a secondary entry is 8
bytes shorter, nothing reads at random to resolve an `_id`, and
the executor's row name survives a sort or a hash, so no write
reads a row twice.

### 2.2 The row and its index entries

| | Key | Value |
| --- | --- | --- |
| row, with a primary key | `relid ‖ encode(pk)` | `tuple` |
| row, keyless | `relid ‖ be64(_id)` | `tuple` |
| secondary, non-unique | `relid ‖ encode(sk) ‖ encode(pk)` | a cut flag, the INCLUDE columns, `encode(pk)` |
| secondary, unique | `relid ‖ encode(sk)` | a cut flag, the INCLUDE columns, `encode(pk)` |

- **The value is the tuple alone** in both kinds of table. A
  keyless row's `_id` is read from its key.
- **`encode(pk)` is never cut.** Past `max_pk_len`, 1024 bytes,
  the row fails with `SM_ERR_KEYTOOLONG`, so that every entry,
  which ends with the row key, keeps room for its own columns.
- **`encode(pk)` is `be64(_id)` in a keyless table**, in an entry
  too.
- **A table's rows are relid 1**, a constant. Which relid means
  what is sm3's (CATALOG.md). Relid `0xFFFE` stays one that no
  index takes, and nothing is written under it.
- **There is no `_id` index.** `rel_find`, `rel_find_latest` and
  `rel_lock_id` are gone with it.

### 2.3 `_id`

- **It is an unsigned 64-bit number**, 8 bytes big-endian, drawn
  from a sequence that belongs to the table. It starts at 1, is
  never reused, survives a crash, may have gaps, and never
  changes.
- **It is MyRocks' hidden primary key.** A keyless row has it as
  its key. A keyed row has none, and every call that gives one
  gives 0.
- **SQL reads it as the system column `_id`**, an `int8`, in a
  keyless table alone. Naming it in a keyed table is "column
  `_id` does not exist". It names the row, not the row version,
  so an UPDATE does not change it.
- **The sequence is made for every table.** A keyed table draws
  nothing from it. A rewrite that drops the primary key draws the
  rows' `_id`s from it, in key order.

### 2.4 Calls by key

Every call that names a row takes its key: `values`, `isnull` and
`n` at sm3, `sm_row_key_t` in the C API. These are `tbl_read`,
`tbl_read_latest`, `tbl_update`, `tbl_delete`, `tbl_lock` and
`tbl_trylock`. The forms by `_id` stay, for a keyless table.
`tbl_read_key` and `tbl_read_keys` take encoded keys, as an entry
gives them.

Every call that handed out an `_id` hands out the row, from which
the caller takes the key's columns:

- **`tbl_insert`** returns the `_id` of a keyless row, and 0 for
  a keyed row.
- **`tbl_insert_arbiter`** returns the conflicting row, and its
  `_id`, 0 in a keyed table.
- **A scan** hands out the row. `rel_scan(first, last)` by `_id`
  is for a keyless table alone, and refuses a keyed one.

### 2.5 A key change

1. The mover erases the old key with a **moved tombstone**
   (`sm1::erase_moved`): a tombstone with bit2 set.
2. The mover inserts the row under the new key. A duplicate is
   `SM_ERR_DUPKEY`, and the statement's savepoint undoes the
   half-done move.
3. sm3 deletes every secondary entry and puts it again, as each
   ends with the row key.

What each party sees:

- **A reader** sees a moved tombstone as any tombstone.
- **A writer at the old key, when the move committed after it
  began,** fails as over any newer commit: `SM_ERR_UPDATED` at
  READ COMMITTED, `SM_ERR_CONFLICT` above.
- **At READ COMMITTED its re-read**, `read_latest`, then fails
  with `SM_ERR_MOVED`, and so does the write taken again
  (scenario f11). Over a plain tombstone the re-read returns
  "absent", which the executor takes as a delete and skips.
- **A writer at the old key, when the move committed before it
  began,** fails with `SM_ERR_MOVED` at once.
- **An insert** goes in over a moved tombstone, as over any
  tombstone.
- **digres** raises `SM_ERR_MOVED` as a serialization failure,
  PostgreSQL's "tuple to be locked was already moved to another
  partition due to concurrent update". PostgreSQL's
  cross-partition move is the precedent: it refuses to follow.
- **A `FOR UPDATE` lock never loses its row to a move.** The
  erase is a write of the old key, so it waits for that lock as
  any write does.
- **The Garbage Collector** keeps the flag when it stamps the
  version, and removes it as a tombstone.

### 2.6 Reading many rows

Nothing reads many rows of a keyed table in `_id` order, since
there is none.

- **ANALYZE** samples by key ranges: slots of 8 KB of the table's
  bytes, each the rows between the keys at its ends. It finds the
  keys by bisecting the key range on sm1's size estimates
  (`rel_keys_at`), disk and memtables together, with no flush.
  The row count is the rows read per byte the slots span, times
  the table's bytes. That is close in sorted files, and within an
  order of magnitude in memtables. Correlation is measured in key
  order. A keyless table is sampled by runs of `_id`s. A keyed
  table has no `_id` limit, 0 in its statistics, and its growth
  since ANALYZE is its bytes' (`STATISTICS.md`).
- **TABLESAMPLE** picks its blocks the same way. SYSTEM's are
  slots of 8 KB of the bytes, each read by a seek to its first
  key, the rest skipped. BERNOULLI's, which reads every row, are
  256 rows of a scan in key order. A keyless table's are blocks
  of 256 `_id`s.
- **The index backfill** walks the rows in key order
  (`tbl_scan_rows`), and resumes from a key, which the builder
  keeps in memory (INDEXBUILD.md).
- **A rewrite** copies the rows in key order. ADD PRIMARY KEY
  drops the `_id`s. DROP PRIMARY KEY draws an `_id` a row, in key
  order.
- **SET NOT NULL's check** walks the rows in key order.

### 2.7 Duplicates and lock status

- **A duplicate key** is reported as the index and the key of a
  row that holds it. `sm_errdup` hands it out, and
  `sm0::dupkey_t` and the persisted index status carry the one
  key. digres reads the row by that key to say its "Key (…)=(…)
  is duplicated".
- **`pg_locks`** shows a row lock as its relation alone, for both
  kinds of table, as sm1 keeps a lock's id and not its key
  (LOCKMGR.md).

### 2.8 Costs

- **A concurrent key change fails the writer** at READ COMMITTED.
  OLTP keys rarely change.
- **A key change moves the row** and rewrites all its entries.
  PostgreSQL's update also adds an entry to every index, unless
  HOT applies.
- **A statement that shifts primary keys can collide with
  itself.** `UPDATE t SET k = k + 1` checks each new key at its
  write, so the first row's new key is the second row's old one:
  a duplicate. The SQL standard checks at the statement's end,
  which nothing in the stack does. InnoDB and MyRocks accept this
  too.
- **A wide key is carried through the plan.** Every row an
  UPDATE, DELETE or `FOR UPDATE` reads carries the key's columns,
  where it carried 6 bytes. A sort or a hash below the write
  carries them too.
- **Secondary entries carry `encode(pk)`**, so a wide key leaves
  less of `max_keylen` to an index's columns.
- **Adding or dropping a primary key rewrites the table.**
- **ANALYZE and TABLESAMPLE estimate from the store's sizes**,
  which RocksDB approximates.
- **The executor and the table AM part from PostgreSQL's**
  wherever a TID was, so a port of an upstream fix there is by
  hand.

---

## 3. Stores

### 3.1 Two kinds of key in one store

- **`sm1::mkstore` makes a store chained or not.** A chained
  store's one column family is `chain.<storeid>`, a versioned
  store's `store.<storeid>`. So the flag is known at open from
  the column family list, with no read. A temporary store's flag
  lives in its `store_t`.
- **sm3 makes every table's store chained**, temporary ones too.
  sm2's rel then uses the `chain_` calls for its rows. The
  catalog's store, store 0, is versioned.
- **A chained store holds both kinds of key.** Its rows are
  chained keys. Its index entries and TOAST chunks are versioned
  keys, reached by the plain calls. A tree's pages are keys of a
  third class (BTREEINDEX.md).
- **The call says which kind a key is**, `chain_insert` or
  `insert`. A chained key and a versioned key of the same bytes
  are two keys.
- **A store stays one column family.** A drop is one
  `DropColumnFamily`, and a key carries no storeid. A storeid is
  never reused. DBMGR.md has the map of column family handles.

### 3.2 The class byte and the comparator

| Class | Key | Holds |
| --- | --- | --- |
| `0x00` | `00 ‖ userkey ‖ ~wts ‖ ~seq` | a versioned key's versions |
| `0x01` | `01 ‖ userkey` | CURR: a chained key's newest version |
| `0x02` | `02 ‖ be64(id)` | OLD: a chained key's older versions |
| `0x04` | a page's key | a page, unversioned (BTREEINDEX.md) |

sm1 adds the class when it builds a key, and strips it when it
hands a key out. A versioned key costs one byte more for it. A
current key costs one byte, not the twelve of a version tail.

The comparator is `wisr.store.v2`:

```
compare(a, b):
    if a[0] != b[0]:  return bytewise(a, b)
    if a[0] != 0x00:  return bytewise(a, b)     # shorter first on a tie
    c = memcmp order of the userkeys            # key less class and tail
    if c != 0:        return c
    return memcmp order of the 12-byte tails    # newest first
```

- **RocksDB's bytewise comparator is wrong for class `0x00`.** A
  key's tail would compare against the extra bytes of a longer
  userkey that begins with it. The versions of `"a"`
  (`61 FF FF …`) would sort after those of `"ab"` (`61 62 …`).
  Scans would return `"ab"` first, and a point read could stop
  early on a foreign key.
- **Every versioned key handed to RocksDB** carries the class
  byte and the full tail: writes, seek targets and iterator
  bounds.
- **`FindShortestSeparator` and `FindShortSuccessor` are
  no-ops.** That is safe, and index blocks are a little larger.
- **`CanKeysWithDifferentByteContentsBeEqual` is false**, so
  RocksDB can build a data block's hash index.
- **The name is part of the format.** RocksDB refuses to open a
  column family under another name, so a new order means a new
  name and a rewrite. `CF:txn` and `CF:meta` keep the bytewise
  comparator.

### 3.3 The prefix extractor

`wisr.prefix.v2` gives a versioned key's class and userkey, its
tail stripped, and any other key whole. One bloom filter serves
both: a versioned read filters on its userkey, a `Get` on its
whole key. The extractor is variable-length, so
`FullLengthEnabled` is false.

### 3.4 A versioned key

```
key   = class   (1B, 0x00)
      | userkey (variable)
      | ~wts    (8B, big-endian, bitwise complement)
      | ~seq    (4B, big-endian, bitwise complement)

value = flags   (1B)   bit0 = tombstone (this version is a delete)
                       bit1 = commit_ts is stamped in this header
                       bit2 = a move's tombstone (sm1::erase_moved)
      | xid     (8B)   the writer, while unstamped
        commit_ts (8B) in its place once stamped
      | payload (variable, absent for a tombstone)
```

There is no "row" object on disk, only versions, one RocksDB
key-value pair each. Take the key `"acct42"`, written by xid 50
(`seq` 1, `wts` 51), then by xid 60 (`seq` 1, `wts` 61),
committed at 70 and 80:

```
class userkey      ~wts              ~seq      flags  who        payload
00    61 63 63 74  FFFFFFFFFFFFFFC2  FFFFFFFE  02     0000...50  "bal=9"
      34 32        (wts 61)          (seq 1)          (ts 80)
00    61 63 63 74  FFFFFFFFFFFFFFCC  FFFFFFFE  00     0000...32  "bal=7"
      34 32        (wts 51)          (seq 1)          (xid 50)
```

Compaction visited the `wts` 61 version, and stamped its commit
timestamp in place of its `xid`. It has not visited the `wts` 51
version, so a reader looks up that writer's fate in `CF:txn`. A
delete has `flags` `01` and no payload. RocksDB's own 8-byte
trailer is after the key in its files, and is unrelated to `seq`.

- **`wts`**, the write timestamp, comes from the one counter,
  under the key's latch, when no other writer of the key is in
  flight. So a key's versions are in commit order by
  construction. The complement makes a key's newest version sort
  first, and a point read one `Seek`.
- **`seq`** is a write's number in its transaction, starting at
  1. A transaction that writes a key twice makes two distinct
  versions, so a rollback to a savepoint is a pure delete and
  needs no undo image. The complement makes the later write sort
  first.
- **The writer's `xid` is in the value**, not the key, for the
  status lookup.
- **The userkey is raw and the tail fixed-width**, so a key
  parses from its tail, with no escaping.

### 3.5 A chained key

```
CURR key   = 01 | userkey
CURR value = flags (1B) | xid or commit_ts (8B) | wts (8B) | seq (4B)
           | prev (8B) | payload
OLD key    = 02 | be64(id)
OLD value  = flags (1B) | xid or commit_ts (8B) | wts (8B) | seq (4B)
           | prev (8B) | displaced (8B) | payload
```

- **`flags`** are a versioned value's three, and two for OLD.
  `chain_unlinked` (`0x10`) says `displaced` holds the ts the
  entry was unlinked at. `chain_displaced_stamped` (`0x20`) says
  `displaced` holds its displacer's commit_ts, not its xid.
- **`wts` and `seq` are in the value**, as the key has no tail.
- **`displaced` is the xid of the writer that replaced this
  version.** The Garbage Collector stamps it with that writer's
  commit_ts.
- **OLD ids come from a counter in memory**, `store_t::next_old`.
  At open it starts past the store's last OLD key
  (`chain_next_old`). A temporary store's starts at 1. Ids drawn
  for writes the log lost are never seen.
- **A delete is a tombstone in CURR** whose `prev` is the row.
- **An OLD entry no chain reaches is unlinked, not deleted**:
  flagged, with the ts of the unlinking. A reader that read CURR
  just before may still be on its way to it. Deleted, it would
  read as "no older version". The Garbage Collector drops it once
  the watermark passes that ts.

### 3.6 Why a table's rows are chained

RocksDB alone, with sm1's options, a million rows of 500 bytes,
compacted, in the block cache:

| a scan, every row in key order | ns a row |
| --- | --- |
| versioned keys | 145–150 |
| plain keys padded with a 12-byte tail | 125–128 |
| plain keys | 83–86 |

| a read by key, cold | µs |
| --- | --- |
| a new iterator, a seek | 5.7 |
| `Get` of the plain key | 4.5 |

- **The 12-byte tail is most of a scan's cost**, so it leaves
  CURR's keys, and is not padded.
- **A point read gains a little**, about 4% of Q7.
- **In digres the scans gained less than these figures
  promised**: q1 stayed at 2.4 to 3.5 times PostgreSQL in one
  run. A raw RocksDB scan of rows all of one size is not dearer
  by the byte. Values of 431 and 501 bytes scan at about 108 ns a
  row, and 403 and 529 at 170 to 210, whatever header makes up
  the size. So a header's size moves a table of fixed-size rows
  by luck.

### 3.7 The Garbage Collector and a chain

The store's one compaction filter judges each key by class
(`gc.cc`, GC.md).

- **`0x04`**: kept as it is.
- **`0x00`**: by the rules of GC.md.
- **`0x01`**, by the writer's fate:
  - Dead: the filter keeps the version and queues the key
    (`chain_report`). The worker, under the key's latch, puts
    back the version before it (`chain_repair`, §9.4).
  - In flight, or committed above the watermark: kept.
  - Committed at or below the watermark, a tombstone: dropped.
    RocksDB writes a delete over the key, so no older entry of it
    comes back. The chain behind it goes by the rule for OLD.
  - Committed at or below the watermark, not a tombstone: stamped
    and kept.
- **`0x02`**, in this order:
  - Unlinked: dropped once `displaced`, its ts, is at or below
    the watermark.
  - Displacer dead or in flight: kept. A dead version still
    points to it until the repair unlinks it. Its own writer is
    stamped if committed.
  - Displacer committed at or below the watermark: dropped. No
    snapshot can reach it, as every snapshot sees its displacer
    first. Nor can one reach anything older in the chain.
  - Otherwise: kept, with its displacer's commit_ts and its own
    writer's stamped, so that the `CF:txn` sweep may delete their
    status records.

A write that copies a version also stamps the copy if its writer
committed (`stamp_copy`). The copy goes into the memtable now,
and a sweep may have stamped the original and may delete the
status record next. Unstamped, the copy would then read as
aborted.

### 3.8 Bulk load

A load stream is declared chained or not (`sm1::load_stream`). A
chained stream puts CURR versions of the load's xid, `prev` 0,
under class `0x01`, as one ingestion into the one column family
(`BULKLOAD.md`). An abort leaves them dead, for the Garbage
Collector to delete. A rollback to a savepoint deletes the
store's keys of every class but pages, with one `DeleteRange`.

---

## 4. The write path

### 4.1 The procedure

**WARNING:** do not wait while you hold the latch. Every wait
below lets it go first, and the writer looks again from the
start.

`sm1::insert`, `sm1::update` and `sm1::erase` put a version into
RocksDB at once, before commit. `access_write` does this for a
versioned key, `chain_write` for a chained one. They differ in
steps 6 and 13 alone.

1. Check the call: an active transaction, a key within
   `max_keylen` (4096), a value within `max_datalen` (64 MB), a
   store live at the view.
2. Refuse a read-only transaction, unless the store is temporary.
3. Refuse the write if a load is open into the store
   (`SM_ERR_INVALID`). Its keys are not in the store yet, so the
   write cannot be checked against them.
4. Take the key's latch, so no other write of the key comes
   between the check and the put.
5. If another transaction holds a conflicting explicit lock, let
   the latch go, wait for it, and go to step 4. A temporary store
   has no such lock.
6. Find the version this write checks against: the newest own
   version in effect, else the newest committed one. If another's
   version in flight comes first, let the latch go, wait for that
   transaction, and go to step 4.
7. Refuse with `SM_ERR_SELFMODIFIED` an update or delete of a row
   the running statement changed already.
8. If that version committed after `snapshot_ts`, apply §4.4 and
   §4.6.
9. Refuse an insert over a live version with `SM_ERR_DUPKEY`.
10. Refuse an update or delete with no live version:
    `SM_ERR_MOVED` over a moved tombstone, else
    `SM_ERR_NOTFOUND`.
11. Refuse a transaction's 2^32nd write (`SM_ERR_INVALID`).
    Allocate the `xid` at the first write.
12. Raise the session's `write_gen`, and allocate `wts`, now that
    no other writer of the key is in flight.
13. Put the version (§4.2). It goes to the memtable and the WAL,
    and is not synced. Only the commit record needs durability.
14. Set `seq` and `seq_hwm` to this write's number, and `wts_hi`
    to `wts`.
15. Clear the key's re-read mark (§4.6).
16. Record the write for a rollback, if a savepoint can still
    reach it (XCTMGR.md).
17. For a temporary store, set `temp_touched` and stop. It has no
    other reader, and bars a prepare.
18. Tell SSI of the write, for the readers it invalidates. A
    reader that arrives later steps past the version instead.
19. Let the latch go.

Step 6 looks at the transaction's own writes first, so a key it
inserted can be updated, and a key it deleted can be inserted
again. An own version in effect is still in flight, and every
other writer of the key waits for it, so no other transaction can
have committed the key in between. The check of step 8 is then
skipped. Own versions undone by a rollback fall through to the
committed version, as if never written.

### 4.2 The put

- **A versioned key** is one `Put` of a new key.
- **A chained key** is one `WriteBatch`:
  - CURR's version moves to OLD under a fresh id, `displaced` the
    writer. The new version goes to CURR with `prev` that id.
  - An update writes the old tuple again, to OLD. A delete copies
    the row likewise.
  - If CURR holds a dead version, the new version overwrites it.
    The version it hid gets `displaced` the writer anew and stays
    where it is. The dead OLD entries passed are unlinked.

A latch is held only within the call, and a write adds no lock
entry. See LOCKMGR.md for the latch, the wait, and the one
exception, `SM_ERR_UPDATED`'s lock (§4.6).

### 4.3 `insert_new` and `erase_moved`

- **`sm1::insert_new`** is `sm1::insert` of a key no version of
  was ever written, which its caller knows, as sm2 knows a key
  made of a row's `_id`. Step 6 has nothing to find, and is left
  out: a read and a seek. Steps 5 and 18 stay. A debug build
  reads, and asserts it finds no version. `chain_insert_new` is
  the same.
- **`sm1::erase_moved`** is `sm1::erase` of a row whose key
  changed. Its tombstone has bit2 set. §2.5 says who sees what.

### 4.4 First-committer-wins

Step 8 rejects a write when the key was changed by a transaction
that committed after the writer's snapshot. Together with the
wait on an uncommitted writer, this is the standard
snapshot-isolation write rule. It applies at REPEATABLE READ and
SERIALIZABLE: the loser aborts with `SM_ERR_CONFLICT`, retryable.
READ COMMITTED does not abort the loser (§4.6). Non-serializable
writes still feed the SSI tracker, but are never its victims.

The loser aborts inside the write, with the key's latch held:
`xct_fail_retryable` calls `discard`. So `discard` must not take
a key's latch. An abort repairs nothing (§4.7), so it needs none.

### 4.5 The old row

`sm1::update` and `sm1::erase` hand back the row they replace,
through an optional out-parameter.

- **It costs nothing to produce.** Step 6 already finds the
  version and decodes its value. A level that maintains indexes
  computes the old entries from it, and would otherwise pay a
  whole read.
- **The bytes are not copied.** They stay in a raw pin: the
  caller's, which keeps the row until the caller empties it, or
  the session's own, shared with `sm1::read`, so either call
  invalidates what the other returned.
- **A write lets go of what its pin held only once it has
  succeeded**, as its new value may view that row. A versioned
  write fills a raw pin of its own, and moves it into the
  caller's at the end. A chained write moves its buffer there
  after the put. On failure the pin keeps what it held.
- **Only a live version is handed back.** A write that fails
  reports no old row.

### 4.6 READ COMMITTED: write, re-read, write

At READ COMMITTED an update or delete that meets a row committed
after `sm1::begin` waits for any writer in flight, as every write
does. It then fails with `SM_ERR_UPDATED`, and takes the key's
lock X. This is no abort: the transaction stays active, keeps the
lock, and `sm1::retryable()` is 0. The exchange is:

1. The write fails with `SM_ERR_UPDATED`, and keeps the key
   locked X.
2. The caller calls `sm1::read_latest`, which returns the newest
   committed version, past the snapshot, and sets the mark.
3. The caller evaluates its `WHERE` clause and new values from
   that version.
4. The caller repeats the write, which goes through and clears
   the mark.

This is what PostgreSQL's executor does at its default level
after a lock wait, and clients written for it have no retry loop.
Two sessions each add one to `R1`, which holds 0. T1 updates it
and T2 waits for T1:

| | T2's update, after T1 commits | R1 |
| --- | --- | --- |
| PostgreSQL | re-reads R1 (1), re-checks `WHERE`, writes 2 | 2 |
| sm1 | `SM_ERR_UPDATED`; T2 reads the latest (1), re-checks, writes 2 | 2 |
| `SM_ERR_CONFLICT` here | T2 aborted | 1, until T2 retries |

The variant is `UPDATE R1 SET n = 1 WHERE n = 0`, which claims
the row once. T2's re-check must fail and T2 must write nothing,
so letting the write through is not enough: the caller has to
look at the newest version first. The executor's side of the
exchange belongs to digres.

The re-read is enforced, not requested:

| Event | Effect on the mark |
| --- | --- |
| first write refused with `SM_ERR_UPDATED` | lock taken, mark not set |
| `read_latest` on that row | mark set |
| write repeated with the mark set | write goes through; mark cleared |
| write repeated without the mark | refused again with `SM_ERR_UPDATED` |
| rollback to a savepoint taken before the lock | lock released, mark gone with it |

- **The mark is one bit on the session's own lock list**, not on
  the shared table, so neither call takes a mutex. Only the
  exclusive holder reads or sets it. The lock is held from the
  refused write on, so the newest committed version cannot change
  between the read and the write.
- **The mark is single-use.** Left set, a write after a savepoint
  rollback, or much later, would slip past the check. A later
  write needs no read while the newest version is the
  transaction's own. It needs one again if a rollback undoes
  those writes: the newest version is the other transaction's
  once more, the lock is still held, and the mark was spent.
- **sm1 forces the read, not the re-check.** Whether the caller
  evaluates its predicate is the caller's.
- **`sm1::read_latest` needs** READ COMMITTED and something that
  keeps the newest version from changing: a lock on the key, or a
  write of the key by this transaction still in flight. A
  temporary store needs neither. Otherwise it fails with
  `SM_ERR_INVALID`. Above READ COMMITTED it reads only a newest
  version that is the transaction's own, which no snapshot hides:
  an insert's arbiter finding a key its own statement took
  (scenario a19). Under a shared lock it reads but sets no mark,
  as a shared holder cannot write without upgrading. An index
  build reads so what committed while it waited (INDEXBUILD.md).
- **An insert** has no predicate to evaluate again. It is judged
  against the newest committed version, `SM_ERR_DUPKEY` if live,
  with no check, as PostgreSQL's unique check does.
- **`sm1::lock_row`** at READ COMMITTED never fails with
  `SM_ERR_CONFLICT`. Above it, a version committed after the
  transaction began fails the call.
- **Version order needs no help**: the write that goes ahead
  takes a `wts` after the lock, so it sorts first.

### 4.7 Rollback and abort

- **A rollback to a savepoint of a versioned key** deletes
  exactly the versions written since the mark, by key, in one
  batch for each instance.
- **A rollback of a chained key must repair.** A version written
  under the savepoint is dead while its transaction lives, and no
  status lookup says so. The undo list records each chained
  write's key. The rollback puts back, under each key's latch,
  the newest version at or below the mark's `seq_hwm` or not its
  own, and unlinks the entries passed (§9.4).
- **An overflowing undo list loses the savepoints**, not the
  write (XCTMGR.md). With no savepoint set, nothing is recorded,
  and nothing needs to be.
- **A rollback ends what may hold a stale view**: the cursors
  opened after the mark, every batch iterator, and the session's
  own raw pin. It raises `write_gen`, so no iterator opened
  before it serves a read after it. The stores made since the
  mark go last.
- **An abort repairs nothing.** Its versions are dead by the
  status lookup. Readers pass them by the chain, at a hop each.
  The next write of the key overwrites one. The Garbage Collector
  repairs the rest. Aborts are rare.
- **A crash is an abort**: there is nothing to recover. The
  `WriteBatch` keeps CURR and OLD together.

---

## 5. The read path

### 5.1 The view

A read's view is `view_t`: `snapshot_ts`, the reader's `xid`, and
`seq_hwm`. `xct_read_view` gives it. At READ COMMITTED it is the
statement's snapshot, fixed while the statement runs. Above it is
the transaction's. The visibility test of a version is
(`visible()`, XCTMGR.md):

1. A version of the reader's own is visible if its `seq` is at or
   below `seq_hwm`.
2. A version with `wts` above `snapshot_ts` is not visible, as
   `commit_ts` exceeds `wts`.
3. A stamped version carries its `commit_ts`. An unstamped one
   looks up its writer, and is not visible unless the writer
   committed.
4. The version is visible if it committed at or before
   `snapshot_ts`.

### 5.2 A versioned point read

`sm1::read`, `access_read`:

1. Look the store up at the view (`enter()`).
2. Under SERIALIZABLE, take the SIREAD entry.
3. Enter a read epoch, held until the statuses are resolved. The
   `CF:txn` sweep waits for it.
4. Take the pin's iterator if it may serve this read (§5.4), else
   open one.
5. Seek to `key ‖ ~max(wts_hi, snapshot_ts) ‖ 00000000`. Under
   SERIALIZABLE, seek to `key ‖ 00×12`.
6. Walk the key's versions, newest first. The first visible one
   decides: a tombstone means no row, a payload is the row.
7. Step past an invisible version. Under SERIALIZABLE, a
   concurrent writer of it is owed an rw-edge.

The loop normally runs once. It passes only versions written by
transactions concurrent with this reader on this exact key.

Why the seek starts where it does:

- **`wts_hi` is the largest `wts` the transaction has written.**
  Under REPEATABLE READ its own versions have `wts` above
  `snapshot_ts`, so a seek at `~snapshot_ts` lands after them.
  Under READ COMMITTED the snapshot moves and can pass `wts_hi`,
  so a seek at `~wts_hi` lands after commits it must see. The
  larger of the two covers both.
- **A `~seq` of 0** lands before every own version of the key,
  and the visibility test picks the one `seq_hwm` allows. Others'
  versions between the snapshot and `wts_hi` are stepped past.

Under SERIALIZABLE two things differ:

- **The SIREAD entry comes before the iterator exists**, whether
  or not a row is found. Reading a missing key is a predicate
  read, so a later insert of it must find the entry. Taken first,
  it closes the window in which a write lands after the
  iterator's implicit snapshot and before the entry exists.
- **The seek starts before every version.** Versions with `wts`
  above `hi` sort before `~hi`, and they are exactly the
  concurrent writers that owe an rw-edge. A writer is concurrent
  if it is not the reader, and is in flight or committed after
  the snapshot. The extra steps are over those versions alone.

`ReadOptions::prefix_same_as_start` holds the iterator to one
userkey, which is what prefix mode and the bloom filter are for.

### 5.3 A chained point read

`sm1::chain_read`, `chain_get`:

1. Check the view and the store, and empty the pin.
2. Take the SIREAD entry, unless the store is temporary.
3. Enter a read epoch.
4. `Get` CURR into the pin.
5. If the view sees the version, a tombstone means no row, and a
   payload is the row.
6. If not, owe its writer an edge under SSI, as a versioned read
   does.
7. If `prev` is 0, there is no row.
8. `Get` `prev` from OLD and go to step 5. A missing id means no
   row: the Garbage Collector dropped it, and no snapshot needed
   it.

A `Get` carries no RocksDB snapshot, and needs none. CURR and OLD
are written in one batch, a repair unlinks an entry and does not
delete it, and the reader's snapshot holds the watermark back, so
a reader on its way finds what it can see.

### 5.4 An iterator sought again

**WARNING:** this applies to a versioned read alone. A chained
read makes a `Get`, so a table's rows have no iterator to seek
again.

A versioned read into a raw pin leaves the row in the iterator
that found it. Opening that iterator, a merging iterator over the
memtables and every level, was over half of a point read: 1.3 µs
of a 2.3 µs read in key order. So the next read into the same pin
seeks the iterator it holds (`reusable()`), when the iterator
holds everything the read may see:

- **Its session and store.** The pin records which read opened
  it.
- **Every commit the view sees.** The pin records `visible_ts`
  read just before the iterator opened. It serves a view whose
  snapshot is no later. Under READ COMMITTED the next statement's
  snapshot may pass it.
- **The session's own writes.** `session_t::write_gen` rises with
  every write, at each rollback to a savepoint, and as a bulk
  load closes, and it never falls, as `seq` does. An iterator
  serves only reads made at the generation it was opened at.
- **The read epoch it was opened in.** Between reads the pin is
  no reader, so the sweep may delete the status records of
  versions the iterator holds unstamped. Once the epoch moves on,
  a new iterator holds them stamped. Within the epoch, the read
  entered before it seeks, so no deletion passes it.
- **Not SERIALIZABLE.** The SIREAD entry must exist before the
  iterator. An older iterator misses a concurrent write made
  between the two, and with it the rw-edge (scenario c36).
- **A healthy iterator**: its status is OK.

`sm1::read`'s lending form reads into the session's own pin, so
it reuses too. A write's check and `sm1::read_latest` open their
own. With reuse, a read into one pin costs what RocksDB's `Get`
of the exact version key does: 3.6 against 3.3 µs at random, 0.9
against 1.1 µs in key order.

### 5.5 Reads past the snapshot

| Call | Reads | Returns |
| --- | --- | --- |
| `read_latest`, `chain_read_latest` | the newest committed version, under a lock (§4.6), passing another's version in flight | 0 row, 1 none, `SM_ERR_MOVED` |
| `read_newest` | the newest version, committed or own | 0 row, 1 gone, 2 another still running wrote it, `writer` set |
| `read_settled`, `read_settled_versioned` | the newest version, if every snapshot sees the same | 0 row, 1 gone, 2 not settled |

- **`read_newest`** is how a unique tree checks a key against the
  rows its equal entries name (BTREEINDEX.md). It locks nothing.
  `sm1::wait_writer` waits for the writer it named, as a write
  waits for an uncommitted version, and the caller reads again.
- **`read_settled`** is the cleaner's. A row is not settled if
  another's or the session's own write is in flight, or if it
  committed past the watermark. It locks nothing and waits for no
  one. `read_settled_versioned` reads a versioned key in any
  store, a TOAST chunk's in a chained store.
- **`sm1::set_no_transaction`** lets a cleaner's session make
  settled reads with no transaction, for good. It needs no
  snapshot, and holds no watermark back.

### 5.6 A scan

`sm1::scan` opens a RocksDB iterator on the store, sought to
`skey` and bounded by `ekey`, on a **RocksDB snapshot** taken at
the cursor's creation. The RocksDB snapshot gives a stable
physical view. The MVCC snapshot gives the logical view. Both are
needed.

```
seek target         = skey | (skey_inclusive ? 00×12 : FF×12)
iterate_upper_bound = ekey | (ekey_inclusive ? FF×12 : 00×12)
                      (with no ekey, the first current key)
```

The bounds carry a full tail, as the comparator needs one. No
real version has an all-`00` or all-`FF` tail.

- **Without the RocksDB snapshot**, other sessions' eager writes
  would appear in mid-iteration, and the iterator's consistency
  would depend on memtable flush timing.
- **A scan does not see the transaction's own writes made after
  `sm1::scan`.** This is deliberate. It is Halloween protection,
  and it makes "update every row I scan" safe.
- **The cursor keeps `seq_hwm` as it was at `sm1::scan`.** Its
  view of its own writes is fixed like the rest.
- **A rollback closes a cursor opened after the savepoint.** A
  frozen `seq_hwm` would go on admitting versions the rollback
  undid. Later use fails with `SM_ERR_SCANCLOSED`. A cursor
  opened before the mark stays: its RocksDB snapshot predates
  every write the rollback undoes, and the `seq` rewind stops a
  reused number from changing what it returns.
- **A cursor holds a read epoch for its whole life.**

**Seek mode.** Store column families carry a prefix extractor,
and a RocksDB iterator there defaults to prefix seek mode, which
returns only keys that share the seek key's prefix. A scan
crosses userkeys, so it sets `ReadOptions::auto_prefix_mode`.
Left at the default, it stops at the first key boundary, and when
`skey` is absent the bloom filter ends it at once.
`auto_prefix_mode` lets RocksDB decide for each seek: prefix mode
and the bloom filter when the bound lies inside the seek key's
prefix, total order otherwise. The flaw documented on that option
concerns userkeys shorter than a full-length prefix, which cannot
arise, as the extractor is variable-length.

**Direction.** The iterator is only ever stepped with `Next()`.
There is no backward scan, so `auto_prefix_mode` never serves
one.

**Reposition.** `sm1::scan_seek` seeks the same iterator again,
and keeps its RocksDB snapshot, MVCC view and upper bound. So a
probe loop costs one cursor, not one a probe. It moves forward
only, and refuses two targets:

- **A target before the last key examined.** The SIREAD range is
  narrowed to that key, and moving back would record less than
  was read.
- **A target below `skey`.** The iterator has no lower bound, so
  a seek under `skey` would return rows outside the range the
  cursor registered.

A target past `ekey` is allowed: the cursor reports its end. A
cursor at its end stays there, as SSI treats it as having read
its whole range. Reviving it and closing it later would narrow
that range.

**Bound lifetime.** `iterate_upper_bound` is a `rocksdb::Slice`,
which borrows. `scan_t` owns the bound's bytes, and the iterator
goes first.

`sm1::scannxt` walks version keys. For each userkey it hands out
the first visible version, and steps past the rest:

1. Step off the row handed out last, if any.
2. At the first version of a new userkey, record it in
   `last_examined`, which is the key's one copy and the key
   handed out.
3. Once the key is decided, step past its older versions.
4. If the view sees the version, the key is decided. A tombstone
   hides the key. A row is handed out where the iterator has it,
   and `on_row` is set.
5. If not, owe its writer an edge under SSI, and go on.

The row is a view into the iterator's block until the next call,
so a scan copies no row. SSI sees nothing move: the versions
newer than the visible one are judged before the call returns,
and the older ones were only ever stepped over.

**SERIALIZABLE.** `sm1::scan` registers the whole range as a
SIREAD predicate when it creates the cursor, before it takes the
RocksDB snapshot. `sm1::scanend` shrinks it to the range covered:
from `skey` through `last_examined`, or through `ekey` if the
scan reached its end.

- **The order matters.** A writer puts its version before it
  calls `on_write`, so a version put after the snapshot is found
  only if the SIREAD entry already exists.
- **Shrinking at the end is safe.** The reader never read past
  `last_examined`. Edges made for writes beyond it are merely
  conservative.
- **A scan owes rw-edges as a point read does.** They matter most
  for versions committed between the reader's snapshot and
  `sm1::scan`: their writers ran `on_write` before the range
  existed, so the read side alone can make the edge. They also
  feed the post-restart rule (XCTMGR.md).

### 5.7 A chained scan

`sm1::chain_scan` iterates class `0x01`, and follows the chain of
a row whose newest version it does not see.

- **It reads CURR and OLD at the cursor's RocksDB snapshot**, and
  its view keeps the watermark back, so nothing it can reach is
  dropped.
- **Its bounds are current keys.** The upper bound is
  `curr_key(ekey)`, with a 0 byte added when inclusive. With no
  `ekey` it is the first OLD key. The lower bound is the start
  key as given.
- **It runs in `auto_prefix_mode`.** In prefix mode, a current
  key being its own prefix, it would stop at the first key, and a
  range scan would skip files that hold no key equal to its
  start.
- **Each key is visited once**, so the key's copy is
  `last_examined` alone. A row is lent by the iterator, or by the
  cursor's `got` when it comes from OLD, until the next call.
- **A stamped version needs no lookup** (`chain_visible`): the
  common case decides by its stamps alone.
- **`scan_seek`** seeks `curr_key(target)`. Its refusals are
  those of §5.6.

### 5.8 The shape of the read calls

`sm1/API.md` is the contract. Three shapes in it encode
decisions:

- **Both range bounds are parameters of `sm1::scan`.** A NULL
  bound means none, and the empty key is a bound. Without an end
  key a range predicate cannot be recorded for SSI, and every
  caller would scan and discard.
- **"No row" is a return value.** `sm1::read` returns 1 for no
  such key, and `sm1::scannxt` 1 at the end of the range. It is
  no error, and does not disturb the error state, so a caller
  tells 1 from -1 before it reads `sm1::errnum()`.
- **`sm1::scanend` returns void.** Releasing a cursor cannot
  usefully fail. It shrinks the predicate, so closing early is
  both correct and tighter than draining.

### 5.9 The size of a rel

A rel's size is its key range in its class. A chained table's
adds the store's OLD, which holds its rows' older versions alone:
`relid_size` with `old`. OLD keys carry no relid, so a table's
memtable count takes in its old rows, as each write counts all
its entries under the row's key.

---

## 6. Pins

### 6.1 The objective

A row goes from RocksDB to the executor with as few copies as
possible, and a client deforms it column by column, as far as it
reads. PostgreSQL copies a row none: its slots hold a pin on the
buffer the row is in, and its heap slot deforms lazily. The
objective is the same for every client of sm: digres now, and
perhaps MySQL later.

- **What a copy costs.** With no pin, every read by key copied
  the row twice, once in sm1 and once for the slot.
- **What a full deform costs.** `count(*)` over `wide_mixed`, 50
  columns: sm2's `tup_deform` was about 23% of digres's time, all
  of it columns nobody reads. PostgreSQL's planner gives a scan
  every column, and its heap slot deforms none unless asked.
- **Why a lazy slot needs a handle.** It deforms after the read
  returned, so the bytes must still be there. A pointer to sm's
  bytes would read freed memory once the scan moved. A ref with a
  generation fails instead.
- **Why in sm, not in digres.** Every client needs it, and what
  it needs to know, the table's layout and the tuple's format, is
  sm3's and sm2's.

### 6.2 Two levels

- **sm1 keeps a raw pin**, `sm1::rawpin_t`: what holds a row's
  bytes. Its caller owns it. sm1 has no pool, no generation and
  no refs: it knows bytes, not tuples.
- **sm3 owns the pins**, `sm3::pin_t`: a raw pin, the row's table
  definition, and how far the row is deformed. The pin a client
  holds has to carry the definition, the generations and release
  rules are what clients see, and sm3 is the level they call.

### 6.3 The raw pin

```cpp
namespace sm1 {
struct rawpin_t;                    /* opaque */
rawpin_t* rawpin_new();             /* empty */
void rawpin_free(rawpin_t* pin);    /* empties it first */
void unpin(rawpin_t* pin);          /* lets go of its row */
}
```

- **It is opaque.** A public header names no RocksDB type, so the
  pin is a handle, and `sm1/src/session.h` has its layout. A
  caller that keeps one makes it once and reuses it.
- **What it holds depends on the read.** A versioned read leaves
  the iterator that found the row. It does not move after that,
  so the view stays valid without `ReadOptions::pin_data`: a
  block's bytes through the block it holds, a memtable's through
  the SuperVersion it refs. A chained read holds a
  `rocksdb::PinnableSlice`, `got`: a `Get` leaves a block pinned
  there, or a copy of a memtable's row.
- **A read or a write fills the pin the caller gives.** A pin
  that held a row lets go of it first. A read that finds no row
  empties the pin. The calls are `read`, `read_latest`,
  `read_newest`, `read_settled`, `update`, `erase`, `erase_moved`
  and their `chain_` forms.
- **The lending form stays.** `sm1::read` with no pin, and a
  write's `old` with none, use the raw pin of the session's own.
  The row lasts until the session's next such call. The end of
  the transaction, a rollback to a savepoint and the session's
  close empty it.
- **Nothing releases a caller's raw pin at the end of a
  transaction.** A raw pin's iterator carries no RocksDB
  snapshot, as visibility is sm1's own, by timestamp. So a raw
  pin that outlives its transaction holds memory, never a view.
- **The read epoch is not held.** `db_read_leave` runs before the
  read returns: the epoch guards resolving the versions' status,
  not the bytes. A pin left behind holds memory, never the
  `CF:txn` sweep.
- **Every raw pin is emptied before the database closes**, as
  RocksDB must not close a DB with an iterator open.
- **A pin covers the row only.** A value out of line is fetched
  and assembled into memory of its own (TOAST.md).
- **A dropped store keeps a pinned row readable.** A rollback to
  a savepoint drops the column families made after its mark, and
  the Garbage Collector drops those whose drop committed, in any
  session's. Neither releases a pin. An iterator's SuperVersion
  holds a reference that outlives `DropColumnFamily` and
  `DestroyColumnFamilyHandle`, so the row stays, and its files
  stay on disk, until the pin lets go.

### 6.4 The pin and its ref

```cpp
/* sm3.h: a plain value the caller holds. Zero names no pin. */
struct pin_ref_t {
  pin_t* pin = nullptr;
  uint64_t gen = 0;
};
```

- **The session owns every pin**, in a pool. A read hands the
  caller a ref. The caller never holds sm's memory, and sm never
  touches the caller's.
- **A ref is stale** when `ref.pin` is null or `ref.gen` is not
  `ref.pin->gen`.
- **A pin is released** by the next read into the same ref, by an
  unpin, by a release of all the session's pins, or at the end of
  the transaction. It is never released by a rollback to a
  savepoint, or by the drop of its store.
- **Releasing** lets go of the raw pin's row, increases the
  generation, and returns the pin to the pool. Every ref to it is
  then stale. An unpin through one does nothing, and a read into
  one takes a fresh pin.
- **`row` is apart from `raw`**, as a cursor's pin views the
  cursor's row and holds no iterator of its own.
- **`def` is set when the row is read**, and assigned only when
  it is a different definition, so a scan of one table copies the
  `shared_ptr` once. A definition is replaced, never changed, so
  the pin's lasts as long as the row. A released pin keeps its
  `def` until it is taken again: memory only, a pool's worth at
  most.
- **The reader is begun on the first deform**, not on the read,
  so a row nobody deforms costs no parse: a `count(*)`, or an
  UPDATE's old row that no index needs.

### 6.5 Why a generation

A released pin goes back to the pool at once, so the next read
may take it for another row. A ref the caller still holds must be
told apart from the ref the pin now belongs to. With a flag
alone, this goes wrong:

1. A read into `r1` takes pin P for row X.
2. An unpin of `r1` releases P to the pool.
3. A read into `r2` takes P again, for row Y.
4. The caller unpins `r1` a second time: a slot cleared twice, or
   one cleared after the transaction that filled it.

A flag finds P held at step 4, and releases `r2`'s row. With a
generation, `r1` holds 1 and P holds 2, so the unpin does
nothing. So digres needs no ResourceOwner and no memory context
callback: a ref it loses on an error is left behind, and the pin
goes at the end of the transaction.

**How many pins a session has.** A pin returns to the pool on its
release, so the pins in use are the rows pinned at that moment,
not the rows read so far.

- An index scan that reads a million rows into one ref holds one
  pin.
- Index maintenance that makes a ref a row and unpins it holds
  one pin, and reuses it.
- A join that keeps its outer row while it reads inner rows holds
  two.

The pool grows to the most pins held at once, and keeps them
until the session closes. A released pin cannot be freed before
that, as a stale ref may still point to it, and an unpin must
find a pin to compare generations with, not freed memory.

### 6.6 Reading into a pin

`tbl_read`, `tbl_read_key`, `tbl_read_latest`, `tbl_update` and
`tbl_delete` take a `pin_ref_t&`, and `rel_read`,
`rel_read_latest`, `rel_update` and `rel_delete` a
`sm1::rawpin_t*`. A client of sm2 alone gets pinned reads, and
keeps each pin itself.

- **A read into a ref that names a held pin reads into that pin**
  (`pin_reread`). Its refs go stale as on a release, but the raw
  pin keeps what it holds, so a versioned read may seek its
  iterator again (§5.4).
- **A read that finds no row** puts the pin back, and the ref
  names none.
- **A write takes a pin of its own for the old row**, so index
  maintenance leaves the row where it is. The pin goes to the ref
  once all of the write has succeeded and the indexes are fixed.
  Until then the ref keeps what it held, as the new values may
  point into that row.
- **A write with no ref** puts its pin back when it ends.
- **A rel keeps a scratch raw pin** for a write of a row with
  values out of line and no pin of the caller's: the old row
  stays pinned while its pointers are found. It is made on first
  use, and `rel_close` frees it.

### 6.7 A scan's row

- **`tbl_scan` returns an sm3 cursor**, `sm3::scan_t`: the sm2
  rel scan, and one pin taken from the pool when it opens.
  `tbl_scannxt`, `tbl_scan_seek` and `tbl_scanend` replace sm2's
  calls on it.
- **`tbl_scannxt` hands back a ref** to that pin, and the row's
  bytes. It increases the generation, points `row` at the
  cursor's row, and resets the reader. A ref to the previous row
  is then stale, as is the last at the end. Its cost for a row is
  that alone. A form without the ref is for callers that want the
  bytes.
- **The bytes are the cursor's**, valid until it moves, as a heap
  scan's tuple lasts in PostgreSQL until the scan moves.
- **A pin with `scan` set is the cursor's:**
  - An unpin through its ref leaves it alone.
  - Releasing all pins, and the end of the transaction, make it
    stale and leave it with the scan. sm1 closes the cursor then
    too (`xct_end_scans`), so its row is gone.
  - Closing the scan releases it to the pool.
- **A tree's cursor** is the same outside. It reads its entries,
  then their rows in batches into raw pins of its own, and its
  pin views the row handed out (§7.3, BTREEINDEX.md).

### 6.8 Deforming

```cpp
int pin_deform(pin_ref_t ref, size_t from, size_t n,
               std::span<datum_t> datums, std::span<bool> isnull);
int pin_decode(pin_ref_t ref, size_t ordinal, datum_t& val, bool& isnull);
int pin_tuple(pin_ref_t ref, std::string_view& tuple);
```

- **`pin_deform`** gives attributes `from` to `n - 1`. It resumes
  where the last deform stopped. For a caller behind that place
  it reads from the header again, and may write earlier
  attributes again, with the values they had. A by-reference
  datum points into the row, and lasts as long as the pin.
- **`from` is the client's**, so the pin keeps no values, only
  the reader. digres passes its slot's `tts_nvalid`, and its
  `tts_values` keep what earlier deforms filled.
- **`pin_decode`** reads one attribute by ordinal, as
  `tup_decode` does. A client that reads columns out of order, as
  MySQL's `read_set` does, decodes each. A column read twice out
  of order is decoded twice.
- **`pin_tuple`** gives the row's bytes, to copy.
- **A stale ref fails with `SM_ERR_INVALID`.** It never reads
  freed memory.
- **A value out of line** comes back as its TOAST pointer.
  `tbl_fetch` reads it.

sm2 gives the reader: `tup_read_begin` parses a tuple's header
once, and `tup_read` deforms its columns in as many steps as the
client takes (`tup_reader_t`, `sm2/TUPLE.md`). Resuming is free:
an expression that needs column 3, then one that needs column 40,
deforms 0 to 3, then 4 to 40, each column once. `tup_deform`
stays for callers that hold bytes, not a pin, and checks the
whole tuple.

**A read checks only what it reads**: the header and bitmap once,
then each entry and varlena it deforms. A malformed part nobody
reads is not found. Writes check a tuple whole before it is
stored, and `tup_check` checks one on demand, so a tuple sm wrote
is whole. A read is checked against damage in what it reads, not
audited.

### 6.9 When a pin is released

- **At the end of the transaction.** `sm3::commit`, `abort` and
  `prepare` release every pin first, and make each cursor's pin
  stale. `prepare` releases them whether or not it succeeds.
- **A rollback to a savepoint releases no pin.** Its row was read
  before the rollback, a pin makes no later read, and whether the
  row is still wanted only the caller knows. A cursor the
  rollback closes has no row any more, so sm3 asks each cursor's
  rel scan after the rollback (`pins_rolled_back`,
  `scan_rolled_back`), and makes the closed ones' pins stale. A
  tree's cursor has no rel scan for the rollback to close. It
  keeps its entry position, discards unread cached rows, and
  reads those rows again at the view after rollback.
- **When sm1 ends the transaction on its own**, as after a
  serialization failure, the pins stay until the client's
  `sm3::abort`. That holds memory only.
- **At `session_close`**, before the client closes sm1's session,
  so no iterator outlives the database (`pins_close`).
- **`release_all_pins(session)`** is for a client that knows,
  short of the transaction's end, that none of its rows is still
  wanted. The transaction stays open. A tree cursor discards
  its cached rows and reads pending rows again. At commit,
  abort, or prepare, the cursor stays closed until its caller
  frees it; a later begin cannot reopen it.

**A tree cursor closes at the transaction's end, and that is
correct.** Its batches are read at one transaction's view, and
its entry positions stand for that view. A cursor that went on
into the next transaction would give rows of two views, and
would give a row the new transaction may not see. A cursor of a
rel scan ends with its transaction too. So `commit`, `abort`
and `prepare` close a tree cursor, and `begin` closes any that
an engine-side abort left. Its handle stays the caller's: use
fails with `SM_ERR_SCANCLOSED` until the caller frees it.

### 6.10 No content lock

A PostgreSQL pin is only a count. Its shared and exclusive modes
are those of the buffer's content lock, taken beside the pin,
because a heap page changes in place. Its cleanup lock waits to
hold the page's only pin, as it moves tuples whose addresses
other pins hold. sm needs neither, as nothing changes a row's
bytes once written:

- **A write adds an entry.** A versioned write is a new key. A
  chained write is a new entry in RocksDB for CURR and for OLD,
  in one batch.
- **RocksDB changes nothing in place.** A memtable's entries are
  in an append-only arena, and an SST file's blocks are never
  written again. `inplace_update_support`, the one exception, is
  off.
- **Nothing moves a row.** Compaction writes new files and does
  not wait for a reader. A held iterator's SuperVersion keeps the
  old memtable and files.
- **A reader sees only whole rows**, so a reader and a writer
  never wait for each other.

What a held pin costs is space, not a wait:

- **Memory.** A memtable stays in memory after its flush while a
  SuperVersion holds it. It does not hold writes up: RocksDB
  stops writes only at `max_write_buffer_number` memtables not
  yet flushed, and an iterator holds a flushed one, or goes on
  beside inserts into the active one. The exception would be a
  write buffer manager, which sm does not set. Were one set, a
  held memtable's memory would count against it, flushes would
  come early, and a long-held pin would slow writes.
- **Disk.** The files a compaction replaced go only when the last
  iterator on them goes.
- **The block cache.** A pinned block is not evicted.
  `strict_capacity_limit` is false, so the cache grows past its
  capacity and does not fail.

An iterator's cost is the versioned read's and the cursor's.
PostgreSQL's pins are bounded by its buffer pool, and these by
how many rows a caller keeps pinned at once.

### 6.11 Hazards

- **A failed statement's pins stay while a cursor is open.** Only
  an error leaves a pin behind, and the release at the end of a
  statement (§10.5) takes it, unless a cursor is open. digres has
  no procedural language and starts no subtransaction of its own.
  A subtransaction is a client's `SAVEPOINT`, and each rollback
  to one is a top-level statement, so a pin an error left goes at
  that statement's end. A client that opens a cursor, then loops
  on `SAVEPOINT`, a failing statement and `ROLLBACK TO`, leaves
  the pins of each failed turn until the cursor closes or the
  transaction ends.
- **A scripting language would make it worse.** One is planned,
  over SPI. A loop that catches an error each turn, as PL/pgSQL's
  `EXCEPTION` block does, runs within one top-level statement,
  and would leave a pin a turn until that statement ends, cursor
  or none. If either case is seen, a ref marked as the
  statement's, which a rollback to a savepoint releases, meets
  it: an addition, not a change to §6.4.
- **A cursor opened inside a savepoint is closed by a rollback to
  it.** This is sm1's rule for cursors, not pins. An outer SQL
  cursor over a nested loop may rescan its inner index scan
  inside the savepoint, which opens a new sm cursor. After
  `ROLLBACK TO`, its next FETCH fails with `SM_ERR_SCANCLOSED`,
  where PostgreSQL goes on: a known incompatibility
  (`../dg/README.md`). `../dg/test/slt/point_read.test` shows
  both sides.
- **A rollback that closes a cursor takes its lent row.** A
  caller reads that row only before the cursor's next call, which
  fails after such a rollback anyway. digres's nested loop
  projects its row before it asks the inner scan for the next.
- **A held cursor holds a SuperVersion.** A cursor left open for
  hours keeps the memtables and files its rows are in.
  PostgreSQL's pin keeps a page, which is cheaper, but blocks
  VACUUM on it. A session idle in a transaction with a cursor
  open is the case to watch.

### 6.12 The copies left

Counted from the bytes in the block cache. A block read on a miss
is RocksDB's, as a page read is PostgreSQL's. The copies of a
Sort, a Hash, an Agg or a Material are not counted, as PostgreSQL
makes them too.

| Scan | Entry | Row | PostgreSQL 18 |
| --- | --- | --- | --- |
| Sequential, sample | — | 0 | 0 |
| Index, bitmap heap | 0 | 0 | 0 |
| Key scan, a write's target, a recheck | — | 1, the slot's copy | 0 |
| Index-only | key columns 1, INCLUDE 0 | — | 1, btree's `currTuples` |
| DELETE's old row | — | 0 | 0 |

- **A row in a memtable** comes from `Get` as RocksDB's copy, a
  batch's iterator (§7.2) copies each row into its pin, and a
  scan's row stays in the iterator's memory.
- **An index-only scan's copy** is its unescaping of a key,
  straight into the varlena it hands out: behind `varlena_room`
  bytes, its header written there by `varlena_in_room`.
- **A read by key with no scan** has no scan to hold its ref, so
  it reads, copies into the slot, and unpins (`read_row_at`).

---

## 7. Batch reads

### 7.1 Why

```sql
-- Q9: 100 rows by a secondary index, their rows next to each other
select sum(c1) from wide_mixed where c2 between :a and :a + 99;
-- Q7: 100 rows by primary key, the keys sorted by the probes
select c1 from wide_mixed where id in (:k1, ..., :k100);
```

A row an entry names is read by its key, and so is each row an IN
list's probes name on the primary key. Each `Get` starts afresh:
the store's check, a seek of the sorted file's index block, a
look in the block cache, a search of the data block. From one
client, on the release build:

| | PostgreSQL | digres, one `Get` a row |
| --- | --- | --- |
| Q9, a row behind an entry | heap fetch by TID, ~0.35 µs | `Get` by key, ~2.4 µs, 68% of the query |
| Q7, a row by key | btree descent and heap fetch, ~3.1 µs | `Get`, ~3.7 µs, 74% of the query |
| a query | Q9 0.212 ms, Q7 0.405 ms | Q9 0.357 ms, Q7 0.498 ms |

RocksDB's `MultiGet` reads keys of one column family in one call,
and shares the index and data block lookups of keys in one block,
and the per-call work. MyRocks reads rows behind secondary
entries and IN lists so, through MySQL's multi-range read.

With batching, Q9's rows lie together, and it went from 0.357 ms
to 0.214 ms in its best run, and typically runs 0.3 to 0.4 ms. Q7
stays at 0.49 ms: keys far apart share little. Half of its 100
Zipfian keys lie in a few data blocks, and the rest lie one a
block.

**A batch reads what as many single reads at the same view
read.** The view is fixed while the statement runs, so reading a
row early returns the version reading it late would.

### 7.2 sm1: `chain_read_batch`

`chain_read_batch` answers each key in the caller's order:
`rc[i]` 0 with its row in `pins[i]`, 1 for none it sees. The call
returns -1 on an error. Each key gets its check and SIREAD entry
as a single read does. One read epoch covers the call. §9.7 has
the steps.

- **Keys in any order go to `MultiGet`**, in one call. RocksDB
  sorts them, as sm1 says they are not sorted.
- **Keys in ascending order** are read by one iterator, as a
  range of an index names its rows when they are close. It seeks
  the first key, and steps up to four times to each next key.
  From the first key the steps miss on, the rest go to
  `MultiGet`, as keys far apart do not gain from the iterator.
  Q9: 1,659k to 1,338k user instructions with an LSM Index,
  1,548k to 1,142k through a tree.
- **An iterator's row is copied into the key's pin**, so the
  iterator can go with the call and the pin stays.
- **A batch of one key** goes to `MultiGet`.
- **Each key is then resolved** as `chain_get` resolves one: from
  its current version, through `get_old` one by one where its
  view needs an older one.

There is no batched read of versioned keys. A versioned read is
an iterator seek across a key's version run, so `MultiGet` does
not apply. With the pin's iterator sought again, a batched call
would add only seeking in sorted order, which an index range
already does. `rel_read_batch` over a versioned store reads each
key in turn.

### 7.3 A kept iterator

A tree's cursor reads its rows in batches of 1, 2, 4, up to 64
(`tree_batch_max`). So that no batch makes and frees an iterator
of its own, the cursor keeps one, `sm1::batch_iter_t`. It is made
at the cursor's first batch of two or more, and passed to each
later batch.

- **Each batch calls RocksDB's `Iterator::Refresh()`** on it. The
  iterator then holds the store as it is now, as a new one would.
  While the store's memtables and files stay the same,
  `Refresh()` only moves the read sequence. When they change, it
  makes the iterator again.
- **The refresh is where a new iterator was made**: after the
  batch's checks and SIREAD entries, inside its read epoch.
- **The session keeps a list of the kept iterators** and lets go
  of them where it closes its cursors: at the transaction's end,
  at a rollback to a savepoint, and at its close. A caller that
  does not free one, as digres on an error, then holds no RocksDB
  file.
- q3 of the benchmark, about 10 rows a probe through a tree: 283k
  to 249k instructions a statement.

### 7.4 Scattered keys

The steps help only when a batch's rows are close in the store,
as an index correlated with the key finds them.

- **A miss marks the iterator.** An uncorrelated index's rows are
  far apart: the batch seeks the first key, misses the second,
  and reads the rest by `MultiGet`. A seek costs more than a key
  of `MultiGet`. So a batch whose steps reach no key past the
  first marks the kept iterator, and lets its RocksDB iterator
  go. The cursor's later batches of the store go to `MultiGet`
  alone. A batch that steps through some keys before a gap keeps
  the iterator. q3: 249k to 234k instructions a statement.
- **ANALYZE says first.** The first batch that misses still makes
  the iterator and seeks once a scan. So a tree's cursor asks the
  table's statistics first. If the absolute correlation of the
  tree's leading key with the rows' order is below 0.5
  (`tree_scattered_below`), its iterator is made scattered
  (`batch_iter_new(true)`), and every batch goes to `MultiGet`
  alone. With no statistics, or a stronger correlation, a batch
  tries the steps. Stale statistics cost speed, never a wrong
  row. q3: 234k to 203k instructions a statement. A tree on
  `id / 10`, whose rows are together: 189.7k to 189.9k.

### 7.5 The levels above

- **sm2**, `rel_read_batch`: puts each key under the table's
  relid, as `rel_read` does one, and passes a kept iterator on.
- **sm3**, `tbl_read_keys(table, keys, n, refs, tuples, rc)`: a
  pin for each key, the one its ref holds or one from the pool,
  put back for each key not seen. It keeps no iterator between
  calls.
- **sm**, `sm_tbl_read_keys`: the same in the C API, with each
  key's length and each tuple's.

digres's read-ahead is §10.6.

---

## 8. The tuple relation

### 8.1 A rel

sm1's store is an ordered map of byte string to byte string.
sm2's is that store with the types put back: a row goes in as a
tuple and comes out as a tuple. This is the whole of sm2's API
surface. The types, the key encoding and the tuple codec are what
it is built of (`sm2/TYPE.md`, `sm2/TUPLE.md`).

A `rel` is opened over exactly one sm1 store, with three things:

- **A schema**: the ordered columns, their types and collations,
  and each key column's direction and null order.
- **A relid** of exactly 2 bytes, big-endian, prepended to every
  key sm2 writes and stripped from every key it reads. It exists
  because one store holds one table and everything of it. sm3
  chooses the value and sm2 never interprets it, so sm2 never
  learns what a table is. A relid is fixed-width, so no rel's
  relid starts another's. It spends 2 of `max_keylen`'s 4096
  bytes.
- **An sm1 session**, where the transaction lives.

`rel_open` lays the schema out, and `rel_open_desc` borrows a
`tup_desc_t` that the caller keeps alive. Both refuse an
attribute with no type, and a key column that is no live
attribute of its type.

### 8.2 Chained or not

**A rel's rows are chained when its store is.** `rel_open` asks
`sm1::store_chained`. Over a chained store a rel reads, writes,
locks, scans and sizes its rows with the `chain_` calls, and so
does the primary key's index rel, whose entries are the rows. A
secondary index's entries and TOAST data stay versioned keys,
under the plain calls.

### 8.3 The calls

A row is named by its key, as `rel_insert` gives it. Each sm1
call has its `chain_` form over a chained store.

| sm2 | what it adds | over |
| --- | --- | --- |
| `rel_insert` | writes a tuple under its key, and returns the key, and a keyless row's new `_id`, 0 for a keyed row. The tuple is encoded from datums, or handed in encoded and checked by `tup_check` before an `_id` is drawn. The `_id` comes from a `seqref_t` looked up at the rel's first draw. A keyless row's key goes in unchecked, as none was before | `sm1::seqref`, `sm1::nextseq`, `sm1::insert`, `sm1::insert_new` |
| `rel_key` | the key of a row from its primary key's values, or a keyless row's from its `_id`. No read | — |
| `rel_row_key` | the key of a row from its `_id` and all its values | — |
| `rel_keyed`, `rel_chained` | whether a rel has a primary key, and whether its rows are chained | — |
| `rel_reserve`, `rel_insert_at` | draws a keyless row's `_id` early, for a caller that writes something of the row first, then inserts as `rel_insert` under it | `sm1::nextseq`, then as `rel_insert` |
| `rel_read` | reads a row by its key, and its `_id`, from the key, 0 for a keyed row. Lent, or into a raw pin | `sm1::read` |
| `rel_read_batch` | the rows at n keys, in one call, each into a raw pin (§7) | `sm1::read` each; chained, `sm1::chain_read_batch` |
| `rel_read_latest` | the newest committed image | `sm1::read_latest` |
| `rel_read_newest`, `rel_read_settled` | a row's newest version, past the snapshot (§5.5) | `sm1::read_newest`, `sm1::read_settled` |
| `rel_lock`, `rel_trylock` | takes a row's lock without writing it: `SELECT … FOR SHARE` and `FOR UPDATE`, and what an index build holds on a row. The relid is the lock's kind, for a host to tell a row's lock from an entry's. `rel_trylock` never waits: 1 with nothing taken | `sm1::lock_row`, `sm1::trylock_row` |
| `rel_update`, `rel_delete` | by key. Hands back the old image as a tuple. `rel_update` takes datums or an encoded tuple, checked as `rel_insert`'s is, and moves a row whose primary key changed (§2.5). With `hold`, the values the old row held out of line are erased at `rel_release`, or kept at `rel_forget` (TOAST.md) | `sm1::update`, `sm1::erase`, `sm1::erase_moved`, `sm1::insert` |
| `rel_load`, `idx_load` | puts a row, or an entry, into a bulk load, as `rel_insert` or `idx_insert` would write it. Nothing is in the store until the load closes. `rel_load` returns 1 for a row it cannot put: past `max_tup_len`, or with a value to move out of line or a TOAST pointer | `sm1::load_put` |
| `rel_scan`, `rel_scan_seek` | tuples in `_id` order, forward only, of a keyless rel. A keyed one's are scanned by key (`idx_scan_rows`) | `sm1::scan`, `sm1::scan_seek` |
| `rel_scan_seek_key` | moves a scan in key order forward to a key | `sm1::scan_seek` |
| `rel_scan_key`, `rel_scan_rolled_back` | the key of the row a cursor last gave, and whether a rollback closed it | `sm1::scan_rolled_back` |
| `rel_keys_at`, `rel_range_size` | the keys at fractions of the rel's bytes, by bisecting its key range on the store's size estimates, and the bytes of a key range. No row is read | `sm1::approxsize` |
| `relid_size` | the bytes under a relid, a rel's or an index's, in files and in memtables apart. It needs no open rel and no transaction. On a chained store the two files' estimates add | `sm1::approxsize`, `sm1::chain_approxsize` |

### 8.4 An index rel

**Two kinds of `rel`, over the same store, told apart by their
relid.** Entries are keyed by encoded columns, and sm3 never
encodes a key itself, so sm2 gives it a `rel` for them too.
LSMINDEX.md and BTREEINDEX.md describe them. What a row depends
on is here.

- **A `rel`** is keyed by its primary key or `_id`, under the
  rows' relid, and its value is the tuple. Its `rel_insert` draws
  a keyless row's `_id` from the table's sequence, named when the
  rel is opened, so the sequence is drawn in one place.
- **An index `rel`** is keyed by the encoded columns it is opened
  with, under the index's relid, and its value is a tuple. It is
  how sm3 writes, deletes and scans entries (`idx_open`,
  `idx_insert`, `idx_delete`, `idx_scan`).
- **A primary key is an index `rel` too** (`idx_open_primary`),
  with no entries of its own. Its entries are the rows, only
  read, and `idx_scan_rows` hands them out in key order as rows.
- **An index `rel` may be opened over a tree**: its entries a
  B-link tree of sm1's pages under its relid (BTREEINDEX.md).
- **An entry's key** is `relid ‖ encode(sk)`, then the row key:
  always in a non-unique index, and in a unique one only for an
  entry with a null key column, so that nulls stay distinct. A
  unique key without one is sm1's uniqueness unit, and a
  duplicate is `SM_ERR_DUPKEY`. A unique index's key is never
  cut.
- **An entry's value** is a tuple: a cut flag, the INCLUDE
  columns, then the row key. A keyless row's `_id`, which a scan
  hands out, is read from that key.
- **An entry gives its key columns back** (`idx_scan_key`), for
  index-only scans. A cut key, or a text column under a
  non-deterministic collation, keeps no datum, and the caller
  reads the row.
- **`idx_find`** gives the row a unique index has a key for, as
  the snapshot shows it, locking nothing. **`idx_find_latest`**
  gives its newest committed version, under a shared lock on the
  key, at READ COMMITTED.
- **`idx_delete` removes an entry committed since the transaction
  began.** sm1 refuses to erase one with `SM_ERR_UPDATED` at READ
  COMMITTED until its newest version is read. An entry has no
  filter to re-check: the decision was on the row, whose lock the
  caller holds. So sm2 reads the newest version itself and
  repeats the erase, and the caller never sees the code from an
  index.
- **A scan's bounds** are the values of the index's leading
  columns, in the index's own order. A bound becomes an sm1 key
  through prefix-freeness: the encoding P of the first j columns
  begins exactly the keys whose first j columns hold those
  values, contiguously. So a bound is P or its successor. A bound
  too long to encode is cut where keys are cut, which can only
  widen the scan, and the scan says so.
- **A key's columns past their budget are cut at one length**,
  `key_budget` of the longest row key an entry may end with: 8
  bytes in a keyless table, `max_pk_len` in a keyed one. So a cut
  bound orders against cut keys as the whole one would against
  theirs.
- **Index relids are below `0xFFFE`**, which also guarantees
  every bound a successor.

### 8.5 The rules

- **sm2 is a codec, not a state machine.** Every durable effect
  is an sm1 call inside the caller's transaction, and sm2 keeps
  nothing across calls that a rollback must undo. So savepoints
  and aborts cost sm2 nothing. The values `hold` keeps for
  `rel_release` are the one list, and `rel_forget` clears it.
- **A row with a value out of line is more than one sm1 call.**
  The row goes first, so a write sm1 refuses has written nothing.
  Then the values the old row held go, then the chunks of the new
  row (TOAST.md).
- **Errors pass through; they are not translated.**
  `SM_ERR_UPDATED` is not retried here. The retry needs the
  statement's filter checked again, and filters live above sm2.
  `rel_read_latest` is what sm2 gives sm3 for it. Only
  `idx_delete` retries, as an entry has no filter.
- **sm2's own failures go through sm1's error state**, its codes
  in `sm_errnum_t`, so one `sm1::errmsg()` serves the whole
  stack.
- **Running out of memory is fatal to the process** once
  `sm1::attach` has installed its new-handler. Before that a
  `std::bad_alloc` passes through sm2 unchanged, and sm2 catches
  nothing. A type's `cmp` and `hash` have no error to return, so
  they must not allocate.
- **The interface is `include/sm2.h`, in `namespace sm2`.**
- **Every column is in the tuple.** An encoded key is an ordering
  device, not storage, so an indexed column is written twice:
  encoded in the index, native in the tuple. So `key_encode`
  never has to be reversible, which is a hard promise to keep
  over collation sort keys, cut keys and any future lossy
  encoding.
- **Decoding is per column, not per row.** Reading one column of
  a wide tuple must not cost the whole tuple. `rel` gets this
  from `tup_decode`'s by-ordinal signature, and from §6.8.
- **`rel` calls the tuple codec, it does not contain it.** The
  codec has its own header and tests, and sm3's INCLUDE columns
  are its other caller.
- **Forward only.** A rel scan inherits sm1's ban. The encoding
  handles a descending column.
- **A decoded datum points into memory sm2 does not copy.** A rel
  hands out the tuple as sm1's view, and a datum points into
  that. It is not NUL-terminated, must not be freed, and lasts as
  the view does: until the next call on the session for a lent
  row, until the pin is emptied for a pinned one. A codec that
  allocated for each datum would dominate every scan's profile.
- **`NOT NULL` is not enforced here.** Constraints are sm3's. A
  codec that refuses data has a policy in it. sm2 stores what it
  is given.
- **A `rel` borrows its schema.** sm3's catalog keeps it alive
  for the rel's lifetime, and takes the table-oid lock before it
  changes it. With no schema version a rel has one schema, the
  current one.
- **A scan stays tuple at a time.** Only rows read by key come
  many in one call.

---

## 9. Implementation

### 9.1 Where the code is

| File | What it holds |
| --- | --- |
| `sm1/src/version.h`, `version.cc` | the class bytes, flags, versioned codec, comparator, prefix extractor |
| `sm1/src/chain.h` | the chained key's layout, `chain_version_t`, the OLD flags |
| `sm1/src/chain.cc` | the chained codec, the `chain_` calls, `chain_restore`, GC's repair, batch reads, chained scans |
| `sm1/src/access.cc` | versioned reads, writes and scans, `sm1::scan_seek`, `sm1::scannxt`, the visibility test, the undo list's entry |
| `sm1/src/pin.cc` | `rawpin_new`, `rawpin_free`, `unpin` |
| `sm1/src/session.h` | `rawpin_t`, `batch_iter_t`, `scan_t` |
| `sm1/src/gc.cc` | `filter_chained`, `filter_old` |
| `sm1/src/load.cc` | a chained stream's keys and values |
| `sm2/src/rel/rel.cc` | the rel |
| `sm3/src/table/pin.h`, `pin.cc` | `pin_t`, `scan_t`, the pool |
| `sm3/src/table/table.cc` | `read_into`, `tbl_read_keys`, `tree_fill`, `tbl_scannxt`, `update_row`, `delete_row` |

### 9.2 `chain_latest`

`chain_latest` walks a key's chain from CURR to the version a
write checks against, into `checked_t`. It holds a read epoch.

1. `Get` CURR. If the key has none, return.
2. An own version decides: `own_seq` is set. It is never above
   `seq_hwm`, as a rollback puts back what it undid.
3. A committed version decides: `commit_ts` is set.
4. Another's version in flight stops the walk, `inflight` set,
   unless the caller passes `skip_inflight`, as `read_latest`
   does.
5. A dead version in CURR sets `head_dead`. A dead version in OLD
   goes on `passed`.
6. Follow `prev` into OLD. At 0, or at an id already gone, return
   with nothing found. Nothing older is anyone's.

`latest_version` is the same walk over a versioned key's run. An
own version above `seq_hwm` is skipped there, as a rollback undid
it.

### 9.3 `chain_write`'s put

§4.2 says what goes into the batch. With a dead head, one new ts
unlinks every entry on `passed`, and the version found in OLD has
its displaced stamp cleared. `count_write` counts every entry
under the row's key, as OLD keys carry no relid. The undo entry
has `chained` set. The old row moves to the pin last, by
`pin_checked`, which moves the buffer and makes the row's slice
anew, as a buffer that copied its value moves the bytes.

### 9.4 `chain_restore`

A rollback calls it with the mark's `seq_hwm`, and the Garbage
Collector's repair with 0. The caller holds the key's latch. A
version is to be undone if it is unstamped, its `xid` is the
given one, and its `seq` is above the given number.

1. `Get` CURR. Do nothing if its version is not one to undo:
   repaired already, or overwritten.
2. Walk `prev` past the versions to undo, and note each OLD entry
   reached as passed, the one kept too.
3. If a version is kept, stamp it if it can be, clear OLD's flags
   and `displaced`, and put it in CURR, its own `prev` unchanged.
4. If none is kept, delete the key from CURR: the key had no
   version before.
5. Unlink each entry passed with one new ts.
6. Write all of it in one batch.

### 9.5 The raw pin and the batch iterator

`rawpin_t` and `batch_iter_t` are in `sm1/src/session.h`.

- **`rawpin_t::it`** is the iterator that found a versioned row,
  **`got`** a chained row's `PinnableSlice`, and **`row`** the
  row's bytes in either.
- **`rawpin_t::reuse`** is what `reusable()` compares: the
  session, `storeid`, `write_gen`, `visible_ts` and `epoch`, set
  by `access_read` alone.
- **`read_row`** clears `row` and keeps `it`, for `access_read`
  to seek again, and empties the pin unless a row is found.
  `read_chained` empties the pin first.
- **`batch_iter_t`** holds its RocksDB iterator, or none, with
  `storeid`, `cf` and `session`, whose list it is on while it
  holds one.
- **`missed`** says `storeid`'s keys are too far apart to step
  to. **`scattered`** says the caller said so first, for every
  store.

### 9.6 The pool

`pin_t` is in `sm3/src/table/pin.h`. The session has `pins`,
every pin made, and three lists: `pins_free`, `pins_held` and
`pins_scan`. `raws_free` holds the raw pins that ended tree
cursors gave back, emptied.

| Call | What it does |
| --- | --- |
| `pin_take` | pops `pins_free`, or makes a pin with its raw pin; on no list yet |
| `pin_reread` | takes a live ref's held pin off its list, clears `row` and `reader`, increases `gen`, keeps the raw pin; the ref names none |
| `pin_hold` | sets `def` if it differs, `row`, an empty `reader`; links it on `pins_held`; points the ref at it |
| `pin_put` | a taken pin that holds nothing the caller keeps: empties the raw pin, links it on `pins_free`; no ref was given out, so `gen` stays |
| `pin_scan_hold` | makes a taken pin a cursor's, on `pins_scan` |
| `pin_scan_move` | sets `row`, resets `reader`, increases `gen` |
| `pin_release` | empties it, increases `gen`, moves it to `pins_free`, clears `scan` |
| `unpin` | `pin_release` if the ref is live and the pin no cursor's; the ref names none |
| `release_all_pins` | releases all of `pins_held`, and makes each pin on `pins_scan` stale |
| `pins_rolled_back` | makes stale the pin of each cursor sm1 closed |
| `pins_close` | frees every pin and raw pin, at `session_close` |

Why it is laid out so:

- **There is no flag for held.** A free pin's `gen` is newer than
  that of every ref ever taken on it, so the generation alone
  tells them apart.
- **`prev` and `next` serve every list**, as a pin is on one. The
  lists are doubly linked, so an unpin unlinks in O(1).
- **`session` points back to the pool**, so an unpin needs no
  session, and digres's slot can unpin with its ref alone.
- **`gen` starts at 1**, so a ref whose `gen` is 0 never matches
  a pin.
- **`sm3.h` declares `pin_t` only.** Its layout is sm3's.

### 9.7 `read_chained_batch`

1. Empty each pin, and set each `rc[i]` to 1.
2. Take the view, and check each key and the store.
3. Unless the store is temporary, take each key's SIREAD entry.
4. Enter a read epoch, held until every key is resolved.
5. Build the keys' current keys, end to end, in buffers the
   thread keeps.
6. Test whether the keys ascend. A batch of one does not. A
   scattered iterator, or a missed one of this store, counts as
   no.
7. If they ascend, take `iter`'s RocksDB iterator, refreshed, or
   make one in total order and keep it in `iter`.
8. Seek the first key. For each next key, step at most four
   times.
9. On a key found, copy its value into the key's pin (`PinSelf`).
   A key passed is `NotFound`.
10. On the first miss, stop stepping. If that is the second key,
    let `iter`'s iterator go and mark it missed.
11. Read the keys left by one `MultiGet` into an array the thread
    keeps, and move each value to its pin.
12. Resolve each key by `chain_resolve`, and empty the pin of a
    key not seen.

### 9.8 The C wrapper

- **`sm_pin_ref_t`** is `{void *pin; uint64_t gen;}`, two words,
  opaque to digres, zero for none. It has no create and no free.
- **`sm_unpin`, `sm_release_all_pins`** call sm3.
- **`sm_pin_deform(ref, from, n, values, isnull)`,
  `sm_pin_decode(ref, ordinal, val, isnull)`,
  `sm_pin_tuple(ref, tuple, len)`** read a pinned row.
- **`sm_rel_scannxt`** has a form that hands back the row's ref,
  not its bytes.
- **`sm_tbl_read`, `sm_tbl_read_latest`, `sm_tbl_update`,
  `sm_tbl_delete`** take a ref and hand back a view.
- **`sm_tbl_deform`, `sm_tbl_decode`** stay, for bytes a client
  holds itself.
- **The order of close** is `sm3::session_close` before
  `sm1::closedb`, as `src/sm.cc` does.

---

## 10. digres

### 10.1 System columns

- **There is no `ctid`.** `SelfItemPointerAttributeNumber` has no
  attribute, no slot has a `tts_tid`, and `slot_getsysattr`
  answers `tableoid` and `_id` alone. The C names
  `RownoAttributeNumber` and `tts_rowno` stay.

### 10.2 The row's name in a plan

- **The row identity columns are the key's columns.** Where the
  planner adds the junk column `ctid`
  (`add_row_identity_columns`, `appendinfo.c`), it adds a Var for
  each key column, named for the result relation. A keyless table
  adds `_id`. digres has no table inheritance, so a plan's rows
  are of one table.
- **Row marks** (`preptlist.c`) do the same when they lock, and
  `ExecAuxRowMark` keeps the attribute numbers of the key's
  columns. One that locks nothing copies the row, a RECORD
  whole-row Var: PostgreSQL's re-fetch fetched the same version
  again, which the copy is.
- **The scan reads the key's columns** because the plan asks for
  them, as any column.

### 10.3 The executor

- **The table AM takes a key.** `tuple_update`, `tuple_delete`,
  `tuple_lock` and `tuple_fetch_row_version` take a `RowKey`:
  `values`, `isnull`, `n` (`access/rowkey.h`). A moved row is
  `TM_Updated` with `SM_ERR_MOVED` raised.
- **ModifyTable** builds the `RowKey` from its junk columns, and
  passes it to `ExecUpdate`, `ExecDelete` and their re-fetches.
- **LockRows and EvalPlanQual** lock and fetch again by the row
  mark's key, copied into the row mark's memory.
- **Triggers**: `GetTupleForTrigger` reads by key. AFTER events
  keep their rows in a tuplestore.
- **`ON CONFLICT DO UPDATE`** takes the key from the row the
  arbiter returned, and locks and updates by it.
- **`WHERE CURRENT OF`** (`execCurrent.c`) returns the cursor's
  current row's key, from the scan's slot. A **key scan** reads
  one row by its key: the plan for CURRENT OF, and for `_id = n`
  or `_id = ANY (…)` in a keyless table. It stands where the TID
  scan stood. There is no TID range scan.
- **`scan_at()`** finds the scan whose current row has the key,
  and hands out its pinned row, so an UPDATE or DELETE of the row
  a scan just read reads nothing.

### 10.4 A slot holding a ref

- **`TTSOpsSmRow`** is a slot type of its own, in `smstore.c`: a
  virtual slot with the ref of the row stored into it. The
  executor compiles a fetch step (`EEOP_*_FETCHSOME`) before an
  expression's first column, and leaves it out for a virtual
  slot, so a lazy slot must be of another type.
  `smtable_slot_callbacks()` returns it, and the sequential,
  index and bitmap heap scans take it from
  `table_slot_callbacks()`.
- **`getsomeattrs(slot, n)`** calls
  `sm_pin_deform(ref, tts_nvalid, n, …)`.
- **Stored with no ref**, as an INSERT's values, COPY's, a
  projection's, a row's copy or an index-only scan's are, it is a
  virtual slot.
- **`materialize`** deforms every column, drops the ref, and
  copies the by-reference values into the slot's memory.
  `copyslot`, `copy_heap_tuple` and `copy_minimal_tuple` deform
  every column first. A slot kept past the row's pin, by
  EvalPlanQual, a trigger, a sort or a cursor held across
  statements, is copied so already.
- **`clear`** drops the ref, and does not unpin. The pin is the
  reader's: the cursor's, or the index or fetch scan's ref.
- **System columns** come from the slot itself.
- **Who reads the values directly** asks the slot for them first:
  an index scan checks each row against its keys (`keys_met()`),
  so it deforms up to each key's column.
- **A slot of another type** given a ref is filled whole at once,
  as a virtual tuple (`ref_into_slot()`).
- **A key scan and a DELETE's RETURNING** read a copy of the row
  by its key, and keep their column counts
  (`smstore_scan_natts()`, `smstore_expr_natts()`).
- **A stale ref is an error**, not a wrong answer: a node that
  kept a slot after its scan moved, with no copy, is found by the
  test that runs it.

### 10.5 Pins in a statement

- **A scan holds a ref.** An index scan, an index-only scan and a
  bitmap heap scan's fetch keep one `sm_pin_ref_t`, into which
  each row they read by key goes, and the scan's end unpins. The
  slot's values point into the row until the scan reads its next.
  The executor keeps to that already, as a scan's slot is
  overwritten at its next fetch.
- **Nothing on an error.** A scan freed by an error leaves its
  ref behind, and sm3 releases the pin. digres's `ereport` throws
  a C++ exception, but a slot is palloc'd memory, and a memory
  context is freed with no destructor run.
- **`keep_row()` keeps a ref, not a `memcpy`**, for the
  statement: the deleted row for RETURNING, and the newest
  version for a recheck.
- **All pins go at the end of a statement.**
  `smstore_statement_end()`, from `finish_xact_command()`, calls
  `sm_release_all_pins()`. That shortens a pin an error left
  behind from the transaction's life to the statement's.
- **The release is skipped while a portal is ready**
  (`ThereAreNoReadyPortals()`). A FETCH leaves its plan's slots
  holding rows the next FETCH may read: a nested loop's outer
  row, a merge join's marked one. The pins then wait for the
  transaction's end, or for the cursor's close and the next
  statement's end. The check also counts a suspended portal and a
  holdable cursor, so the release is sometimes skipped where it
  need not be, which is safe.
- **A holdable cursor** copied its rows out at its commit, so it
  holds no ref. In `CommitTransaction`, `PreCommit_Portals()`
  runs before `smstore_commit()`. At abort a holdable cursor of
  the aborted transaction is dropped.
- **`keep_row()`'s ref** lasts for the statement, and goes with
  the rest.
- **The statement is the top-level one, the client's.** A
  statement a function runs through SPI is not the end of
  anything the top-level plan holds.

### 10.6 Two scans read ahead

- **A secondary index's scan that reads rows**
  (`smstore_index_getnext()`, `next_read_row()`) reads the next
  entries from the index cursor into a buffer: each entry's `_id`
  and its row's key, copied, as the cursor moves on. It reads
  their rows in one `sm_tbl_read_keys()`, and hands them out in
  the entries' order, a row not seen passed over.
- **An IN list's probes on the whole primary key**
  (`open_probe()`, `next_probe()`) read the next probes' keys,
  already in order, in one call.
- **The batch starts at 1 and doubles to `read_batch_max`**, 8,
  so a `LIMIT 1` or an EXISTS reads no more than a row or two
  ahead. 8 is enough keys a call to share a data block's lookups
  among rows next to each other, and few enough that a scan
  stopped early has read little it did not use.
- **The row the scan is on** is the buffered row's: its key for
  `scan_key()` and WHERE CURRENT OF (`scan_at()`), its pin in the
  slot. The pins of the rows read ahead are the scan's, released
  when it ends, rescans, or the transaction does.
- **`ra_done` ends the read-ahead.** An array's probes, once the
  last ended, have their odometer back at its first value
  (`advance_probe()`). Asked past their end, they begin again,
  and a scan of an IN list on a text index never ended.
- **Left as they are**: an index-only scan, whose entries give
  their columns with no row read; a primary key's range scan, a
  cursor over rows already; a keyless table's reads by `_id`.

---

## 11. Cases

### 11.1 A chained store

Each case of the checklist. The isolation scenarios run on
versioned and chained stores both, and the rel tests on chained
tables. "As a versioned key" means the case touches only
versioned keys.

| Case | What happens | Tested by |
| --- | --- | --- |
| TOAST | as a versioned key. An update or delete erases the old chunks. A snapshot that reads an older row version in OLD sees them, as their erase committed after it. The chunks' tombstones and the OLD version become no one's at the same watermark | rel test_vchain §2, test_toast |
| non-unique index | as a versioned key: entries name the row by key. A hit visible to a snapshot reads the row at that snapshot, and the chain gives the version that holds the entry | rel test_vchain §1, test_index |
| unique index | as a versioned key: the check is on the entry's versions. `idx_find_latest` reads the row with `chain_read_latest` under a lock | rel test_index, test_build |
| primary key | the rows themselves: an equality is a `Get`. An insert over a live head is `SM_ERR_DUPKEY`, over another's uncommitted head it waits, over a committed tombstone it chains through it | scenarios b06, b12; sm3 test_cluster |
| expression, partial, INCLUDE, cut keys | as a versioned key | rel test_partial, test_recheck, test_index |
| index-only scan | as a versioned key: the entry's versions say what the reader sees | rel test_vchain §1, test_index |
| update A→B→A | rows: CURR A, then OLD B, then OLD A, each snapshot stopping at the one it sees. Index: `A‖1` erased at B and put again at A, as versions | scenario g01; rel test_vchain §1 |
| INCLUDE-only update | as a versioned key: the entry is erased and put again | rel test_vchain §1 |
| primary key change | the old key gets a moved tombstone, the new key a new chain (§2.5). An old snapshot reads the old key's chain | scenario f11 |
| delete, then insert again | the tombstone moves to OLD behind the new version. An older snapshot passes the new version and the tombstone to the row. The Garbage Collector drops a committed tombstone left in CURR, and the next insert then has `prev` 0 | scenarios a10, b07 |
| abort | nothing repaired. Readers pass the dead head by the chain, the next writer overwrites it, the Garbage Collector repairs the rest. Until then each read of the row hops and looks up a dead xid | scenarios a03, g02; rel test_vchain §3 |
| two writes by one transaction, then abort | both dead, and readers pass both. The repair walks to the first version not the transaction's | scenario g02; rel test_vchain §3 |
| rollback to a savepoint | the undo list's chained keys are put back under their latches, and the entries passed are unlinked. A cursor of the transaction, opened before, still reads them at its RocksDB snapshot | scenarios b25, b31, c29, f10; rel test_vchain §5 |
| undo list past its cap | with a savepoint set, the savepoints are lost. With none, nothing is recorded, and nothing needs to be | rel test_vchain §0, test_doomed |
| crash | an abort of every transaction in flight. The `WriteBatch` keeps CURR and OLD together. OLD ids drawn for writes the log lost are never seen | rel test_vchain §6 |
| prepared transaction across a restart | its versions are in flight: readers pass them, writers wait. Its rollback is an abort | rel test_2pc |
| two writers on one row | the second waits for the first's version, and first committer wins above READ COMMITTED | scenarios b01 to b36 |
| old REPEATABLE READ snapshot | follows the chain, a `Get` a version newer than it. A long snapshot over hot rows pays the most | scenarios a13, g01; rel test_vchain §1, §2, §4 |
| READ COMMITTED re-read | `chain_read_latest` under a lock | scenarios f01 to f11 |
| SERIALIZABLE | SIREAD on the row's key and the scan's range. A version passed over owes its writer an edge | scenarios c01 to c36, p01 to p10 |
| open cursor | reads CURR and OLD at one RocksDB snapshot, and its view keeps the watermark back, so nothing it can reach is dropped. Its own transaction's later versions are passed by their seq | scenarios a09, a14, c18; rel test_cursor |
| GC and the watermark | OLD goes when its displacer committed at or below it, an unlinked entry when its ts is. A reader that reaches a dropped id would have seen its displacer first | rel test_vchain §2, §4; sm1 test_chain |
| CF:txn sweep | the filter stamps a kept OLD version's writer and displacer, and CURR's committed versions, before the sweep deletes their records. A dead version whose record is gone still reads dead | sm1 test_chain |
| CREATE INDEX, pending | the backfill reads each row with `chain_read_latest` under a shared lock, and puts versioned entries | rel test_build |
| DROP INDEX | as a versioned key | rel test_index |
| TRUNCATE, ALTER rewrite | a new store, chained, and the old one dropped whole | rel test_truncate; sm3 test_table |
| temporary table | chained too. Its flag lives in its `store_t` | rel test_vchain §7, test_temp |
| keyless table | rows keyed by `_id`, put with `chain_insert_new`, which reads nothing | every rel test |
| bulk load | a chained stream puts CURR versions of the load's xid, `prev` 0. An abort leaves them dead, for the Garbage Collector to delete. A rollback to a savepoint deletes the store's keys, every class but pages | rel test_vchain §8; sm3 test_load |

### 11.2 A batch read

| Case | What happens |
| --- | --- |
| a row deleted or not yet committed at the view | `rc[i]` 1: passed over, as a single read passes it |
| a row updated since the view | resolved through OLD, one `get_old` at a time |
| this transaction's own writes, earlier statements | seen, as a single read sees them |
| this statement's own writes | not seen, read early or late alike |
| a row whose key moved | `rc[i]` as `rel_read` answers it |
| TOAST | a row's values out of line are read when its columns are |
| unique, non-unique, expression, partial, INCLUDE index | rows read by the keys their entries give: alike |
| cut keys | the entry gives the row's key whole |
| READ COMMITTED, a row the plan locks | read at the statement's view, then locked and checked by EvalPlanQual |
| SERIALIZABLE | a SIREAD entry for each key read, read ahead or not |
| an open cursor | its rows read at its view, its read-ahead pins its own until it closes; WHERE CURRENT OF names the row handed out |
| a savepoint rolled back, abort | pins released with the transaction's |
| temporary table | its store, no SIREAD; alike |
| DROP or TRUNCATE meanwhile | the table's lock keeps them out while the scan runs |
| LIMIT, EXISTS, a nested loop's inner scan | the batch starts at 1 and doubles: little read past what is used |

---

## 12. Rejected

### 12.1 Keys and stores

- **The relid in place of the class byte.** A table store's
  userkeys already open with the relid, so the comparator could
  take its rule from it, with OLD keys under `0xFFFE`, a byte
  shorter a key. Rejected for four reasons. sm1 would know sm2's
  numbering, and write OLD keys under a relid sm2 owns. Not every
  store has relids: the catalog, sm1's tests and the driver's
  `--chained` runs do not. It changes the format. The byte is not
  shown to matter: about 1 + k bytes a row with k indexes, and a
  scan's cost does not follow a row's bytes closely. To revisit,
  measure the q1 scan and `../bench.old/relq6` first, with a
  current row's prefix cut to 2 bytes. A smaller step keeps sm1
  apart from sm2: a store that is not chained could drop the
  byte, which saves it on the catalog only.
- **Three column families a store.** An earlier build split a
  store so, with unversioned index entries. Both went, for
  simplicity.
- **A padded tail on current keys.** The tail is most of a scan's
  cost (§3.6).
- **A frozen header of 1 byte**, a row every snapshot sees
  keeping its flags alone. Built and reverted: smbench's 500-byte
  rows got faster, the bench's `wide_fixed` slower (q1 165 to 258
  ms), and q6 and q7 did not move.
- **Unversioned index entries.** They need a stamp, a list, a
  recheck of every hit and a deferred drop: a step of its own.
- **Unversioned TOAST, and chaining the catalog store.** Neither
  is needed.
- **An abort's eager repair**: a rollback buffer, or a repair
  queue fed by readers. The Garbage Collector's repair is enough
  while aborts are rare.
- **Following a moved row.** sm3 once followed a row through its
  `_id` entry, under a shared lock, at READ COMMITTED. That went
  with the `_id` entry. PostgreSQL refuses to follow too.
- **A copy of `_id` in the value.** A keyless row's `_id` is in
  its key.

### 12.2 Reads

- **A blanket `total_order_seek` on a scan.** `auto_prefix_mode`
  keeps the bloom filter for a single-key range.
- **Seeking a cursor backward**, for probes out of key order.
  `last_examined` would have to become a high-water mark, with a
  flag for a cursor that once hit its end, or the predicate would
  silently shrink. Forward only costs the level above a sort of
  its probes, which a batched index join does anyway.
- **Registering a scan's SIREAD at `sm1::scanend` alone.** A
  write into the range while the cursor is open is not in its
  RocksDB snapshot, so the reader never steps past it, and the
  writer finds nothing to conflict with.
- **An `int* found` out-parameter.** "No row" is a return value.
- **A batched read of versioned keys** (§7.2).
- **Batching keys across a nested loop's inner index scans.**
  That is MySQL's batched key access, a change to the executor.
- **Batching an index-only scan's rows.** It reads a row only
  where its key does not give the columns back.

### 12.3 Pins

- **Copying.** Two copies of every row read by key.
- **A pointer to sm's bytes in a lazy slot.** It reads freed
  memory once the scan moves.
- **A pool in sm1.** sm1 knows bytes, not tuples.
- **A pin the caller owns, linked into the session.** sm would
  release it by writing into the caller's memory, which the
  caller may have freed: digres frees it on every error.
- **A ResourceOwner, or a memory context callback, in digres.**
  Either releases a lost pin early, but makes sm's correctness
  depend on its host's machinery. sm's own end of the transaction
  is enough.
- **One generation for each transaction.** It tells a ref of an
  earlier transaction from a current one, but not two refs of one
  transaction. It would work only if a pin stayed with its ref
  until the transaction ends. An UPDATE of a million rows, with a
  ref a row, would then hold a million pins at its commit.
- **Releasing at a savepoint the pins taken after its mark**, as
  cursors are. An outer cursor fetched inside the savepoint stays
  usable after the rollback, as PostgreSQL documents, and a
  nested loop or a merge join over it reads the row it held from
  one FETCH to the next.
- **Releasing a pin before its store drops.** A session could
  release only its own pins, not those another session holds on a
  store the Garbage Collector drops.
- **A whole-tuple check on a read.** A read checks what it reads
  (§6.8).
- **Block cache and compression work.** The other part of the Q1
  profile is a separate change.

---

## 13. Tests

The isolation scenarios are the specification of a chained key as
much as of a versioned one. `make scenarios` runs every script in
both modes (`driver --chained`), and diffs both transcripts
against the one expected transcript in `sm1/mvcc/good`.

| Layer | Where | What it proves |
| --- | --- | --- |
| mvcc | `sm1/mvcc`, both modes | the isolation rules of §4 and §5: every scenario of §11.1, c36 for a reused iterator, f11 for a moved row, g01 and g02 for chains, a19 for `read_latest` of an own version |
| sm1 | `sm1/test/test_chain.cc` | the layout, chained writes and cursors, rollback, GC's repair and harvest, the sweep, chained loads, batch reads against single reads, the kept iterator, scattered keys |
| sm1 | `test_pin.cc` | two rows pinned at once, a read into a pin, a raw pin past its transaction, a rollback, the old row, a dropped store under the address sanitizer, iterator reuse |
| sm1 | `test_access.cc`, `test_read_latest.cc`, `test_version.cc` | versioned writes and reads, the re-read exchange, the codec and comparator |
| sm2 | `sm2/test/test_rel.cc`, `test_cluster.cc`, `test_toast.cc`, `test_tuple.cc` | rel calls on raw pins, `SM_ERR_MOVED`, a tuple read in steps against one deformed whole |
| sm3 | `sm3/test/test_pin.cc` | generations, a ref stale after `scannxt`, resuming a deform, a stale ref refused, the end of a transaction, a rollback that closes a cursor |
| sm3 | `sm3/test/test_cluster.cc`, `test_load.cc`, `test_table.cc` | a keyed table, chained loads, rewrites |
| rel | `rel/test/test_vchain.rel` | each case of §11.1 the scenarios do not reach, its sections cited there; a chained table's bytes in memtables |
| rel | `rel/test/test_pkey.rel`, the verb `read-keys` | a batch against single reads over an update, a delete and another session's uncommitted insert |
| dg | `../dg/test/slt/batch_read.test` | PostgreSQL 18's results: a unique index in reverse key order over rows updated and deleted, past several batches; LIMIT and OFFSET; a cursor with WHERE CURRENT OF; an UPDATE whose rows the index finds; IN lists; a keyless table |
| dg | `../dg/test/slt/point_read.test` | the cursor and savepoint case of §6.11 |
| dg | `../dg/test/slt/in_list.test` | `ra_done` (§10.6): it hangs without it |

digres's slot tests cover `SELECT *` with a selective filter on a
wide table, a join's outer row kept while the inner scan runs, a
cursor across statements, DELETE … RETURNING, and EvalPlanQual.

The batch read tests were shown to fail in three ways: with the
rows handed out in sorted order and not the entries', with a row
not seen handed out, and with the read-ahead's pins released
before their rows are. Another session's uncommitted writes are
sm1's test's.

A dropped chained store's pinned row is not tested: `test_pin.cc`
drops a versioned one.

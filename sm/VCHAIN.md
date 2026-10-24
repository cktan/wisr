# VCHAIN — the newest version in place, the older ones chained

**Built** (branch `wip-vchain-1`): steps 1 to 3 of §11, the
class byte, chained rows in sm1, and every table's rows chained
by sm2 and sm3; each case of §10 is tested as its last column
says. **Not built**: step 4, the benchmark.

sm1 keeps every version of a
key as a key of its own, `userkey ‖ wts ‖ seq`, so a read seeks
an iterator to the newest version it may see, and a scan reads
12 bytes of version in every key. This file keeps a table row's
newest version under its plain key, and the older ones in a
chain behind it. **A store stays one column family.** Nothing
else changes: indexes, TOAST and the catalog keep today's
versioned keys.

Paths are relative to `sm/`; digres's are under `../dg/`. An
earlier build (branches `aborted-wip-vchain-1` to `-3`, and
`../VCHAIN-REPORT.md` there) used three column families a store
and unversioned index entries; both are dropped here, for
simplicity.

---

## 0. In brief

- **Every key in a store opens with a class byte**, sm1's own,
  which no caller sees: `0x00` versioned (today's keys), `0x01`
  current (a row's newest version, CURR), `0x02` old (a row's
  older versions, OLD).
- **The comparator** orders by class first, then today's order
  within `0x00`, bytewise within `0x01` and `0x02`.
- **A row's versions are a chain**: CURR points to the version
  before it in OLD, which points to the one before it.
- **A reader `Get`s CURR**, and follows the chain while it does
  not see the version.
- **Aborts repair nothing.** Readers pass a dead version by the
  chain; the next write of the key, or GC, removes it.
- **GC drops an OLD version** once no snapshot can reach it.

---

## 1. Why

`../bench/getseek`, RocksDB alone with sm1's options: a million
rows of 500 bytes, compacted, in the block cache.

| a scan, every row in key order | ns a row |
| --- | --- |
| today: versioned keys | 145–150 |
| plain keys padded with a 12-byte tail | 125–128 |
| plain keys | 83–86 |

| a read by key, cold | µs |
| --- | --- |
| today: a new iterator, a seek | 5.7 |
| `Get` of the plain key | 4.5 |

- **Scans gain a third or more**; Q1 to Q3, sequential scans,
  are where digres is furthest behind PostgreSQL (2.3 to 3.1
  times). The 12-byte tail is most of the cost, so it must
  leave CURR's keys, not be padded.
- **A point read gains a little**, about 4% of Q7.

---

## 2. The key space

- **Class byte first**: `00 ‖ userkey ‖ ~wts ‖ ~seq`,
  `01 ‖ userkey`, `02 ‖ be64(id)`. sm1 adds it when it builds a
  key and strips it when it hands one out.
- **Comparator `wisr.store.v2`**:

  ```
  compare(a, b):
      if a[0] != b[0]:  return a[0] - b[0]
      if a[0] == 0x00:  return version_order(a+1, b+1)  # today's
      return memcmp_order(a+1, b+1)                    # shorter first on a tie
  ```

- **Prefix extractor**: class `0x00`, the class byte and the
  userkey, as today; classes `0x01` and `0x02`, the whole key.
  One bloom filter on it, as today: a versioned read filters on
  its userkey, a `Get` on its whole key.
- **A scan of class `0x01` runs in total order**
  (`auto_prefix_mode`, as today's scans): in prefix mode, a
  range scan would skip files holding no key equal to its
  start.
- **A versioned key costs one byte more** than today; a current
  key, one byte instead of twelve.

---

## 3. A chained row

- **`sm1::mkstore` makes a store chained** or not. A chained
  store's one column family is named `chain.<storeid>` instead of
  `store.<storeid>`, so the flag is known at open from the CF
  list, with no read (`../QUESTION.md` Q1); a temporary store's
  lives in its `store_t`. sm3 chains every table's store,
  temporary ones too; sm2's row rel then uses the `chain_` calls
  for its rows. TOAST and index entries, in the same store, stay
  versioned.
- **CURR**: key `01 ‖ rowkey`; value `flags | xid or commit_ts |
  wts | seq | prev | tuple`, today's version header plus
  `prev`, the OLD id of the version before, `0` for none.
- **OLD**: key `02 ‖ be64(id)`; value CURR's, plus `displaced`,
  the xid of the writer that replaced it (its commit_ts once GC
  stamps it). Ids come from a counter in memory, started at
  open past the store's last key.
- **A delete** is a tombstone in CURR whose `prev` is the row.
- **OLD entries no chain reaches are unlinked, not deleted**:
  flagged, with the ts of the unlinking. A reader that read CURR
  just before may still be on its way to one; deleted, it would
  read as "no older version". GC drops it once the watermark
  passes that ts.

---

## 4. Reads

- **`Get` CURR**, check the version as today (`sm1/SM1_DESIGN.md`
  §6.2); not seen, `Get` `prev` from OLD and check again, until
  one is seen, `prev` is `0`, or the id is gone: no row.
- **A scan iterates class `0x01`** and follows the chain of a
  row whose newest version it does not see, at the cursor's
  RocksDB snapshot.
- **`read_latest`, locks and SSI** as today, on the row's key,
  walking the chain the same way.
- **Sizes**: a rel's size is its key range in its class; a
  chained table's adds the store's OLD, which holds its rows'
  older versions alone.

---

## 5. Writes

- **Under the key's latch**, walk the chain to the newest
  version not written by a dead transaction, and check it as
  today: conflicts, waits, `SM_ERR_DUPKEY`, `SM_ERR_NOTFOUND`,
  `SM_ERR_MOVED`, `SM_ERR_UPDATED`.
- **One WriteBatch**: CURR's version moves to OLD under a fresh
  id, `displaced` the writer; the new version goes to CURR with
  `prev` that id.
- **CURR's version dead**: it is overwritten instead; the
  version it hid gets `displaced` the writer anew, and the dead
  OLD entries passed are unlinked.
- **An update writes the old tuple again**, to OLD. A delete
  copies the row likewise.
- **A conflict that fails the transaction** lets go of the latch
  first.

---

## 6. Rollback and abort

- **A rollback to a savepoint must repair**: a version written
  under the savepoint is dead while its transaction lives, and
  no status lookup says so. sm1's undo list, kept while a
  savepoint is set as today, records each chained write's key;
  the rollback puts back, under each key's latch, the newest
  version at or below the mark's `seq_hwm` or not its own, and
  unlinks the entries passed. Overflow loses the savepoints, as
  today (`sm1/SM1_DESIGN.md` §6.5).
- **An abort repairs nothing.** Its versions are dead by the
  status lookup, as today: readers pass them by the chain, at a
  hop each; the next write of the key overwrites one; GC repairs
  the rest (§7). Aborts are rare.
- **A crash** is an abort: nothing to recover.

---

## 7. GC

The store's one compaction filter judges each key by class.

- **`0x00`**: as today.
- **`0x01`**: a version committed at or below the watermark is
  stamped; a tombstone so committed is dropped. A dead one is
  queued for the GC worker, which, under the key's latch, puts
  back the version before it and unlinks its entry.
- **`0x02`**: an unlinked entry goes once the watermark passes
  its ts; an entry whose displacer committed at or below the
  watermark goes. Otherwise it is kept, and stamped with its
  own writer's commit_ts and its displacer's, so the CF:txn
  sweep may delete their status records.

---

## 8. Bulk load

A load stream is declared chained or not; a chained one puts
CURR versions, `prev` `0`, under class `0x01`. One ingestion,
one CF, as today.

---

## 9. Not in this design

- **Unversioned index entries.** They need a stamp, a list, a
  recheck of every hit, and a deferred drop; a step of its own,
  once rows are measured.
- **An abort's eager repair**, a rollback buffer, a repair queue
  fed by readers. GC's repair is enough while aborts are rare.
- **Unversioned TOAST**, and chaining the catalog store.

---

## 10. The cases

Each case of the checklist, against this design, and what tests
it: the isolation scenarios run on versioned and chained stores
both (`sm1/mvcc/DRIVER.md` §7), the rel tests on chained tables.
"As today" means the case touches only versioned keys, whose
handling does not change.

| case | what happens | tested by |
| --- | --- | --- |
| TOAST | as today. An update or delete erases the old chunks as versioned keys; a snapshot reading an older row version in OLD sees them, as their erase committed after it. The chunks' tombstones and the OLD version become no one's at the same watermark. | rel test_vchain §2, test_toast |
| non-unique index | as today: versioned entries, naming the row by key. A hit visible to a snapshot reads the row at that snapshot, and the chain gives the version that holds the entry. | rel test_vchain §1, test_index |
| unique index | as today: the uniqueness check is on the entry's versions; `idx_find_latest` reads the row with `chain_read_latest` under a lock. | rel test_index, test_build |
| primary key | the rows themselves: an equality is a `Get`. An insert over a live head is `SM_ERR_DUPKEY`; over another's uncommitted head it waits; over a committed tombstone it chains through it. | scenarios b06, b12 (chained); sm3 test_cluster |
| expression, partial, INCLUDE, cut keys | as today: index entries unchanged. | rel test_partial, test_recheck, test_index |
| index-only scan | as today: the entry's versions say what the reader sees. | rel test_vchain §1 (INCLUDE), test_index |
| update A→B→A | rows: CURR A, then OLD B, then OLD A, each snapshot stopping at the one it sees. Index: `A‖1` erased at B and put again at A, as versions, as today. | scenario g01 (both modes); rel test_vchain §1 |
| INCLUDE-only update | as today: the entry is erased and put again, versioned. | rel test_vchain §1 (n changes) |
| primary key change | the old key gets a moved tombstone, the new key a new chain; a writer meeting the tombstone gets `SM_ERR_MOVED`; an old snapshot reads the old key's chain. | scenario f11 (both modes) |
| delete, then insert again | the tombstone moves to OLD behind the new version; an older snapshot passes the new version and the tombstone to the row. GC drops a committed tombstone left in CURR; the next insert then has `prev` `0`. | scenarios a10, b07 (both modes) |
| abort | nothing repaired. Readers pass the dead head by the chain; the next writer overwrites it; GC repairs the rest. The cost: until then, each read of the row hops and looks up a dead xid. | scenarios a03, g02 (both modes); rel test_vchain §3 |
| two writes by one transaction, then abort | both dead; readers pass both; GC's repair walks to the first version not the transaction's. | scenario g02; rel test_vchain §3 |
| rollback to a savepoint | the undo list's chained keys are put back under their latches; the entries passed are unlinked. A cursor of the transaction, opened before, still reads them at its RocksDB snapshot. | scenarios b25, b31, c29, f10 (both modes); rel test_vchain §5 |
| undo list past its cap | with a savepoint set, the savepoints are lost, as today; with none, nothing is recorded, and nothing needs to be. | rel test_vchain §0, test_doomed |
| crash | an abort of every transaction in flight; the WriteBatch keeps CURR and OLD together. OLD ids drawn for writes the log lost are never seen. | rel test_vchain §6 |
| prepared transaction across a restart | its versions are in flight: readers pass them, writers wait. Its rollback is an abort. | rel test_2pc |
| two writers on one row | the second waits for the first's version, under the latch, as today; first committer wins above READ COMMITTED. | scenarios b01–b36 (both modes) |
| old REPEATABLE READ snapshot | follows the chain, a `Get` a version newer than it: a long snapshot over hot rows pays the most. | scenarios a13, g01 (both modes); rel test_vchain §1, §2, §4 |
| READ COMMITTED re-read | `chain_read_latest` under a lock, as `read_latest` today. | scenarios f01–f11 (both modes) |
| SERIALIZABLE | SIREAD on the row's key and the scan's range; a version passed over owes its writer an edge, as today. | scenarios c01–c36, p01–p10 (both modes) |
| open cursor | reads CURR and OLD at one RocksDB snapshot, and its sm1 view keeps the watermark back, so nothing it can reach is dropped. Its own transaction's later versions are passed by their seq. | scenarios a09, a14, c18 (both modes); rel test_cursor |
| GC and the watermark | OLD goes when its displacer committed at or below it; an unlinked entry when its ts is; a reader that reaches a dropped id would have seen its displacer first. | rel test_vchain §2, §4; sm1 test_chain |
| CF:txn sweep | the filter stamps a kept OLD version's writer and displacer, and CURR's committed versions, before the sweep deletes their records; a dead version whose record is gone still reads dead. | sm1 test_chain |
| CREATE INDEX, pending | the backfill reads each row with `chain_read_latest` under a shared lock, and puts versioned entries, as today. | rel test_build |
| DROP INDEX | as today. | rel test_index |
| TRUNCATE, ALTER rewrite | a new store, chained; the old one dropped whole, as today. | rel test_truncate; sm3 test_table |
| temporary table | chained too; its flag lives in its `store_t`. | rel test_vchain §7, test_temp |
| keyless table (`_id`) | rows keyed by `_id`, put with `chain_insert_new`, which reads nothing. | every rel test |
| bulk load | a chained stream puts CURR versions of the load's xid, `prev` `0`. An abort leaves them dead, for GC to delete; a rollback to a savepoint deletes the store's keys, every class, as today. | sm3 test_load |
| the isolation scenarios | they call sm1's versioned calls alone (`mvcc/driver/run.cc`), so today they never touch a chained row. The driver gains a mode that makes its store chained and its verbs the `chain_` calls; every scenario must give its expected output unchanged in both modes. | `make scenarios`, both modes |
---

## 11. How it is built

1. **The class byte and `wisr.store.v2`**, every store, nothing
   chained. → every sm and dg suite; comparator tests, and a
   scan of `0x01` with the extractor set returns its whole
   range.
2. **Chained rows in sm1** (§3–§7), and the bulk load; the
   driver's chained mode. → sm1's suite; the 112 isolation
   scenarios unchanged, versioned and chained.
3. **sm2 and sm3 chain every table's rows.** → every suite.
4. **Benchmark against PostgreSQL.**

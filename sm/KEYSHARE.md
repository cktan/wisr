# KEYSHARE — the key-share row lock modes

Status: a design, for the user's review. §7 lists the questions.

---

## 0. What this is

A **parent** is the table a foreign key references; a **child**
is the table that holds the foreign key.

```sql
create table customer (id int8 primary key, name text, phone text);
create table orders (id int8 primary key,
                     customer_id int8 references customer (id));

-- session A: a new order, still open
begin;
insert into orders values (100, 7);  -- checks customer 7, and locks it

-- session B: a new phone for customer 7
update customer set phone = '555-0101' where id = 7;
-- digres: waits until A ends.  PostgreSQL: runs at once.
```

A's insert must keep customer 7's key until it commits, or the
order points at nothing. PostgreSQL's foreign key check locks the
parent row `FOR KEY SHARE`, which keeps its key alone; B's
update, which leaves the key as it is, takes `FOR NO KEY UPDATE`,
and the two do not conflict. digres maps `FOR KEY SHARE` to sm's
shared mode and every write to exclusive, so B waits. A parent
row that many open transactions reference, a customer or a
product, makes every update of it wait behind all of them.

## 1. PostgreSQL's four row modes

| Held \ asked | KEY SHARE | SHARE | NO KEY UPDATE | UPDATE |
| --- | --- | --- | --- | --- |
| KEY SHARE | ok | ok | ok | no |
| SHARE | ok | ok | no | no |
| NO KEY UPDATE | ok | no | no | no |
| UPDATE | no | no | no | no |

| Statement | Its mode on the row |
| --- | --- |
| a foreign key's check of the parent, `SELECT ... FOR KEY SHARE` | KEY SHARE |
| `SELECT ... FOR SHARE` | SHARE |
| an `UPDATE` that changes no key column, `FOR NO KEY UPDATE` | NO KEY UPDATE |
| an `UPDATE` that changes a key column, `DELETE`, `FOR UPDATE` | UPDATE |

**A key column** is a column of a unique index that a foreign key
may reference: unique, not partial, on columns, not expressions,
and not an `INCLUDE` column (`INDEX_ATTR_BITMAP_KEY`). A primary
key's columns are key columns.

**KEY SHARE passes over an update that kept the key**, one still
running or one committed since the snapshot, and locks the newer
version (`heap_lock_tuple()`, `heapam.c`: `TM_BeingModified` and
`TM_Updated` both reach the test of `HEAP_KEYS_UPDATED`).

## 2. digres and sm1 today

- **sm1's row locks are S and X** of Gray's five modes
  ([`LOCKMGR.md`](LOCKMGR.md#14-modes-and-conversion) §1.4).
  `smstore_lock_row()` maps KEY SHARE and SHARE to S, NO KEY
  UPDATE and UPDATE to X.
- **A write keeps no lock**: its uncommitted version is the lock
  (§1.2). Under the key's latch, `lock_write_check` waits for a
  held lock that conflicts with X, and a version in flight makes
  the writer wait for its transaction (§1.7).
- **`lock_row` waits for any version in flight** (§1.5 step 6).

So a foreign key check (S) and a non-key update (X) conflict both
ways: the update waits for the check's lock, and a check waits
for the update's version in flight.

## 3. The design

### 3.1 Two row modes

`sm_lockmode_t` (`include/sm_lock.h`) gains, after SIX, as its
values are persisted:

- `SM_LOCKMODE_KS = 5`: KEY SHARE.
- `SM_LOCKMODE_NKU = 6`: NO KEY UPDATE.

They are row modes: `sm1::lock_row` and its forms take them,
`sm1::lock_oid` refuses them (`SM_ERR_INVALID`). Among a row's
modes the conflicts are §1's, KS, S, NKU and X for its four. A
row lock and an oid lock never meet (§1.3), so KS and NKU never
meet IS, IX or SIX.

**A conversion** goes to the weakest mode covering both: KS below
S and NKU, and S with NKU is X.

### 3.2 A write says whether it kept the key

`sm1::update` and `sm1::chain_update` gain `bool key_kept`. With
it, the write's own mode is NKU, not X:

- `lock_write_check` waits only for a held lock that conflicts
  with NKU: S, NKU or X, not KS.
- The version is flagged `version_key_kept` (`0x08`, a value
  flag, in a versioned value and in a chained one, CURR's and the
  copy OLD keeps).

An insert, a delete, a move and an update that changes a key keep
X, and no flag.

### 3.3 KS and a version in flight

Under the latch, `sm1::lock_row` in KS passes over a version in
flight that is flagged `version_key_kept` and is no tombstone: it
takes the lock and does not wait. Any other version in flight is
waited for, as now. A later write of that key by the same writer
that changes the key, or deletes it, meets the KS lock under the
latch and waits.

### 3.4 KS and the currency check

Above READ COMMITTED, `sm1::lock_row` fails with
`SM_ERR_CONFLICT` when a version committed after the snapshot
(§1.6). In KS, only a version that changed the key fails it: one
not flagged, or a tombstone. A committed update that kept the key
is passed over, as PostgreSQL's KEY SHARE passes over it. The
check walks the key's versions newer than the snapshot: CURR,
then OLD by `prev`.

### 3.5 Who decides that a write kept the key

sm3's `tbl_update` decides: it has the old row and the new, and
it already compares them for each index's entries. The key kept
is no change in any key column (§1) of the table's unique
indexes: the primary key, and each unique index on columns, not
partial, whatever its state. A pending unique index counts: one
more X than needed is safe, one fewer is not. A table with no
unique index keeps its key on every update.

### 3.6 digres

`smstore_lock_row()` maps each mode to its own: KEY SHARE to KS,
SHARE to S, NO KEY UPDATE to NKU, UPDATE to X. A foreign key's
check, `SELECT ... FOR KEY SHARE` from `ri_triggers.c`, takes KS
with no change of its own. sm3's other shared row locks stay S:
the arbiter's read of the row it found, and an index build's
backfill, both of which must keep out a non-key update.

## 4. Cases

| Case | What happens |
| --- | --- |
| a child's insert, then a parent's update of a non-key column | no wait either way: KS and NKU |
| a child's insert, then a parent's update of its key, or delete | the parent's write waits for the child's transaction |
| a parent's non-key update in flight, then a child's insert | the check passes over the version (§3.3) |
| a parent's key update in flight, then a child's insert | the check waits, then sees the new key: the child's insert fails if the old key is gone |
| `FOR SHARE`, then a non-key update | waits, as in PostgreSQL |
| two children's inserts on one parent | KS and KS: no wait |
| an update of the key A to B and back to A | X: the key column changed in each write |
| an update of an `INCLUDE` column alone | NKU: not a key column |
| a primary key change | the row moves: a delete and an insert, X |
| a delete, then a re-insert of the key | X both |
| an abort, a savepoint's rollback | the version is dead or gone, as now; its flag goes with it |
| a crash, 2PC across a restart | the `PREPARED` record keeps each lock's mode, KS and NKU among them; the flag is on the version |
| an old REPEATABLE READ snapshot, a key-kept commit since | KS: no conflict (§3.4); S, NKU, X: `SM_ERR_CONFLICT`, as now |
| READ COMMITTED | KS, then the newest version, as now |
| SSI | unchanged: a lock is no SIREAD |
| the GC, the compaction filter, the `CF:txn` sweep | a stamp keeps the flags; the filter reads no new flag |
| a TOAST value in a key column | compared by value, as the index entry is made from it |
| a unique index pending, DROP INDEX | pending counts as a key (§3.5); a dropped one does not |
| `TRUNCATE`, an ALTER rewrite | a new store: no lock or version carries over |
| a temporary table | no row lock, as now (§1.5 step 2) |
| a keyless table, `_id` | no unique index: every update keeps its key |
| a bulk load, and its rollback | inserts: X, unflagged, as now |

## 5. Tests

- **mvcc scenarios**, new: KS and NKU do not wait for each other;
  KS waits for X; S waits for NKU; KS passes over a key-kept
  version in flight, and waits for a key change in flight; at
  REPEATABLE READ, KS after a key-kept commit succeeds, and after
  a key change fails; KS converts to NKU, then to X. The driver
  gains the modes `keyshare` and `nokeyupdate` for `lock`, and a
  write that keeps the key (`write <row> <value> keykept`).
- **sm3 unit tests**: `tbl_update` keeps the key, or not, for a
  primary key, a unique index, an `INCLUDE` column, a partial
  and an expression unique index, and a keyless table.
- **rel**: the modes through `sm.h`.
- **dg**: `server.sh` with held sessions: a parent's non-key
  update goes ahead beside an open child insert; its key update
  waits.
- **PostgreSQL's isolation specs**
  (`../dg/test/isolation_specs.txt`): ten are marked to fail for
  want of these modes, and are to pass: `delete-abort-savept-2`,
  `lock-update-traversal`, `multixact-no-forget`,
  `tuplelock-conflict`, `tuplelock-update`,
  `tuplelock-upgrade-no-deadlock`, `fk-contention`,
  `fk-deadlock`, `propagate-lock-delete` and
  `update-locked-tuple`. `deadlock-soft` and `deadlock-soft-2`
  stay failing: they need a wait queue to reorder, which sm1
  has none of.

## 6. Cost

- **A write computes key_kept**: a compare of the key columns'
  old and new values, which `tbl_update` already holds.
- **One flag bit** in a version's value.
- **The currency check in KS** may walk versions newer than the
  snapshot, where today it reads one. It runs only above READ
  COMMITTED, on a row a commit changed since the snapshot.

## 7. Questions for the user

1. **sm3 decides that a write kept the key** (§3.5), or the host
   says so? Recommended: sm3, which holds both rows and the
   indexes.
2. **A key column is PostgreSQL's** (§1): unique, not partial, on
   columns, not `INCLUDE`; a pending unique index counts.
   Recommended.
3. **KS passes over a committed key-kept update above READ
   COMMITTED** (§3.4), as PostgreSQL does. Recommended.
4. **The new modes are appended**, 5 and 6, to the persisted
   enum. Recommended; no database on disk is kept across the
   change.

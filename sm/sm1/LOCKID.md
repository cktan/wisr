# LOCKID — a lock is its id, not its key

A row lock in sm1 keeps its whole key: in the session's lock
list, in a prepared transaction's `PREPARED` record, and in
what `sm1::lock_status` reports. The shared lock table does
not: it keys an entry by two 64-bit words, the storeid and a
hash of the key. This file is the plan to keep only those
words, the lock's **id**, everywhere.

**Status: planned.** Nothing here is built. It is the first
part of `../NEXT.md` item 4, sm1's lock table, a hash table of
its own. It starts from the split of `sm1::lock` into
`sm1::lock_row` and `sm1::lock_oid`, each with its `try`
form, which made the oid lock's id exact and its name fixed.
Paths are relative to `sm1/`. `SM1_DESIGN.md` §5.1
and `../LOCK.md` §1 describe the lock table as it stands.

---

## 0. The proposal in brief

- **A lock is `(type, word1, word2)`.** For an oid,
  `(LOCKTYPE_OID, 0, oid)`, exact. For a row,
  `(LOCKTYPE_ROW, storeid, hash(key))`.
- **The mode is beside the id, not in it.** Every mode of one
  name must meet in one table entry, or S and X would never
  conflict.
- **The lock list holds ids.** It stays a blist
  (`lock_list_t`) of id, mode and flags; `lock_keys` goes.
- **The lock manager takes an id**: `lock(id, mode, held&)`.
  It adds a list entry only when the transaction did not hold
  the id already; a conversion raises the entry's mode, as it
  does now.
- **The public calls do not change.** `sm1::lock_row` still
  takes the store and key: it reads the key under its latch,
  for the uncommitted writer, `../LOCK.md` §1.1's check and
  `as_read`, then makes the id. `sm1::lock_oid` makes the id
  from the oid.

---

## 1. What a row lock keeps today

| Where | What | Why |
| --- | --- | --- |
| the lock table, `db->lock_table` | `lockid_t {storeid ^ type bit, lock_row_hash(key)}` and the holders | conflicts, decided on the hash |
| the session's lock list, `s->locks` | a `lockref_t` per lock: the id, the mode, `row`/`row_len` pointing into `s->lock_keys` | release, savepoint rollback, prepare |
| `s->lock_keys` | the key's bytes | `is_ref` and `exact_ref` tell apart two keys that hash alike |
| `holder_t::refs` | how many list entries share one table hold | two keys of one transaction that hash alike |
| the `PREPARED` record, `src/xct.cc` | `type ‖ be64(storeid) ‖ key` and the mode, per lock | `lock_restore` hashes the key again after a restart |
| `s->waiting_row` | the key a waiter asks for | `sm1::lock_status` of a waiter |
| `sm1::lock_status`, `src/session.cc` | the storeid and the key | `sm_lock_status` in `../src/sm.cc` reads the relid and the `_id` from the key, for `pg_locks` |

A transaction that already holds a lock and asks again adds no
entry today: `lock_try` finds its own and raises the mode. A
second entry, and `refs`, exist for keys that hash alike, and
for an OID lock's fast-path slot moved to the table after the
transaction's hold there was made (`fp_move`).

---

## 2. The design

### 2.1 The id

`lockid_t` keeps its two words and gains nothing: the type is
already its top bit. A row's second word is a hash of the key
that sm1 fixes itself (§2.4), not `std::hash`.

### 2.2 The lock list

`lockref_t` keeps `id`, `type`, `mode`, `latest`, `slot` and
`strong`, gains `kind` (§4.1), and loses `row` and `row_len`.
`lock_keys_t` and the `keys` half of `lock_mark_t` go. A
savepoint's mark is the list's mark alone.

### 2.3 The lock manager

`lock_try`, `lock_wait` and `lock_oid` take a `lockid_t`, not
`(type, storeid, key)`, once the keys go (§7 step 5): until then
they need the key for the list and for `waiting_row`.
`exact_ref` goes, and `is_ref` and `find_lockref` compare ids.
`holder_t::refs` goes: one id is one hold, made with a list
entry and released with it. The one second entry left is an OID
lock's slot moved into the transaction's hold; it raises the
hold's mode and is released alone, so a rollback past it keeps
the hold (`mvcc/in/h07`). `lock_write_check` and `lock_held`
make the id from the store and key they are given, as
`lock_row` does.

The id interface is the boundary `../NEXT.md` item 4 draws for
its module, `src/locktab.h`: a holder is an opaque owner, and
the module knows no key.

### 2.4 Prepare and recovery

A lock in the `PREPARED` record is 20 bytes: the type, the two
words, the mode and the kind (§4.1). `lock_restore` puts the id
back as it is, with no hash. So the row hash is persisted and
must give the same id in every build: `fmix64` (MurmurHash3's
finalizer) over the key's bytes, combined in a fixed order. The
test hook `lock_test_rows_collide` stays.

### 2.5 `lock_status`

`sm1::lock_status_t` reports the id's words in place of the
key. For a row, `sm_lock_status` finds the table by its
storeid, as now, but no longer the tuple, nor whether the key
was a row's or an index entry's (§4.1).

---

## 3. What it costs, case by case

| Case | Outcome |
| --- | --- |
| Two keys of one transaction hash alike | One lock, held in the stronger mode. Safe: another transaction's write of either meets it. A 64-bit hash makes it rare |
| `read_latest` of a key under a lock of another that hashes alike | Let through. Safe: no other writer of the key passes the shared id |
| A write converting its own lock | Converts the id's entry, as now; with keys alike, both keys' |
| Savepoint rollback | Unchanged: a lock first taken past the mark goes; a conversion is never undone |
| Abort, commit | Unchanged: the list is walked, each id's hold dropped |
| Prepare, crash, restart | Unchanged in effect; the record keeps ids, and the hash is fixed (§2.4) |
| The fast path, OID locks | Unchanged: keyed by id already, an oid's id exact |
| `FOR UPDATE` of a million rows with long primary keys | No key bytes kept, none copied under `lock_mu` |
| `pg_locks` | A row lock's table, but not its tuple (§4.1) |

---

## 4. Open decisions

### 4.1 Telling a row's lock from an index entry's

`../../dg/test/select_for_update_many.sql` counts a session's
locks with `locktype = 'tuple'`, which `sm_lock_status` decides
from the relid at the key's start: a table's store holds its
rows and its indexes' entries. Without the key it cannot.

**Decided (2026-10-31): the caller passes a kind.**
`sm1::lock_row` and `sm1::trylock_row` (and their `chain_`
forms) take a `uint16_t kind`, 0 by default, which sm1 keeps
beside the id and never reads: in the list entry, in the
`PREPARED` record, and in `lock_status_t` as `kind`. sm2
passes the key's relid. `sm_lock_status` reads the kind where
it read the key's first two bytes, and reports a row as
`SM_LOCKNAME_ROW` as now. What is lost is a row's `_id`: a
tuple is reported by its table, not by which row. Two keys of
one transaction that hash alike keep the kind of the first.

The options not taken:

- **The id keeps the relid**, in the first word's high bits
  beside the type, storeids being far below 2^47. sm1 then
  knows a relid, which is sm2's, not its own.
- **`pg_locks` reports a row lock as its table's, with no
  kind**, and the dg test counts by mode alone. The simplest,
  and `pg_locks` says less.

### 4.2 `held` above sm1

The table lookup tells the lock manager whether the
transaction holds an id. Nothing in sm2, sm3 or dg needs it,
so it stays inside sm1 unless a caller asks.

---

## 5. What has to be built

| Where | What |
| --- | --- |
| `src/db.h` | `lockref_t` without `row`/`row_len`; `lock_keys_t` and its mark gone; `holder_t` without `refs` |
| `src/lockmgr.cc` | `lockid` with `fmix64`; `lock_try`, `lock_wait`, `lock_oid` by id; `exact_ref` gone; `lock_put_name`, `lock_name_size`, `lock_from_name` as 20 bytes |
| `src/access.cc`, `src/chain.cc` | `lock_versioned` and `lock_chained` make the id and call by it |
| `../sm2/src/rel/rel.cc`, `index.cc`, `catkv.cc` | pass the key's relid as the row lock's `kind` |
| `src/xct.cc`, `src/recover.cc` | the record's lock set as ids |
| `src/session.cc`, `src/session.h` | `lock_status` from ids; `waiting_row` gone |
| `include/sm1.h`, `../src/sm.cc` | `lock_row`'s `kind`; `lock_status_t`'s key as the id's words, and its `kind`; `sm_lock_status` reads the kind (§4.1) |
| `SM1_DESIGN.md`, `API.md`, `../LOCK.md`, `../NEXT.md` | §5.1, §8.4 and §9.1; `lock_status`; §1's "Across a restart"; item 4's "lock ids are never persisted" |

---

## 6. How it is tested

- **Every scenario in `mvcc/` passes unchanged.** None reads a
  row lock's key; `e07` reads `lock_status` for OID locks only.
- **Tests that change:** `test/test_lock.cc`'s cases of keys
  that hash alike (`lock_test_rows_collide`), which now expect
  one lock where they expected two, and its `lock_status` case;
  `test/test_2pc.cc`'s check of a prepared transaction's locks,
  which `lock_status` lists by key after a restart.
- **New in `test/test_lock.cc`:** the row hash of a fixed key
  is a fixed value, so a build that changes it fails; a
  prepared lock set restored after a restart blocks the same
  writers it did before.
- **dg:** `select_for_update_many.sql` unchanged, its
  `tuple` counts from the kind (§4.1), and
  the whole suite.

---

## 7. Order of work

1. Decide §4.1. Done: the caller passes a kind.
2. `fmix64` as the row hash, still keeping the keys. Done:
   `lock_row_hash` in `src/lockmgr.cc`.
   → verify: the fixed-hash test; all of `mvcc/` and `test/`.
3. The lock manager by id, `refs` and `exact_ref` gone, the
   keys still in the list for the record and `lock_status`.
   Done: a hold goes with its first entry; the signatures by
   id wait for step 5 (§2.3).
   → verify: the collision tests expecting one lock; all
   suites.
4. The record and recovery by id.
   → verify: `test_2pc.cc` across a restart.
5. `lock_status` and `sm_lock_status` by id, then `lock_keys`
   and `waiting_row` dropped.
   → verify: the full sm, sq and dg suites.
6. The documents of §5.

Then the rest of `../NEXT.md` item 4: the module of its own,
its benchmark, the partitions.

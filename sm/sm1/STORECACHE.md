# STORECACHE — a session's handles of the stores it reads

Status: built (`../../dg/WORKNOTE.md` step 316).

## 1. What

A read of one row by key, `sm1::read` of a chained store, asks
the database for the store's handle twice: `chained_store()`
for the store, and `catalog_lookup()` for whether it is a
temporary one. Each asks `db_store()` (`src/db.cc`), which
takes `store_mu` shared and copies a `store_t`, whose three
`shared_ptr`s (`mem`, `files`, `next_old`) each count up as
the copy is made and down as it goes. Every session's read of
a store touches the same counters and the same lock word.

With 16 sessions reading by key (`../../bench/DESIGN.md` §5,
Q8), those atomics took 37% of digres's samples, and the
server ran 4 of the machine's 12 cores: a read waits on a
cache line, not on work. Reads alone scaled 10 times from one
session to 16, where PostgreSQL's scaled 17.6 times.

## 2. Design

- **The database counts changes to its store map**:
  `db_t::stores_version`, a `std::atomic<uint64_t>`, raised
  by one, with release order, by every change to
  `db_t::stores`: a store made (`db_create_store_cf()`), and
  a store dropped (`db_drop_store_cf()`), before its handle
  is destroyed. A reader loads it with acquire order; it
  takes no lock to.
- **A session keeps the handles it read**:
  `session_t::store_handles`, a map of storeid to a `store_t`
  and the version it was read at. `session_store(s,
  storeid)` returns a reference to the session's entry when
  its version is the database's now; else it reads the
  version, then the entry under `store_mu` shared, as
  `db_store()` does, and keeps both. A store not there is a
  `store_t` with no CF, as now.
- **No copy on the read path**: `chained_store()` and its
  callers in `src/chain.cc`, `catalog_lookup()`, and
  `store_of()` hold the handle by `const store_t &`. A copy
  of a cached entry would count up the same shared counters.
- **An entry lives as long as its session's map holds it**:
  an entry is refreshed in place, never erased while the
  session runs a call, so a reference a call holds stays
  good across a nested lookup; an entry another lookup
  refreshes is the same store's. At a transaction's end the
  entries of an older version are erased, so a session that
  made and dropped many temporary tables keeps none of them.
- **A dropped store is no more dangerous than now**: today a
  reader's copy of `store_t` keeps the raw `cf` pointer past
  `store_mu`, and what keeps a store from being dropped under
  a reader -- its table's lock, its catalog row read at the
  reader's view (`catalog_lookup()`) -- keeps it so with a
  cached entry. The version's raise before the handle's
  destruction means a lookup after the drop finds it gone.

## 3. Cases

| Case | What happens |
| --- | --- |
| store made in a transaction (CREATE TABLE, TRUNCATE, an ALTER's rewrite) | the version is raised; the session's next lookup reads the new store |
| store dropped at commit, or a made store dropped at abort or a savepoint's rollback | the version is raised before the handle goes; a lookup after finds no CF |
| temporary store | the same; its owner is in its `store_t`, as now |
| prepared transaction, a restart | the prepared object is a session of its own, its map empty; nothing is kept on disk |
| GC, recovery, the CF:txn sweep | read `db_t::stores` under `store_mu`, as now |
| bulk load, statistics, `chain_approxsize` | use `db_store()` as now, or `session_store()` where they have a session; one lookup a call |
| two sessions | each its own map; they share only the version, which they load |

## 4. Tests

- sm1 unit test (`test/test_chain.cc`): session A reads a
  store; B drops it and GC drops its CF: A's handle has none.
  B makes another: A reads it, and at the end of A's
  transaction its handle of the dropped store is gone; a
  store made and aborted, and one rolled back to a savepoint,
  likewise. With the version not raised on a drop, A's
  handle keeps the dropped CF; with no erase at a
  transaction's end, the old handles stay.
- `rel` (`../rel/test/test_catalog.rel` §9): a table one
  session read, dropped and made again by another, then
  truncated, reads as the new one. A table made again is a
  new storeid, so this cannot see a version not raised; the
  unit test does.
- dg's `make tsan`: its eight-session runs and `server.sh`,
  sessions making and dropping temporary tables while others
  read.
- Measured: Q8 and Q7 at 16 clients, before and after
  (`../../dg/WORKNOTE.md` step 316).

## 5. Left out

- **`db_read_enter()`** (1.5%) and RocksDB's own atomics in
  a read are left as they are, to measure after.

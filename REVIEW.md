# Code Reviews

The findings of code reviews that are still open. A finding
leaves this file when it is fixed, or decided not to be; git
history keeps the closed reviews (`REVIEW.md` up to
`1a8407e2`).

1. **`aa/`**: open; finding 1 fixed, the rest not yet checked.
2. **`sm/`**: open, not yet checked against the code.
3. **Cleanups not taken** from the ALTER COLUMN TYPE reviews.

# Review: `aa/` (Authentication and Authorization)

**Status: open.** Finding 1, the lockout bypass, is fixed;
the others are not yet checked against the code.

Reviewed `aa/include/`, `aa/src/`, and `aa/test/` against
`CLAUDE.md`, `sm/CODE.md`, `aa/AA_DESIGN.md`, `aa/ROLE.md`, and
`aa/USAGE.md`.

---

### 1. Summary

| Category | Status | Notes |
| --- | --- | --- |
| Public header opacity | PASS | `aa_env_t` and `aa_session_t` are opaque structs in `include/aa.h`. |
| No `throw`, `catch`, or RTTI | PASS | Zero `throw`/`try`/`catch` in library code; builds with `-fno-rtti`. |
| Naming (`snake_case`, `_t`, `m_`) & Yoda conditions | PASS | Types end in `_t`, private fields in `home_t` use `m_`, and comparisons use `0 == x`. |
| SCRAM-SHA-256 & cycle detection | PASS | Constant-time digest comparison (`timingsafe_eq`), salt/iteration checks, and DFS cycle detection in `grant_role`. |
| Correctness & concurrency | 2 High, 4 Medium | Partial-epoch invalidation loss, and `env->objects` / `pg_global` consistency races. |
| `sm/CODE.md` & style rules | 5 Low | `std::map`/`std::pair` in `aa.h`, separate error state, duplicate `session_guard_t`. |

---

### 2. Correctness, Security, and Concurrency Findings

#### [HIGH] 2. Partial-epoch eviction in `env->changed` drops cache invalidations (`aa_objects_moved`)

- **Location:** `aa/src/aa.cc:546-557`, `aa/src/aa.cc:630-635`
- **Issue:** `publish_objects()` pushes one `{epoch, key}` entry
  per modified object (all sharing the same `epoch`) and trims
  `env->changed` to `CHANGED_KEPT` (4,096) via `pop_front()`. In
  `aa_objects_moved()`, overflow is tested with
  `env->changed.front().first > s->objects_epoch + 1` (line 631):
  1. If a single transaction at `epoch = s->objects_epoch + 1`
     modifies more than 4,096 objects (for example, `REASSIGN
     OWNED` or `DROP OWNED`), the first `N - 4096` objects are
     evicted, yet `env->changed.front().first` still equals
     `s->objects_epoch + 1`.
  2. If `epoch = s->objects_epoch + 1` modified `k >= 2` objects
     and later epochs evict `1 .. k-1` of those entries while at
     least one entry with `epoch == s->objects_epoch + 1` remains
     at `front()`, the condition also evaluates to `false`.
  In both cases, `*all` is `false` and the evicted object OIDs are
  omitted from `*oids`, leaving stale relation cache entries in
  other sessions.
- **Fix:** Group each commit's changed keys into a single entry
  per epoch (or record `evicted_epoch` whenever `pop_front()`
  evicts an entry and set `*all = true` when `evicted_epoch >
  s->objects_epoch`).

#### [HIGH] 3. In-memory `env->objects` snapshot can diverge from `pg_global` on disk

- **Location:** `aa/src/aa.cc:472-506`, `aa/src/aa.cc:539-560`,
  `aa/src/aa.cc:571-598`
- **Issue:**
  1. **Reorder race between `write_objects()` and
     `publish_objects()`:** In `aa_post_commit()`,
     `write_objects()` commits to `pg_global` without holding
     `env->mutex`, and `publish_objects()` then acquires
     `env->mutex` and applies only this session's delta (`last`,
     `dropped_dbs`) to `*env->objects`. If two sessions update
     the same object's ACL/owner and commit to `pg_global` in
     order `T1 -> T2`, but acquire `env->mutex` in
     `publish_objects()` in order `T2 -> T1`, `env->objects` in
     memory keeps `T1`'s state while `pg_global` on disk holds
     `T2`'s state until restart.
  2. **Intermediate restrictive entries (`before`) are never
     published to `env->objects`:** `aa_commit()` writes `before`
     (`i.middle`) only to `pg_global` on disk (line 472) and does
     not update `env->objects`. Between `aa_commit()` and
     `aa_post_commit()`—or permanently if `aa_post_commit()`
     fails—other sessions continue reading the old, more
     permissive privileges from `env->objects` in memory,
     contrary to `aa/AA_DESIGN.md` §5.
- **Fix:** Publish `before` to `env->objects` in `aa_commit()`,
  and reload or serialize `env->objects` updates in commit order
  under `env->mutex`.

#### [MEDIUM] 4. `intermediates()` skips objects with no prior `pg_object` row when revoking default `PUBLIC` privileges

- **Location:** `aa/src/aa.cc:372-374`
- **Issue:** `intermediates()` skips any key where `all->end() ==
  found`. A database with no entry in `pg_object` (such as
  `postgres` after `initdb`) grants `AA_CONNECT | AA_CREATE_TEMP`
  to `AA_PUBLIC` by default (`aa_acl_default`). When a transaction
  revokes `CONNECT` from `PUBLIC` on that database, `all->end() ==
  found` causes `aa_commit()` to write no restrictive record
  before the database commit. If a crash or `aa_post_commit()`
  failure occurs, the database still has no row in `pg_object` and
  continues granting `CONNECT` to `PUBLIC`.
- **Fix:** When `all->end() == found` for an object whose class
  has non-empty default `PUBLIC` privileges (`AA_CLASS_DATABASE`,
  `AA_CLASS_FUNCTION`) and `i.after` is present, synthesize `old`
  using `aa_acl_default` before intersecting with `*i.after`.

#### [MEDIUM] 5. `aa_commit()` unconditionally sets `s->epoch = ++s->env->epoch`, masking prior role commits

- **Location:** `aa/src/aa.cc:505`
- **Issue:** If another session committed role changes earlier
  (moving `s->env->epoch` from `E0` to `E1` while this session's
  `s->epoch` remained `E0`), and this session then commits a role
  change, `s->epoch = ++s->env->epoch` sets both to `E2`. A
  subsequent `aa_roles_moved(s)` call returns `false`, so the
  caller never invalidates its caches for `E1`'s role changes.
- **Fix:** Only set `s->epoch = s->env->epoch` when `s->epoch` was
  already equal to the previous `s->env->epoch`.

#### [MEDIUM] 6. Post-commit `aa_store_read_roles` failure in `aa_commit()` leaves `env->roles` stale after disk commit

- **Location:** `aa/src/aa.cc:476-506`
- **Issue:** In `aa_commit()`, `sm_commit(session)` commits the
  role changes on disk at line 476, and `aa_store_read_roles` is
  called afterward at line 502 to rebuild `next`. If
  `aa_store_read_roles` fails, `aa_commit()` returns `-1` after
  the `pg_global` transaction has already committed on disk and
  leaves `s->env->roles` stale in memory.
- **Fix:** Construct `next` from `s->working` before
  `sm_commit(session)` (or read back before committing `session`)
  so no fallible read occurs after the commit point.

#### [MEDIUM] 7. Temporary object owners and ACLs are not rolled back on `aa_abort()` / `aa_subabort()`

- **Location:** `aa/src/aa.cc:242-279`, `aa/src/aa.cc:328-332`,
  `aa/src/aa.cc:514-532`
- **Issue:** `aa_set_temp_owner()`, `aa_set_temp_acl()`, and
  `aa_forget_temp()` mutate `s->temp_owners` and `s->temp_acls`
  in place without savepoint or transaction undo state. Rolling
  back a transaction or savepoint after `GRANT`, `REVOKE`,
  `ALTER ... OWNER TO`, or `DROP` on a temporary table leaves the
  in-memory temporary owner and ACL changes in effect.
- **Fix:** Track temporary object owner/ACL changes in
  `s->changes` (or a parallel subtransaction-tagged log) so
  `aa_abort()` and `aa_subabort()` restore prior entries.

---

### 3. `sm/CODE.md` and `CLAUDE.md` Rule Findings

- **[LOW] `std::map` and `std::pair` in `aa/include/aa.h`
  (`sm/CODE.md` §2):** `aa/include/aa.h:20, 23, 111, 114, 200`
  exposes `std::pair` (`aa_key_t`) and `std::map`
  (`aa_objects_t`), whereas `sm/CODE.md` §2 restricts public
  headers to `std::string`, `std::string_view`, `std::vector`, and
  `std::shared_ptr` (note that `aa/README.md:110-111` explicitly
  documents `std::map` and `std::pair` for `aa.h`).
- **[LOW] Separate error enum (`sm/CODE.md` §4):**
  `aa/include/aa.h:42-53` and `aa/src/aa_error.cc` define a
  standalone `aa_errnum_t` and thread-local error state rather
  than using `sm_error.h` (as designed in `aa/AA_DESIGN.md` §9).
- **[LOW] Duplicate `struct session_guard_t` definition:**
  `aa/src/aa.cc:36-39` and `aa/src/aa_store.cc:69-72` both define
  `struct session_guard_t` outside an anonymous namespace. Move it
  to `aa/src/aa_internal.h` or wrap each in `namespace {}` to
  avoid an ODR hazard.
- **[LOW] `aa/Makefile` and `aa/test/home.h` (`CLAUDE.md` §1):**
  - `aa/Makefile:53` sets `FORMAT_FILES := $(wildcard src/*.cc
    src/*.h test/*.cc include/*.h)`, omitting `test/*.h`.
  - `aa/test/home.h:24` uses
    `std::filesystem::temp_directory_path()`, which defaults to
    `/tmp` when `TMPDIR` is unset.
- **[LOW] Minor comment and test step formatting (`CLAUDE.md`):**
  - `aa/src/aa.cc:84, 542, 627`: `std::lock_guard<std::mutex>
    lock(env->mutex);` calls lack a brief comment stating why the
    lock is acquired.
  - `aa/test/test_roles.cc:53` and `aa/test/test_objects.cc:217`:
    combine two step numbers on one comment line (`// 2. 3.`).

# Review: `sm/` (Storage Manager Stack)

**Status: open.** No finding is fixed yet, and none has been
checked against the code.

Reviewed `sm/sm0/`, `sm/sm1/`, `sm/sm2/`, `sm/sm3/`,
`sm/include/`, `sm/src/`, and `sm/rel/` against `CLAUDE.md`,
`sm/CODE.md`, `sm/DESIGN.md`, and each level's design documents.

---

### 1. Summary

| Level / Area | Layering & Opacity | Naming (`_t`, `m_`, no `strncpy`) | Findings |
| --- | --- | --- | --- |
| `sm0/` | PASS (no RocksDB, depends only on `sm_error.h`) | PASS | 1 High, 3 Medium, 3 Low |
| `sm1/` | PASS (`sm1.h` opaque, sole RocksDB channel) | PASS | 3 High, 4 Medium, 3 Low |
| `sm2/` | PASS (`sm2.h` opaque, no RocksDB handles) | PASS | 1 High, 2 Medium, 3 Low |
| `sm3/` & `rel/` | PASS (`sm3.h` opaque, no RocksDB handles) | PASS (minor `using` aliases) | 3 High, 3 Medium, 5 Low |
| `include/` & `src/` | FAIL (`std::optional` in `sm_stats.h`) | PASS | 1 High, 2 Medium, 3 Low |

---

### 2. High-Severity Findings

#### [HIGH] 1. `sm0`: Missing RAII unregistration in `sm_wait()` leaves dangling stack pointers on exception

- **Location:** `sm/sm0/src/interrupt.cc:170-204`
- **Issue:** `sm_wait()` stores pointers to the caller's
  stack-allocated `cv` and `lock->mutex()` in `t->waiter_cv` and
  `t->waiter_mu` (lines 173-175), loops calling `done(arg)` and
  `cv->wait(...)`, and clears `t->waiter_cv` and `t->waiter_mu`
  only at lines 199-203 without an RAII guard. If `done(arg)` or
  `cv->wait` throws an exception (`sm/CODE.md` §5 notes that
  exceptions may unwind frames), `t->waiter_cv` and
  `t->waiter_mu` remain non-null dangling pointers. Any later
  `sm_interrupt(t, ...)` from another thread dereferences those
  destroyed stack objects (Use-After-Free).
- **Fix:** Register a `defer` guard immediately after setting
  `t->waiter_cv` and `t->waiter_mu` to clear both under `t->mu`.

#### [HIGH] 2. `sm1`: Use-After-Free on `rocksdb::ColumnFamilyHandle*` (`st.cf`) across GC, stats, and rollback/abort

- **Location:** `sm/sm1/src/db.cc:1036-1042`,
  `sm/sm1/src/db.cc:1111-1139`, `sm/sm1/src/gc.cc:334-343`,
  `sm/sm1/src/gc.cc:386-414`, `sm/sm1/src/chain.cc:1135-1142`,
  `sm/sm1/src/stats.cc:41-50`, `sm/sm1/src/page.cc:305-327`
- **Issue:** `sm1/SM1_DESIGN.md` §3.1 states that
  `ColumnFamilyHandle*` is reference-counted and destroyed only
  after the last user releases it. In the code, `store_t`
  (`sm/sm1/src/db.h:92-104`) holds a raw
  `rocksdb::ColumnFamilyHandle *cf`. `db_store()` copies
  `store_t` under shared `db->store_mu` and immediately releases
  `store_mu`. Meanwhile, `db_drop_store_cf()` acquires `store_mu`
  exclusively, drops the column family, and immediately destroys
  `st.cf` via `DestroyColumnFamilyHandle(st.cf)`.
  `db_drop_store_cf` is called without `gc_run_mu` and without
  waiting for `db_watermark` in `drop_created()`
  (`xct.cc:253`), `rollback()` (`xct.cc:590`), `mkstore()`
  (`catalog.cc:92`), `drop_temp_dropped()` (`xct.cc:267`), and
  `sm1::closedb()` (`session.cc:172`). Concurrent threads that
  copied `st.cf` (such as `sweep_txn_locked`, `purge_one`,
  `chain_repair`, `approxsize`, `probes`, `compact`, and
  `page_drop`) use `st.cf` outside `store_mu` after it is freed.
  In addition, `sweep_txn_locked()` snapshots all `db->stores`
  including sessions' temporary stores (`st.owner != nullptr`)
  and calls `CompactRange` on them.
- **Fix:** Manage `ColumnFamilyHandle` lifetime via
  `std::shared_ptr` (or keep a pin/epoch across every `db_store`
  user) and skip `st.owner != nullptr` in `sweep_txn_locked()`.

#### [HIGH] 3. `sm1`: Chained store MVCC and `CF:txn` sweep race leaves committed versions unstamped

- **Location:** `sm/sm1/src/chain.cc:409-447`,
  `sm/sm1/src/chain.cc:1083-1115`, `sm/sm1/src/gc.cc:222-224`,
  `sm/sm1/src/gc.cc:374-448`
- **Issue:** `sweep_txn_locked()` records `w0 = db_watermark(db)`,
  runs `CompactRange` on each store column family to stamp all
  committed versions with `commit_ts <= w0`, and then deletes all
  `COMMITTED` records with `commit_ts <= w0` from `CF:txn`.
  Unlike versioned stores, chained stores move and rewrite
  existing versions between `CURR` (`0x02`) and `OLD` (`0x03`)
  into the active memtable during normal writes and rollbacks:
  1. In `chain_write()` (`chain.cc:409-415`, `438-443`), when a
     committed unstamped version (`commit_ts <= w0`) is moved
     from `CURR` to `OLD` (or updated in `OLD`) after
     `CompactRange`'s memtable flush, it is written to the new
     memtable without setting `version_stamped` and `commit_ts`.
  2. In `chain_restore()` (`chain.cc:1085-1088`, `1104-1114`),
     restored `CURR` and `OLD` entries are written without
     stamping committed versions.
  3. In `gc_filter_t::filter_old()` (`gc.cc:222-224`), when
     `v.flags & chain_unlinked` and `v.displaced > m_watermark`,
     `filter_old` returns `false` before calling `stamp_own()`,
     leaving `v`'s committed writer (`commit_ts <= w0`)
     unstamped.
  Once `CF:txn` deletes `xid` (`commit_ts <= w0`) and `xid`
  leaves `commit_cache`, `xct_status` treats those committed
  versions as `XCT_DEAD` (aborted).
- **Fix:** Stamp resolved committed versions when writing them in
  `chain_write()` and `chain_restore()`, and call `stamp_own()`
  before returning `false` in `gc_filter_t::filter_old()`.

#### [HIGH] 4. `sm1`: Savepoint rollback does not unpin `s->readpin`, leaving a live iterator on dropped column families

- **Location:** `sm/sm1/src/xct.cc:571-593`
- **Issue:** Unlike `discard()` (`xct.cc:299`) and `commit`
  (`xct.cc:1182`), `rollback()` closes `s->scans` and releases
  `batch_iters`, but never calls `sm1::unpin(&s->readpin)` (or
  increments `s->write_gen`). If a transaction creates a store
  after a savepoint, reads from it into `s->readpin`, and rolls
  back to the savepoint, `rollback()` calls `db_drop_store_cf` at
  line 590 and destroys `st.cf` while `s->readpin.it` still holds
  an active `rocksdb::Iterator` on that column family.
- **Fix:** Call `sm1::unpin(&s->readpin)` and increment
  `s->write_gen` in `rollback()` before dropping created stores.

#### [HIGH] 5. `sm2`: `rel->held` retains uncommitted TOAST pointers when `update_row(..., hold=true)` fails after `settle_old`

- **Location:** `sm/sm2/src/rel/rel.cc:328-336`,
  `sm/sm2/src/rel/rel.cc:531-536`
- **Issue:** When `hold == true`, `settle_old()` appends the old
  row's TOAST pointers into `rel->held` before `update_row()`
  calls `write_moved(rel, row)` at line 534 (and before later
  iterations of `settle_old`'s validation loop finish). If
  `write_moved` or a later validation fails, `update_row` returns
  `-1` while the old row's TOAST pointers remain in `rel->held`.
  If the caller rolls back to a savepoint and later calls
  `rel_release(rel)`, `toast_erase` deletes TOAST chunks still
  referenced by the live row.
- **Fix:** Run `write_moved(rel, row)` before `settle_old()`, and
  validate all pointers in `settle_old()` before appending to
  `rel->held`.

#### [HIGH] 6. `sm3`: `tbl_insert_arbiter` inserts duplicate entries into unique B+tree arbiter indexes

- **Location:** `sm/sm3/src/table/table.cc:786-794`,
  `sm/sm3/src/table/table.cc:835-857`,
  `sm/sm3/src/table/table.cc:863-874`
- **Issue:** At lines 786-794, unique B+tree arbiter indexes
  (`table->def->indexes[i].tree`) are moved out of `arb` into
  `trees`. At lines 835-857, `tbl_insert_arbiter` inserts into
  each index in `trees` via `add_entry()`. On the success path,
  `trees` is never merged back into `arb` (merging happens only
  on the `goto look` retry paths). Then the loop over remaining
  indexes at lines 863-874 checks only `std::find(arb.begin(),
  arb.end(), i) != arb.end()`, misses every index in `trees`, and
  calls `add_entry()` a second time on each B+tree arbiter index.
  Because `unique_tree_insert` (`table.cc:1859`) treats an
  existing entry with the same row key (`r == row`) as
  non-conflicting and falls through to `sm2::idx_insert_unique`,
  a duplicate entry is inserted into the B+tree.
- **Fix:** Skip indexes present in `trees` (or merge `trees` back
  into `arb` before line 863).

#### [HIGH] 7. `sm3`: `builder_detach()` uninterruptible wait and worker-thread join race

- **Location:** `sm/sm3/src/builder/builder.cc:253-258`,
  `sm/sm3/src/builder/builder.cc:339-349`
- **Issue:**
  1. Line 339 (`b.cv.wait(lock, [&] { return
     b.catalogs.at(cat).busy == 0; });`) uses raw
     `std::condition_variable::wait` instead of `sm1::sm_wait`
     (`sm/CODE.md` §7 violation).
  2. In `work()`, a worker decrements `b.catalogs.at(task.cat).busy--`
     and calls `changed(b)` at lines 254-255 before looping back
     to `while (!b.queue.empty())` (line 209) and before
     decrementing `b.running--` (line 258). When `builder_detach`
     wakes at line 339, erases `cat`, sees `b.catalogs.empty()`,
     and moves `b.threads` out to join them (lines 344-348), a
     concurrent `builder_attach` + `builder_enqueue` can enqueue
     work while `b.running` is stale and the detaching worker
     thread is still in its loop.
- **Fix:** Use `sm1::sm_wait` in `builder_detach`, and hold
  worker lifecycle state until threads actually exit the queue
  loop.

#### [HIGH] 8. `sm3`: Exception in `builder.cc:enqueue()` leaks `b.running` and wedges index builds

- **Location:** `sm/sm3/src/builder/builder.cc:266-284`,
  `sm/sm3/src/builder/builder.cc:351-360`
- **Issue:** `enqueue()` inserts `(cat, index)` into `b.active`,
  pushes the task to `b.queue`, and increments `b.running++`
  before calling `b.threads.emplace_back(work)` (line 283). If
  `emplace_back` throws `std::system_error` (caught in
  `builder_enqueue()` at line 357), `b.running` stays incremented
  and `(cat, index)` stays in `b.active` and `b.queue` even if
  zero workers are running, causing `builder_wait()` to hang.
- **Fix:** Increment `b.running` only after `emplace_back`
  succeeds, and remove the task from `b.queue` and `b.active` if
  no worker thread is running.

#### [HIGH] 9. `include/`: `std::optional` exposed in public header `sm/include/sm_stats.h` (`sm/CODE.md` §2)

- **Location:** `sm/include/sm_stats.h:19, 31, 39` (included by
  `sm/include/sm.h:75`)
- **Issue:** `sm/CODE.md` §2 and `sm/include/sm.h:4-5` forbid
  `std::optional` in public headers (`include/sm.h`, `sm_sys.h`).
  `sm_stats.h` includes `<optional>` and exposes
  `std::optional<uint64_t> index` and `std::optional<float>
  correlation` in `struct sm_stat_t`.
- **Fix:** Replace the two `std::optional` fields in `sm_stat_t`
  with boolean flags (`has_index`, `has_correlation`) and plain
  value fields.

---

### 3. Medium-Severity Findings

1. **`sm0` (`sm/sm0/src/interrupt.cc:74-76, 193, 307-309`) —
   Signed overflow in `time_point_of(int64_t us)`:** Multiplying
   `us * 1000` to form nanoseconds overflows `int64_t` when `us >
   INT64_MAX / 1000` (e.g., `SM_FOREVER`), wrapping to a negative
   `time_point` and causing `timer_main` or `sm_wait` to spin.
2. **`sm0` (`sm/sm0/src/interrupt.cc:326-333, 372-385`) —
   Periodic timer catch-up can starve `sm_timer_cancel`:** When
   `next = when + timer.period_us <= sm_now_us()`, `timer_main`
   loops and re-fires `id` without releasing `s->mu` or sleeping
   on `s->cv`, preventing a thread waiting in `sm_timer_cancel`
   on `s->fired_cv` from acquiring `s->mu`.
3. **`sm0` (`sm/sm0/src/vfd.cc:161, 484-491`) — Null dereference
   in `sm_vfd_open_real`:** Unlike `sm_vfd_open`,
   `sm_vfd_open_real` does not check `cache == nullptr` or `path
   == nullptr`; passing `path == nullptr` causes
   `std::string("open \"") + path` at line 161 to dereference
   `nullptr`.
4. **`sm1` (`sm/sm1/src/load.cc:470-503`) — `sm1::load_open` and
   `sm1::store_empty` bypass catalog visibility, store-0, and
   temp-store owner checks:** Both look up `storeid` via
   `db_store(s->db, storeid)` instead of `catalog_lookup(s,
   xct_view(s), storeid)` and do not reject `storeid == 0` or
   `st.owner != s`.
5. **`sm1` (`sm/sm1/src/chain.cc:294-305`,
   `sm/sm1/src/catalog.cc:100-134`) — `chain_write` and `rmstore`
   do not check `load_open_on(s, storeid)`:** `sm1.h:1542`
   states that other writes to a store with an open bulk loader
   are refused with `SM_ERR_INVALID`, which `access_write`
   enforces but `chain_write` and `rmstore` omit.
6. **`sm1` (`sm/sm1/src/bufpool.cc:241-246`) —
   `bufpool_t::acquire` leaves `state_wait` set after waking:**
   Setting `f->state.fetch_or(state_wait)` unconditionally before
   `try_pin(f->state, ex, true)` leaves `state_wait` set on the
   frame when the last waiter acquires it, forcing subsequent
   shared readers off the fast path onto `q.mu`.
7. **`sm1` (`sm/sm1/src/walfs.cc:163-170`,
   `sm/sm1/src/stats.cc:165`) — `try`/`catch` and `std::stoull`
   in `sm1` (`sm/CODE.md` §5):** `walfs.cc` uses `try`/`catch`
   and `stats.cc` calls throwing `std::stoull(v)` instead of
   `std::from_chars`.
8. **`sm2` (`sm/sm2/src/tree/tree.cc:700-730`) —
   `tree_insert_unique` walks right siblings past `high()`:** On
   `first` (when `lower_bound == count()`) and on empty leaves
   left by `tree_prune` (`v.count() == 0`), `more` remains `true`
   even when `has_high() && !begins(high(), prefix)`, pinning
   unnecessary right siblings.
9. **`sm2` (`sm/sm2/src/tree/tree.cc:738-754`) —
   `tree_insert_unique` calls `finish_split(cur)` while holding
   `first.pin` EX:** Releasing `first.pin` before `finish_split`
   avoids holding two leaf EX pins while ascending the tree on a
   path that immediately sets `restart = true`.
10. **`sm3` (`sm/sm3/src/catalog/ddl.cc:926-984`) —
    `cat_get_stats()` caches stats in shared `cat->stats` using
    session's uncommitted `table_def_t`:** If a session has
    uncommitted `DROP COLUMN` or `DROP INDEX` DDL and calls
    `cat_get_stats()`, it filters the committed stats against its
    uncommitted `def` and stores the truncated stats into shared
    `cat->stats`.
11. **`sm3` (`sm/sm3/src/catalog/internal.h:453-459`) —
    `eval_index()` does not set `sm1::errnum()` when callback
    returns non-zero:** Callers in `table.cc:520` and
    `analyze.cc:149` return `-1` with `errnum() == SM_OK` if the
    host callback fails without calling `sm_seterr`.
12. **`sm3` (`sm/sm3/src/table/pin.cc:71-77`) — Prohibited `new
    (std::nothrow)` and `SM_ERR_NOMEM` (`sm/CODE.md` §5):**
    `pin_new()` uses `new (std::nothrow) pin_t` and sets
    `SM_ERR_NOMEM`, contrary to `sm/CODE.md` §5.
13. **`src/sm.cc` (`sm/src/sm.cc:2193-2197`) — `sm_tbl_truncate`
    overwrites `*tbl->def` in place:** Move-assigning
    `api_def(...)` into `*tbl->def` reallocates internal vectors
    and strings while callers of `sm_tbl_def` may hold pointers
    into them; only `tbl->def->version` needs updating.
14. **`src/sm.cc` (`sm/src/sm.cc:601, 608, 843, 940-1006, 1733,
    1880-1983, 2205-2312`) — Inconsistent null checks before
    `guard()`:** Several entry points dereference `session`,
    `env`, or `tbl` (`tbl->session`) before entering `guard()`.

---

### 4. Low-Severity and Style Findings

- **`sm0` (`sm/sm0/src/error.cc:75, 118-123`):** `seterr` uses
  `memcpy` into `t_errmsg` (use `memmove` in case a caller passes
  `errmsg()`), and `set_errno_error` calls `strerror` instead of
  `strerror_r`.
- **`sm0` (`sm/sm0/src/interrupt.cc:105-110`):** `sm_interrupt()`
  writes to `wake_fd` on every spin retry of
  `waiter_mu->try_lock()` instead of once.
- **`sm0` (`sm/sm0/include/sm_sys.h:12`,
  `sm/sm0/include/sm_vfd.h`):** Stale comment mentioning `sys_`
  functions in `sm_sys.h:12`; `sm_vfd.h` is omitted from
  `sm0/Makefile` `FORMAT_FILES`.
- **`sm1` (`sm/sm1/src/session.cc:20-32`):** `other_options()`
  omits `load_small_bytes`, `load_sort_bytes`, and
  `load_file_bytes` when checking if an idle `db_t` matches new
  `options_t`.
- **`sm1` (`sm/sm1/src/page.cc:301-309`):** `page_drop` when
  `!session->active` skips `SM_ERR_PREPARED` and temp-store owner
  checks, and reports `"store 0 holds no pages"` on
  `check_page_key` failure.
- **`sm1` (`sm/sm1/include/sm1.h:1425-1450, 1491-1496`,
  `sm/sm1/src/seq.cc:185, 223`):** Missing individual doc
  comments on `chain_*` declarations in `sm1.h` and misplaced doc
  comment above `may_draw()` in `seq.cc`.
- **`sm2` (`sm/sm2/src/type/decimal.cc:219-246`):** `decimal_key`
  does not verify that the datum's embedded scale bits match
  `typmod.scale`.
- **`sm2` (`sm/sm2/src/rel/toast.cc:92-106`):** `toast_read`
  closes `sm1::scan_t*` manually instead of via `sm0::defer`.
- **`sm2` (`sm/sm2/include/sm2.h:3-4, 656, 963-966`,
  `sm/sm2/src/tree/tree.h:16`):** Stale comments referencing the
  retired `_id` index and `tree_remove`.
- **`sm3` (`sm/sm3/include/sm3.h:938`,
  `sm/sm3/src/catalog/load.cc:18`,
  `sm/sm3/src/catalog/ddl.cc:19`):** Type aliases `index_eval_fn`
  and `kv_ptr` lack the `_t` suffix (`kv_ptr` is also duplicated
  in `load.cc` and `ddl.cc`).
- **`sm3` (`sm/sm3/src/ddl/ddl.cc:37-40, 1048-1051`):**
  Duplicate `live_attnum()` helper definition in `ddl.cc`, and
  misplaced doc comments at `ddl.cc:291` and `table.cc:569`.
- **`rel` (`sm/rel/src/rel_ops.cc:1964-1983, 2326-2336,
  2364-2374`):** `sm_pin_ref_t` handles leak if `janet_panic`
  `longjmp`s out of `cfun_read_keys`, `cfun_update`, or
  `cfun_delete`.
- **`src/sm.cc` (`sm/src/sm.cc:111, 164-165, 282, 1282`):**
  `guard()` catches `std::bad_alloc` (`sm/CODE.md` §5 says not to
  catch `std::bad_alloc` or return `SM_ERR_NOMEM`); misplaced doc
  comments above `api_fkey()` and `sm_cat_namespace_epoch()`.

# Cleanups Not Taken

From the reviews of ALTER COLUMN TYPE and of the code merged
with it, closed otherwise (`REVIEW.md` at `1a8407e2`).

- **Two copies of ALTER COLUMN TYPE's refusals**:
  `check_retype()` in `dg/src/backend/access/sm/smstore.c`
  repeats sm3's `column_typed_in()`, and its `texts` test sm3's
  text-and-varchar test. A refusal added on one side can be
  missed on the other.
- **rel's table of type widths**: `byval_width()` in
  `sm/rel/src/rel_ops.cc` hard-codes sm2's by-value widths,
  as `sm.h` gives no width. A width call in `sm.h`, such as
  `sm_type_len()`, would remove it.
- **`collatable()` repeats `create_index`'s check** of the
  types that take a collation, in `sm/sm3/src/ddl/ddl.cc`.
- **A check in `tree_kind()` cannot fail**: `tree &&
  stmt->primary` in `smstore.c`, as no primary key is a tree.

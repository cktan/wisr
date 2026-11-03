# Code Reviews

The findings of code reviews that are still open. A finding
leaves this file when it is fixed, or decided not to be; git
history keeps the closed reviews (`REVIEW.md` up to
`1a8407e2`).

1. **`aa/`**: open; findings 1 to 7 fixed; the rule findings not
   yet checked.
2. **`sm/`**: open; every High closed; the Mediums and Lows being
   checked.
3. **Cleanups not taken** from the ALTER COLUMN TYPE reviews.

# Review: `aa/` (Authentication and Authorization)

**Status: open.** Findings 1 to 7 are fixed: the lockout bypass,
the snapshot out of step with pg_global (3, by one commit of
objects at a time), the invalidations lost past the changes kept,
no intermediate for an object with no entry, an own role commit
hiding another's, a failing read after the roles' commit, and a
temporary object's owner and ACL not undone. The rule findings
are not yet checked.

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
| Correctness & concurrency | none open | |
| `sm/CODE.md` & style rules | 5 Low | `std::map`/`std::pair` in `aa.h`, separate error state, duplicate `session_guard_t`. |

---

### 2. Correctness, Security, and Concurrency Findings

None open: findings 1 to 7 are fixed.

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

**Status: open.** High 1, a wait left registered by an
exception, High 2, a store's handle freed while copies use it,
High 3, a committed version copied unstamped (its part 3,
filter_old()'s unlinked versions, holds: only readers of the old
epoch reach them), and High 4, a rollback keeping the read pin (a
Low: seq_hwm hides the undone versions, so it kept resources, not
rows), and High 5, TOAST pointers held past a failed write (wider
than written: an index's refusal after the row write, rolled back
to a savepoint, let the next write erase the live row's chunks),
and High 6, a tree arbiter's entry added twice (a Low: a tree's
insert of an entry it has is no change, so it cost a second unique
check, not a duplicate), are fixed. High 7 is closed: its raw
wait is now a documented exception, as a detach must outlast its
builds; its join race is none, as a worker that takes new work
only makes the detach wait for it. High 8, a worker counted whose
thread was never made, and High 9, std::optional in sm_stats.h, are
fixed. Of the Mediums, 1, a deadline that overflowed into the
past, 2, a periodic timer's catch-up that starved its cancel, and
3, a real fd's open of no path, are fixed; the others and the Lows
are being checked.

Reviewed `sm/sm0/`, `sm/sm1/`, `sm/sm2/`, `sm/sm3/`,
`sm/include/`, `sm/src/`, and `sm/rel/` against `CLAUDE.md`,
`sm/CODE.md`, `sm/DESIGN.md`, and each level's design documents.

---

### 1. Summary

| Level / Area | Layering & Opacity | Naming (`_t`, `m_`, no `strncpy`) | Findings |
| --- | --- | --- | --- |
| `sm0/` | PASS (no RocksDB, depends only on `sm_error.h`) | PASS | 3 Low |
| `sm1/` | PASS (`sm1.h` opaque, sole RocksDB channel) | PASS | 4 Medium, 3 Low |
| `sm2/` | PASS (`sm2.h` opaque, no RocksDB handles) | PASS | 2 Medium, 3 Low |
| `sm3/` & `rel/` | PASS (`sm3.h` opaque, no RocksDB handles) | PASS (minor `using` aliases) | 3 Medium, 5 Low |
| `include/` & `src/` | PASS | PASS | 2 Medium, 3 Low |

---

### 2. High-Severity Findings

None open: Highs 1 to 9 are closed.

---

### 3. Medium-Severity Findings

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

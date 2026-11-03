# Code Review: `aa/` (Authentication and Authorization)

Reviewed `aa/include/`, `aa/src/`, and `aa/test/` against
`CLAUDE.md`, `sm/CODE.md`, `aa/AA_DESIGN.md`, `aa/ROLE.md`, and
`aa/USAGE.md`.

---

## 1. Summary

| Category | Status | Notes |
| --- | --- | --- |
| Public header opacity | PASS | `aa_env_t` and `aa_session_t` are opaque structs in `include/aa.h`. |
| No `throw`, `catch`, or RTTI | PASS | Zero `throw`/`try`/`catch` in library code; builds with `-fno-rtti`. |
| Naming (`snake_case`, `_t`, `m_`) & Yoda conditions | PASS | Types end in `_t`, private fields in `home_t` use `m_`, and comparisons use `0 == x`. |
| SCRAM-SHA-256 & cycle detection | PASS | Constant-time digest comparison (`timingsafe_eq`), salt/iteration checks, and DFS cycle detection in `grant_role`. |
| Correctness & concurrency | 3 High, 4 Medium | Lockout bypass bug, partial-epoch invalidation loss, and `env->objects` / `pg_global` consistency races. |
| `sm/CODE.md` & style rules | 5 Low | `std::map`/`std::pair` in `aa.h`, separate error state, duplicate `session_guard_t`. |

---

## 2. Correctness, Security, and Concurrency Findings

### [HIGH] 1. Lockout recovery (`as_owner`) bypassed when OS user name equals bootstrap superuser role name

- **Location:** `aa/src/aa_auth.cc:38-44`
- **Issue:** `aa_auth_method()` checks `if (0 == strcmp(os_user,
  rolname)) return AA_PEER;` (line 38) before checking `if
  (os_owner && bootstrap_name(*aa_roles(env)) == rolname)` (line
  41), while `*as_owner` is initialized to `false` (line 35). In
  standard `initdb` deployments, the bootstrap superuser's
  `rolname` equals the data-directory owner's OS user name. When
  that OS owner connects locally as the bootstrap superuser,
  `aa_auth_method()` returns early at line 39 with `*as_owner =
  false`. As a result, `aa_login_okay()` (`aa/src/aa_auth.cc:81`)
  does not bypass `NOLOGIN` or `VALID UNTIL`, locking out the
  data-directory owner and violating `aa/ROLE.md` §5 and
  `aa/USAGE.md` §3.
- **Fix:** Check `os_owner && bootstrap_name(*aa_roles(env)) ==
  rolname` before the general `0 == strcmp(os_user, rolname)`
  branch so `*as_owner = true` is set whenever the data-directory
  owner logs in as the bootstrap superuser.

### [HIGH] 2. Partial-epoch eviction in `env->changed` drops cache invalidations (`aa_objects_moved`)

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

### [HIGH] 3. In-memory `env->objects` snapshot can diverge from `pg_global` on disk

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

### [MEDIUM] 4. `intermediates()` skips objects with no prior `pg_object` row when revoking default `PUBLIC` privileges

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

### [MEDIUM] 5. `aa_commit()` unconditionally sets `s->epoch = ++s->env->epoch`, masking prior role commits

- **Location:** `aa/src/aa.cc:505`
- **Issue:** If another session committed role changes earlier
  (moving `s->env->epoch` from `E0` to `E1` while this session's
  `s->epoch` remained `E0`), and this session then commits a role
  change, `s->epoch = ++s->env->epoch` sets both to `E2`. A
  subsequent `aa_roles_moved(s)` call returns `false`, so the
  caller never invalidates its caches for `E1`'s role changes.
- **Fix:** Only set `s->epoch = s->env->epoch` when `s->epoch` was
  already equal to the previous `s->env->epoch`.

### [MEDIUM] 6. Post-commit `aa_store_read_roles` failure in `aa_commit()` leaves `env->roles` stale after disk commit

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

### [MEDIUM] 7. Temporary object owners and ACLs are not rolled back on `aa_abort()` / `aa_subabort()`

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

## 3. `sm/CODE.md` and `CLAUDE.md` Rule Findings

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

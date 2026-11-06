# Using aa

aa is the library that stores roles and privileges and answers
questions about them: "may this client log in?", "is this role a
member of that one?", "may this role read this table?". digres
calls it; any C++ program that links sm can.

This guide shows how to call aa. It spends most of its time on
reads (checking logins and privileges), because a server does
those on every connection and every statement. Changing roles
and privileges is covered briefly at the end. For how aa works
inside, see `AA_DESIGN.md`. Every function is declared in
`include/aa.h`.

---

## Quick reference

| I want to... | Call |
| --- | --- |
| Load aa when the server starts | `aa_load()` |
| Start / end a client session | `aa_session_new()` / `aa_session_free()` |
| Decide between peer and password login | `aa_auth_method()` |
| Get a role's password secret for SCRAM | `aa_role_secret()` |
| Check LOGIN, VALID UNTIL, CONNECTION LIMIT | `aa_login_okay()` |
| Check CONNECT on a database | `aa_connect_okay()` |
| Look up a role by name or oid | `aa_session_roles()` |
| Ask if a role is a superuser | `aa_is_superuser()` |
| Ask if a role has another role's privileges | `aa_has_privs_of_role()` |
| Ask if a role may SET ROLE to another | `aa_can_set_role()` |
| Ask if a role is a member of another | `aa_is_member_of_role()` |
| Ask if a role may administer another | `aa_is_admin_of_role()` |
| Check a role's privileges on an object (fast, cached) | `aa_object_mask()` |
| Keep the privileges of an object whose ACL you read yourself | `aa_kept_mask()`, `aa_keep_mask()`, `aa_forget_kept_masks()` |
| Compute privileges from an ACL you already have | `aa_acl_mask()` |
| Get an object's owner and ACL | `aa_find_object()` |
| Find who a GRANT should be recorded as | `aa_select_best_grantor()` |
| List what a role owns or is granted | `aa_objects_of()` |
| Learn that another session changed something | `aa_roles_moved()`, `aa_objects_moved()` |

---

## 1. Basic ideas

**The environment (`aa_env_t`).** One per server. It holds the
roles, memberships, owners and ACLs, read into memory when the
server starts. Every session shares it.

**The session (`aa_session_t`).** One per client connection,
used only by that connection's thread. It holds the changes the
connection has made but not yet committed, and caches the
answers to recent questions.

**Always pass the session when you have one.** Most functions
take both `env` and a session `s`. With a session:

- the call sees that session's own uncommitted changes;
- role and membership answers are cached, so asking again is
  cheap and takes no lock.

You may pass `NULL` for the session where there is none (for
example, in a background process). The call then reads the last
committed state and caches nothing.

**The database id (`dbid`).** Several functions need to know
which database the session is connected to, because a
database's owner is treated as a member of the predefined role
`pg_database_owner` in that database only. Pass sm's id of the
database. In digres that is `MyDatabaseId - 16384`. Before the
session has chosen a database, pass `AA_NO_DATABASE`.

**Numbers are PostgreSQL's.** Role and object ids are
PostgreSQL oids (`aa_oid_t`). Privilege bits match PostgreSQL's
`ACL_*` bits: `AA_SELECT`, `AA_INSERT`, `AA_UPDATE`,
`AA_CONNECT`, and so on.

**Errors.** Functions that can fail return `0` on success and
`-1` on failure. After a failure, `aa_errnum()` gives the error
code and `aa_errmsg()` the message. Lookups return `false` when
nothing is found. The "okay" checks return `true` when the
answer is yes; when it is no, they return `false` and set their
`why` argument to a short reason meant for the server log. aa
never throws exceptions.

---

## 2. Setting up

```cpp
#include "aa.h"

// At initdb: create pg_global, with the superuser "admin" and the
// SCRAM secret of its password (or NULL for no password).
if (aa_create(sm_env, "admin", scram_secret) != 0)
  fatal("%s", aa_errmsg());

// When the server starts, once, before any client connects.
aa_env_t *env;
if (aa_load(sm_env, &env) != 0)
  fatal("%s", aa_errmsg());   // AA_ERR_NODB: the data directory
                              // has no pg_global; run initdb

// When a client connects.
aa_session_t *s = aa_session_new(env);

// When the client disconnects.
aa_session_free(s);

// When the server stops, after every session has ended.
aa_close(env);
```

---

## 3. Authentication: letting a client in

A connection goes through four checks, in this order. Your code
does the network work (reading the socket, running the SCRAM
exchange); aa makes the decisions.

Whatever check fails, tell the client only
`password authentication failed for user "..."`. Write aa's
reason to the server log. This way a stranger learns nothing
about which roles exist.

### Step 1: Which login method? (before the password exchange)

```cpp
bool as_owner;
aa_method_t method = aa_auth_method(
    env,
    "alice",   // the role the client asks for
    true,      // true: local Unix socket; false: TCP
    "alice",   // the OS user at the other end of the socket,
               // or NULL if unknown or over TCP
    false,     // true if that OS user owns the data directory
    &as_owner);

if (method == AA_PEER) {
  // The OS user is proof enough: no password needed.
} else {
  // AA_PASSWORD: run the SCRAM password exchange.
}
```

The rules:

- On the local socket, if the OS user's name equals the role's
  name, the answer is `AA_PEER`.
- On the local socket, if the OS user owns the data directory
  and asks for the bootstrap superuser, the answer is `AA_PEER`
  and `as_owner` is set to `true`. This is the way back in if
  the superuser's password is lost. Keep `as_owner` for step 3.
- Everything else, and everything over TCP, is `AA_PASSWORD`.

### Step 2: Which password secret? (during the exchange)

```cpp
std::string secret, why;
if (aa_role_secret(env, "alice", now, &secret, &why)) {
  // Verify the client's SCRAM proof against `secret`.
} else {
  log("%s", why.c_str());   // no such role, no password,
                            // or the password has expired
}
```

`now` is the current time as PostgreSQL's `TimestampTz`
(microseconds since 2000-01-01 UTC); it is compared with the
role's VALID UNTIL.

### Step 3: May the role log in? (after the exchange)

```cpp
const char *why;
if (!aa_login_okay(
        role,              // the aa_role_t, looked up by name (§4.1)
        now,
        sessions_of_role,  // how many sessions this role has open,
                           // counting this one
        as_owner,          // from step 1
        &why))             // set to the reason when refused
  refuse_and_log("Role \"%s\" %s.", role.rolname.c_str(), why);
```

This checks that the role has LOGIN, that VALID UNTIL has not
passed, and that the role is within its CONNECTION LIMIT. A
superuser has no connection limit. The data directory's owner
(`as_owner`) always passes.

### Step 4: May the role connect to this database?

```cpp
const char *why;
if (!aa_connect_okay(env, s, dbid, role.oid, &why))
  refuse_and_log("Role %s \"%s\".", why, dbname);
```

This checks the CONNECT privilege on the database. Every role
has it through PUBLIC unless it was revoked. A role also has it
if any role it inherits from was granted it. A superuser always
passes.

---

## 4. Authorization: checking privileges

These checks run on every statement, so they are built to be
fast. The first answer for a role is computed; later answers
come from the session's cache until someone changes a role,
membership, owner or ACL.

### 4.1 Look up a role

```cpp
std::shared_ptr<const aa_roles_t> roles = aa_session_roles(s);

for (const aa_role_t &r : roles->roles)
  if (r.rolname == "alice") {
    // r.oid, r.rolsuper, r.rolcanlogin, r.rolconnlimit, ...
  }
```

`roles->roles` lists every role, and `roles->members` every
membership, both sorted by oid. The list includes the session's
own uncommitted changes.

The pointer is a snapshot: it does not change while you hold
it. Fetch it once and keep it for the whole lookup. Don't fetch
it again for every check in a busy loop: each fetch updates a
reference count that all sessions share.

### 4.2 Is the role a superuser?

```cpp
if (aa_is_superuser(env, s, roleid)) {
  // A superuser passes every privilege check.
}
```

The bootstrap superuser (oid 10) always is.

### 4.3 Questions about role membership

Each of these returns `true` or `false`. Each also returns
`true` if `member == role`, and (except where noted) if
`member` is a superuser.

| Question | Call | PostgreSQL equivalent |
| --- | --- | --- |
| Does `member` have `role`'s privileges? | `aa_has_privs_of_role(env, s, dbid, member, role)` | `has_privs_of_role()` |
| May `member` SET ROLE to `role`? | `aa_can_set_role(env, s, dbid, member, role)` | `member_can_set_role()` |
| Is `member` a member of `role` at all? | `aa_is_member_of_role(env, s, dbid, member, role, false)` | `is_member_of_role()` |
| Same, but not counting superusers | `aa_is_member_of_role(env, s, dbid, member, role, true)` | `is_member_of_role_nosuper()` |
| May `member` grant and revoke `role`? | `aa_is_admin_of_role(env, s, dbid, member, role)` | `is_admin_of_role()` |
| Which role gives `member` that ADMIN? | `aa_select_best_admin(env, s, dbid, member, role)` | `select_best_admin()` |

Which one to use:

- **To check a privilege, use `aa_has_privs_of_role()`.** It
  follows only grants made WITH INHERIT TRUE, which is what
  PostgreSQL does for privileges. Use it for ownership too: a
  member of a table's owning role may do what the owner may.
- **To check SET ROLE, or creating an object owned by another
  role, use `aa_can_set_role()`.** It follows only grants made
  WITH SET TRUE.
- **`aa_is_member_of_role()` ignores the grant options.**
  PostgreSQL warns that it is rarely the right check.
- **A role never administers itself:**
  `aa_is_admin_of_role(..., A, A)` is `false` unless A is a
  superuser.

### 4.4 May a role do something to an object?

This is the check a server makes most often, so it is one call,
and it is cached:

```cpp
uint64_t held = aa_object_mask(env, s, dbid,
                               table_oid,          // the object
                               AA_CLASS_RELATION,  // its kind
                               roleid,
                               AA_SELECT | AA_UPDATE);

bool can_select = (held & AA_SELECT) != 0;
bool can_update = (held & AA_UPDATE) != 0;
```

It returns the privileges, among those asked for, that the
role holds on the object. Use `AA_CLASS_RELATION` for a table,
view or sequence, `AA_CLASS_NAMESPACE` for a schema, and
`AA_CLASS_DATABASE` with object oid 0 for a database itself.

What it does for you:

- **Finds the owner**: a temporary object's (kept in the
  session), else the object's own entry, else its database's
  owner.
- **Finds the ACL**: the object's, or the owner's default ACL
  if it has none (the owner holds every privilege; for a
  database, PUBLIC also holds CONNECT and TEMPORARY).
- **Follows memberships**: a role holds a privilege granted to
  itself, to PUBLIC, or to any role whose privileges it has
  (§4.3). Anyone with the owner's privileges holds every grant
  option.
- **Caches the answer**: the first call for an object and a role
  computes every privilege the role holds on it. Later calls,
  for any privilege on that object, are a hash lookup: no lock,
  no memory allocated, no ACL parsed. The cache empties itself
  when a role, membership, owner or ACL changes.

Notes:

- **Grant options** ("WITH GRANT OPTION") are the privilege bit
  shifted left by 32: `AA_SELECT << 32`.
- **Superusers.** The mask gives a superuser only what some ACL
  item grants. PostgreSQL checks for a superuser first and skips
  the check; do the same with `aa_is_superuser()`.
- **Pass the session.** With `NULL`, nothing is cached and every
  call does the whole lookup.

**If you have the ACL items yourself**, compute the privileges
directly with `aa_acl_mask()`. It is not cached:

```cpp
std::vector<aa_acl_item_t> acl = aa_acl_items(acl_text);
uint64_t held = aa_acl_mask(env, s, dbid, acl.data(), acl.size(),
                            roleid, owner, AA_SELECT, false);
```

The last argument `false` returns exactly which of the asked-for
privileges the role holds; `true` returns as soon as it finds
any one of them, for when you only need zero or not zero.

**If you already have a PostgreSQL `Acl`**, pass its items
directly; no copy is needed. `AclItem` and `aa_acl_item_t` have
the same memory layout:

```cpp
const aa_acl_item_t *items =
    reinterpret_cast<const aa_acl_item_t *>(ACL_DAT(acl));
uint64_t held = aa_acl_mask(env, s, dbid, items, ACL_NUM(acl),
                            roleid, owner, mask, false);
```

digres's `acl.c` does this, with a `static_assert` that checks
the two layouts match.

### 4.5 Which role should a GRANT be recorded as?

When a role that is not the owner grants a privilege, PostgreSQL
records the grant as made by the role that actually holds the
grant option. This finds it, given the object's ACL items and
owner (from `aa_find_object()`, as GRANT changes the object):

```cpp
aa_object_t obj;
aa_find_object(env, s, aa_key_t(dbid, table_oid), &obj);
std::vector<aa_acl_item_t> acl = aa_acl_items(obj.acl);

aa_oid_t grantor;
uint64_t grant_options;
aa_select_best_grantor(env, s, dbid,
                       roleid,      // who is running GRANT
                       AA_SELECT,   // what is being granted
                       acl.data(), acl.size(), obj.owner,
                       &grantor, &grant_options);
```

If the role is the owner or a superuser, `grantor` is the owner.
Otherwise it is the role, among those whose privileges it has,
that holds the most of the needed grant options (the closest
one wins a tie). If none holds any, `grantor` is the role itself
and `grant_options` is 0: PostgreSQL then grants nothing, with a
warning, or an error if the role holds no privilege on the
object at all.

### 4.6 What does a role own, or hold grants on?

Used by DROP ROLE (to list what depends on the role), REASSIGN
OWNED and DROP OWNED:

```cpp
std::vector<aa_object_t> owned, granted;
aa_objects_of(env, s, roleid, &owned, &granted);
```

This covers every database. A database itself appears with
object oid 0. Pass `NULL` for either list you don't need.

### 4.7 Noticing changes made by other sessions

aa's own caches update themselves. If your program keeps caches
of its own (digres keeps PostgreSQL's), check at the start of
each transaction whether another session changed anything:

```cpp
if (aa_roles_moved(s)) {
  // Some role or membership changed: drop cached role data.
}

std::vector<aa_oid_t> oids;
bool all;
if (aa_objects_moved(s, dbid, &oids, &all)) {
  if (all)
    drop_all_cached_relations();   // too many to list
  else
    drop_cached_relations(oids);   // these objects in dbid changed
}
```

Each returns `true` once per change. When nothing has changed,
each costs a single atomic read.

---

## 5. Changing roles and privileges

Changes are private to the session until it commits. The session
sees its own changes immediately; other sessions see them only
after the commit.

```cpp
// Roles and memberships.
aa_oid_t oid;
aa_new_oid(s, &oid);           // a new oid for a role or membership
aa_put_role(s, role);          // create or update a role
aa_delete_role(s, oid);
aa_put_member(s, membership);  // create or update a membership
aa_delete_member(s, oid);

// Owners and ACLs. `level` is the subtransaction nesting level,
// so that rolling back to a savepoint undoes the change.
aa_set_object(s, level, obj);       // set owner and/or ACL
aa_forget_object(s, level, key);    // the object was dropped
aa_forget_database(s, dbid);        // the database was dropped
aa_set_temp_owner(s, objid, owner); // temporary: never saved

// Savepoints.
aa_subcommit(s, level);   // RELEASE SAVEPOINT
aa_subabort(s, level);    // ROLLBACK TO SAVEPOINT
```

Committing takes three calls around the database's own commit,
so that a crash between them can only leave fewer privileges
granted, never more:

```cpp
// Before the database commits.
if (aa_commit(s) != 0) {
  // AA_ERR_CONFLICT: another session changed the same role
  // first. Fail the transaction; the client may retry it.
  fail_transaction();
}

if (database_committed) {
  // After the database commits.
  if (aa_post_commit(s) != 0)
    warn("%s", aa_errmsg());
} else {
  // The database's commit failed: undo what aa_commit() wrote.
  if (aa_abort(s) != 0)
    warn("%s", aa_errmsg());
}
```

A transaction that aborts without trying to commit calls only
`aa_abort()`.

---

## 6. Performance tips

- **Use `aa_object_mask()` to check an object.** A repeated
  check is a hash lookup with no lock and no allocation.
  `aa_find_object()` and `aa_acl_mask()` are not cached: use
  them for changes, or when you already hold the ACL.
- **Keep your own objects' masks in aa.** For an object whose
  owner and ACL you read yourself, as digres's functions in
  `pg_proc`, call `aa_kept_mask()` first. On a miss, compute
  every privilege, give it to `aa_keep_mask()`, and call
  `aa_forget_kept_masks()` when your ACLs change. A change of
  roles empties it, as it does `aa_object_mask()`'s cache.
- **Pass the session.** With a session, repeated checks are
  cache hits with no lock. With `NULL`, every call is computed
  again.
- **Don't refetch snapshots in a loop.** `aa_session_roles()` and
  `aa_objects()` return shared pointers; each call updates a
  count every session shares. Fetch once, use many times.
- **Check for a superuser first.** `aa_is_superuser()` is cached;
  a superuser needs no ACL check.
- **Pass PostgreSQL ACLs to `aa_acl_mask()` without copying
  them**, as at the end of §4.4.

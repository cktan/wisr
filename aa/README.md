# aa

Roles, authentication and authorization: what every database of
a server shares about who may do what. It is a library,
`build/libaa.a`, with its interface in `include/aa.h`, and
digres links it beside sm and sq. `ROLE.md` is the design: what
digres takes of PostgreSQL's roles and privileges, where they
are kept, and how a client is let in.

```
make -j4         build build/libaa.a and its tests
make test        build, and run the tests in test/
make tsan        the tests under the thread sanitizer
make asan        the tests under the address and UB sanitizers
make clean       remove build/
make format      clang-format include/, src/ and test/
```

`DEBUG=1` builds `-O0 -g`, with `assert()`; without it, `-O3
-DNDEBUG`, as sm's levels do. `BUILD` names where the objects
go, and the tests link sm's library from the same place under
`../sm`: `BUILD=build/tsan` links `../sm/build/tsan/libsm.a`,
which digres's tsan and release builds rely on.

## What it keeps

aa keeps, in the sm database `pg_global` (`ROLE.md` §3), which
no client connects to:

- `pg_authid`, a role a row, and `pg_auth_members`, a
  membership a row, each keyed by its oid, which the sequence
  `role_oid` hands out from 16384;
- `pg_object`, an object's owner and ACL a row, by the sm id
  of its database and its oid there; a database's own with oid
  0. An ACL is text, `grantee,grantor,privs` an item, by role
  oid, so a rename leaves it be (`aa_acl_text()`).

sm knows nothing of roles: these are tables of aa's, through
`sm.h`. aa knows no SQL and no PostgreSQL: it includes no
header of PostgreSQL's, and takes oids, privilege bits and
class ids as PostgreSQL numbers them. digres turns what aa
hands back into catalog rows, `Acl`s, privilege checks and
messages (`../dg/src/backend/utils/init/pg_global.c`).

## How it is put together

| File | What it holds |
|------|---------------|
| `include/aa.h` | the interface |
| `src/aa.cc` | the process's state, a session's, and the commit protocol |
| `src/aa_store.cc` | `pg_global`'s tables: made, read, written |
| `src/aa_acl.cc` | ACLs: their text, the default, the intersection |
| `src/aa_auth.cc` | how a client gets in: the method, the secret, the login checks |
| `src/aa_authz.cc` | who holds which privilege: closures, ACL masks, grantors, CONNECT |
| `src/aa_error.cc` | the thread's error state |
| `src/aa_internal.h` | what aa's files share |
| `test/` | doctest cases, on a throwaway home (`home.h`) |

- **The process's state** (`aa_env_t`, from `aa_load()`) is
  `pg_global` as last committed: one snapshot of the roles and
  one of the objects' entries, each replaced as a whole under
  one mutex, and an epoch of each. A lookup copies a
  snapshot's pointer and reads no transaction of
  `pg_global`'s.
- **A session's state** (`aa_session_t`) is its
  transaction's changes: a working copy of the roles, the
  changes to objects' entries, each at its subtransaction's
  nesting level, the databases dropped, and its temporary
  objects' owners and ACLs, which are never written.
- **The commit protocol** brackets the database's commit.
  `aa_commit()`, before it, writes the role changes in one
  transaction of `pg_global`'s, a row another session changed
  meanwhile failing it (`AA_ERR_CONFLICT`); in the same
  transaction it writes each entry that is to grant less as
  granting only what it grants before and after, so a crash
  between the two commits leaves less granted, never more
  (`ROLE.md` §8, question 7). `aa_post_commit()`, after it,
  writes each entry as the transaction left it.
  `aa_abort()` puts back what `aa_commit()` wrote if the
  database's commit failed.
- **How a client gets in** (`ROLE.md` §5) is decided here and
  carried out by digres: `aa_auth_method()` before the
  exchange, peer or password; `aa_role_secret()` during it;
  `aa_login_refused()` after it, LOGIN, VALID UNTIL and
  CONNECTION LIMIT; `aa_connect_refused()` once the database
  is chosen, CONNECT. digres reads the socket's peer, the
  clock and its sessions, and passes them in.
- **Who holds which privilege** is computed here, as
  PostgreSQL's `acl.c` does: the roles one is a member of,
  through grants with INHERIT, with SET, or any, breadth
  first, a database's owner in `pg_database_owner` there;
  WITH ADMIN OPTION; an ACL's mask for a role; the grantor a
  GRANT is made as. digres's `has_privs_of_role()`,
  `aclmask()` and the rest call it, an `Acl`'s items passed as
  they are, laid out as `aa_acl_item_t`. A session caches
  the roles and each closure under a stamp, the shared epochs
  and its own version, so a hit takes no lock.
- **Other sessions learn of a change by its epoch**:
  `aa_roles_moved()` once the roles are replaced, and
  `aa_objects_moved()` with the oids of the objects changed,
  of the last 4096, or all of them if more.

## The interface

As sm's (`../sm/CODE.md` §2): C-like functions named `aa_`,
returning 0 on success and -1 on failure, with the failure's
code and message in the thread's error state (`aa_errnum()`,
`aa_errmsg()`); a lookup that finds nothing returns false.
Only `std::string`, `std::vector`, `std::map`, `std::pair`
and `std::shared_ptr` of a `const` cross it. aa never throws.

## Coding in aa

`../sm/CODE.md`'s rules hold here, and the repository's
`CLAUDE.md` over them. aa calls sm only through `sm.h`, and
nothing in sm calls aa. Every test has a synopsis and
numbered steps, and a change is tested at every layer it
reaches: here, and digres's `test/server.sh` for what a
client sees.

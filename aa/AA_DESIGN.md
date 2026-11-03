# aa design

This document explains how aa works inside: what it stores,
where, how it keeps that data consistent, and how it answers
questions about logins and privileges. It is the design of
record: when the code and this document disagree, one of them
is wrong.

Related documents:

- `ROLE.md` explains why: which parts of PostgreSQL's roles and
  privileges digres supports, and the rules for letting a client
  in.
- `USAGE.md` explains how to call aa.
- `README.md` lists the files and explains the build.

**The guiding rule: checks must be cheap; changes may be
slow.** A server checks privileges on every statement, many
times a second, but creates, alters and grants roles rarely.
So aa does work when something changes, or the first time a
question is asked, and keeps the answer, so that a repeated
check is a lookup: no lock, no memory allocated, no text
parsed, no object copied. Making changes, and the first answer
after one, may cost whatever it must.

---

## 1. What aa does, and what it does not

aa is a library. It:

- stores roles, role memberships, and the owner and access
  control list (ACL) of each table, view, sequence, schema and
  database;
- decides whether a client may log in, and how it must prove
  who it is;
- decides which privileges a role holds on an object, following
  role memberships the way PostgreSQL does;
- commits changes to all of the above safely, alongside the
  commit of the database the change was made in.

aa does **not**:

- parse SQL, or know about PostgreSQL's catalogs, caches or
  error messages. digres does all of that and calls aa;
- do any network I/O, such as reading the client's socket or
  running the SCRAM password exchange;
- support column, function, type or language privileges,
  row-level security, or `ALTER DEFAULT PRIVILEGES` (see
  `ROLE.md` for what comes later).

aa depends only on sm (through `sm.h`) and the C++ standard
library. It includes no PostgreSQL header: where it needs
PostgreSQL's numbers (oids, privilege bits, catalog ids), it
defines them again in `aa.h`. sm never calls aa.

---

## 2. Where the data lives: the `pg_global` database

In PostgreSQL, roles live in shared catalogs that every
database's transactions can read and write. In sm, each database
is separate and no transaction spans two of them. So aa keeps
everything about roles in a database of its own, `pg_global`.
No client ever connects to it.

`pg_global` is created by `aa_create()` when the data directory
is initialized, in a single transaction, so it is either
complete or absent. A data directory without one is refused at
startup with `AA_ERR_NODB`.

It holds three tables (`src/aa_store.cc`):

| Table | One row per | Looked up by |
| --- | --- | --- |
| `pg_authid` | role | oid; name (unique) |
| `pg_auth_members` | membership of one role in another | oid; (role, member, grantor) (unique) |
| `pg_object` | object that has an owner or ACL | (database id, object oid) |

Details:

- **Oids for new roles and memberships** come from a sequence,
  `role_oid`, starting at 16384, above PostgreSQL's built-in
  oids. When created, `pg_global` holds the bootstrap superuser
  (oid 10), PostgreSQL 18's 16 predefined roles such as
  `pg_read_all_data`, and the 3 memberships that make up
  `pg_monitor`.
- **An object is identified** by the sm id of its database and
  its oid within that database. A database's own entry uses
  object oid 0.
- **ACLs are stored as text**: a list of
  `grantee,grantor,privileges` items, using role oids rather
  than names, so renaming a role does not change any ACL.
- **An object with no ACL** gets PostgreSQL's default ACL: its
  owner has every privilege (and, for a database, PUBLIC may
  CONNECT and create temporary tables).
- **An object with no entry at all** is treated as owned by its
  database's owner. Objects created by initdb have no entry,
  and a crash at the wrong moment can leave a new object
  without one (§5); either way the result is safe.

---

## 3. The shared state

When the server starts, `aa_load()` reads all of `pg_global`
into memory (`src/aa.cc`). This in-memory state, `aa_env_t`, is
shared by every session. It contains:

- **Two snapshots**: one of all roles and memberships, one of
  all objects' owners and ACLs. A snapshot is never modified;
  when something changes, a new snapshot replaces the old one
  as a whole.
- **A mutex** that protects the two snapshot pointers while they
  are copied or replaced.
- **Two epoch counters**, one per snapshot, increased each time
  that snapshot is replaced. A session compares an epoch with
  the value it saw last to learn cheaply whether anything
  changed.
- **A list of recently changed objects**: for each of the last
  4096 changes, the epoch and the object. This lets a session
  learn *which* objects changed, not just that something did.

Reading roles or privileges never starts a transaction in
`pg_global`; it reads the snapshots in memory.

---

## 4. Per-session state

Each client session has its own `aa_session_t`, used only by
that session's thread. It contains:

- **Uncommitted role changes**: on the session's first change
  to a role or membership, it copies the roles snapshot and
  edits the copy. The session sees its copy; other sessions
  keep seeing the shared snapshot. It also keeps the snapshot
  it copied from, to detect conflicts at commit (§5).
- **Uncommitted owner and ACL changes**: a list of changes,
  each tagged with the savepoint nesting level that made it, so
  rolling back to a savepoint can drop exactly those.
- **Databases dropped** in the current transaction, whose
  entries are deleted after the commit.
- **Temporary objects' owners and ACLs**. These belong to the
  session only and are never written to `pg_global`.
- **A connection to `pg_global`**, opened the first time it is
  needed: to hand out a new oid, or to commit.
- **Caches** of recent answers (§7).

A new session starts with its epochs at 0, so the first time it
asks "did anything change?" the answer is yes. This makes the
host discard anything it cached before the session existed.

---

## 5. Committing safely across two databases

A transaction can change both its own database (say, creating a
table) and `pg_global` (say, granting SELECT on it). These are
two sm databases, and no transaction spans both. aa therefore
commits in a fixed order, chosen so that a crash between the two
commits can only leave **fewer** privileges granted, never more:

1. **`aa_commit()`, before the database commits.** In one
   transaction in `pg_global`, it:
   - writes the role and membership changes. If a row is no
     longer what the session originally read, another session
     changed it in the meantime; the commit fails with
     `AA_ERR_CONFLICT` rather than overwrite that change;
   - for each object whose owner or ACL is changing, writes a
     temporary *intermediate* version that grants only what both
     the old and the new version grant: if the owner changes,
     the bootstrap superuser owns it for now, and the ACL is the
     intersection of the old and new ACLs. It remembers the old
     versions.

   It then re-reads the roles from `pg_global` while holding the
   mutex, so the newest commit always wins, replaces the roles
   snapshot, and increases the roles epoch.
2. **The database commits** (or fails to).
3. **`aa_post_commit()`, after the database committed.** It
   writes each object's final owner and ACL, deletes the entries
   of dropped databases, replaces the objects snapshot, and
   increases the objects epoch. If this step fails, the server
   only logs a warning: the transaction has already committed,
   and the objects keep their intermediate, more restrictive,
   versions.
4. **`aa_abort()`, if the database's commit failed** (or the
   transaction aborted): discards the session's changes and
   writes back the old versions saved in step 1.

So, at every moment, an object grants at most what both the old
and the new version grant, until both commits are done.

**One commit of objects' entries at a time.** The database's
commit releases the transaction's locks before step 3 writes and
publishes the entries. So a session that changed entries holds a
mutex of aa's from step 1 to the end of step 3 or 4, across the
database's commit:

- pg_global and the objects' snapshot change in one order, the
  commits';
- a statement that changes an entry first waits out a commit in
  flight (`aa_object_settled()`): GRANT and REVOKE, and an owner's
  change, read the entry as that commit leaves it. Without the
  wait, a GRANT could build on the ACL as it was before another
  session's, and lose it.

A transaction that changes no entry, DML or a temporary object's,
never takes it. Commits that change entries wait for one another,
each across its database's WAL sync.

Role changes (CREATE, ALTER, DROP ROLE) are always committed
whole in step 1. digres refuses them inside a transaction block,
so they never need to be undone by a savepoint.

---

## 6. Authentication (`src/aa_auth.cc`)

`ROLE.md` §5 sets the rule for letting a client in. aa makes the
decisions; the host does the I/O and passes in what it learned
(the socket's peer, the current time, the number of sessions).
There are four decisions, each at a different point in the
connection:

| Function | When | What it decides |
| --- | --- | --- |
| `aa_auth_method()` | before the password exchange | peer login if the OS user's name equals the role's name, or if the OS user owns the data directory and asks for the bootstrap superuser; otherwise a password, and always a password over TCP |
| `aa_role_secret()` | during the exchange | the role's SCRAM secret, or why there is none: no such role, no password, password expired |
| `aa_login_okay()` | after the exchange | LOGIN, VALID UNTIL, CONNECTION LIMIT (none for a superuser); the data directory's owner always passes |
| `aa_connect_okay()` | after the database is chosen | the CONNECT privilege on the database, including through role memberships; a superuser always passes |

A refusal comes back as a short reason for the server log. The
client is always told the same generic message, so it cannot
learn which check failed.

---

## 7. Authorization (`src/aa_authz.cc`)

aa computes privileges the way PostgreSQL's `acl.c` does.

**Role memberships.** For a given role, aa computes the list of
roles it belongs to, directly or indirectly, starting with the
role itself, nearest first. It follows one kind of grant at a
time:

- grants WITH INHERIT TRUE: the roles whose privileges it has;
- grants WITH SET TRUE: the roles it may SET ROLE to;
- any grant: the roles it is a member of at all.

A database's owner is also a member of the predefined role
`pg_database_owner`, in that database only. For ADMIN OPTION, aa
looks for the nearest role in the list that was granted the
target role WITH ADMIN OPTION; a role never administers itself.

**Superusers** have every role's privileges. The bootstrap
superuser (oid 10) is always a superuser.

**Privileges on an object.** Given an object's ACL and owner, a
role holds a privilege if it was granted to the role itself, to
PUBLIC, or to any role whose privileges it has. Anyone with the
owner's privileges also holds every grant option. Direct grants
and PUBLIC are checked first, because checking memberships costs
more.

**The grantor of a GRANT.** If the role running GRANT is the
owner or a superuser, the grant is recorded as made by the
owner. Otherwise it is recorded as made by the role, among those
whose privileges it has, that holds the most of the needed grant
options, the nearest one winning a tie.

**Caching.** Each session caches:

- the roles snapshot;
- the answer to the last "is this role a superuser?";
- for each kind of grant, the last membership list computed
  (like PostgreSQL, one per kind, for the most recent role);
- for each (database, object, role) asked about, every
  privilege the role holds on the object (`aa_object_mask()`),
  up to 4096 of them.

Each cache is stamped with the two shared epochs and a counter
of the session's own changes, which every change the session
makes (to roles, owners, ACLs, temporary objects) increases. A
cached answer is used while its stamp matches the current
values; when it does not, the cache is emptied and the answer
recomputed.

**The hot path.** "May this role access this object?" is one
call, `aa_object_mask()`. When the answer is cached, the call
reads the three numbers of the stamp and finds the object in a
hash map by a plain 16-byte key. It takes no lock, allocates no
memory, copies no `shared_ptr` (which would touch a count all
sessions share), and parses no ACL. On a miss it does the
work once: the owner (a temporary object's, the object's entry,
or its database's), the ACL (or the owner's default), parsed,
and the role's privileges through its memberships; then it
keeps all of them, so a later check for any privilege on the
object is a hit.

Looking up an object's owner and ACL directly
(`aa_find_object()`) is not cached: it copies the objects
snapshot's pointer under the mutex each time. It is for
changes, not for checks.

---

## 8. Threads

A digres server is one process; each session runs in its own
thread.

- The shared state (§3) is used by all threads. The snapshot
  pointers are only read or replaced under the mutex; the
  epochs are atomic.
- Snapshots are never modified after they are made, so a session
  can keep reading an old one safely while another session
  replaces it.
- A session's own state (§4) is used only by its own thread, so
  it needs no locking.

---

## 9. Errors

Functions that can fail return 0 on success and -1 on failure.
After a failure, `aa_errnum()` gives a code and `aa_errmsg()` a
message, kept per thread. Lookups that find nothing return
`false`. aa never throws exceptions.

| Code | Meaning | What the host should do |
| --- | --- | --- |
| `AA_ERR_SM` | sm reported an error | report sm's message |
| `AA_ERR_NODB` | the data directory has no `pg_global` | refuse to start; run initdb |
| `AA_ERR_NOTABLE` | `pg_global` lacks a table | refuse to start; run initdb |
| `AA_ERR_CONFLICT` | another session changed the same role first | fail the transaction as a serialization failure; it may be retried |
| `AA_ERR_LIMIT` | no oid is left | report it |

---

## 10. Testing

aa's tests (`test/`) run without a server, on a temporary sm
data directory (`test/home.h`). They cover:

- the roles initdb creates, and a data directory without
  `pg_global`;
- role changes: commit, restart, conflicting sessions, deletes;
- owners and ACLs: commit, savepoints, a crash between the two
  commits, an abort putting old versions back, dropped objects
  and databases, temporary objects;
- ACL text, default ACLs, and intersection;
- each authentication decision;
- membership lists, ACL privileges, grantor selection, CONNECT,
  and each cache noticing the changes it must.

Each test was checked by deliberately breaking the code it
covers and confirming the test fails (`../dg/WORKNOTE.md`, steps
326 on). digres's `test/server.sh` and `test/initdb.sh` test the
same behavior through a running server, as a client sees it.

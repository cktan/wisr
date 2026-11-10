# aa design

This document explains how aa works inside: what it stores,
where, how it keeps that data consistent, and how it answers
questions about logins and privileges. It is the design of
record: when the code and this document disagree, one of them
is wrong.

Related documents:

- [`ROLE.md`](ROLE.md) explains why: which parts of
  PostgreSQL's roles and privileges digres supports, and the
  rules for letting a client in.
- [`USAGE.md`](USAGE.md) explains how to call aa.
- [`README.md`](README.md) lists the files and explains the
  build.

**The guiding rule: checks must be cheap; changes may be
slow.** A server checks privileges on every statement, many
times a second. It creates, alters and grants roles rarely.

So aa does its work when something changes, or the first time a
question is asked, and keeps the answer. A repeated check is
then a lookup. It takes no lock, allocates no memory, parses no
text and copies no object. A change, and the first answer after
it, may cost whatever they must.

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
  [`ROLE.md`](ROLE.md) for what comes later).

aa depends only on sm (through `sm.h`) and the C++ standard
library. It includes no PostgreSQL header. Where it needs
PostgreSQL's numbers (oids, privilege bits, catalog ids), it
defines them again in `aa.h`. sm never calls aa.

---

## 2. Where the data lives: the `pg_global` database

In PostgreSQL, roles live in shared catalogs. Every database's
transactions can read and write them. In sm, each database is
separate, and no transaction spans two databases. So aa keeps
everything about roles in a database of its own, `pg_global`.
No client ever connects to it.

`aa_create()` creates `pg_global` when the data directory is
initialized. It does this in a single transaction, so
`pg_global` is either complete or absent. At startup, a data
directory without `pg_global` is refused with `AA_ERR_NODB`.

It holds three tables (`src/aa_store.cc`):

| Table | One row per | Looked up by |
| --- | --- | --- |
| `pg_authid` | role | oid; name (unique) |
| `pg_auth_members` | membership of one role in another | oid; (role, member, grantor) (unique) |
| `pg_object` | object that has an owner or ACL | (database id, object oid) |

Details:

- **Oids for new roles and memberships** come from a sequence,
  `role_oid`. It starts at 16384, above PostgreSQL's built-in
  oids.
- **A new `pg_global` holds**:
  - the bootstrap superuser (oid 10);
  - PostgreSQL 18's 16 predefined roles, such as
    `pg_read_all_data`;
  - the 3 memberships that make up `pg_monitor`.
- **An object is identified** by the sm id of its database and
  its oid within that database. A database's own entry uses
  object oid 0.
- **ACLs are stored as text**: a list of
  `grantee,grantor,privileges` items. The items use role oids,
  not names, so renaming a role does not change any ACL.
- **An object with no ACL** gets PostgreSQL's default ACL: its
  owner has every privilege. For a database, PUBLIC may also
  CONNECT and create temporary tables.
- **An object with no entry at all** is treated as owned by its
  database's owner. Objects that initdb creates have no entry. A
  crash at the wrong moment can also leave a new object without
  one (§5). In both cases, the result is safe.

---

## 3. The shared state

When the server starts, `aa_load()` reads all of `pg_global`
into memory (`src/aa.cc`). Every session shares this in-memory
state, `aa_env_t`. It contains:

- **Two snapshots**: one of all roles and memberships, and one
  of all objects' owners and ACLs. A snapshot is never modified.
  When something changes, a new snapshot replaces the old one as
  a whole. Each snapshot is held in an atomic `shared_ptr`,
  which is read without a lock.
- **A mutex** that orders the replacement of snapshots, and
  protects the list of recently changed objects.
- **Two epoch counters**, one for each snapshot. A counter
  increases each time its snapshot is replaced. A session
  compares an epoch with the value it saw last, to learn cheaply
  whether anything changed.
- **A list of recently changed objects.** For each of the last
  4096 changes, it holds the epoch, the object and the object's
  class. So a session can learn *which* objects changed, not only
  that something changed. It can also learn whether a schema or a
  database is among them. digres flushes its cached rows for a
  schema or a database only in that case, because the flush also
  resets its plan cache.

Reading roles or privileges never starts a transaction in
`pg_global`. It reads the snapshots in memory.

---

## 4. Per-session state

Each client session has its own `aa_session_t`, used only by
that session's thread. It contains:

- **Uncommitted role changes.** On the session's first change
  to a role or membership, the session copies the roles snapshot
  and edits the copy. The session sees its copy, and other
  sessions keep seeing the shared snapshot. The session also
  keeps the snapshot it copied from, to detect conflicts at
  commit (§5).
- **Uncommitted owner and ACL changes**: a list of changes. Each
  change is tagged with the savepoint nesting level that made it,
  so a rollback to a savepoint drops exactly those changes.
- **Databases dropped** in the current transaction, whose
  entries are deleted after the commit.
- **Temporary objects' owners and ACLs**. These belong to the
  session only and are never written to `pg_global`.
- **A connection to `pg_global`**, opened the first time it is
  needed: to give out a new oid, or to commit.
- **Caches** of recent answers (§7).

A new session starts with its epochs at 0. So the first time it
asks "did anything change?", the answer is yes. This makes the
host discard anything it cached before the session existed.

---

## 5. Committing safely across two databases

A transaction can change both its own database (for example,
it creates a table) and `pg_global` (for example, it grants
SELECT on the table). These are two sm databases, and no
transaction spans both. So aa commits in a fixed order. The
order makes sure that a crash between the two commits can only
leave **fewer** privileges granted, never more:

1. **`aa_commit()`, before the database commits.** In one
   transaction in `pg_global`, it:
   - writes the role and membership changes. If a row is no
     longer what the session read, another session changed it in
     the meantime. Then the commit fails with `AA_ERR_CONFLICT`,
     and does not overwrite that change.
   - writes a temporary *intermediate* version of each object
     whose owner or ACL changes. The intermediate version grants
     only what both the old and the new version grant. If the
     owner changes, the bootstrap superuser owns the object for
     now. The ACL is the intersection of the old and new ACLs.
     aa remembers the old versions.

   Then, while holding the mutex, it reads the roles again from
   `pg_global`, so the newest commit always wins. It replaces the
   roles snapshot, and increases the roles epoch.
2. **The database commits**, or fails to.
3. **`aa_post_commit()`, after the database commits.** It:
   - writes each object's final owner and ACL;
   - deletes the entries of dropped databases;
   - replaces the objects snapshot, and increases the objects
     epoch.

   If this step fails, the server only logs a warning: the
   transaction has already committed. The objects keep their
   intermediate versions, which are more restrictive. aa also
   replaces the objects snapshot with those versions and
   increases the epoch, so the privileges in force are the ones a
   restart reads. aa does not retry the write.
4. **`aa_abort()`, if the database's commit failed or the
   transaction aborted.** It discards the session's changes, and
   writes back the old versions saved in step 1.

So until both commits are done, an object grants at most what
both the old and the new version grant.

**One commit of objects' entries at a time.** The database's
commit releases the transaction's locks before step 3 writes and
publishes the entries. So a session that changed entries holds a
mutex of aa's from step 1 to the end of step 3 or step 4, across
the database's commit. This gives three guarantees:

- **One order.** `pg_global` and the objects snapshot change in
  the same order: the order of the commits.
- **A change waits for a commit in flight.** Before a statement
  changes an entry, it calls `aa_object_settled()`, which waits
  for a commit in flight to end. So GRANT, REVOKE and an owner
  change read the entry as that commit leaves it. Without the
  wait, a GRANT could build on the ACL as it was before another
  session's change, and lose that change.
- **A lost update fails at commit.** A statement that reads an
  entry to change it also notes the objects epoch of its read.
  In step 1, under the mutex, `aa_commit()` fails with
  `AA_ERR_CONFLICT` in two cases: the log of changes shows that
  another commit changed the entry after that epoch, or the log
  lost entries after it. Without this check, two transactions
  could read one ACL before either commits, and the second could
  restore a privilege that the first revoked. PostgreSQL fails
  the same case with "tuple concurrently updated". The statement
  does not wait, so the conflict shows at the commit.

An owner change rewrites the ACL, then sets the owner. Setting
the owner calls `aa_object_settled()` again, which notes a newer
epoch. So the owner change records the rewritten ACL first: the
entry's first change keeps the epoch of the ACL's read, and the
commit check uses that epoch. Otherwise the second call could
hide, from the check, a revoke that committed in between.

A transaction that changes no entry never takes the mutex: DML,
for example, or a change to a temporary object. Commits that
change entries wait for one another, each across its database's
WAL sync.

Role changes (CREATE, ALTER and DROP ROLE) are always committed
whole in step 1. digres refuses them inside a transaction block,
so a savepoint never needs to undo them.

---

## 6. Authentication (`src/aa_auth.cc`)

[`ROLE.md`](ROLE.md#5-how-a-client-gets-in) §5 sets the rule for
letting a client in. aa makes the decisions. The host does the
I/O, and passes in what it learned: the socket's peer, the
current time, and the number of sessions. There are four
decisions, each at a different point in the connection:

| Function | When | What it decides |
| --- | --- | --- |
| `aa_auth_method()` | before the password exchange | over TCP, always a password. On the local socket, a peer login if the OS user's name equals the role's name, or if the OS user owns the data directory and asks for the bootstrap superuser; otherwise a password |
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
roles that it belongs to, directly or indirectly. The list
starts with the role itself, and the nearest roles come first.
aa follows one kind of grant at a time:

- grants WITH INHERIT TRUE: the roles whose privileges it has;
- grants WITH SET TRUE: the roles it may SET ROLE to;
- any grant: the roles it is a member of at all.

A database's owner is also a member of the predefined role
`pg_database_owner`, in that database only.

For ADMIN OPTION, aa looks for the nearest role in the list that
was granted the target role WITH ADMIN OPTION. A role never
administers itself.

**Superusers** have every role's privileges. The bootstrap
superuser (oid 10) is always a superuser.

**Privileges on an object.** A role holds a privilege on an
object if the object's ACL grants it to one of these:

- the role itself;
- PUBLIC;
- any role whose privileges the role has.

A role with the owner's privileges also holds every grant
option. aa checks direct grants and PUBLIC first, because
checking memberships costs more.

**The grantor of a GRANT.** If the role that runs GRANT is the
owner or a superuser, aa records the owner as the grantor.
Otherwise, aa looks at the roles whose privileges the running
role has. The grantor is the one that holds the most of the
needed grant options. If two tie, the nearer one wins.

**Caching.** Each session caches:

- the roles snapshot;
- the answer to the last "is this role a superuser?";
- for each kind of grant, the last membership list computed.
  As in PostgreSQL, there is one list for each kind, for the
  most recent role.
- for each (database, object, role) asked about, every privilege
  the role holds on the object (`aa_object_mask()`), up to 4096
  entries.
- for each (class, object, role) of the host's own, every
  privilege the host computed for it (`aa_kept_mask()`), up to
  4096 entries. These are objects whose owner and ACL are not in
  `pg_global`, such as digres's functions, which `pg_proc`
  holds.

Each cache is stamped with three numbers: the two shared epochs,
and a counter of the session's own changes. Every change the
session makes increases the counter: to roles, owners, ACLs or
temporary objects. A cached answer is used while its stamp
matches the current values. When the stamp does not match, the
cache is emptied and the answer is computed again.

**The hot path.** "May this role access this object?" is one
call, `aa_object_mask()`. When the answer is cached, the call
reads the three numbers of the stamp, and finds the object in a
hash map by a plain 16-byte key. It takes no lock, allocates no
memory, and parses no ACL. It also copies no `shared_ptr`, which
would touch a reference count that all sessions share.

On a miss, the call does the work once:

1. Find the owner: a temporary object's, the object's entry, or
   the database's owner.
2. Find the ACL, or the owner's default ACL, and parse it.
3. Compute the role's privileges through its memberships.
4. Keep all of the role's privileges on the object, so a later
   check for any privilege on the object is a hit.

**The host's objects.** A host that reads an object's owner and
ACL itself can keep the answer in aa, with two calls:

- `aa_kept_mask()` looks up the answer under the same stamp.
- After a miss, `aa_keep_mask()` keeps the mask as of the stamp
  that the miss saw. So if something changed in between, the
  next lookup misses.

aa does not see a change to the host's ACLs. The host calls
`aa_forget_kept_masks()` when one changes.

A direct lookup of an object's owner and ACL
(`aa_find_object()`) is not cached: it loads the pointer of the
objects snapshot each time. It is for changes, not for checks.

---

## 8. Threads

A digres server is one process, and each session runs in its
own thread.

- All threads use the shared state (§3). The snapshot pointers
  and the epochs are atomic. They are read without a lock, and
  replaced under the mutex.
- Snapshots are never modified after they are made, so a session
  can keep reading an old one safely while another session
  replaces it.
- A session's own state (§4) is used only by its own thread, so
  it needs no locking.

---

## 9. Errors

A function that can fail returns 0 on success and -1 on
failure. After a failure, `aa_errnum()` gives a code and
`aa_errmsg()` a message. Each thread has its own. A lookup that
finds nothing returns `false`. aa never throws exceptions.

| Code | Meaning | What the host should do |
| --- | --- | --- |
| `AA_ERR_SM` | sm reported an error | report sm's message |
| `AA_ERR_NODB` | the data directory has no `pg_global` | refuse to start; run initdb |
| `AA_ERR_NOTABLE` | `pg_global` lacks a table | refuse to start; run initdb |
| `AA_ERR_CONFLICT` | another session changed the same role, membership or object entry first (§5) | fail the transaction as a serialization failure; it may be retried |
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

Each test was checked by breaking the code it covers on
purpose, and confirming that the test fails
([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md), steps 326 on).
digres's `test/server.sh` and `test/initdb.sh` test the same
behavior through a running server, as a client sees it.

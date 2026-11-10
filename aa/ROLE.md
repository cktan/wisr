# ROLE — users, roles, privileges, and how a client gets in

**Being built**, a step at a time (§9): steps 1 to 6 are
done; the later steps are not. Roles and memberships are
`pg_global`'s, made, changed and dropped by SQL; an object
is owned by the role that made it, and privileges on
databases, schemas, tables, views and sequences are granted,
revoked and checked; a column's and a function's are later
(§2). This file says what roles
and privileges digres takes from PostgreSQL, where sm keeps
them, and how a client is let in, by a rule much simpler
than `pg_hba.conf`. aa ([`README.md`](README.md)) is the code
that keeps
them. Paths are relative to `aa/`; sm's are under `../sm/`,
digres's under `../dg/`.

---

## 0. In brief

- **Roles are PostgreSQL's**: CREATE, ALTER and DROP ROLE,
  membership with GRANT role TO role, SET ROLE, ownership,
  and GRANT and REVOKE on databases, schemas, tables, views
  and sequences. PostgreSQL 18's semantics, a subset.
- **They live in a database of their own**, `pg_global`, an
  sm database no client connects to, as PostgreSQL keeps them
  in shared catalogs: a role is every database's.
- **Invariant: all role data is in `pg_global`, and only
  there** (§3). Roles, memberships, and every owner and
  privilege of every object, by database and object; a
  database's own directory holds nothing that names a role.
  A database copied or replicated to another server takes no
  role with it, and that server gives it its own.
- **PostgreSQL's privilege checks stay as they are.** The 183
  calls into `aclchk.c` are in digres's tree, and aa hands
  them the owners, privileges and roles it stores. The work
  was to store them and to hand them to the checks; not to
  write checks.
- **No `pg_hba.conf`.** Who may connect, to what, and how, is
  four checks, all from SQL: the role may log in, it may
  connect to the database, and it proves who it is by the
  socket's OS user or by its password. Nothing is read from a
  file, nothing is ordered, and a refusal says, in the
  server's log, which check failed.

---

## 1. Where digres stood

Before step 1 ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
318), what this design
set out from:

- **One role, oid 10.** initdb writes its name, and the
  SCRAM secret of its password if one is given, to the data
  directory's `superuser` file
  ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 8).
  `superuser_arg(10)` is true, and no other oid is a role.
- **Authentication is a fixed rule** (step 92): peer on the
  local socket, SCRAM-SHA-256 over TCP; a superuser with no
  password connects only locally. `hba.c` is gone; no TLS, no
  GSSAPI.
- **Every relation's owner is oid 10** (relcache), and sm3's
  records hold no owner and no privileges.
- **What it costs**: of PostgreSQL's regression files, 1,366
  statements are taken out at a role's catalog (`pg_authid`),
  47 at default privileges, 18 at policies, before what each
  takes with it (`../dg/test/pgregress`). It is the largest gap
  there, and [`../sm/NEXT.md`](../sm/NEXT.md) puts it first for
  adoption: an
  application, a read-only and an administrator's role are
  the least a deployment asks.

---

## 2. Scope

**In**, the first build:

- `CREATE`, `ALTER`, `DROP ROLE` (and `USER`, `GROUP`):
  `LOGIN`, `SUPERUSER`, `CREATEDB`, `CREATEROLE`, `INHERIT`,
  `PASSWORD`, `VALID UNTIL`, `CONNECTION LIMIT`.
- Membership: `GRANT role TO role` with `ADMIN`, `INHERIT`
  and `SET` options, `REVOKE`; `SET ROLE`, `RESET ROLE`,
  `SET SESSION AUTHORIZATION`; `current_user`,
  `session_user`.
- Ownership of databases, schemas, tables, views, sequences;
  `ALTER ... OWNER TO`, `REASSIGN OWNED`, `DROP OWNED`.
- `GRANT`, `REVOKE` on databases (`CONNECT`, `CREATE`,
  `TEMPORARY`), schemas (`USAGE`, `CREATE`), tables and views
  (`SELECT`, `INSERT`, `UPDATE`, `DELETE`, `TRUNCATE`,
  `REFERENCES`, `TRIGGER`, `MAINTAIN`), and sequences
  (`USAGE`, `SELECT`, `UPDATE`); `PUBLIC`; `WITH GRANT
  OPTION`.
- The catalogs psql and drivers read: `pg_authid`,
  `pg_roles`, `pg_user`, `pg_auth_members`, and the `acl`
  columns of `pg_class`, `pg_namespace`, `pg_database`;
  `\du`, `\dp`, `\dn+`, `\l`'s owners. `has_table_privilege()`
  and the rest of its kind.

**Later**, each on its own: column privileges (§8 question 2);
`ALTER DEFAULT PRIVILEGES`; row-level security, which needs roles
first
([`../sm/FEATURELIST.md`](../sm/FEATURELIST.md#7-replication--backup--ops)
§7); GRANT and REVOKE on functions,
types and languages, which are compiled into digres: each is
`PUBLIC`'s, as PostgreSQL's are, but the functions PostgreSQL's
initdb takes EXECUTE on from `PUBLIC`, which are the superuser's
alone, or a predefined role's too as initdb grants them
([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 334); most of what
PostgreSQL's
predefined roles imply, which exist, all 16
([`AA_DESIGN.md`](AA_DESIGN.md#2-where-the-data-lives-the-pg_global-database)
§2).

**Not at all**: `pg_hba.conf`, `pg_ident.conf`, and the
methods only they choose — trust, ident, md5, password in the
clear, LDAP, RADIUS, PAM, GSSAPI, SSPI (§5).

---

## 3. Where roles live

A role is the whole server's: one connects to any database,
and owns objects in several. PostgreSQL keeps roles in shared
catalogs, which every database's transaction reads and writes.
In sm a database is a RocksDB of its own
([`../sm/DBMGR.md`](../sm/DBMGR.md#4-the-home-directory) §4), and
no transaction spans
two.

**Decision: a database for what is shared, `pg_global`.**
initdb makes it beside `postgres`; no client connects to it,
and `pg_database` does not list it.

**Invariant: all role data is in `pg_global`, and only
there** (decided, §8 question 6). Nothing in a database's
directory names a role: not an owner, not an `aclitem`, not
a setting. So a database is its directory still
([`../dg/CODE.md`](../dg/CODE.md), the overriding objective), and
a copy of it
on another server — replicated
([`../sm/LOGICALDECODE.md`](../sm/LOGICALDECODE.md)) or
imported — finds no role of its source there, and that
server's `pg_global` gives it roles of its own. Its tables,
aa's (§6), hold:

- **roles**: oid, name, attributes, the SCRAM secret,
  `VALID UNTIL`, `CONNECTION LIMIT`, `ALTER ROLE ... SET`,
  `COMMENT ON ROLE`;
- **memberships**: role, member, grantor, and the `ADMIN`,
  `INHERIT` and `SET` options;
- **each database's owner, privileges and role settings**
  (`ALTER ROLE ... IN DATABASE ... SET`), by `dbid`;
- **each object's owner and privileges**, by `dbid` and the
  object's oid in its database: a table's, a view's, a
  sequence's and a schema's; later, default privileges and
  row-level security's policies' roles.

sm1's root database maps a name to its `dbid` and keeps
nothing else; it stays so. The superuser is the role of oid
10, and the `superuser` file goes.

**Keys that are never reused.** A `dbid` is never reused
([`../sm/DBMGR.md`](../sm/DBMGR.md#4-the-home-directory) §4), nor
an object's oid in its
database, which comes from the sequence `sm3.oid`
([`../sm/CATALOG.md`](../sm/CATALOG.md)). So an entry left behind
for a
dropped object or database, by a crash between its two
commits, names nothing and can never name a new one; it is
swept, not trusted.

**An object with no entry is its database's owner's**, with
PostgreSQL's default privileges, and a database with no owner
the superuser's. That is what a copied or replicated
database is on its new server, every object in it, until its
owner is set there (`ALTER DATABASE ... OWNER TO`) and grants
are made; and what an object is whose entry a crash lost
(below).

**What it costs: role DDL is a transaction of its own**
(decided, §8 question 1).
CREATE, ALTER and DROP ROLE, GRANT and REVOKE of a membership,
and GRANT on a database write `pg_global`, not the database
the session is in. Each runs alone, outside a transaction
block, as CREATE DATABASE already does in digres and in
PostgreSQL; in PostgreSQL CREATE ROLE may share a transaction
with other statements, and here it may not. A script that
makes a role and grants it a table in one transaction does
the two in two.

The alternatives:

- **Rows in sm1's root database**: as atomic as `pg_global`,
  and no more, and sm1 would learn what a role is. No.
- **A copy in every database's catalog**, written to each:
  no transaction spans them either, and a copy can be left
  behind by a crash between two. No.
- **The `superuser` file, grown**: not transactional, and
  written by many sessions. No.
- **One sm1 transaction over two databases**: what would make
  role DDL transactional with the rest. A two-phase commit
  inside the server, between `pg_global` and the session's
  database, sm1 has the parts of (prepared transactions).
  Worth it only if scripts that mix the two are common; not
  for now (§8, question 1, decided).

**What the invariant costs: an object and its owner commit
apart.** `CREATE TABLE` writes the session's database, and its
owner, `current_user`, is an entry in `pg_global`; `DROP
TABLE` likewise; and `GRANT`, `REVOKE` and `ALTER ... OWNER
TO` on an object write `pg_global` alone. No transaction spans
the two, so they are written at the database's commit
(decided, §8 question 7):

- **The entry is written after the database commits**, by the
  session, and a rollback writes none. Until then the session
  sees its own pending entries, as it sees its own uncommitted
  catalog. A crash between the two commits loses the entry:
  the object falls to its database's owner, which fails
  closed for its creator, not open for anyone else.
- **A dropped object's entries are removed after its drop
  commits**; a crash leaves them, naming nothing, to be
  swept. `DROP DATABASE` removes its `dbid`'s.
- **`GRANT`, `REVOKE` and `ALTER ... OWNER TO` on an object**
  run in a transaction block, as a migration script asks, and
  are written at the database's commit as the owner is: a
  `REVOKE`, and the old owner's removal, before the commit; a
  `GRANT`, and the new owner, after. So a crash leaves fewer
  privileges than asked, never more.
- **A commit's entries are one `pg_global` transaction**: an
  object's owner and its grants are written together, or not
  at all; a crash never leaves a grant without its owner.
- **A view whose owner's entry was lost** falls to its
  database's owner, and runs with that owner's privileges.
  Only that owner may use it, so nothing is exposed, but its
  creator did not choose it: the sweep reports such objects.

The alternative, an owner and an `aclitem[]` in the object's
own record, commits them together, and breaks the invariant:
a copy of the database names its source's role oids, which on
another server name no role, or another role, which then
holds what was granted.

**Sessions share aa's snapshot of the roles.** aa reads
`pg_global` once, when the server starts, and replaces a
snapshot whole when something changes
([`AA_DESIGN.md`](AA_DESIGN.md#3-the-shared-state) §3); a
session compares the snapshot's epoch with its own at a
statement's start, as a database's catalog is checked
([`../dg/SCHEMACACHE.md`](../dg/SCHEMACACHE.md)): a REVOKE takes
effect at the next
statement of every session, as in PostgreSQL.

**A replicated database leaves `pg_global` behind.** The unit of
replication is a database
([`../sm/LOGICALDECODE.md`](../sm/LOGICALDECODE.md)): A, B and C
on one server may go to X, Y and Z. `pg_global` is not replicated
with them. By the invariant, that loses nothing A needs to be
read, and leaks nothing of A's server:

- **X's roles are X's own.** On X, A is its owner's, every
  object in it, with no grant, until X's administrator sets
  them (above); at the least, X's superuser alone.
- **No oid of A's server reaches X**, so none can name a role
  of X's, and X may hold databases of its own, or from other
  servers, beside A.
- **Grants and role DDL on A's server**, after replication
  starts or before, do not reach X; X's are made on X. Nor
  does [`../sm/LOGICALDECODE.md`](../sm/LOGICALDECODE.md)'s feed
  carry them: they are
  not in A.

---

## 4. Privileges

**PostgreSQL's checks run unchanged.** `aclchk.c` asks for an
object's owner and its `aclitem[]`: a relation's through the
relcache (`relowner`, `relacl`), a schema's and a database's
through the syscache. digres fills those from `pg_global`'s
entries for the session's database, and `superuser_arg()` and
role membership read the session's role cache. A check that fails
today on a missing catalog row is the work; a check itself is
not.

- **What a new object grants is PostgreSQL 15's**: its owner
  everything, `PUBLIC` nothing on a table; on a database,
  `CONNECT` and `TEMPORARY` to `PUBLIC`; the schema `public`
  owned by `pg_database_owner`, `USAGE` to `PUBLIC`, `CREATE`
  to no one else.
- **The `aclitem[]` is stored as aa's text**, its items'
  `grantee,grantor,privs` by role oid, space-separated
  (`aa_acl_text()`, `src/aa_acl.cc`), so a role's rename
  leaves it be, in the object's entry in `pg_global`: sm3
  keeps it and does not read it, as it keeps a column's
  default. No entry, or no `aclitem[]` in one, is
  PostgreSQL's null ACL: the owner's default privileges.
- **A dropped role leaves nothing behind.** PostgreSQL refuses
  `DROP ROLE` while the role owns an object or holds a
  privilege in any database (`pg_shdepend`). Here every such
  entry is in `pg_global`, so `DROP ROLE` reads one database,
  in one transaction, and names a database with a dependent
  object as PostgreSQL's message does.
  `REASSIGN OWNED` and `DROP OWNED` act in the session's
  database, as in PostgreSQL.

---

## 5. How a client gets in

`pg_hba.conf` confuses because it is a second language for
what SQL already says. Its lines match in order and the first
wins; a line names databases, roles, groups with `+`, files
with `@` and addresses, in its own syntax; it picks among a
dozen methods, one of which, `trust`, lets anyone in; a role's
name may differ from the OS user's by a second file,
`pg_ident.conf`; a change waits for a reload; and a refused
client is told nothing of which line refused it. Most of it
restates a privilege: who may reach which database.

**digres's rule is four checks, in this order, and nothing
else:**

1. **The role may log in**: it exists, has `LOGIN`, is not
   past `VALID UNTIL`, and is under its `CONNECTION LIMIT`.
2. **It may connect to the database**: `CONNECT` on it, which
   `PUBLIC` has unless revoked.
3. **On the local socket**, the OS user of the client's
   process is the role's name (peer); or else the role's
   password.
4. **Over TCP**, the role's password, by SCRAM-SHA-256. A role
   with no password cannot connect over TCP.

Every part is SQL: `CREATE ROLE app LOGIN PASSWORD '...'`,
`REVOKE CONNECT ON DATABASE prod FROM PUBLIC`, `GRANT CONNECT
ON DATABASE prod TO app`. A change holds at the next connect,
with no reload. The client is told PostgreSQL's
`password authentication failed for user "..."` whatever
failed, so a stranger learns nothing; the server's log says
which check, and why.

| What `pg_hba.conf` says | What digres has instead |
| --- | --- |
| which roles may reach which database | `CONNECT` on the database |
| reject a role | `NOLOGIN`, or no `CONNECT` |
| local only | a role with no password |
| `trust` | nothing: a role proves who it is, always |
| `peer` with an `ident` map | peer by the same name, or a password |
| `md5`, `password` | SCRAM alone |
| from which addresses | `listen_addresses`, and the host's firewall |
| `hostssl`, `hostnossl` | one setting, `ssl`: when on, every TCP client must use TLS, and the local socket never does (§8 question 4, [`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 422) |
| ldap, radius, pam, gss, sspi, cert | not at all, for now |

**Locked out** (§8 question 3). On the local socket, a client
whose OS user is the data directory's owner, the user the
server runs as, is let in as the bootstrap superuser, oid 10,
by peer, whatever the catalog says of it: its password, its
`LOGIN`, its `CONNECTION LIMIT`. Role oid 10 cannot be dropped,
as in PostgreSQL. No single-user mode is kept for this.

### 5.1 A password set in cleartext

SCRAM sends no password when a client logs in. A password set
by SQL does: `CREATE ROLE` or `ALTER ROLE ... PASSWORD 'x'`
carries it in the statement's text, and without TLS that text
crosses the network in the clear
([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
380).

- **Over TCP without TLS, a password in cleartext is
  refused**: `a password in cleartext is refused over an
  unencrypted connection`, with a hint to use psql's
  `\password`. The statement crossed the network once, but the
  password is not set, so what crossed is no role's password.
  PostgreSQL takes it.
- **Taken as before**: a SCRAM secret, which psql's `\password`
  and libpq's `PQchangePassword()` make on the client and send
  in place of the password; an empty password, which clears
  it; and any password over the local socket, which crosses no
  network. Over TLS too.
- **An md5 hash is refused on every connection**: `an MD5
  password is not supported`. A login is by SCRAM alone (§5),
  so no login would accept it. `password_encryption` takes only
  `scram-sha-256`, so no password in cleartext is hashed to
  md5. PostgreSQL 18 takes both, with a warning.
- **No log shows a password**: a statement that a log line
  shows, an error's `STATEMENT:`, `log_statement` and the
  durations, has each string after the keyword `PASSWORD`
  replaced by `'********'` (`query_for_log()` in `elog.c`).
  `UPDATE t SET password = 'x'` is not a role's password, and
  stays as it is. A statement that fails to parse may have its
  `PASSWORD` misspelled: its `STATEMENT:` line masks every
  string after `CREATE` or `ALTER` with `ROLE`, `USER` or
  `GROUP`. Another statement that fails to parse keeps its
  strings.
- **`pg_stat_activity` shows a running statement as it is**, as
  PostgreSQL does: only to its own role and to a role with
  `pg_read_all_stats`, and only while it runs.

---

## 6. What changes, by level

**sm**: nothing. `pg_global` is an sm database like any
other, and its tables are aa's (`src/aa_store.cc`):
`pg_authid`, a role a row, keyed by oid, with a unique index
on the name; memberships, owners and privileges. aa reads and
writes them through `sm.h`'s table calls, and digres reaches
them through aa (`../dg/src/backend/utils/init/pg_global.c`,
its adapter), so sm learns
nothing of roles, as §3 wants of sm1, and needs no record kind
of its own for them.

**digres**:

- initdb makes `pg_global` and the superuser's role in it;
  the `superuser` file goes. A data directory made before,
  with no `pg_global`, is refused at start with a message to
  run initdb again (§8 question 5). Built, step 1.
- The roles' cache: one snapshot of `pg_global`'s committed
  roles, which the postmaster reads at start and every session
  shares (`shmem->pg_global`), replaced whole by role DDL
  after it commits; `superuser_arg()`, `has_privs_of_role()`
  and `is_member_of_role()` read it, through `pg_authid` and
  the syscache. Built for reading, step 1.
- `user.c`'s CREATE, ALTER and DROP ROLE, GRANT and REVOKE of
  a membership, and `aclchk.c`'s GRANT and REVOKE, writing
  `pg_global` instead of catalog tuples: the session queues
  its changes, sees them itself, and writes them at its
  commit, in one transaction of `pg_global`'s, a row another
  session changed meanwhile failing it as a serialization
  failure; role DDL refused in a transaction block. Built for
  roles and memberships, step 2.
- relcache: `relowner` and `relacl` from the object's entry,
  else its database's owner; the entry written for
  `current_user` after a CREATE commits, and removed after a
  DROP commits (§3).
- The catalog relations of §2
  ([`../dg/SYSCATALOG.md`](../dg/SYSCATALOG.md)), and
  `pg_get_userbyid()` reading the cache.
- Authentication by §5, in place of `check_hba()`'s fixed
  rule.

---

## 7. Tests

Each step tests what it builds at every layer that reaches
it ([`../CLAUDE.md`](../CLAUDE.md), "Test at every layer"):

- **sm3, rel**: none. sm keeps no role of its own: `pg_global`
  is tables, which sm's tests already cover, and no check of
  sm's reads them.
- **mvcc**: none; roles change nothing of sm1.
- **aa**: its unit tests (`test/`), of the API, the stores
  and the checks ([`AA_DESIGN.md`](AA_DESIGN.md#10-testing) §10).
- **digres**: `initdb.sh` for `pg_global` made, its
  superuser's name and secret, and a data directory without
  it refused; `server.sh` for `pg_authid` and `pg_roles`, a
  role's lookup by name and oid, and `pg_global` hidden; and
  `server.sh` for each of §5's four checks,
  admitted and refused, on the socket and over TCP; a role
  refused a table, then granted it; a REVOKE seen by another
  session at its next statement. pgregress's `privileges`,
  `roleattributes`, `create_role`, `rowsecurity` later, and
  the files taken out at `pg_authid`, ported again.

---

## 8. Decisions

1. **Role DDL outside a transaction block** (§3). Decided
   (2026-10-27): accepted for now. A two-phase commit between
   `pg_global` and the session's database, which would make
   role DDL transactional with the rest, waits until scripts
   that mix the two are found common.
2. **Column privileges**: decided (2026-10-27), later, a step
   of their own after table privileges.
3. **A way in when every superuser is gone**: decided
   (2026-10-27), the data directory's owner by peer, always,
   as the bootstrap superuser (§5); no `digres --single`.
4. **TLS**: decided (2026-10-27), after roles, by one setting:
   when on, every TCP client must use it; the local socket
   never does.
5. **A data directory made before roles**: decided
   (2026-10-27), refused, with a message to run initdb again;
   no upgrade in place.
6. **All role data is in `pg_global`, and only there**:
   decided (2026-10-27), an invariant (§3). A database is
   replicated or copied without its roles, and its new server
   gives it its own.
7. **An object and its owner commit apart** (§3): decided
   (2026-10-27), `GRANT`, `REVOKE` and `ALTER ... OWNER TO` on
   an object run in a transaction block and are written at
   the database's commit, a `REVOKE` before it and a `GRANT`
   after, from step 4; not refused, as migration tools grant
   inside their transactions.

---

## 9. Order of work

1. `pg_global` and the role records; the superuser moves
   there from the file; `pg_authid`, `pg_roles`, `\du`. Done
   ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 318).
2. CREATE, ALTER, DROP ROLE; membership; SET ROLE. Done
   ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 319).
3. §5's four checks, in place of the fixed rule. Done
   ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 320).
4. Owners: what initdb leaves, as PostgreSQL's (the schemas'
   ACLs, the predefined roles), and a role that is not a
   superuser running statements, done
   ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md)
   step 321); an object's entry in `pg_global`, `current_user`
   after CREATE commits, the database's owner without one,
   `ALTER ... OWNER TO`, done (step 322). Not done: the sweep
   of entries a crash left behind, which name nothing.
5. GRANT and REVOKE on tables, views, sequences, schemas and
   databases; the checks that then refuse. Done
   ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step 323).
6. `DROP ROLE` and its dependents, `REASSIGN OWNED`, `DROP
   OWNED`. Done ([`../dg/WORKNOTE.md`](../dg/WORKNOTE.md) step
   324).
7. Later: column privileges, default privileges, row-level
   security, TLS.

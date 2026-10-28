# ROLE — users, roles, privileges, and how a client gets in

**Proposed**, not built. digres has one role, the superuser
initdb names, and no privilege is ever refused. This file says
what roles and privileges digres takes from PostgreSQL, where
sm keeps them, and how a client is let in, by a rule much
simpler than `pg_hba.conf`. Paths are relative to `sm/`;
digres's are under `../dg/`.

---

## 0. In brief

- **Roles are PostgreSQL's**: CREATE, ALTER and DROP ROLE,
  membership with GRANT role TO role, SET ROLE, ownership,
  and GRANT and REVOKE on databases, schemas, tables, views,
  sequences and columns. PostgreSQL 18's semantics, a subset.
- **They live in a database of their own**, `pg_global`, an
  sm database no client connects to, as PostgreSQL keeps them
  in shared catalogs: a role is every database's. An object's
  owner and its privileges live in its own record, in its own
  database.
- **PostgreSQL's privilege checks stay as they are.** The 183
  calls into `aclchk.c` are in digres's tree and pass today
  only because every role is the superuser. The work is to
  store owners, privileges and roles, and to hand them to the
  checks; not to write checks.
- **No `pg_hba.conf`.** Who may connect, to what, and how, is
  four checks, all from SQL: the role may log in, it may
  connect to the database, and it proves who it is by the
  socket's OS user or by its password. Nothing is read from a
  file, nothing is ordered, and a refusal says, in the
  server's log, which check failed.

---

## 1. Where digres stands

- **One role, oid 10.** initdb writes its name, and the
  SCRAM secret of its password if one is given, to the data
  directory's `superuser` file (`../dg/WORKNOTE.md` step 8).
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
  there, and `NEXT.md` puts it first for adoption: an
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
  `REFERENCES`, `TRIGGER`, `MAINTAIN`), their columns, and
  sequences (`USAGE`, `SELECT`, `UPDATE`); `PUBLIC`; `WITH
  GRANT OPTION`.
- The catalogs psql and drivers read: `pg_authid`,
  `pg_roles`, `pg_user`, `pg_auth_members`, and the `acl`
  columns of `pg_class`, `pg_namespace`, `pg_database`;
  `\du`, `\dp`, `\dn+`, `\l`'s owners. `has_table_privilege()`
  and the rest of its kind.

**Later**, each on its own: `ALTER DEFAULT PRIVILEGES`;
row-level security, which needs roles first (`FEATURELIST.md`
§7); privileges on functions, types and languages, which are
compiled into digres and all `PUBLIC`'s; PostgreSQL's
predefined roles (`pg_read_all_data` and the rest); TLS.

**Not at all**: `pg_hba.conf`, `pg_ident.conf`, and the
methods only they choose — trust, ident, md5, password in the
clear, LDAP, RADIUS, PAM, GSSAPI, SSPI (§5).

---

## 3. Where roles live

A role is the whole server's: one connects to any database,
and owns objects in several. PostgreSQL keeps roles in shared
catalogs, which every database's transaction reads and writes.
In sm a database is a RocksDB of its own (`sm1/SM1_DESIGN.md`
§3.5), and no transaction spans two.

**Decision: a database for what is shared, `pg_global`.**
initdb makes it beside `postgres`; no client connects to it,
and `pg_database` does not list it. Its sm3 catalog holds:

- **roles**: oid, name, attributes, the SCRAM secret,
  `VALID UNTIL`, `CONNECTION LIMIT`;
- **memberships**: role, member, grantor, and the `ADMIN`,
  `INHERIT` and `SET` options;
- **databases' owners and privileges**, by database oid. sm1's
  root database maps a name to its dbid and keeps nothing
  else; it stays so.

The superuser is the role of oid 10 there, and the `superuser`
file goes.

**What it costs: role DDL is a transaction of its own.**
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
  for the first build (§8, question 1).

**An object's owner and privileges are in its own record**: a
table's, a view's, a sequence's and a schema's in sm3's
catalog of its database, beside its columns. So `CREATE TABLE`
and `GRANT SELECT` on it commit together, and dropping a table
takes its privileges with it.

**A session caches the roles.** It reads `pg_global` at
connect, and again when its epoch has moved, checked at a
statement's start as a database's catalog is
(`../dg/SCHEMACACHE.md`): a REVOKE takes effect at the next
statement of every session, as in PostgreSQL.

---

## 4. Privileges

**PostgreSQL's checks run unchanged.** `aclchk.c` asks for an
object's owner and its `aclitem[]`: a relation's through the
relcache (`relowner`, `relacl`), a schema's and a database's
through the syscache. digres fills those from sm3's records
and from `pg_global`, and `superuser_arg()` and role
membership read the session's role cache. A check that fails
today on a missing catalog row is the work; a check itself is
not.

- **What a new object grants is PostgreSQL 15's**: its owner
  everything, `PUBLIC` nothing on a table; on a database,
  `CONNECT` and `TEMPORARY` to `PUBLIC`; the schema `public`
  owned by `pg_database_owner`, `USAGE` to `PUBLIC`, `CREATE`
  to no one else.
- **The `aclitem[]` is stored as PostgreSQL's text form**,
  `{alice=arw/bob}`, by role oid, in the record: sm3 keeps it
  and does not read it, as it keeps a column's default.
- **A dropped role leaves nothing behind.** PostgreSQL refuses
  `DROP ROLE` while the role owns an object or holds a
  privilege in any database (`pg_shdepend`). digres asks each
  database's catalog, which is in memory
  (`sm3/CATALOG_INMEM.md`), opening those not open; a database
  with a dependent object names it, as PostgreSQL's message
  does.
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
| `hostssl`, `hostnossl` | when TLS comes, one setting: every TCP client must use it, or not |
| ldap, radius, pam, gss, sspi, cert | not at all, for now |

**Locked out.** A superuser who loses its password connects
on the local socket as the OS user of its name, by peer, as
now. One whose role is gone is found again by `digres
--single`, PostgreSQL's single-user mode, if it is kept
(§8, question 3).

---

## 6. What changes, by level

**sm3**: role, membership and database records, in
`pg_global`'s catalog; an owner and an `aclitem[]` in table,
view, sequence and namespace records, versioned as the record
is (`sm3/SM3_DESIGN.md` §2); lookups of a role by oid and by
name, and the list of a role's members. **sm1, sm2**: nothing.
**sm.h**: the calls for those.

**digres**:

- initdb makes `pg_global` and the superuser's role in it;
  the `superuser` file goes, and `datadir.c` reads the role.
- The session's role cache, refreshed by `pg_global`'s epoch;
  `superuser_arg()`, `has_privs_of_role()` and
  `is_member_of_role()` read it.
- `user.c`'s CREATE, ALTER and DROP ROLE, GRANT and REVOKE of
  a membership, and `aclchk.c`'s GRANT and REVOKE, writing
  sm3's records instead of catalog tuples; role DDL refused in
  a transaction block.
- relcache: `relowner` and `relacl` from the record; the
  owner given at CREATE as the session's `current_user`.
- The catalog relations of §2 (`../dg/SYSCATALOG.md`), and
  `pg_get_userbyid()` reading the cache.
- Authentication by §5, in place of `check_hba()`'s fixed
  rule.

---

## 7. Tests

Each step tests what it builds at every layer that reaches
it (`../CLAUDE.md`, "Test at every layer"):

- **sm3**: records of roles, memberships and privileges kept,
  loaded back after a restart, versioned, and dropped.
- **rel**: none, unless `rel` gains roles: sm keeps them, and
  no check of sm's reads them.
- **mvcc**: none; roles change nothing of sm1.
- **digres**: `server.sh` for each of §5's four checks,
  admitted and refused, on the socket and over TCP; a role
  refused a table, then granted it; a REVOKE seen by another
  session at its next statement. pgregress's `privileges`,
  `roleattributes`, `create_role`, `rowsecurity` later, and
  the files taken out at `pg_authid`, ported again.

---

## 8. Open questions

1. **Role DDL outside a transaction block** (§3). Accept it
   for the first build, or make role DDL transactional with
   the session's database by a two-phase commit between the
   two? The proposal: accept it, and see whether scripts
   that mix them are common.
2. **Column privileges in the first build**, or later?
   PostgreSQL's regression files use them; most
   applications do not.
3. **A way in when every superuser is gone**: keep
   `digres --single`, or let the data directory's owner,
   by peer, always connect as superuser?
4. **TLS**: when, and whether "every TCP client must use it"
   is the only setting, as §5 proposes.

---

## 9. Order of work

1. `pg_global` and the role records; the superuser moves
   there from the file; `pg_authid`, `pg_roles`, `\du`.
2. CREATE, ALTER, DROP ROLE; membership; SET ROLE.
3. §5's four checks, in place of the fixed rule.
4. Owners: the record's, `current_user` at CREATE, `ALTER
   ... OWNER TO`.
5. GRANT and REVOKE on tables, views, sequences, schemas and
   databases; the checks that then refuse.
6. `DROP ROLE` across databases, `REASSIGN OWNED`, `DROP
   OWNED`.
7. Later: default privileges, row-level security, column
   privileges if not in step 5, TLS.

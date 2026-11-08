# NAMESPACE: Namespace

A Namespace is a named container that scopes object names. It is
SQL's `SCHEMA`. This document covers what a Namespace holds, how
a name is taken and found, the lock that keeps a Namespace from
going while something is put in it, and the epoch that stands in
for a lock on a lookup by name. The code is in
`sm3/src/catalog/ddl.cc` (`cat_put_namespace`,
`cat_del_namespace`, `use_nspace`), `sm3/src/catalog/session.cc`
(the lookups, `nspace_in_use`, the epoch),
`sm3/src/catalog/load.cc` and `sm3/src/ddl/ddl.cc`
(`create_namespace`, `drop_namespace`). `sm3/include/sm3.h`
declares the calls, and `include/sm.h` gives them to the host. A
Namespace is one object of the Catalog:
[`CATALOG.md`](CATALOG.md) has the
keys, the overlay and publishing.

---

## 1. Design

### 1.1 A worked example

```sql
CREATE SCHEMA app;             -- create_namespace: an oid, say 16390
CREATE TABLE app.t (a int);    -- create_table(16390, "t", ...)
CREATE INDEX t ON app.t (a);   -- SM_ERR_DUPKEY: t is taken in app
DROP SCHEMA app;               -- SM_ERR_INUSE: app holds a table
DROP TABLE app.t;
DROP SCHEMA app;               -- drop_namespace(16390)
```

1. `create_namespace` takes an oid from `sm3.oid`.
   `cat_put_namespace` inserts a `NAME` key for `app` in
   namespace 0, and a `NAMESPACE` key holding the name.
2. `create_table` reaches `cat_put_table`, which takes the
   Namespace's oid lock S to the end of the transaction. It then
   checks the Namespace is there, and inserts the `NAME` key of
   `t` under oid 16390.
3. `CREATE INDEX t` inserts a `NAME` key that is already there.
   sm1 refuses it with `SM_ERR_DUPKEY`, because a table and an
   index share one space of names.
4. `drop_namespace` takes the oid lock X, reads memory, finds a
   table in `app`, and fails with `SM_ERR_INUSE`.
5. Once the table is gone, the same call erases the two keys of
   `app`. At commit the Namespace leaves the shared catalog, and
   the namespaces' epoch moves.

### 1.2 Decisions

- **A Namespace stores nothing but its own name.** It has no
  owner, no privileges and no options in sm. digres keeps its
  owner and its privileges ([`../aa/ROLE.md`](../aa/ROLE.md)).
- **A name is a key.** Every named object owns a `NAME` key under
  its Namespace's oid. So the name's uniqueness, and its race,
  are sm1's insert and write-write conflict. No lock and no check
  in memory decides it (§2).
- **Names are looked up in memory**, not in the `NAME` keys and
  not as of the snapshot, as PostgreSQL finds a name in its
  latest catalog (§4). The `NAME` keys are what makes a name
  durable and unique.
- **A lookup by name takes no lock.** The namespaces' epoch
  stands in for one, for a host that caches Namespaces or a
  search path (§5).
- **Two Namespaces are built in**, at PostgreSQL's oids, because
  digres compiles its built-ins in with those numbers:
  `pg_catalog` at 11 (`nsp_pg_catalog`) and `public` at 2200
  (`nsp_public`). `cat_init` makes them, and neither can be
  dropped.
- **A Namespace's oid is a Catalog oid.** It comes from
  `sm3.oid`, from 16384 on, and is 64 bits
  ([`CATALOG.md`](CATALOG.md)).
- **sm3 keeps no temporary Namespace.** No oid or name is
  reserved for one. A temporary object has namespace 0,
  `nsp_temp`, and that is what marks it temporary (§2.3). The
  host names its own temporary Namespace, with whatever oid it
  likes, as PostgreSQL does.
- **The search path is the host's.** sm3 finds a name in one
  Namespace, and the host tries each one on its path. digres maps
  its temporary Namespace, `PG_TEMP_NAMESPACE`, to `nsp_temp`
  (`sm_nspoid`), and looks there first.
- **A drop refuses a Namespace that holds anything.**
  `DROP SCHEMA ... CASCADE` is the host's: digres drops the
  contents one by one, then the Namespace (§6.3).
- **An object does not change Namespace, and a Namespace has no
  rename.** `cat_put_table` refuses a record in another
  Namespace, and sm3 has no call to rename one. `SET SCHEMA` is
  refused until asked for.

---

## 2. Names

### 2.1 What shares a name

Within a Namespace, one space of names holds tables, indexes,
sequences, views and collations. Each takes a `NAME` key, so a
name taken by any of them fails for the others with
`SM_ERR_DUPKEY`. PostgreSQL's relations share names in the same
way. A collation joins them here, which it does not in
PostgreSQL.

A column has no `NAME` key: its name is unique among the table's
live columns. A foreign key has none either: its name is unique
among its table's foreign keys, checked in memory under the
table's lock X.

The `NAME` key is a kind, the Namespace's oid and the name. Its
value is what the name names: a kind, an oid and, for an index,
its table's oid.

### 2.2 How a name is taken

1. Insert the `NAME` key in the DDL's transaction. The name goes
   first, so a taken name fails before anything else is written.
2. A name taken and committed fails with `SM_ERR_DUPKEY`.
3. Two transactions taking one name at once meet in sm1's
   write-write conflict: the second waits for the first.

A rename inserts the new `NAME` key before it erases the old, so
a taken name leaves the object as it was (`cat_put_table`,
`cat_put_view`).

### 2.3 Namespace 0

The value 0 has two uses, and they never meet.

- **On disk it is the Namespaces' own Namespace**
  (`nsp_of_namespaces`). A Namespace's name is a `NAME` key in
  namespace 0, so a Namespace's name is unique in the database.
- **In memory it is `nsp_temp`.** A temporary table or view has
  it for its namespace, and the lookups by name take it to mean
  the session's own temporary objects. A temporary name has no
  `NAME` key, and is checked in memory by `temp_name_taken`.

The creators refuse 0. `create_table` and `use_nspace` fail with
`SM_ERR_NOTFOUND`: "namespace 0 does not exist".
`create_temp_table` and `create_view` with `nsp_temp` are the two
ways in ([`TEMPTABLE.md`](TEMPTABLE.md), [`VIEW.md`](VIEW.md)). A
temporary name and the
same name in a Namespace do not meet, and two sessions may each
have a temporary table of one name.

---

## 3. The Namespace lock

A Namespace has a lock on its oid, of the same kind as a
table-oid lock: `lock_table`, which is `sm1::lock_oid`. Every
object draws its oid from one sequence, so the name is
unambiguous.

- **A put takes it S.** `cat_put_table` of a new table,
  `cat_put_collation`, `cat_put_sequence` and `cat_put_view` of a
  new view all go through `use_nspace` in shared mode, to the end
  of the transaction.
- **A del takes it X**, to the end of the transaction. It waits
  out the puts and they wait for it, while two puts do not wait
  for each other.
- **Whether the Namespace is there, and whether it is empty, are
  read from memory.** Memory has every commit by the time the
  lock is granted. The transaction's snapshot may be older than a
  table just made or a del just committed.
- **For that, memory keeps each member's Namespace.** Tables and
  views have it in their definitions. Each collation's and each
  sequence's are found at `cat_open` by one scan of the `NAME`
  keys, and kept at each publish.
- **An object that exists takes no Namespace lock.** A reader or
  a writer of a table takes the table-oid lock alone: the table
  holds the Namespace by being in it.

`use_nspace` is:

1. Refuse `nsp_temp` with `SM_ERR_NOTFOUND`.
2. Take the Namespace's oid lock in the mode asked for.
3. Look the Namespace up as the session sees it, in memory. None
   is `SM_ERR_NOTFOUND`.

---

## 4. Name lookup

Each lookup reads memory. It finds the transaction's own changes
first, then the temporary catalog for `nsp_temp`, then the shared
catalog. It takes no lock and reads no store. So a transaction
sees every committed name, whatever its snapshot.

| Call | Finds | Own changes | Then |
| --- | --- | --- | --- |
| `cat_get_table(ss, nspoid, name)` | a table | the overlay's names, newest first | `temp_names` for `nsp_temp`, else `table_names` |
| `cat_get_index(ss, nspoid, name, table, index)` | an index and its table | the definitions of the tables the transaction changed | the temporary tables for `nsp_temp`, else `index_names`, unless the transaction changed that table |
| `cat_get_view(ss, nspoid, name, oid)` | a view | the newest definition of each view it changed | the temporary views for `nsp_temp`, else `view_names`, unless the transaction changed that view |
| `cat_get_sequence(ss, nspoid, name, oid)` | a sequence | its creates and drops, newest first | `sequence_names` |
| `cat_get_namespace(ss, name, oid)` | a Namespace | its creates and drops, over the shared map | a look at each |

- **The kinds are looked up one by one.** The Namespace's names
  are shared, so at most one call finds a name. There is no
  lookup of a `NAME` key by name alone. digres's
  `lookup_relation_oid` tries a table, an index, a view and a
  sequence in turn.
- **A collation has no lookup by name.** Only its oid and its
  Namespace are in memory.
- **`NAME` keys are read at `cat_open` only**, for each
  collation's and sequence's Namespace.
- **A lookup and a lock are two steps.** A caller finds an oid by
  name, then takes the object's lock by oid. A table dropped
  between them is `SM_ERR_NOTFOUND` at `cat_lock_table`. A table
  renamed between them is found stale by the host, which looks
  the name up again
  ([`../dg/SCHEMACACHE.md`](../dg/SCHEMACACHE.md)).
- **Listing is the same.** `cat_list_tables`, `cat_list_views`
  and `cat_list_sequences` give the names a lookup would find,
  sorted, so a list never names what a lookup does not find.
- **`cat_get_namespace(cat, ...)`**,
  **`cat_get_table(cat, ...)`** and their lists take a catalog
  and no session, and see only what is committed.
  `enqueue_pending_indexes` walks them.

---

## 5. The namespaces' epoch

**Warning: the epoch dates a commit, not a transaction's own
DDL.** A host must itself drop what its own `CREATE SCHEMA` or
`DROP SCHEMA` makes stale, and hold that through a rollback.

A table's lookup is checked under the table's lock: the host
compares the table's version there. No lock guards a Namespace's
lookup by name, so that check has nothing to stand on. The
**namespaces' epoch**, `cat_namespace_epoch`, stands in for it.
It is the count of the Catalog's publish that last made or
dropped a Namespace.

- **It moves at publish.** `publish` stores the new count in
  `nsp_seq` when the overlay has a Namespace change. An abort or
  a rollback moves nothing.
- **It is 0 at each load**, as the other counts are: no host
  cache survives a restart.
- **It is one atomic load**, with no mutex.
- **It moves before the count of publishes.** `publish` stores
  `nsp_seq`, then the count. So a reader who sees `cat_publishes`
  moved, then reads the epoch, sees the epoch moved too. digres
  relies on this order.

digres uses it so:

1. At a transaction's start and after each lock, ask whether the
   count of publishes moved (`smstore_catalog_moved`).
2. If it moved, ask whether the namespaces' epoch moved
   (`smstore_namespaces_moved`).
3. If that moved, flush `pg_namespace` locally, which drops the
   search path too.

`cat_epoch` is the wider number, which moves at every publish
([`CATALOG.md`](CATALOG.md)). `cat_namespace_epoch` lets a host
keep its search
path across the DDL that does not touch a Namespace.

---

## 6. Implementation

### 6.1 Storage and memory

| Where | What |
| --- | --- |
| `NAME` key in namespace 0 | the Namespace's name; value: kind `NAMESPACE`, its oid |
| `NAMESPACE` key, by oid | a blob of one part: its name `text` |
| `catalog_t::nspaces` | oid to name, every committed Namespace; no map by name, as they are few |
| `catalog_t::nsp_seq` | the namespaces' epoch |
| `session_t::nspaces` | the overlay: `nspace_change_t`, its depth, oid, and name or a drop |

`cat_init` writes the two built-in Namespaces at their fixed
oids. `cat_open` fills `nspaces` by one scan of the `NAMESPACE`
keys. `nspaces_of` gives the Namespaces as a session sees them:
the shared map with the session's changes over it, the newest of
each winning.

### 6.2 Create

`create_namespace(session, name, oid)` refuses an empty name with
`SM_ERR_INVALID`, then:

1. Take an oid (`cat_new_oid`).
2. Insert the `NAME` key in namespace 0. A taken name is
   `SM_ERR_DUPKEY`, and fails before the blob is written.
3. Insert the `NAMESPACE` key.
4. Record the Namespace in the overlay.

It takes no oid lock: no other transaction can reach a Namespace
that is not committed.

### 6.3 Drop

`drop_namespace(session, oid)` is `cat_del_namespace`:

1. Refuse `pg_catalog` and `public` with `SM_ERR_INVALID`.
2. Take the Namespace's oid lock X, and check it is there
   (`use_nspace`).
3. Ask `nspace_in_use`. A Namespace that holds a table, a
   sequence, a view or a collation is `SM_ERR_INUSE`.
4. Take an X lock on the `NAMESPACE` key, and read it: the newest
   committed version at READ COMMITTED, else at the snapshot.
5. Erase the `NAMESPACE` key and the `NAME` key.
6. Record the drop in the overlay.

`nspace_in_use` reads memory, with the transaction's changes over
it:

1. Look at the transaction's collations, then the last change to
   each of its tables, sequences and views.
2. Look at the shared views, sequences and tables, passing over
   any the transaction changed.
3. Look at the shared collations. There is no del of a collation,
   so a Namespace that has held one stays.

digres's `DROP SCHEMA ... CASCADE` (`drop_namespace` in
`../dg/src/backend/access/sm/smstore.c`) is a loop over these
calls, in the one transaction:

1. Drop its views, with the views elsewhere that read them.
2. Drop its tables, each with its indexes and the sequences its
   columns own.
3. Drop its other sequences, listed in a new statement, which
   sees the drops.
4. Drop the Namespace.

### 6.4 Calls

| sm3 | `sm.h` | Does |
| --- | --- | --- |
| `create_namespace` | `sm_cat_create_namespace` | make one |
| `drop_namespace` | `sm_cat_drop_namespace` | drop an empty one |
| `cat_put_namespace`, `cat_del_namespace` | — | the Catalog's writes under them |
| `cat_get_namespace` | `sm_cat_namespace_oid` | a name to an oid |
| `cat_get_namespace_name` | `sm_cat_namespace_name` | an oid to a name |
| `cat_list_namespaces` | `sm_cat_list_namespaces` | the names, sorted |
| `cat_namespace_epoch` | `sm_cat_namespace_epoch` | the epoch |

`SM_NSP_PG_CATALOG`, `SM_NSP_PUBLIC` and `SM_NSP_TEMP` are the
constants in `sm.h`.

---

## 7. Not built

- **`DROP COLLATION`**, so a Namespace that has held a collation
  cannot be dropped.
- **A dependency index.** A cascade in sm waits for one
  ([`CATALOG.md`](CATALOG.md)); digres's cascade does not need
  it.
- **`ALTER SCHEMA ... RENAME`** and **`SET SCHEMA`**.

---

## 8. Cases

| Case | What happens |
| --- | --- |
| Drop and a create in it, the create first | the drop waits on X, then finds the new object: `SM_ERR_INUSE` if the create committed |
| Drop and a create in it, the drop first | the create waits on S, then finds no Namespace: `SM_ERR_NOTFOUND` |
| Two creates in one Namespace | S and S: neither waits |
| Two sessions create one Namespace name | they meet on the `NAME` key in namespace 0 |
| Create and drop in one transaction | the overlay has both; the last change wins, and nothing is published but the epoch |
| Create, then abort or rollback | the overlay's change goes, and sm1 undoes the two keys; the epoch stays |
| Drop, then a rollback to a savepoint before it | the Namespace is back, with no publish |
| Drop, then create of the same name in one transaction | the `NAME` key is erased, then inserted; the new Namespace has a new oid |
| Old REPEATABLE READ snapshot | it sees every committed name in memory, those after its snapshot too |
| REPEATABLE READ drops a Namespace made after its snapshot | the X lock on the `NAMESPACE` key fails on the newer version with `SM_ERR_CONFLICT` |
| Crash | an uncommitted Namespace is dead; `cat_open` reloads the rest; the epoch restarts at 0 |
| Two-phase commit | `prepare` refuses a transaction that made a Namespace, as any DDL |
| Temporary table in the drop's way | it is in no Namespace, so it is no reason to refuse; the host's temporary Namespace is not sm3's |
| A built-in Namespace | a table may be put in `pg_catalog` at sm3; digres's `check_system_namespace` refuses it above |

---

## 9. Rejected

- **A temporary Namespace in sm3.** Its oid and name would be
  reserved for what is the host's to name. Namespace 0 marks a
  temporary object with no reservation.
- **Reading a Namespace's contents from the snapshot.** The
  snapshot may be older than a table just made or a del just
  committed. Memory under the lock has every commit.
- **A map of Namespaces by name.** They are few, so a lookup
  looks at each.

---

## 10. Tests

- `sm3/test/test_ddl.cc`: a Namespace made and found by name, its
  name found by oid, a drop only when empty, and a sequence
  holding its Namespace.
- `sm3/test/test_lock.cc`: a drop waits for a create in it and
  then refuses; a create waits for a drop and then finds none.
- `sm3/test/test_temp.cc`: a temporary table is in no Namespace,
  and `create_table` refuses `nsp_temp`.
- `sm3/test/test_overlay.cc`: a table's and an index's name found
  as the session sees them.
- `sm3/test/test_view.cc`: a Namespace holding a view.
- `rel/test/test_catalog.rel`: Namespaces through the C API:
  create, abort, a taken name, a built-in refused.
- `../dg/test/server.sh`: a new schema replans a held session, by
  the epoch.

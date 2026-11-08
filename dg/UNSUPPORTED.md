# What digres does not support

digres is PostgreSQL 18 on sm, and leaves much of PostgreSQL
out. This file lists what a PostgreSQL user will miss: features
refused with an error, features removed outright, and features
that run but behave differently; and, in §10, what digres takes
that PostgreSQL does not. It is a map, not the record:
each entry names where the reasons are, a
[`WORKNOTE.md`](WORKNOTE.md) step,
[`DESIGN.md`](DESIGN.md)'s decisions, or a design document under
`../sm/`.

Each entry is marked:

| Mark | Meaning |
| --- | --- |
| **refused** | an error, often worded "... yet": not built |
| **never** | refused or removed by a decision of record: not to be built |
| **removed** | the code is gone; the feature cannot be asked for |
| **differs** | it runs, and answers otherwise than PostgreSQL |
| **added** | digres's own: PostgreSQL refuses it (§10) |

"pgregress: N" is how many statements of PostgreSQL's
regression files are taken out for it (`test/pgregress`,
`--dg-skip` markers, 6,063 in all).

---

## 1. Tables and storage

- **Table inheritance** — never. `CREATE TABLE ... INHERITS`
  and `ALTER TABLE ... INHERIT` are refused
  ([`DESIGN.md`](DESIGN.md#4-decisions-of-record) §4,
  2026-10-23): rows are named by their key, and the tables of
  a tree would name theirs by different columns.
- **Partitioning** — refused. `PARTITION BY` and `PARTITION
  OF`; the partition functions answer "not partitioned"
  (step 198).
- **TIDs and `ctid`** — never. No row has a TID: a row is
  named by its key, the values of its primary key's columns, or
  a keyless table's `_id`
  ([`../sm/ROWSTORE.md`](../sm/ROWSTORE.md), step 292).
  `ctid` is no system column, so a table may have a column of
  that name (step 295); `xmin`, `xmax`, `cmin` and `cmax` are
  "column does not exist" (step 200); the `tid` type cannot be
  stored.
- **TID scans** — differs. A TID scan only for `WHERE CURRENT
  OF`, and for `_id = n` in a table without a primary key
  (step 288); the TID range scan and `currtid2()` are removed
  (step 293).
- **`_id`** — differs: a system column of a table without a
  primary key, an `int8`, its rows' key in sm; a table with
  one has none (step 299). No table may have a column named
  `_id`.
- **CREATE TABLE beyond columns, NOT NULL, PRIMARY KEY, UNIQUE,
  CHECK, generated columns and TEMPORARY** — refused: EXCLUDE
  constraints, `OF type`, `UNLOGGED`, `TABLESPACE`, `USING`.
  Column `COLLATE` and a DEFERRABLE `REFERENCES` are refused too.
  CHECK constraints are built
  ([`../sm/CHECK.md`](../sm/CHECK.md), step 411), and generated
  columns, stored and virtual
  ([`../sm/GENERATED.md`](../sm/GENERATED.md), step 415), but
  for ADD COLUMN of one, SET and DROP EXPRESSION,
  ALTER COLUMN TYPE of one, and a NOT NULL virtual one. `WITH`
  takes only `fillfactor` and `autovacuum_enabled`, and ignores
  them (step 208).
- **Unlogged tables, temporary sequences, ON COMMIT on a
  permanent table** — refused (step 80).
- **Materialized views, foreign tables, composite types,
  foreign data wrappers, servers, user mappings** — refused:
  "a relation's storage cannot be made" (pgregress: 124, and
  329 for their ALTERs and DDL); `REFRESH MATERIALIZED VIEW`
  (step 64).
- **Large objects** — removed (step 60).
- **Tablespaces** — refused (step 62).
- **The heap** — removed. Heap, nbtree, TOAST tables, the
  visibility map, free space map, buffer manager, smgr,
  checksums and pages are gone (steps 58, 59, 65, 243); every
  table is in sm. `pg_relation_size()` and the other size
  functions answer a table's or an index's bytes in sm
  (steps 120, 121).
- **Tables are clustered on their primary key** — differs. A
  SELECT with no ORDER BY reads in key order; a primary key is
  capped at 1024 encoded bytes; `CLUSTER` does nothing, every
  table being clustered ([`../sm/CLUSTER.md`](../sm/CLUSTER.md)).
  A primary key
  index takes no bytes of its own, its entries being the rows:
  its size is 0 (step 298).
- **VACUUM, CLUSTER, REINDEX, CHECKPOINT** — differs. VACUUM
  compacts each table's store (step 284); CLUSTER and REINDEX
  warn and do nothing (step 64); CHECKPOINT does nothing
  (step 14).

## 2. Indexes and plans

- **Index methods other than btree** — removed: hash, GIN,
  GiST, SP-GiST and BRIN ([`README.md`](README.md); pgregress:
  39).
- **NULLS NOT DISTINCT, non-default operator classes and
  collations, interval keys** — refused (step 50, 99). An
  index is btree over columns and expressions, ASC or DESC,
  UNIQUE, partial, with INCLUDE.
- **Partial and expression indexes** — differs
  ([`EXPRINDEX.md`](EXPRINDEX.md),
  step 311): an expression key of type numeric needs a
  precision and scale, and one of interval is refused, as a
  column of it is; an expression in a collation other than
  the default is refused, as a column key in another than its
  column's is. An error an expression raises in a
  build after the commit says `could not create index "x":`
  before PostgreSQL's message, with another SQLSTATE.
- **CREATE INDEX waits for every transaction using the table**
  — differs: it takes the table's lock X
  ([`../sm/INDEXBUILD.md`](../sm/INDEXBUILD.md)
  §2), where PostgreSQL's SHARE lock lets readers go on, so an
  open transaction that read the table holds it up.
- **A unique index or constraint made on a table with rows**
  — differs: it is built after the commit
  ([`DESIGN.md`](DESIGN.md#4-decisions-of-record) §4),
  so in a transaction block a duplicate key is reported at
  `COMMIT`, after the block's other work has committed, and
  the index is not there.
- **Bitmap scans** — never. No BitmapAnd or BitmapOr: an OR
  over two indexes is a sequential scan
  ([`../sm/CLUSTER.md`](../sm/CLUSTER.md#6-digres) §6,
  step 287).
- **Backward index scans** — differs. `ORDER BY ... DESC
  LIMIT` sorts.
- **Index quals** — refused: a row comparison, `(a, b) >
  (...)`; differs: `IN (...)` and `= ANY` on a column after
  a range's are a filter of the range, not a skip scan
  ([`INLIST.md`](INLIST.md), step 314).
- **Index-only scans of a table a statement writes or locks**
  — differs: none, as EvalPlanQual cannot recheck one
  (step 292).
- **Parallel query, JIT** — removed (steps 2, 10).
- **DROP INDEX CONCURRENTLY** — refused (pgregress: 11);
  `REINDEX CONCURRENTLY` is accepted and does nothing.

## 3. DDL

- **ALTER TABLE beyond ADD COLUMN, DROP COLUMN, ALTER COLUMN
  TYPE, SET and DROP NOT NULL, SET and DROP DEFAULT, ADD and DROP
  CONSTRAINT of a foreign key, a primary key, a unique or a check
  constraint, a foreign key's or a check's VALIDATE CONSTRAINT,
  and RENAME** — refused:
  `ADD CONSTRAINT ... USING INDEX`, `SET SCHEMA`, row level
  security, replica identity (step 231); dropping a column an
  index uses (step 110). A primary key added or dropped re-keys the rows,
  which a scan then gives in key order ([`PKDDL.md`](PKDDL.md),
  step 312).
- **ALTER COLUMN TYPE of a column an expression or a partial
  index reads, a foreign key uses or references, or of an
  identity column; with COLLATE; and a USING that reads another
  column the same ALTER TABLE retypes** — refused
  ([`ALTERTYPE.md`](ALTERTYPE.md#8-decided) §8, step 378).
  PostgreSQL makes the index,
  the foreign key or the sequence again, and evaluates every
  USING on the row as it was.
- **VALIDATE CONSTRAINT of a foreign key** — as PostgreSQL's:
  it takes `SHARE UPDATE EXCLUSIVE` on the child and `ROW SHARE`
  on the parent, and neither conflicts with a write. digres
  checks the rows under IS (see below); `sm3::validate_fkey`
  then sets the flag under the child's lock IX and the key's
  own record locked X, and publishes it at commit
  ([`../sm/CATALOG.md`](../sm/CATALOG.md)). Writers of both
  tables go on. Left:
  - **The flag shows at commit.** Until then the session that
    validated reads `convalidated` as false.
  - **A table the transaction changed, or a temporary table,
    takes the table X**, as any DDL does.
  - **Two validations of one key meet**; the second waits for
    the first to end.
  - `ADD FOREIGN KEY` holds both tables X (`sm3::create_fkey`),
    as before.
- **ALTER INDEX, ALTER TYPE, ALTER MATERIALIZED VIEW** —
  refused; ALTER VIEW takes only options and RENAME.
- **ALTER SCHEMA ... RENAME** — refused: sm renames no
  namespace yet (step 392). `OWNER TO` works.
- **DEFERRABLE constraints** — never for a primary key
  ([`../sm/CLUSTER.md`](../sm/CLUSTER.md) §10.4); refused for a
  unique constraint
  (step 294), and for a foreign key, with `NOT ENFORCED`,
  `MATCH PARTIAL` and `PERIOD` (step 177). Nothing is checked
  at commit.
- **Sequence options** — refused: `MINVALUE`, `MAXVALUE`,
  `CACHE`, `CYCLE`, `OWNED BY`, `ALTER SEQUENCE`, `setval()`,
  `SELECT` from a sequence, `TRUNCATE ... RESTART IDENTITY`.
  A sequence takes `START`, `INCREMENT` and `AS`.
- **User-defined types, domains, operators, operator classes
  and families, collations, conversions, access methods,
  aggregates** — refused: each would write a catalog row
  (pgregress: 250 and more).
- **Rules, triggers, event triggers, policies, extended
  statistics** — refused (pgregress: 132, 48, 43, about 90,
  106). No trigger can be made: there is no language to write
  one in.
- **COMMENT ON** — refused (step 122); `obj_description` and
  `col_description` answer null (step 120).
- **CREATE DATABASE options, ALTER DATABASE** — refused
  (step 63).
- **Publications, subscriptions, the REPLICATION attribute** —
  refused (step 231).

## 4. Queries and DML

- **MERGE into a table** — refused (pgregress: 64).
- **LISTEN and NOTIFY** — refused; UNLISTEN does nothing;
  `pg_notify()` is gone (step 43).
- **Backward movement of a cursor not declared SCROLL** —
  refused: a table scan runs forwards only. A SCROLL cursor
  works (step 55).
- **Whole-row references to a table** — refused: a table has
  no composite type, so `row_to_json(t)` and `t::tabletype`
  fail (pgregress: 38).
- **RANGE window frames with an offset** — refused
  (pgregress: 56).
- **ANALYZE of a column list, BUFFER_USAGE_LIMIT** — refused;
  a temporary table is skipped (step 80).
- **Exported snapshots** — refused: `pg_export_snapshot()`,
  `SET TRANSACTION SNAPSHOT` (steps 149, 201).
- **A key update that collides with itself** — differs:
  `UPDATE t SET k = k + 1` can fail on a duplicate key, rows
  being read in key order
  ([`../sm/CLUSTER.md`](../sm/CLUSTER.md#7-costs) §7).
- **A row whose primary key another transaction changed** —
  differs: the writer that meets it fails with a serialization
  failure, at every level, where PostgreSQL follows an update
  chain ([`../sm/ROWSTORE.md`](../sm/ROWSTORE.md#25-a-key-change)
  §2.5).
- **A scan begun after a savepoint** ends at a rollback to it
  — differs ([`README.md`](README.md), known incompatibilities).

## 5. Types

- **Types sm cannot store** — refused as columns (pgregress:
  217): json, jsonb, xml, arrays, ranges, multiranges, inet,
  cidr, macaddr, bit, varbit, money, timetz, `"char"`, oid,
  xid, tid, pg_lsn. A column is bool, int2, int4, int8,
  float4, float8, numeric, text, varchar, char(n), name, bytea,
  date, time, timestamp, timestamptz, interval or uuid.
- **Geometric types** — never
  ([`DESIGN.md`](DESIGN.md#4-decisions-of-record) §4).
- **Text search** — removed: tsvector, tsquery, their DDL and
  functions (step 2).
- **numeric** — differs: decimal64 or decimal128, at most 36
  digits, no NaN or infinity; an unconstrained numeric column
  is numeric(17,3) ([`DECIMAL.md`](DECIMAL.md)).

## 6. Functions and languages

- **Procedural languages and SQL functions** — removed: no
  PL/pgSQL, no `sql` language, no `DO`, no function bodies
  (steps 5, 6; pgregress: 826). SPI stays, for a scripting
  language to come.
- **Default arguments of built-in functions** — differs: none,
  so `jsonb_set`, `make_interval` and the like take every
  argument (step 70).
- **Loadable libraries and extensions** — refused: `LOAD`, C
  functions, `shared_preload_libraries`, `CREATE EXTENSION`;
  contrib is left out (step 251, [`README.md`](README.md)).

## 7. Server and administration

- **Replication and recovery** — removed: physical and
  logical replication, WAL, slots, base backup, PITR,
  archiving, hot standby (steps 2, 12, 18, 231).
- **Two-phase commit** — removed: `PREPARE TRANSACTION` is
  refused (step 47).
- **Transaction ids** — removed: `txid_current()`,
  `pg_current_xact_id()`, `age(xid)` and the rest fail
  (steps 45, 137, 232); no commit timestamps (step 46).
- **Process-level subsystems** — removed: autovacuum, the
  syslogger, csvlog and jsonlog, cumulative statistics and the
  `pg_stat_*` views, background workers, AIO, synchronized
  scans, crash restart (steps 11, 20, 21, 28 and others). A
  session is a thread
  ([`DESIGN.md`](DESIGN.md#1-one-process-a-thread-a-session) §1).
- **pg_hba.conf, pg_ident.conf** — removed (step 92): a client
  gets in by
  [`../aa/ROLE.md`](../aa/ROLE.md#5-how-a-client-gets-in) §5's
  checks, peer or a password
  on the socket, a password over TCP (steps 320, 327).
- **GSSAPI** — refused: its build option is off and its files
  not compiled ([`README.md`](README.md)).
- **TLS** — differs: digres always links OpenSSL, and with
  `ssl` on every client over TCP must use it, which PostgreSQL
  asks of a `hostssl` line; the local socket never does
  ([`../aa/ROLE.md`](../aa/ROLE.md#8-decisions) §8, step 422).
  No client certificate is asked for, as no `pg_hba.conf` says
  `clientcert`.
- **Roles** — differs: roles, memberships, owners and ACLs are
  `pg_global`'s ([`../aa/ROLE.md`](../aa/ROLE.md), steps 318 to
  324), and role
  DDL is refused in a transaction block; a column's
  privileges, a function's, ALTER ROLE ... SET and ALTER
  DEFAULT PRIVILEGES fail. UTF8 only, collation C; `initdb`
  takes no other option (step 8).
- **A password in cleartext over TCP** — refused: `CREATE` or
  `ALTER ROLE ... PASSWORD` over TCP without TLS takes only a
  hashed password, a SCRAM secret as psql's `\password` sends;
  the local socket takes either
  ([`../aa/ROLE.md`](../aa/ROLE.md#51-a-password-set-in-cleartext)
  §5.1, step
  380). A password is masked in every logged statement.
- **An MD5 password** — refused: an md5 hash as a password, on
  every connection, as no login accepts it; `password_encryption
  = md5` and `md5_password_warnings` are gone (step 382).

## 8. Locking and isolation

- **FOR KEY SHARE and FOR NO KEY UPDATE** — differs: they are
  FOR SHARE and FOR UPDATE (step 106).
- **LOCK TABLE's eight modes** — differs: mapped onto sm's
  five, so some conflict more (step 49).
- **Lock wait queues** — differs: none, so a soft deadlock
  aborts a transaction where PostgreSQL reorders the waiters
  ([`../sm/LOCKMGR.md`](../sm/LOCKMGR.md#32-the-lock-table)
  §3.2).
- **SERIALIZABLE** — differs: sm's own, not PostgreSQL's SSI
  ([`../sm/XCTMGR.md`](../sm/XCTMGR.md#7-ssi-serializable-snapshot-isolation)
  §7).
- **pg_locks** — differs: sm's mode names; a keyed row's page
  and tuple are NULL (step 193).

## 9. Catalog and introspection

- **Catalog tables** — removed: the catalog is sm's, and psql's
  `\d` reads relations made from it, read only
  ([`SYSCATALOG.md`](SYSCATALOG.md)). `information_schema` and
  the statistics
  views are missing; `pg_description` is empty (step 123);
  `pg_stats` is a function (step 78).

## 10. What digres adds

What digres takes that PostgreSQL does not. A schema that uses
it is digres's: PostgreSQL refuses it, and `pg_dump` from
digres does not carry it.

- **A column's update default** — added
  ([`ONUPDATE.md`](ONUPDATE.md), step
  307): `DEFAULT x ON UPDATE`, the value an UPDATE
  writes in the column when it does not assign it, beside an
  optional plain `DEFAULT y` for INSERT. `x` and `y` are
  constants or `CURRENT_TIMESTAMP`, `LOCALTIMESTAMP` or
  `CURRENT_DATE`. It keeps a modification time or a state an
  edit moves a row to, which PostgreSQL keeps with a BEFORE
  UPDATE trigger, and digres has none (§6). sq's grammar takes
  it as its one rule beyond PostgreSQL's
  ([`../sq/DESIGN.md`](../sq/DESIGN.md)
  §2); psql's `\d` shows the plain default alone.

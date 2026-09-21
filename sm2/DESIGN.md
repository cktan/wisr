# DESIGN — sm2, records and indexes

The level that turns sm1's ordered byte strings into rows. This
document is the design of record; it follows the decisions in
`OBJECTIVE.md`:

| Decision | Choice |
| --- | --- |
| Row identity | the primary key, clustered, as sm1 relations already are |
| Index entry | `encode(index columns) ‖ encode(pk)` → empty or INCLUDE columns |
| API shape | open: batch-oriented (sets of rows, filters pushed down) was forced by an IPC boundary, which the threaded fork removes (§1) |
| Relation mapping | many sm2 objects multiplexed onto few sm1 relations |
| PostgreSQL | a target that shapes decisions, never a build dependency |

**Status: nothing here is implemented.** This file exists so the
formats are argued before they are written, because two of them
— the key encoding and the record layout — are on disk and
effectively permanent once any data exists. `PLAN.md` has the
order of work. The PostgreSQL target (`PART3.md`) shapes these
decisions but is never a build dependency: sm2 must build and be
tested without a PostgreSQL source tree. A throwaway fork spike,
built early to find which wall bites first, is how to learn what
the permanent formats must carry before data exists
(`PART3.md` §7).

---

## 1. What sm1 provides, and what it costs

sm2 is written against the API in `sm1/include/sm.h`. The parts
that matter here, and the constraints that come with them.

**Deployment: one process, one thread per session.** The
PostgreSQL fork is threaded (`PART3.md` §2), not process per
connection, so sm1 and sm2 are linked into the one server
process and every backend thread shares sm1's transaction table,
lock manager and SSI graph. A session is used by one thread at a
time (`sm1/API.md`). The boundary between the executor and sm2
is a function call, not an RPC, so a per-row interface is no
longer ruled out. Whether sm2 keeps a batch-shaped API anyway,
for filter pushdown and fewer calls per scan, is open, and
nothing below depends on the answer.

| sm1 gives sm2 | and the constraint it brings |
| --- | --- |
| relations as ordered key→value maps, clustered on the key | keys are ordered **bytewise only**; there is no comparator to install (`sm1/OBJECTIVE.md`) |
| transactions at RC / RR / SERIALIZABLE | index maintenance is atomic with the base write for free, because both are in one transaction |
| `sm_update` / `sm_delete` returning the **old image** | the old index entries can be computed without a read of their own |
| `SM_ERR_DUPKEY` from `sm_insert` | unique enforcement needs no separate existence check |
| `sm_lock(SHARED\|EXCLUSIVE)` | the online index build protocol (§5) |
| `SM_ERR_UPDATED` and `sm_read_latest` at READ COMMITTED | a filtered update or delete must re-check its predicate, and recompute its index entries, from the newest version (§5); the error does not abort and is not retryable |
| nested savepoints | a statement can be undone without losing the transaction |
| forward scans with `sm_scan_seek` | probe loops cost one cursor, not one per probe |
| `SM_MAX_KEYLEN` 4096, `SM_MAX_DATALEN` 64MB | an index entry's `sk ‖ pk` must fit 4096 bytes together |
| no backward scan, ever | a descending column is stored with inverted bytes and scanned forward, in the same index; there are no mirror relations, and the fork's planner never asks for a backward scan (§2, §8) |
| one column family per relation, hundreds to low thousands | sm2 must multiplex (§6) |

---

## 2. Key encoding

*To be written.* The rules it must satisfy:

- `encode(a) < encode(b)` bytewise **iff** `a < b` in the type's
  own order, for every supported type.
- Multi-column keys concatenate without ambiguity: no encoded
  value may be a prefix of another that should sort after it.
- Per column: ascending or descending, and nulls first or last.
  A descending column is the ascending encoding with its bytes
  inverted, since sm1 will not scan backwards.
- Text ordering is a **collation sort key**, not the UTF-8
  bytes. The transform is lossy, so a collated key column stores
  the sort key *and* the original value.
- The encoder's signature takes, per column,
  `(type, collation, direction, nulls-order)`. Baking in
  "ascending, nulls low, bytewise" makes half of SQL's `ORDER
  BY` clauses unrepresentable.
- PostgreSQL orders text with `strcoll` or ICU, not `memcmp`, so
  the collation is an input to the encoder and not a property
  of the bytes. Sort keys are not stable across collation
  library versions, and PostgreSQL has had index corruption from
  exactly that (`pg_collation.collversion` exists for it), so the
  collation **version is recorded per relation** (§4) and a
  mismatch is detected, not trusted.
- The ban on backward scans stands, and the planner is changed
  not to ask for one (`PART3.md` §5). A DESC column is therefore
  the inverted encoding scanned forward, and an index-ordered
  `ORDER BY x DESC LIMIT n` becomes a top-N sort over a forward
  scan (§8).

Open: over-long keys against `SM_MAX_KEYLEN`, collation version
stability, and non-deterministic collations — see
`OBJECTIVE.md`.

---

## 3. Record format

*To be written.* The rules it must satisfy:

- A null bitmap, a fixed/variable split, and enough alignment
  discipline that reading a column does not require decoding
  every column before it.
- A **schema version stamp per row**, so `ADD COLUMN` is O(1):
  an old row is read under its own layout and the missing column
  comes from its default.
- Tolerance for dropped columns, since the column at a given
  position may no longer exist.
- **Cheap to turn into a PostgreSQL tuple.** The executor works
  in tuple slots, so a bespoke encoding pays a conversion on
  every row, in both directions. The threaded fork removes the
  IPC boundary, not the conversion.
- Large values are **chunked out of line** above a threshold.
  sm1 permits a 64MB value, but a transaction's undo, lock and
  read sets are linear in the keys it touches (`sm1/DESIGN.md`
  §13), and very large values hurt LSM compaction, so the
  threshold is well below sm1's ceiling.

---

## 4. Catalog

*To be written.* Tables, indexes, columns, types and schema
versions, stored in sm2's own relations and written in the same
sm1 transaction as the DDL that changes them — which is what
makes DDL transactional for free.

**Decision: the catalog is held in memory.** It is loaded from
sm1 at startup; a change is made in memory and written through
to sm1, which is used for persistence only. It replaces
PostgreSQL's heap catalogs (`PART3.md` §4), so sm1's status
record is the one commit point.

**Decision: schema changes are lock-based, as in PostgreSQL.**
The catalog is not versioned, and an old snapshot does not see
an old definition. PostgreSQL's catalogs are MVCC heap tables,
but it does not give a transaction the old schema either: it
keeps DDL and users of a table apart with table locks, so a
transaction that first touches a table after a DDL commits sees
the new definition, alongside its old data snapshot. sm2 does
the same with sm1's row locks:

- every transaction that reads or writes a table takes
  `sm_lock(SHARED)` on that table's schema row, held to the end
  of the transaction, before it reads the definition;
- DDL takes the schema row exclusive by updating it, which
  waits out the transactions in flight and blocks new ones
  until it commits — the protocol §5 already uses for online
  index builds, applied to all DDL;
- so once a transaction holds the shared lock, the definition
  in memory cannot change under it. A catalog served from
  memory must take this lock, or DDL has nothing to wait on.
  At READ COMMITTED sm1's `sm_lock` no longer fails when the
  schema row committed after the transaction began, so the
  lock alone is what protects the reader, and the in-memory
  definition, not a read of the schema row, is what it uses.

Deadlocks between DDL and DML are found by sm1's detector like
any other.

PostgreSQL hardwires its catalogs and sequences to heap, so the
fork rewrites that seam instead of living with a hybrid, where
DDL atomicity would sit exactly on the seam. sm1's sequences are
used. Because the fork is threaded, one shared in-memory catalog
is coherent without cross-process invalidation. What it costs is
the code that reads the catalogs: the relation cache, syscache
and system-table scans. Logical decoding could be fed from sm1
instead of a second copy in PostgreSQL's WAL, but sm1 has no
commit-ordered change feed yet, and physical replication and PITR
over PostgreSQL's WAL do not apply to data held in RocksDB
(`PART3.md` §4).

Making memory authoritative moves the hard part; it does not
remove it. A definition that outlives the transaction that
changed it is the usual place such a design goes wrong. Open:

- **Privacy until commit.** A DDL change must be invisible to
  other transactions until it commits, and undone on abort and
  on savepoint rollback. The shape expected is a
  per-transaction overlay, written to sm1 inside the DDL's own
  transaction and published to the shared catalog only after
  the commit, in the order sm1 uses for its commit cache
  (`sm1/DESIGN.md` §3.4).
- **Size and startup.** Load time and memory for a schema
  with many partitions.

---

## 5. Secondary indexes

*To be written.* The shape is already argued in
`sm1/SECINDEX.md`, which was an assessment of what a layer like
this one needs; every gap it found is now closed in sm1.

- **Entry:** `encode(sk) ‖ encode(pk)` → empty, or the INCLUDE
  columns. The pk suffix makes every entry unique, so a
  non-unique index needs no further disambiguation.
- **Unique indexes:** the uniqueness unit is the key *without*
  the pk suffix, so it is a separate existence check under
  `sm_lock(EXCLUSIVE)` rather than a property of the KV key. At
  READ COMMITTED the check reads with `sm_read_latest`, not
  `sm_read`: a statement snapshot can predate a duplicate that
  committed while the lock was awaited, and the lock is what
  keeps the newest version stable.
- **Maintenance:** on update and delete, the old image from
  `sm_update`/`sm_delete` gives the old index entries to remove.
- **Read committed writes.** An update or delete that meets a
  row committed after the transaction began fails with
  `SM_ERR_UPDATED` (`sm1/DESIGN.md` §5.4). It does not abort and
  it is not retryable, so sm2 must not treat it as an error to
  surface. It calls `sm_read_latest` on the base row, re-checks
  the statement's filter against that image, and repeats the
  write only if the row still qualifies, or skips it. The old
  image the retried write hands back is the newest version's, so
  the index entries removed are the ones that are actually
  there. An insert has no filter and needs none of this. sm1
  refuses a write repeated without the read, so an executor
  cannot forget it.
- **Online build on a populated relation**, from
  `sm1/SECINDEX.md` gap 1, the protocol `sm_lock` exists for:
  1. every writer takes `sm_lock(SHARED)` on the table's schema
     row and reads it, so what it maintains is current;
  2. the builder takes the schema row **exclusive** by updating
     it, which waits out the writers in flight and blocks new
     ones;
  3. the builder scans the base relation — its snapshot is taken
     after the lock, so it sees every committed row — fills the
     index, and commits;
  4. blocked writers then get the shared lock and read the new
     schema. At READ COMMITTED their `sm_lock` no longer fails
     on the newer commit; they read the schema row with
     `sm_read_latest`. It is the schema-row lock that keeps
     them out while the build runs, not a conflict check. At
     the higher levels `sm_lock` still fails with a conflict,
     and the writer retries.
  `sm1/test/test_lock.cc` has this end to end.
- Partial, expression and INCLUDE indexes mean maintenance takes
  a predicate and an expression evaluator, not a column list.

---

## 6. Object mapping

*To be written.* sm1 gives each relation a column family and
targets hundreds to low thousands of them; a SQL schema with
partitions and indexes passes that easily. So an sm2 object id
is a key prefix inside a shared sm1 relation, and the number of
sm1 relations is a tuning decision rather than a function of the
schema.

This interacts with `sm_rmrel`: dropping an sm2 table is then a
range delete inside a shared relation, not sm1's O(1) column
family drop. The trade has to be made explicitly.

---

## 7. Statistics

*To be written.* Statistics are not optional, because the
planner is kept. It needs `reltuples`, `relpages`, histograms and
correlations, and `ANALYZE` needs a sampling path: row counts,
sizes, and a sampling primitive from sm1. For an index-organized
table the primary key's correlation is 1.0 by construction,
which is worth reporting rather than measuring. How accurate
RocksDB's approximate statistics are for this purpose has not
been measured, and this section depends on it.

---

## 8. Known trade-offs

*To be filled in as the design settles, in the manner of
`sm1/DESIGN.md` §13 — the costs this design accepts, written
down where the next reader will find them.*

Accepted so far, from the PostgreSQL target (`PART3.md`):

- **Plans lost to the backward-scan ban.** An index-ordered
  `ORDER BY x DESC LIMIT n` and a merge join that wants
  descending order become a top-N sort over a forward scan, not
  an early-terminating index scan. `MAX()`, which PostgreSQL
  answers with a backward index scan, is expected to lose that
  plan; `MIN()` keeps its forward one. Scrollable cursors get a
  Materialize node, because `ExecSupportsBackwardScan` is
  changed to return false for every table. These lose plans, not
  correctness, and the size of the loss is expected from how the
  planner works but has not been measured.
- **Collated keys are stored twice.** The sort key is lossy, so
  the original value is stored beside it.
- **Relations are multiplexed.** DROP TABLE is a range delete
  inside a shared relation, not sm1's O(1) drop (§6).
- **The version lag is permanent.** The fork replaces the
  storage and transaction layers and makes PostgreSQL thread
  safe, so tracking upstream releases is harder than the version
  lag it already counts as a liability.
- **Not unblocked by sm2:** replication, PITR and logical
  decoding over PostgreSQL's WAL (§4).

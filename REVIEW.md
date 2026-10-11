# REVIEW — decisions made on wip-dev while you were away

Each entry: what was decided, why, and what to look at. Newest
last. Delete this file when the branch is merged.

## 1. The branch is `wip-dev`

You asked for a dev branch; `dg/CODE.md` §11 says work goes on
a `wip-*` branch, so it is `wip-dev`, made from `main` at
`7c80019` after pushing that commit.

## 2. dg's `make tsan`: clean

Run at `afe8fcd` (after the sys rename and the never-throw
change): `run` and `server.sh` both PASSED under the thread
sanitizer, with no report.

## 3. "sys" in comments became `sm_sys.h`

Comments and documents that named the module "sys" now name
`sm_sys.h`; Markdown puts it in backticks. `dg/WORKNOTE.md` was
left alone: it is a log of what was true then.

## 4. CREATE SCHEMA and DROP SCHEMA (dg only; sm had the calls)

- **sm records no owner.** `CREATE SCHEMA ... AUTHORIZATION r`
  still checks you may become `r`, then keeps nothing.
- **DROP SCHEMA ... CASCADE** drops the schema's tables, with a
  NOTICE in PostgreSQL's wording. A schema holding a collation
  still fails as in use: dg has no CREATE COLLATION path yet.
- **`public` cannot be dropped**, unlike PostgreSQL: sm builds
  it in and refuses. The error is "schema "public" cannot be
  dropped", SQLSTATE 0A000.
- **Other sessions learn of a schema** through an invalidation
  of `pg_namespace` (`CacheInvalidateCatalog`); the test was
  shown to fail without it.
- **Left out**: ALTER SCHEMA.

## 5. CREATE SEQUENCE: a sequence is an object in sm3's catalog

**Decision.** A SQL sequence is a new kind of sm3 catalog
object, backed by an sm1 sequence, rather than a bare sm1
sequence that dg names. Why: in PostgreSQL a sequence is a
relation in a schema. `nextval('s')` resolves `s` through the
search path, a sequence and a table cannot share a name, and
DROP SCHEMA must know a schema holds one. sm1's sequences are a
flat name and a counter, below the catalog (`sm.h`), and know
none of that.

- **Kind 11, `SEQUENCE`**, keyed by its oid; kind 10 is taken
  by the statistics design (`sm3/SM3_DESIGN.md` §6.9). Its value
  is one part: namespace `int8`, name `text`, start `int8`,
  increment `int8`. Its name is a `NAME` key in its namespace,
  so it shares names with tables and indexes, as in PostgreSQL.
- **Its counter is the sm1 sequence `sm3.seq.<oid>`**, made and
  dropped with the entry, in the same transaction. `nextval`
  is sm1's `nextseq` on it: not transactional, as PostgreSQL's.
- **In memory, only which namespace holds it**
  (`sequence_nsp`, with an overlay entry), as collations have:
  a namespace's drop checks emptiness against memory, which has
  every commit, where its snapshot may not. Name lookups read
  the `NAME` key in the transaction.
- **The format version stays 4**: a new kind changes no key's
  layout (`sm3/CATALOG.md` §3).
- **What SQL gets**: CREATE SEQUENCE [IF NOT EXISTS] name
  [START [WITH] n] [INCREMENT [BY] n], with n at least 1;
  `nextval`, `currval`, `lastval`; DROP SEQUENCE [IF EXISTS],
  and DROP SCHEMA ... CASCADE drops a schema's sequences.
  Refused: MINVALUE, MAXVALUE, CYCLE, CACHE, AS, OWNED BY, a
  negative increment, TEMP, `setval` and ALTER SEQUENCE (gap 9
  in `sm/PART4.md`): sm1's counters only go forward.

**Alternative not taken**: keep sequences outside sm3, named
`<nspoid>.<name>` in sm1 by dg. Smaller, but `nextval('s')`
would need its own name resolution, `s` could share a name
with a table, and DROP SCHEMA could leave orphans.

## 6. Sequences, as built: the smaller calls made on the way

- **A name lookup that misses a table and an index now reads
  sm's catalog for a sequence**, in the transaction, which it
  begins. Every unqualified name is looked up in `pg_catalog`
  first, so each statement naming a user table pays one more
  sm read there. Cheap, but it is per name per statement; if it
  shows up, keep sequence names in memory as tables' are.
- **A sequence's name is seen at the transaction's snapshot**,
  a table's from memory, which has every commit. Under
  REPEATABLE READ a sequence made by another session after the
  snapshot is not found; a table would be.
- **`currval` and `lastval` live in `tcb->sequence`** (the
  thread control block, per `dg/CODE.md` §3), not in new
  `thread_local`s. DISCARD SEQUENCES clears them.
- **DROP SEQUENCE takes no PostgreSQL lock**: sm takes an X
  lock on the sequence's catalog key, which serializes drops; a
  concurrent `nextval` sees the drop at its own snapshot.
- **Reading a sequence as a relation is refused** with
  `"q" is a sequence, which cannot be used as a relation yet`,
  raised where the relation cache finds no table
  (`relcache.c`): SELECT, DROP TABLE, CREATE INDEX and TRUNCATE
  on one all say so.
- **A value past `bigint`** fails as PostgreSQL's does at
  MAXVALUE; sm's counter would go on to 2^64.
- **New sm call `sm_cat_sequence_name`** (by oid), for
  messages and for telling a sequence's oid from a table's.

## 7. dg's `make tsan` after the sequence work: clean

`run` and `server.sh` PASSED under the thread sanitizer, with no
report, at `fd87b6e`: four sessions drawing from one sequence at
once among them.

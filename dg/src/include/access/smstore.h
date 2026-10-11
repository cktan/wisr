/*-------------------------------------------------------------------------
 *
 * smstore.h
 *	  digres's way into sm, the storage stack (../sm/include/sm.h): its
 *	  catalog of what a user creates, and its tables.
 *
 * Only smstore.c includes sm.h; the rest of the backend speaks to sm in
 * PostgreSQL's types, through here.
 *
 * IDENTIFICATION
 *	  src/include/access/smstore.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMSTORE_H
#define SMSTORE_H

#include "catalog/objectaddress.h"
#include "access/skey.h"
#include "executor/tuptable.h"
#include "storage/lockdefs.h"
#include "nodes/parsenodes.h"
#include "utils/relcache.h"

/*
 * Whether rel is sm's: every relation a user made is, and only those.
 */
#define RelationIsSm(rel) (RelationGetRelid(rel) >= FirstNormalObjectId)

/* A column of a table in sm. */
typedef struct smstore_column_t {
  const char *name;
  Oid typid; /* InvalidOid if dropped */
  int32 typmod;
  bool not_null;
  bool dropped;
} smstore_column_t;

/*
 * An index of a table in sm: its columns by place in the table, 1-based,
 * the keys first and the INCLUDE columns after, and how each key sorts.
 */
typedef struct smstore_index_t {
  Oid oid;
  const char *name;
  bool unique;
  bool usable; /* the session may scan it now (sm_tbl_index_usable) */
  int nkeys;
  int ncolumns;
  int16 *attnums;    /* ncolumns of them */
  bool *desc;        /* nkeys of them */
  bool *nulls_first; /* nkeys of them */
} smstore_index_t;

/* A table in sm, as its relation descriptor is built from it. */
typedef struct smstore_table_t {
  Oid oid;
  Oid nspoid;
  const char *name;
  int ncolumns;
  smstore_column_t *columns;
  int nindexes;
  smstore_index_t *indexes;
} smstore_table_t;

extern void smstore_statement_start(void);
extern void smstore_commit(void);
extern void smstore_abort(void);
extern void smstore_subcommit(void);
extern void smstore_subabort(void);
extern Oid smstore_namespace_oid(const char *name);
extern Oid smstore_relation_oid(const char *name, Oid nspoid);
extern Oid smstore_create_namespace(const char *name);
extern void smstore_drop_namespaces(DropStmt *stmt);
extern void smstore_analyze(List *rels, bool verbose);
extern bool smstore_estimate_size(Oid oid, double *pages, double *tuples);
extern bool smstore_estimate_index_size(Oid table, Oid index, double *pages,
                                        double *tuples);
extern ObjectAddress smstore_create_sequence(CreateSeqStmt *stmt);
extern void smstore_drop_sequences(DropStmt *stmt);
extern int64 smstore_nextval(Oid oid);
extern bool smstore_sequence_exists(Oid oid);
extern const char *smstore_sequence_name(Oid oid);
extern ObjectAddress smstore_create_table(CreateStmt *stmt);
extern void smstore_drop_tables(DropStmt *stmt);
extern ObjectAddress smstore_create_index(Oid relid, IndexStmt *stmt);
extern void smstore_drop_indexes(DropStmt *stmt);
extern void smstore_lock_table(Relation rel, LOCKMODE lockmode, bool nowait);
extern void smstore_truncate(Relation rel);
extern smstore_table_t *smstore_table_desc(Oid oid);
extern Oid smstore_index_table(Oid indexoid);
extern bool smstore_index_usable(Relation rel, Oid indexoid);
extern void smstore_insert(Relation rel, TupleTableSlot *slot);

/* A scan of a table in sm, in rowno order. */
typedef struct smstore_scan_t smstore_scan_t;

extern smstore_scan_t *smstore_beginscan(Relation rel);
extern bool smstore_getnext(smstore_scan_t *scan, TupleTableSlot *slot);
extern void smstore_endscan(smstore_scan_t *scan);
extern smstore_scan_t *smstore_index_beginscan(Relation heap, Relation index,
                                               ScanKey keys, int nkeys);
extern bool smstore_index_getnext(smstore_scan_t *scan, TupleTableSlot *slot);
extern bool smstore_index_getrowno(smstore_scan_t *scan, uint64 *rowno);
extern bool smstore_index_getentry(smstore_scan_t *scan, TupleTableSlot *slot);
extern smstore_scan_t *smstore_beginfetch(Relation rel);
extern bool smstore_fetch(smstore_scan_t *scan, uint64 rowno,
                          TupleTableSlot *slot);
extern void smstore_rowno_to_tid(uint64 rowno, ItemPointer tid);
extern uint64 smstore_tid_to_rowno(ItemPointer tid);

#endif /* SMSTORE_H */

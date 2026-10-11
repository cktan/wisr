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
#include "executor/tuptable.h"
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

/* A table in sm, as its relation descriptor is built from it. */
typedef struct smstore_table_t {
  Oid oid;
  Oid nspoid;
  const char *name;
  int ncolumns;
  smstore_column_t *columns;
} smstore_table_t;

extern void smstore_statement_start(void);
extern Oid smstore_namespace_oid(const char *name);
extern Oid smstore_relation_oid(const char *name, Oid nspoid);
extern ObjectAddress smstore_create_table(CreateStmt *stmt);
extern smstore_table_t *smstore_table_desc(Oid oid);
extern void smstore_insert(Relation rel, TupleTableSlot *slot);

/* A scan of a table in sm, in rowno order. */
typedef struct smstore_scan_t smstore_scan_t;

extern smstore_scan_t *smstore_beginscan(Relation rel);
extern bool smstore_getnext(smstore_scan_t *scan, TupleTableSlot *slot);
extern void smstore_endscan(smstore_scan_t *scan);
extern void smstore_rowno_to_tid(uint64 rowno, ItemPointer tid);
extern uint64 smstore_tid_to_rowno(ItemPointer tid);

#endif /* SMSTORE_H */

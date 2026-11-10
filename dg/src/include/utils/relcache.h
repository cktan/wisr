/*-------------------------------------------------------------------------
 *
 * relcache.h
 *	  Relation descriptor cache definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/utils/relcache.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELCACHE_H
#define RELCACHE_H

#include "access/tupdesc.h"
#include "common/relpath.h"
#include "nodes/bitmapset.h"

typedef struct RelationData *Relation;

/* ----------------
 *		RelationPtr is used in the executor to support index scans
 *		where we have to keep track of several index relations in an
 *		array.  -cim 9/10/89
 * ----------------
 */
typedef Relation *RelationPtr;

/*
 * Routines to open (lookup) and close a relcache entry
 */
#ifdef USE_ASSERT_CHECKING
extern void AssertCouldGetRelation(void);
#else
static inline void AssertCouldGetRelation(void) {}
#endif
extern Relation RelationIdGetRelation(Oid relationId);
extern bool RelationCacheKind(Oid relationId, char *kind);
extern void RelationCheckSmVersion(Oid relid, Oid table, uint64 version);
extern bool RelationGetClassForm(Oid relid, struct FormData_pg_class *form);
extern void RelationClose(Relation relation);

/*
 * Routines to compute/retrieve additional cached information
 */
extern List *RelationGetFKeyList(Relation relation);
extern List *RelationGetIndexList(Relation relation);
extern List *RelationGetStatExtList(Relation relation);
extern Oid RelationGetPrimaryKeyIndex(Relation relation, bool deferrable_ok);
extern Oid RelationGetReplicaIndex(Relation relation);
extern List *RelationGetIndexExpressions(Relation relation);
extern List *RelationGetDummyIndexExpressions(Relation relation);
extern List *RelationGetIndexPredicate(Relation relation);
extern bytea **RelationGetIndexAttOptions(Relation relation, bool copy);

/*
 * Which set of columns to return by RelationGetIndexAttrBitmap.
 */
typedef enum IndexAttrBitmapKind {
  INDEX_ATTR_BITMAP_KEY,
  INDEX_ATTR_BITMAP_PRIMARY_KEY,
  INDEX_ATTR_BITMAP_IDENTITY_KEY,
  INDEX_ATTR_BITMAP_HOT_BLOCKING,
  INDEX_ATTR_BITMAP_SUMMARIZED,
} IndexAttrBitmapKind;

extern Bitmapset *RelationGetIndexAttrBitmap(Relation relation,
                                             IndexAttrBitmapKind attrKind);


extern void RelationGetExclusionInfo(Relation indexRelation, Oid **operators,
                                     Oid **procs, uint16 **strategies);

extern void RelationInitIndexAccessInfo(Relation relation);

extern void RelationInitTableAccessMethod(Relation relation);

/*
 * Routines to support ereport() reports of relation-related errors
 */
extern int errtable(Relation rel);
extern int errtablecol(Relation rel, int attnum);
extern int errtablecolname(Relation rel, const char *colname);
extern int errtableconstraint(Relation rel, const char *conname);

/*
 * Routines for backend startup
 */
extern void RelationCacheInitialize(void);

/*
 * Routine to create a relcache entry for an about-to-be-created relation
 */
extern Relation
RelationBuildLocalRelation(const char *relname, Oid relnamespace,
                           TupleDesc tupDesc, Oid relid, Oid accessmtd,
                           RelFileNumber relfilenumber, Oid reltablespace,
                           bool shared_relation, bool mapped_relation,
                           char relpersistence, char relkind);

/*
 * Routines for flushing/rebuilding relcache entries in various scenarios
 */
extern void RelationForgetRelation(Oid rid);

extern void RelationCacheInvalidateEntry(Oid relationId);

extern void RelationCacheInvalidate(bool debug_discard);

extern void AtEOXact_RelationCache(bool isCommit);
extern void AtEOSubXact_RelationCache(bool isCommit, SubTransactionId mySubid,
                                      SubTransactionId parentSubid);

#endif /* RELCACHE_H */

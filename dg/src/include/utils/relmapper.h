/*-------------------------------------------------------------------------
 *
 * relmapper.h
 *	  Catalog-to-filenumber mapping
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/relmapper.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELMAPPER_H
#define RELMAPPER_H

#include "lib/stringinfo.h"

extern RelFileNumber RelationMapOidToFilenumber(Oid relationId, bool shared);

extern Oid RelationMapFilenumberToOid(RelFileNumber filenumber, bool shared);
extern void RelationMapUpdateMap(Oid relationId, RelFileNumber fileNumber,
                                 bool shared, bool immediate);

extern void RelationMapRemoveMapping(Oid relationId);

extern void RelationMapInvalidate(bool shared);
extern void RelationMapInvalidateAll(void);

extern void AtCCI_RelationMap(void);
extern void AtEOXact_RelationMap(bool isCommit);

extern void CheckPointRelationMap(void);

extern void RelationMapFinishBootstrap(void);

extern void RelationMapInitialize(void);
extern void RelationMapInitializePhase2(void);
extern void RelationMapInitializePhase3(void);

#endif /* RELMAPPER_H */

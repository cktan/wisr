/*-------------------------------------------------------------------------
 *
 * rewriteheap.h
 *	  Declarations for heap rewrite support functions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994-5, Regents of the University of California
 *
 * src/include/access/rewriteheap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef REWRITE_HEAP_H
#define REWRITE_HEAP_H

#include "access/htup.h"
#include "storage/itemptr.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/* struct definition is private to rewriteheap.c */
typedef struct RewriteStateData *RewriteState;

extern RewriteState begin_heap_rewrite(Relation old_heap, Relation new_heap,
                                       TransactionId oldest_xmin,
                                       TransactionId freeze_xid,
                                       MultiXactId cutoff_multi);
extern void end_heap_rewrite(RewriteState state);
extern void rewrite_heap_tuple(RewriteState state, HeapTuple old_tuple,
                               HeapTuple new_tuple);
extern bool rewrite_heap_dead_tuple(RewriteState state, HeapTuple old_tuple);

#endif /* REWRITE_HEAP_H */

/*-------------------------------------------------------------------------
 *
 * nodeMemoize.h
 *
 *
 *
 * Portions Copyright (c) 2021-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeMemoize.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEMEMOIZE_H
#define NODEMEMOIZE_H

#include "nodes/execnodes.h"

extern MemoizeState *ExecInitMemoize(Memoize *node, EState *estate, int eflags);
extern void ExecEndMemoize(MemoizeState *node);
extern void ExecReScanMemoize(MemoizeState *node);
extern double ExecEstimateCacheEntryOverheadBytes(double ntuples);

#endif /* NODEMEMOIZE_H */

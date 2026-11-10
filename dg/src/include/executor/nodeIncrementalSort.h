/*-------------------------------------------------------------------------
 *
 * nodeIncrementalSort.h
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/executor/nodeIncrementalSort.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEINCREMENTALSORT_H
#define NODEINCREMENTALSORT_H

#include "nodes/execnodes.h"

extern IncrementalSortState *
ExecInitIncrementalSort(IncrementalSort *node, EState *estate, int eflags);
extern void ExecEndIncrementalSort(IncrementalSortState *node);
extern void ExecReScanIncrementalSort(IncrementalSortState *node);

#endif /* NODEINCREMENTALSORT_H */

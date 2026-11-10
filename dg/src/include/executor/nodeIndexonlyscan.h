/*-------------------------------------------------------------------------
 *
 * nodeIndexonlyscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/executor/nodeIndexonlyscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEINDEXONLYSCAN_H
#define NODEINDEXONLYSCAN_H

#include "nodes/execnodes.h"

extern IndexOnlyScanState *ExecInitIndexOnlyScan(IndexOnlyScan *node,
                                                 EState *estate, int eflags);
extern void ExecEndIndexOnlyScan(IndexOnlyScanState *node);
extern void ExecIndexOnlyMarkPos(IndexOnlyScanState *node);
extern void ExecIndexOnlyRestrPos(IndexOnlyScanState *node);
extern void ExecReScanIndexOnlyScan(IndexOnlyScanState *node);

#endif /* NODEINDEXONLYSCAN_H */

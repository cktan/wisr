/*-------------------------------------------------------------------------
 *
 * nodeForeignscan.h
 *
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/executor/nodeForeignscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEFOREIGNSCAN_H
#define NODEFOREIGNSCAN_H

#include "nodes/execnodes.h"

extern ForeignScanState *ExecInitForeignScan(ForeignScan *node, EState *estate,
                                             int eflags);
extern void ExecEndForeignScan(ForeignScanState *node);
extern void ExecReScanForeignScan(ForeignScanState *node);

extern void ExecShutdownForeignScan(ForeignScanState *node);

extern void ExecAsyncForeignScanRequest(AsyncRequest *areq);
extern void ExecAsyncForeignScanConfigureWait(AsyncRequest *areq);
extern void ExecAsyncForeignScanNotify(AsyncRequest *areq);

#endif /* NODEFOREIGNSCAN_H */

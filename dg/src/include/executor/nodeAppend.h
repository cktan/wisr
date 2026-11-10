/*-------------------------------------------------------------------------
 *
 * nodeAppend.h
 *
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/executor/nodeAppend.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NODEAPPEND_H
#define NODEAPPEND_H

#include "nodes/execnodes.h"

extern AppendState *ExecInitAppend(Append *node, EState *estate, int eflags);
extern void ExecEndAppend(AppendState *node);
extern void ExecReScanAppend(AppendState *node);

extern void ExecAsyncAppendResponse(AsyncRequest *areq);

#endif /* NODEAPPEND_H */

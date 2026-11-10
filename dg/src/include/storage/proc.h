/*-------------------------------------------------------------------------
 *
 * proc.h
 *	  per-process shared memory data structures
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/storage/proc.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PROC_H_
#define _PROC_H_

#include "storage/latch.h"
#include "storage/lock.h"
#include "storage/procnumber.h"
#include "storage/procsignal.h"

/* configurable options */

/*
 * Function Prototypes
 */
extern void InitProcess(void);
extern void InitProcessPhase2(void);

extern void ProcReleaseLocks(bool isCommit);

#endif /* _PROC_H_ */

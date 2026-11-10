/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*-------------------------------------------------------------------------
 *
 * smindex.h
 *	  The index access method of an index in sm (smindex.c).
 *
 * IDENTIFICATION
 *	  src/include/access/smindex.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMINDEX_H
#define SMINDEX_H

#include "access/amapi.h"

extern IndexAmRoutine *smindex_routine(void);

#endif /* SMINDEX_H */

/*-------------------------------------------------------------------------
 *
 * nbtree.h
 *	  What is left of PostgreSQL's btree access method: its operator
 *	  classes' strategy and support function numbers, which the indexes in
 *	  sm (access/smindex.h), sorts and the planner use.  The access method
 *	  itself went with the heap: an index is sm's.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/nbtree.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef NBTREE_H
#define NBTREE_H

#include "access/stratnum.h"
#include "catalog/pg_index.h"

/*
 *	Operator strategy numbers for B-tree have been moved to
 *access/stratnum.h, because many places need to use them in ScanKeyInit()
 *calls.
 *
 *	The strategy numbers are chosen so that we can commute them by
 *	subtraction, thus:
 */
#define BTCommuteStrategyNumber(strat)	(BTMaxStrategyNumber + 1 - (strat))

/*
 *	When a new operator class is declared, we require that the user
 *	supply us with an amproc procedure (BTORDER_PROC) for determining
 *	whether, for two keys a and b, a < b, a = b, or a > b.  This routine
 *	must return < 0, 0, > 0, respectively, in these three cases.
 *
 *	To facilitate accelerated sorting, an operator class may choose to
 *	offer a second procedure (BTSORTSUPPORT_PROC).  For full details, see
 *	src/include/utils/sortsupport.h.
 *
 *	To support window frames defined by "RANGE offset PRECEDING/FOLLOWING",
 *	an operator class may choose to offer a third amproc procedure
 *	(BTINRANGE_PROC), independently of whether it offers sortsupport.
 *	For full details, see doc/src/sgml/btree.sgml.
 *
 *	To facilitate B-Tree deduplication, an operator class may choose to
 *	offer a forth amproc procedure (BTEQUALIMAGE_PROC).  For full details,
 *	see doc/src/sgml/btree.sgml.
 *
 *	An operator class may choose to offer a fifth amproc procedure
 *	(BTOPTIONS_PROC).  These procedures define a set of user-visible
 *	parameters that can be used to control operator class behavior.  None of
 *	the built-in B-Tree operator classes currently register an "options"
 *proc.
 *
 *	To facilitate more efficient B-Tree skip scans, an operator class may
 *	choose to offer a sixth amproc procedure (BTSKIPSUPPORT_PROC).  For full
 *	details, see src/include/utils/skipsupport.h.
 */

#define BTORDER_PROC		1
#define BTSORTSUPPORT_PROC	2
#define BTINRANGE_PROC		3
#define BTEQUALIMAGE_PROC	4
#define BTOPTIONS_PROC		5
#define BTSKIPSUPPORT_PROC	6
#define BTNProcs			6

/* Remaps pg_index flag bits to the uppermost byte of a scan key's flags. */
#define SK_BT_INDOPTION_SHIFT  24
#define SK_BT_DESC			(INDOPTION_DESC << SK_BT_INDOPTION_SHIFT)
#define SK_BT_NULLS_FIRST	(INDOPTION_NULLS_FIRST << SK_BT_INDOPTION_SHIFT)

#endif /* NBTREE_H */

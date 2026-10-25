/*-------------------------------------------------------------------------
 *
 * rowkey.h
 *	  A row's name in a table in sm, where PostgreSQL named it by its TID
 *	  (../../../../sm/ROWKEY.md section 1).
 *
 * A row is named by its key: the values of its table's primary key's
 * columns, in the key's order, or in a table without one its `_id` alone.
 * The executor carries the key's columns where PostgreSQL carried ctid, and
 * the table access method finds the row by them.  A value by reference
 * points into memory its holder keeps; RowKeyCopy() makes a copy that
 * outlives it.
 *
 * src/include/access/rowkey.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ROWKEY_H
#define ROWKEY_H

#include "executor/tuptable.h"
#include "pg_config_manual.h"
#include "utils/relcache.h"

typedef struct RowKey
{
	int			nkeys;
	Datum		values[INDEX_MAX_KEYS];
	bool		isnull[INDEX_MAX_KEYS];
} RowKey;

extern int	RowKeyAttnums(Relation rel, AttrNumber *attnums);
extern const char *RowKeyColumnName(Relation rel, int i, AttrNumber attnum);
extern void RowKeyFromSlot(Relation rel, TupleTableSlot *slot, RowKey *key);
extern void RowKeyCopy(Relation rel, const RowKey *src, RowKey *dst);

#endif							/* ROWKEY_H */

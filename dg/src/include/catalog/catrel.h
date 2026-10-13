/*-------------------------------------------------------------------------
 *
 * catrel.h
 *	  The catalog relations: catalogs a query can read (SYSCATALOG.md).
 *
 * src/include/catalog/catrel.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef CATREL_H
#define CATREL_H

#include "access/tableam.h"

/* The table access method of a catalog relation (catalog/catrel.c). */
extern const TableAmRoutine *catrel_routine(void);

#endif /* CATREL_H */

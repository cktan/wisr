/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

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
#include "nodes/execnodes.h"

/* pg_roles's oid: a view in PostgreSQL, with none fixed (gen_cat.py). */
#define CATREL_PG_ROLES 12100
/* pg_views's, likewise. */
#define CATREL_PG_VIEWS 12101

/* The table access method of a catalog relation (catalog/catrel.c). */
extern const TableAmRoutine *catrel_routine(void);

/* Whether a catalog relation has the rows compiled in alone, none of sm's. */
extern bool catrel_is_builtin(Oid relid);
/* Whether a catalog relation has pg_global's rows, the roles'. */
extern bool catrel_is_global(Oid relid);

/* A qual a scan of a catalog relation is narrowed by: column = value. */
typedef struct catrel_key_t {
  AttrNumber attno;
  ExprState *value;
} catrel_key_t;

extern List *catrel_scan_keys(Relation rel, List *qual, Index scanrelid,
                              PlanState *parent);
extern TableScanDesc catrel_beginscan(Relation rel, Snapshot snapshot,
                                      List *keys, ExprContext *econtext);

#endif /* CATREL_H */

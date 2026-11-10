/*-------------------------------------------------------------------------
 *
 * indexing.c
 *	  Writing and scanning a system catalog's table: refused.
 *
 * digres has no catalog tables: what is built in is compiled in
 * (catalog/gen_cat.py), and a table's definition is sm's.  These entry
 * points are left for the code that still calls them, which goes with the
 * heap's other callers; each raises the error opening a catalog table
 * would have.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/catalog/indexing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/tableam.h"
#include "catalog/indexing.h"
#include "utils/rel.h"

/* Refuse a write to, or scan of, rel, a catalog table. */
static void refuse(Relation rel) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("relation %u cannot be written: digres has no catalog "
                         "tables",
                         RelationGetRelid(rel))));
}

/* Open the indexes of a catalog table: refused. */
CatalogIndexState CatalogOpenIndexes(Relation heapRel) {
  refuse(heapRel);
  return NULL; /* not reached */
}

/* Close what CatalogOpenIndexes() opened, which is nothing. */
void CatalogCloseIndexes(CatalogIndexState indstate) {}

/* Insert into a catalog table: refused. */
void CatalogTupleInsert(Relation heapRel, HeapTuple tup) { refuse(heapRel); }

/* Insert into a catalog table, its indexes open: refused. */
void CatalogTupleInsertWithInfo(Relation heapRel, HeapTuple tup,
                                CatalogIndexState indstate) {
  refuse(heapRel);
}

/* Insert rows into a catalog table: refused. */
void CatalogTuplesMultiInsertWithInfo(Relation heapRel, TupleTableSlot **slot,
                                      int ntuples, CatalogIndexState indstate) {
  refuse(heapRel);
}

/* Update a row of a catalog table: refused. */
void CatalogTupleUpdate(Relation heapRel, ItemPointer otid, HeapTuple tup) {
  refuse(heapRel);
}

/* Update a row of a catalog table, its indexes open: refused. */
void CatalogTupleUpdateWithInfo(Relation heapRel, ItemPointer otid,
                                HeapTuple tup, CatalogIndexState indstate) {
  refuse(heapRel);
}

/* Delete a row of a catalog table: refused. */
void CatalogTupleDelete(Relation heapRel, ItemPointer tid) { refuse(heapRel); }

/* The next row of a scan of a catalog table: refused. */
HeapTuple catalog_scan_next(TableScanDesc scan) {
  refuse(scan->rs_rd);
  return NULL; /* not reached */
}

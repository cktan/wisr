/*-------------------------------------------------------------------------
 *
 * relscan.h
 *	  POSTGRES relation scan descriptor definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/access/relscan.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef RELSCAN_H
#define RELSCAN_H

#include "access/htup_details.h"
#include "access/itup.h"
#include "nodes/tidbitmap.h"
#include "storage/buf.h"
#include "storage/relfilelocator.h"
#include "utils/relcache.h"

/*
 * Generic descriptor for table scans. This is the base-class for table scans,
 * which needs to be embedded in the scans of individual AMs.
 */
typedef struct TableScanDescData {
  /* scan parameters */
  Relation rs_rd;                   /* heap relation descriptor */
  struct SnapshotData *rs_snapshot; /* snapshot to see */
  int rs_nkeys;                     /* number of scan keys */
  struct ScanKeyData *rs_key;       /* array of scan key descriptors */

  /*
   * Scan type-specific members
   */
  union {
    /* Iterator for Bitmap Table Scans */
    TBMIterator rs_tbmiterator;
  } st;

  /*
   * Information about type and behaviour of the scan, a bitmask of members
   * of the ScanOptions enum (see tableam.h).
   */
  uint32 rs_flags;

} TableScanDescData;
typedef struct TableScanDescData *TableScanDesc;

/*
 * Base class for fetches from a table via an index. This is the base-class
 * for such scans, which needs to be embedded in the respective struct for
 * individual AMs.
 */
typedef struct IndexFetchTableData {
  Relation rel;
} IndexFetchTableData;

struct IndexScanInstrumentation;

/*
 * We use the same IndexScanDescData structure for both amgettuple-based
 * and amgetbitmap-based index scans.  Some fields are only relevant in
 * amgettuple-based scans.
 */
typedef struct IndexScanDescData {
  /* scan parameters */
  Relation heapRelation;            /* heap relation descriptor, or NULL */
  Relation indexRelation;           /* index relation descriptor */
  struct SnapshotData *xs_snapshot; /* snapshot to see */
  int numberOfKeys;                 /* number of index qualifier conditions */
  int numberOfOrderBys;             /* number of ordering operators */
  struct ScanKeyData *keyData;      /* array of index qualifier descriptors */
  struct ScanKeyData *orderByData;  /* array of ordering op descriptors */
  bool xs_want_itup;                /* caller requests index tuples */
  bool xs_temp_snap;                /* unregister snapshot at scan end? */

  /* signaling to index AM about killing index tuples */
  bool kill_prior_tuple;     /* last-returned tuple is dead */
  bool ignore_killed_tuples; /* do not return killed entries */

  /* index access method's private state */
  void *opaque; /* access-method-specific info */

  /*
   * Instrumentation counters maintained by all index AMs during both
   * amgettuple calls and amgetbitmap calls (unless field remains NULL)
   */
  struct IndexScanInstrumentation *instrument;

  /*
   * In an index-only scan, a successful amgettuple call must fill either
   * xs_itup (and xs_itupdesc) or xs_hitup (and xs_hitupdesc) to provide the
   * data returned by the scan.  It can fill both, in which case the heap
   * format will be used.
   */
  IndexTuple xs_itup;                /* index tuple returned by AM */
  struct TupleDescData *xs_itupdesc; /* rowtype descriptor of xs_itup */
  HeapTuple xs_hitup; /* index data returned by AM, as HeapTuple */
  struct TupleDescData *xs_hitupdesc; /* rowtype descriptor of xs_hitup */

  ItemPointerData xs_heaptid; /* result */
  bool xs_heap_continue;      /* T if must keep walking, potential
                               * further results */
  IndexFetchTableData *xs_heapfetch;

  bool xs_recheck; /* T means scan keys must be rechecked */

  /*
   * When fetching with an ordering operator, the values of the ORDER BY
   * expressions of the last returned tuple, according to the index.  If
   * xs_recheckorderby is true, these need to be rechecked just like the
   * scan keys, and the values returned here are a lower-bound on the actual
   * values.
   */
  Datum *xs_orderbyvals;
  bool *xs_orderbynulls;
  bool xs_recheckorderby;

} IndexScanDescData;

struct TupleTableSlot;

/* Struct for storage-or-index scans of system tables */
typedef struct SysScanDescData {
  Relation heap_rel;               /* catalog being scanned */
  Relation irel;                   /* NULL if doing heap scan */
  struct TableScanDescData *scan;  /* only valid in storage-scan case */
  struct IndexScanDescData *iscan; /* only valid in index-scan case */
  struct SnapshotData *snapshot;   /* snapshot to unregister at end of scan */
  struct TupleTableSlot *slot;
  struct HeapTupleData *copy; /* the row last read, if copied out of slot */
} SysScanDescData;

#endif /* RELSCAN_H */

/*-------------------------------------------------------------------------
 *
 * toast_internals.c
 *	  Functions for internal use by the TOAST system.
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/common/toast_internals.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/relation.h"
#include "access/tableam.h"
#include "access/genam.h"
#include "access/heaptoast.h"
#include "access/table.h"
#include "access/toast_internals.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "miscadmin.h"
#include "utils/fmgroids.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"

/* ----------
 * toast_compress_datum -
 *
 *	Create a compressed version of a varlena datum
 *
 *	If we fail (ie, compressed result is actually bigger than original)
 *	then return NULL.  We must not use compressed data if it'd expand
 *	the tuple!
 *
 *	We use VAR{SIZE,DATA}_ANY so we can handle short varlenas here without
 *	copying them.  But we can't handle external or compressed datums.
 * ----------
 */
Datum toast_compress_datum(Datum value, char cmethod) {
  struct varlena *tmp = NULL;
  int32 valsize;
  ToastCompressionId cmid = TOAST_INVALID_COMPRESSION_ID;

  Assert(!VARATT_IS_EXTERNAL(DatumGetPointer(value)));
  Assert(!VARATT_IS_COMPRESSED(DatumGetPointer(value)));

  valsize = VARSIZE_ANY_EXHDR(DatumGetPointer(value));

  /* If the compression method is not valid, use the current default */
  if (!CompressionMethodIsValid(cmethod))
    cmethod = default_toast_compression;

  /*
   * Call appropriate compression routine for the compression method.
   */
  switch (cmethod) {
  case TOAST_PGLZ_COMPRESSION:
    tmp = pglz_compress_datum((const struct varlena *)value);
    cmid = TOAST_PGLZ_COMPRESSION_ID;
    break;
  case TOAST_LZ4_COMPRESSION:
    tmp = lz4_compress_datum((const struct varlena *)value);
    cmid = TOAST_LZ4_COMPRESSION_ID;
    break;
  default:
    elog(ERROR, "invalid compression method %c", cmethod);
  }

  if (tmp == NULL)
    return PointerGetDatum(NULL);

  /*
   * We recheck the actual size even if compression reports success, because
   * it might be satisfied with having saved as little as one byte in the
   * compressed data --- which could turn into a net loss once you consider
   * header and alignment padding.  Worst case, the compressed format might
   * require three padding bytes (plus header, which is included in
   * VARSIZE(tmp)), whereas the uncompressed format would take only one
   * header byte and no padding if the value is short enough.  So we insist
   * on a savings of more than 2 bytes to ensure we have a gain.
   */
  if (VARSIZE(tmp) < valsize - 2) {
    /* successful compression */
    Assert(cmid != TOAST_INVALID_COMPRESSION_ID);
    TOAST_COMPRESS_SET_SIZE_AND_COMPRESS_METHOD(tmp, valsize, cmid);
    return PointerGetDatum(tmp);
  } else {
    /* incompressible data */
    pfree(tmp);
    return PointerGetDatum(NULL);
  }
}

/* ----------
 * toast_get_valid_index
 *
 *	Get OID of valid index associated to given toast relation. A toast
 *	relation can have only one valid index at the same time.
 */
Oid toast_get_valid_index(Oid toastoid, LOCKMODE lock) {
  int num_indexes;
  int validIndex;
  Oid validIndexOid;
  Relation *toastidxs;
  Relation toastrel;

  /* Open the toast relation */
  toastrel = table_open(toastoid, lock);

  /* Look for the valid index of the toast relation */
  validIndex = toast_open_indexes(toastrel, lock, &toastidxs, &num_indexes);
  validIndexOid = RelationGetRelid(toastidxs[validIndex]);

  /* Close the toast relation and all its indexes */
  toast_close_indexes(toastidxs, num_indexes, NoLock);
  table_close(toastrel, NoLock);

  return validIndexOid;
}

/* ----------
 * toast_open_indexes
 *
 *	Get an array of the indexes associated to the given toast relation
 *	and return as well the position of the valid index used by the toast
 *	relation in this array. It is the responsibility of the caller of this
 *	function to close the indexes as well as free them.
 */
int toast_open_indexes(Relation toastrel, LOCKMODE lock, Relation **toastidxs,
                       int *num_indexes) {
  int i = 0;
  int res = 0;
  bool found = false;
  List *indexlist;
  ListCell *lc;

  /* Get index list of the toast relation */
  indexlist = RelationGetIndexList(toastrel);
  Assert(indexlist != NIL);

  *num_indexes = list_length(indexlist);

  /* Open all the index relations */
  *toastidxs = (Relation *)palloc(*num_indexes * sizeof(Relation));
  foreach (lc, indexlist)
    (*toastidxs)[i++] = index_open(lfirst_oid(lc), lock);

  /* Fetch the first valid index in list */
  for (i = 0; i < *num_indexes; i++) {
    Relation toastidx = (*toastidxs)[i];

    if (toastidx->rd_index->indisvalid) {
      res = i;
      found = true;
      break;
    }
  }

  /*
   * Free index list, not necessary anymore as relations are opened and a
   * valid index has been found.
   */
  list_free(indexlist);

  /*
   * The toast relation should have one valid index, so something is going
   * wrong if there is nothing.
   */
  if (!found)
    elog(ERROR, "no valid index found for toast relation with Oid %u",
         RelationGetRelid(toastrel));

  return res;
}

/* ----------
 * toast_close_indexes
 *
 *	Close an array of indexes for a toast relation and free it. This should
 *	be called for a set of indexes opened previously with
 *toast_open_indexes.
 */
void toast_close_indexes(Relation *toastidxs, int num_indexes, LOCKMODE lock) {
  int i;

  /* Close relations and clean up things */
  for (i = 0; i < num_indexes; i++)
    index_close(toastidxs[i], lock);
  pfree(toastidxs);
}

/* ----------
 * get_toast_snapshot
 *
 *	Return the TOAST snapshot. Detoasting *must* happen in the same
 *	transaction that originally fetched the toast pointer.
 */
Snapshot get_toast_snapshot(void) {
  /*
   * We cannot directly check that detoasting happens in the same
   * transaction that originally fetched the toast pointer, but at least
   * check that the session has some active snapshots. It might not if, for
   * example, a procedure fetches a toasted value into a local variable,
   * commits, and then tries to detoast the value. Such coding is unsafe,
   * because once we commit there is nothing to prevent the toast data from
   * being deleted. (This is not very much protection, because in many
   * scenarios the procedure would have already created a new transaction
   * snapshot, preventing us from detecting the problem. But it's better
   * than nothing.)
   */
  if (!HaveRegisteredOrActiveSnapshot())
    elog(ERROR, "cannot fetch toast data without an active snapshot");

  return &SnapshotToastData;
}

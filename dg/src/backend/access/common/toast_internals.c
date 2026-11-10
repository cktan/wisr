/*-------------------------------------------------------------------------
 *
 * toast_internals.c
 *	  Functions for internal use by the TOAST system.
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * IDENTIFICATION
 *	  src/backend/access/common/toast_internals.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/heaptoast.h"
#include "access/toast_internals.h"
#include "miscadmin.h"

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

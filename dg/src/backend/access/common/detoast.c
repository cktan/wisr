/*-------------------------------------------------------------------------
 *
 * detoast.c
 *	  Retrieve compressed or external variable size attributes.
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * IDENTIFICATION
 *	  src/backend/access/common/detoast.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/smstore.h"
#include "access/toast_internals.h"
#include "common/int.h"
#include "common/pg_lzcompress.h"
#include "utils/expandeddatum.h"

static struct varlena *toast_decompress_datum(struct varlena *attr);
static struct varlena *toast_decompress_datum_slice(struct varlena *attr,
                                                    int32 slicelength);

/* ----------
 * detoast_external_attr -
 *
 *	Public entry point to get back a toasted value from
 *	external source (possibly still in compressed format).
 *
 * This will return a datum that contains all the data internally, ie, not
 * relying on external storage or memory, but it can still be compressed or
 * have a short header.  Note some callers assume that if the input is an
 * EXTERNAL datum, the result will be a pfree'able chunk.
 * ----------
 */
struct varlena *detoast_external_attr(struct varlena *attr) {
  struct varlena *result;

  if (VARATT_IS_EXTERNAL_SM(attr)) {
    /* A value sm stored out of line: read it from its table */
    result = smstore_fetch_toast(attr);
  } else if (VARATT_IS_EXTERNAL_INDIRECT(attr)) {
    /*
     * This is an indirect pointer --- dereference it
     */
    struct varatt_indirect redirect;

    VARATT_EXTERNAL_GET_POINTER(redirect, attr);
    attr = (struct varlena *)redirect.pointer;

    /* nested indirect Datums aren't allowed */
    Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

    /* recurse if value is still external in some other way */
    if (VARATT_IS_EXTERNAL(attr))
      return detoast_external_attr(attr);

    /*
     * Copy into the caller's memory context, in case caller tries to
     * pfree the result.
     */
    result = (struct varlena *)palloc(VARSIZE_ANY(attr));
    memcpy(result, attr, VARSIZE_ANY(attr));
  } else if (VARATT_IS_EXTERNAL_EXPANDED(attr)) {
    /*
     * This is an expanded-object pointer --- get flat format
     */
    ExpandedObjectHeader *eoh;
    Size resultsize;

    eoh = DatumGetEOHP(PointerGetDatum(attr));
    resultsize = EOH_get_flat_size(eoh);
    result = (struct varlena *)palloc(resultsize);
    EOH_flatten_into(eoh, result, resultsize);
  } else {
    /*
     * This is a plain value inside of the main tuple - why am I called?
     */
    result = attr;
  }

  return result;
}

/* ----------
 * detoast_attr -
 *
 *	Public entry point to get back a toasted value from compression
 *	or external storage.  The result is always non-extended varlena form.
 *
 * Note some callers assume that if the input is an EXTERNAL or COMPRESSED
 * datum, the result will be a pfree'able chunk.
 * ----------
 */
struct varlena *detoast_attr(struct varlena *attr) {
  if (VARATT_IS_EXTERNAL_SM(attr)) {
    /* A value sm stored out of line, never compressed: read it */
    attr = smstore_fetch_toast(attr);
  } else if (VARATT_IS_EXTERNAL_INDIRECT(attr)) {
    /*
     * This is an indirect pointer --- dereference it
     */
    struct varatt_indirect redirect;

    VARATT_EXTERNAL_GET_POINTER(redirect, attr);
    attr = (struct varlena *)redirect.pointer;

    /* nested indirect Datums aren't allowed */
    Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

    /* recurse in case value is still extended in some other way */
    attr = detoast_attr(attr);

    /* if it isn't, we'd better copy it */
    if (attr == (struct varlena *)redirect.pointer) {
      struct varlena *result;

      result = (struct varlena *)palloc(VARSIZE_ANY(attr));
      memcpy(result, attr, VARSIZE_ANY(attr));
      attr = result;
    }
  } else if (VARATT_IS_EXTERNAL_EXPANDED(attr)) {
    /*
     * This is an expanded-object pointer --- get flat format
     */
    attr = detoast_external_attr(attr);
    /* flatteners are not allowed to produce compressed/short output */
    Assert(!VARATT_IS_EXTENDED(attr));
  } else if (VARATT_IS_COMPRESSED(attr)) {
    /*
     * This is a compressed value inside of the main tuple
     */
    attr = toast_decompress_datum(attr);
  } else if (VARATT_IS_SHORT(attr)) {
    /*
     * This is a short-header varlena --- convert to 4-byte header format
     */
    Size data_size = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT;
    Size new_size = data_size + VARHDRSZ;
    struct varlena *new_attr;

    new_attr = (struct varlena *)palloc(new_size);
    SET_VARSIZE(new_attr, new_size);
    memcpy(VARDATA(new_attr), VARDATA_SHORT(attr), data_size);
    attr = new_attr;
  }

  return attr;
}

/* ----------
 * detoast_attr_slice -
 *
 *		Public entry point to get back part of a toasted value
 *		from compression or external storage.
 *
 * sliceoffset is where to start (zero or more)
 * If slicelength < 0, return everything beyond sliceoffset
 * ----------
 */
struct varlena *detoast_attr_slice(struct varlena *attr, int32 sliceoffset,
                                   int32 slicelength) {
  struct varlena *preslice;
  struct varlena *result;
  char *attrdata;
  int32 slicelimit;
  int32 attrsize;

  if (sliceoffset < 0)
    elog(ERROR, "invalid sliceoffset: %d", sliceoffset);

  /*
   * Compute slicelimit = offset + length, or -1 if we must fetch all of the
   * value.  In case of integer overflow, we must fetch all.
   */
  if (slicelength < 0)
    slicelimit = -1;
  else if (pg_add_s32_overflow(sliceoffset, slicelength, &slicelimit))
    slicelength = slicelimit = -1;

  if (VARATT_IS_EXTERNAL_SM(attr)) {
    /* sm reads no part of a value: the whole of it, then the slice */
    preslice = smstore_fetch_toast(attr);
  } else if (VARATT_IS_EXTERNAL_INDIRECT(attr)) {
    struct varatt_indirect redirect;

    VARATT_EXTERNAL_GET_POINTER(redirect, attr);

    /* nested indirect Datums aren't allowed */
    Assert(!VARATT_IS_EXTERNAL_INDIRECT(redirect.pointer));

    return detoast_attr_slice(redirect.pointer, sliceoffset, slicelength);
  } else if (VARATT_IS_EXTERNAL_EXPANDED(attr)) {
    /* pass it off to detoast_external_attr to flatten */
    preslice = detoast_external_attr(attr);
  } else
    preslice = attr;

  Assert(!VARATT_IS_EXTERNAL(preslice));

  if (VARATT_IS_COMPRESSED(preslice)) {
    struct varlena *tmp = preslice;

    /* Decompress enough to encompass the slice and the offset */
    if (slicelimit >= 0)
      preslice = toast_decompress_datum_slice(tmp, slicelimit);
    else
      preslice = toast_decompress_datum(tmp);

    if (tmp != attr)
      pfree(tmp);
  }

  if (VARATT_IS_SHORT(preslice)) {
    attrdata = VARDATA_SHORT(preslice);
    attrsize = VARSIZE_SHORT(preslice) - VARHDRSZ_SHORT;
  } else {
    attrdata = VARDATA(preslice);
    attrsize = VARSIZE(preslice) - VARHDRSZ;
  }

  /* slicing of datum for compressed cases and plain value */

  if (sliceoffset >= attrsize) {
    sliceoffset = 0;
    slicelength = 0;
  } else if (slicelength < 0 || slicelimit > attrsize)
    slicelength = attrsize - sliceoffset;

  result = (struct varlena *)palloc(slicelength + VARHDRSZ);
  SET_VARSIZE(result, slicelength + VARHDRSZ);

  memcpy(VARDATA(result), attrdata + sliceoffset, slicelength);

  if (preslice != attr)
    pfree(preslice);

  return result;
}

/* ----------
 * toast_decompress_datum -
 *
 * Decompress a compressed version of a varlena datum
 */
static struct varlena *toast_decompress_datum(struct varlena *attr) {
  ToastCompressionId cmid;

  Assert(VARATT_IS_COMPRESSED(attr));

  /*
   * Fetch the compression method id stored in the compression header and
   * decompress the data using the appropriate decompression routine.
   */
  cmid = (ToastCompressionId)TOAST_COMPRESS_METHOD(attr);
  switch (cmid) {
  case TOAST_PGLZ_COMPRESSION_ID:
    return pglz_decompress_datum(attr);
  case TOAST_LZ4_COMPRESSION_ID:
    return lz4_decompress_datum(attr);
  default:
    elog(ERROR, "invalid compression method id %d", cmid);
    return NULL; /* keep compiler quiet */
  }
}

/* ----------
 * toast_decompress_datum_slice -
 *
 * Decompress the front of a compressed version of a varlena datum.
 * offset handling happens in detoast_attr_slice.
 * Here we just decompress a slice from the front.
 */
static struct varlena *toast_decompress_datum_slice(struct varlena *attr,
                                                    int32 slicelength) {
  ToastCompressionId cmid;

  Assert(VARATT_IS_COMPRESSED(attr));

  /*
   * Some callers may pass a slicelength that's more than the actual
   * decompressed size.  If so, just decompress normally.  This avoids
   * possibly allocating a larger-than-necessary result object, and may be
   * faster and/or more robust as well.  Notably, some versions of liblz4
   * have been seen to give wrong results if passed an output size that is
   * more than the data's true decompressed size.
   */
  if ((uint32)slicelength >= TOAST_COMPRESS_EXTSIZE(attr))
    return toast_decompress_datum(attr);

  /*
   * Fetch the compression method id stored in the compression header and
   * decompress the data slice using the appropriate decompression routine.
   */
  cmid = (ToastCompressionId)TOAST_COMPRESS_METHOD(attr);
  switch (cmid) {
  case TOAST_PGLZ_COMPRESSION_ID:
    return pglz_decompress_datum_slice(attr, slicelength);
  case TOAST_LZ4_COMPRESSION_ID:
    return lz4_decompress_datum_slice(attr, slicelength);
  default:
    elog(ERROR, "invalid compression method id %d", cmid);
    return NULL; /* keep compiler quiet */
  }
}

/* ----------
 * toast_raw_datum_size -
 *
 *	Return the raw (detoasted) size of a varlena datum
 *	(including the VARHDRSZ header)
 * ----------
 */
Size toast_raw_datum_size(Datum value) {
  struct varlena *attr = (struct varlena *)DatumGetPointer(value);
  Size result;

  if (VARATT_IS_EXTERNAL_SM(attr)) {
    /* sm's pointer gives the value's length, its header left out */
    result = smstore_toast_size(attr) + VARHDRSZ;
  } else if (VARATT_IS_EXTERNAL_INDIRECT(attr)) {
    struct varatt_indirect toast_pointer;

    VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

    /* nested indirect Datums aren't allowed */
    Assert(!VARATT_IS_EXTERNAL_INDIRECT(toast_pointer.pointer));

    return toast_raw_datum_size(PointerGetDatum(toast_pointer.pointer));
  } else if (VARATT_IS_EXTERNAL_EXPANDED(attr)) {
    result = EOH_get_flat_size(DatumGetEOHP(value));
  } else if (VARATT_IS_COMPRESSED(attr)) {
    /* here, va_rawsize is just the payload size */
    result = VARDATA_COMPRESSED_GET_EXTSIZE(attr) + VARHDRSZ;
  } else if (VARATT_IS_SHORT(attr)) {
    /*
     * we have to normalize the header length to VARHDRSZ or else the
     * callers of this function will be confused.
     */
    result = VARSIZE_SHORT(attr) - VARHDRSZ_SHORT + VARHDRSZ;
  } else {
    /* plain untoasted datum */
    result = VARSIZE(attr);
  }
  return result;
}

/* ----------
 * toast_datum_size
 *
 *	Return the physical storage size (possibly compressed) of a varlena
 *datum
 * ----------
 */
Size toast_datum_size(Datum value) {
  struct varlena *attr = (struct varlena *)DatumGetPointer(value);
  Size result;

  if (VARATT_IS_EXTERNAL_SM(attr)) {
    /* Stored as it is, uncompressed */
    result = smstore_toast_size(attr);
  } else if (VARATT_IS_EXTERNAL_INDIRECT(attr)) {
    struct varatt_indirect toast_pointer;

    VARATT_EXTERNAL_GET_POINTER(toast_pointer, attr);

    /* nested indirect Datums aren't allowed */
    Assert(!VARATT_IS_EXTERNAL_INDIRECT(attr));

    return toast_datum_size(PointerGetDatum(toast_pointer.pointer));
  } else if (VARATT_IS_EXTERNAL_EXPANDED(attr)) {
    result = EOH_get_flat_size(DatumGetEOHP(value));
  } else if (VARATT_IS_SHORT(attr)) {
    result = VARSIZE_SHORT(attr);
  } else {
    /*
     * Attribute is stored inline either compressed or not, just calculate
     * the size of the datum in either case.
     */
    result = VARSIZE(attr);
  }
  return result;
}

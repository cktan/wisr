/*-------------------------------------------------------------------------
 *
 * heaptoast.c
 *	  Flattening a HeapTuple's out-of-line values into it.
 *
 * What is left of the heap's TOAST code: a tuple built in memory, a
 * composite value's, may hold a value out of line, and these read it back
 * in.  The heap's TOAST storage went with the heap.
 *
 * Copyright (c) 2000-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/common/heaptoast.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/detoast.h"
#include "access/heaptoast.h"

/* ----------
 * toast_flatten_tuple -
 *
 *	"Flatten" a tuple to contain no out-of-line toasted fields.
 *	(This does not eliminate compressed or short-header datums.)
 *
 *	Note: we expect the caller already checked HeapTupleHasExternal(tup),
 *	so there is no need for a short-circuit path.
 * ----------
 */
HeapTuple toast_flatten_tuple(HeapTuple tup, TupleDesc tupleDesc) {
  HeapTuple new_tuple;
  int numAttrs = tupleDesc->natts;
  int i;
  Datum toast_values[MaxTupleAttributeNumber];
  bool toast_isnull[MaxTupleAttributeNumber];
  bool toast_free[MaxTupleAttributeNumber];

  /*
   * Break down the tuple into fields.
   */
  Assert(numAttrs <= MaxTupleAttributeNumber);
  heap_deform_tuple(tup, tupleDesc, toast_values, toast_isnull);

  memset(toast_free, 0, numAttrs * sizeof(bool));

  for (i = 0; i < numAttrs; i++) {
    /*
     * Look at non-null varlena attributes
     */
    if (!toast_isnull[i] && TupleDescCompactAttr(tupleDesc, i)->attlen == -1) {
      struct varlena *new_value;

      new_value = (struct varlena *)DatumGetPointer(toast_values[i]);
      if (VARATT_IS_EXTERNAL(new_value)) {
        new_value = detoast_external_attr(new_value);
        toast_values[i] = PointerGetDatum(new_value);
        toast_free[i] = true;
      }
    }
  }

  /*
   * Form the reconfigured tuple.
   */
  new_tuple = heap_form_tuple(tupleDesc, toast_values, toast_isnull);

  /*
   * Be sure to copy the tuple's identity fields.  We also make a point of
   * copying visibility info, just in case anybody looks at those fields in
   * a syscache entry.
   */
  new_tuple->t_self = tup->t_self;
  new_tuple->t_tableOid = tup->t_tableOid;

  new_tuple->t_data->t_choice = tup->t_data->t_choice;
  new_tuple->t_data->t_ctid = tup->t_data->t_ctid;
  new_tuple->t_data->t_infomask &= ~HEAP_XACT_MASK;
  new_tuple->t_data->t_infomask |= tup->t_data->t_infomask & HEAP_XACT_MASK;
  new_tuple->t_data->t_infomask2 &= ~HEAP2_XACT_MASK;
  new_tuple->t_data->t_infomask2 |= tup->t_data->t_infomask2 & HEAP2_XACT_MASK;

  /*
   * Free allocated temp values
   */
  for (i = 0; i < numAttrs; i++)
    if (toast_free[i])
      pfree(DatumGetPointer(toast_values[i]));

  return new_tuple;
}

/* ----------
 * toast_flatten_tuple_to_datum -
 *
 *	"Flatten" a tuple containing out-of-line toasted fields into a Datum.
 *	The result is always palloc'd in the current memory context.
 *
 *	We have a general rule that Datums of container types (rows, arrays,
 *	ranges, etc) must not contain any external TOAST pointers.  Without
 *	this rule, we'd have to look inside each Datum when preparing a tuple
 *	for storage, which would be expensive and would fail to extend cleanly
 *	to new sorts of container types.
 *
 *	However, we don't want to say that tuples represented as HeapTuples
 *	can't contain toasted fields, so instead this routine should be called
 *	when such a HeapTuple is being converted into a Datum.
 *
 *	While we're at it, we decompress any compressed fields too.  This is not
 *	necessary for correctness, but reflects an expectation that compression
 *	will be more effective if applied to the whole tuple not individual
 *	fields.  We are not so concerned about that that we want to deconstruct
 *	and reconstruct tuples just to get rid of compressed fields, however.
 *	So callers typically won't call this unless they see that the tuple has
 *	at least one external field.
 *
 *	On the other hand, in-line short-header varlena fields are left alone.
 *	If we "untoasted" them here, they'd just get changed back to
 *short-header format anyway within heap_fill_tuple.
 * ----------
 */
Datum toast_flatten_tuple_to_datum(HeapTupleHeader tup, uint32 tup_len,
                                   TupleDesc tupleDesc) {
  HeapTupleHeader new_data;
  int32 new_header_len;
  int32 new_data_len;
  int32 new_tuple_len;
  HeapTupleData tmptup;
  int numAttrs = tupleDesc->natts;
  int i;
  bool has_nulls = false;
  Datum toast_values[MaxTupleAttributeNumber];
  bool toast_isnull[MaxTupleAttributeNumber];
  bool toast_free[MaxTupleAttributeNumber];

  /* Build a temporary HeapTuple control structure */
  tmptup.t_len = tup_len;
  ItemPointerSetInvalid(&(tmptup.t_self));
  tmptup.t_tableOid = InvalidOid;
  tmptup.t_data = tup;

  /*
   * Break down the tuple into fields.
   */
  Assert(numAttrs <= MaxTupleAttributeNumber);
  heap_deform_tuple(&tmptup, tupleDesc, toast_values, toast_isnull);

  memset(toast_free, 0, numAttrs * sizeof(bool));

  for (i = 0; i < numAttrs; i++) {
    /*
     * Look at non-null varlena attributes
     */
    if (toast_isnull[i])
      has_nulls = true;
    else if (TupleDescCompactAttr(tupleDesc, i)->attlen == -1) {
      struct varlena *new_value;

      new_value = (struct varlena *)DatumGetPointer(toast_values[i]);
      if (VARATT_IS_EXTERNAL(new_value) || VARATT_IS_COMPRESSED(new_value)) {
        new_value = detoast_attr(new_value);
        toast_values[i] = PointerGetDatum(new_value);
        toast_free[i] = true;
      }
    }
  }

  /*
   * Calculate the new size of the tuple.
   *
   * This should match the reconstruction code in
   * heap_toast_insert_or_update.
   */
  new_header_len = SizeofHeapTupleHeader;
  if (has_nulls)
    new_header_len += BITMAPLEN(numAttrs);
  new_header_len = MAXALIGN(new_header_len);
  new_data_len = heap_compute_data_size(tupleDesc, toast_values, toast_isnull);
  new_tuple_len = new_header_len + new_data_len;

  new_data = (HeapTupleHeader)palloc0(new_tuple_len);

  /*
   * Copy the existing tuple header, but adjust natts and t_hoff.
   */
  memcpy(new_data, tup, SizeofHeapTupleHeader);
  HeapTupleHeaderSetNatts(new_data, numAttrs);
  new_data->t_hoff = new_header_len;

  /* Set the composite-Datum header fields correctly */
  HeapTupleHeaderSetDatumLength(new_data, new_tuple_len);
  HeapTupleHeaderSetTypeId(new_data, tupleDesc->tdtypeid);
  HeapTupleHeaderSetTypMod(new_data, tupleDesc->tdtypmod);

  /* Copy over the data, and fill the null bitmap if needed */
  heap_fill_tuple(tupleDesc, toast_values, toast_isnull,
                  (char *)new_data + new_header_len, new_data_len,
                  &(new_data->t_infomask), has_nulls ? new_data->t_bits : NULL);

  /*
   * Free allocated temp values
   */
  for (i = 0; i < numAttrs; i++)
    if (toast_free[i])
      pfree(DatumGetPointer(toast_values[i]));

  return PointerGetDatum(new_data);
}

/* ----------
 * toast_build_flattened_tuple -
 *
 *	Build a tuple containing no out-of-line toasted fields.
 *	(This does not eliminate compressed or short-header datums.)
 *
 *	This is essentially just like heap_form_tuple, except that it will
 *	expand any external-data pointers beforehand.
 *
 *	It's not very clear whether it would be preferable to decompress
 *	in-line compressed datums while at it.  For now, we don't.
 * ----------
 */
HeapTuple toast_build_flattened_tuple(TupleDesc tupleDesc, Datum *values,
                                      bool *isnull) {
  HeapTuple new_tuple;
  int numAttrs = tupleDesc->natts;
  int num_to_free;
  int i;
  Datum new_values[MaxTupleAttributeNumber];
  Pointer freeable_values[MaxTupleAttributeNumber];

  /*
   * We can pass the caller's isnull array directly to heap_form_tuple, but
   * we potentially need to modify the values array.
   */
  Assert(numAttrs <= MaxTupleAttributeNumber);
  memcpy(new_values, values, numAttrs * sizeof(Datum));

  num_to_free = 0;
  for (i = 0; i < numAttrs; i++) {
    /*
     * Look at non-null varlena attributes
     */
    if (!isnull[i] && TupleDescCompactAttr(tupleDesc, i)->attlen == -1) {
      struct varlena *new_value;

      new_value = (struct varlena *)DatumGetPointer(new_values[i]);
      if (VARATT_IS_EXTERNAL(new_value)) {
        new_value = detoast_external_attr(new_value);
        new_values[i] = PointerGetDatum(new_value);
        freeable_values[num_to_free++] = (Pointer)new_value;
      }
    }
  }

  /*
   * Form the reconfigured tuple.
   */
  new_tuple = heap_form_tuple(tupleDesc, new_values, isnull);

  /*
   * Free allocated temp values
   */
  for (i = 0; i < num_to_free; i++)
    pfree(freeable_values[i]);

  return new_tuple;
}

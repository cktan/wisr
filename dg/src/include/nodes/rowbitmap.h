/*-------------------------------------------------------------------------
 *
 * rowbitmap.h
 *	  A set of row numbers, as bitmap scans of tables in sm carry them.
 *
 * A row in sm is a 64-bit rowno, with no block or offset: a RowBitmap is
 * a bitmap over that space, bit r set for row r.  Bitmap index scans of
 * an index in sm make one; BitmapAnd and BitmapOr combine them; a bitmap
 * heap scan of a table in sm reads its rows in rowno order.
 *
 * src/include/nodes/rowbitmap.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef ROWBITMAP_H
#define ROWBITMAP_H

/* Private to rowbitmap.c; callers can IsA(x, RowBitmap) on it. */
typedef struct RowBitmap RowBitmap;
typedef struct RowBitmapIterator RowBitmapIterator;

extern RowBitmap *rbm_create(void);
extern void rbm_free(RowBitmap *rbm);
extern void rbm_add(RowBitmap *rbm, uint64 rowno);
extern void rbm_union(RowBitmap *a, const RowBitmap *b);
extern void rbm_intersect(RowBitmap *a, const RowBitmap *b);
extern bool rbm_is_empty(const RowBitmap *rbm);

extern RowBitmapIterator *rbm_begin_iterate(RowBitmap *rbm);
extern bool rbm_iterate(RowBitmapIterator *iter, uint64 *rowno);
extern void rbm_end_iterate(RowBitmapIterator *iter);

#endif /* ROWBITMAP_H */

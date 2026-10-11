/*-------------------------------------------------------------------------
 *
 * rowbitmap.c
 *	  A set of row numbers, as bitmap scans of tables in sm carry them.
 *
 * The bitmap is sparse: a hash table holds each of its 64-bit words that
 * has a bit set, keyed by the word's place, rowno / 64.  It is exact, and
 * grows as it must: there is no lossy form, as a row in sm has no block
 * to fall back on.
 *
 * IDENTIFICATION
 *	  src/backend/nodes/rowbitmap.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "nodes/nodes.h"
#include "nodes/rowbitmap.h"
#include "port/pg_bitutils.h"

/* A word of the bitmap: bit b of word w is rowno w * 64 + b. */
typedef struct RowBitmapWord {
  uint64 wordno;
  uint64 bits;
  char status; /* for simplehash */
} RowBitmapWord;

#define SH_PREFIX rowwords
#define SH_ELEMENT_TYPE RowBitmapWord
#define SH_KEY_TYPE uint64
#define SH_KEY wordno
#define SH_HASH_KEY(tb, key) murmurhash64(key)
#define SH_EQUAL(tb, a, b) a == b
#define SH_SCOPE static inline
#define SH_DEFINE
#define SH_DECLARE
#include "lib/simplehash.h"

struct RowBitmap {
  NodeTag type;
  rowwords_hash *words;
};

struct RowBitmapIterator {
  uint64 *wordnos; /* the words with a bit set, ascending */
  uint64 *bits;    /* each one's bits, still to report */
  int nwords;
  int next; /* the word reporting now */
};

/* An empty bitmap, in CurrentMemoryContext. */
RowBitmap *rbm_create(void) {
  RowBitmap *rbm = makeNode(RowBitmap);

  rbm->words = rowwords_create(CurrentMemoryContext, 128, NULL);
  return rbm;
}

/* Free a bitmap. */
void rbm_free(RowBitmap *rbm) {
  rowwords_destroy(rbm->words);
  pfree(rbm);
}

/* Set rowno's bit. */
void rbm_add(RowBitmap *rbm, uint64 rowno) {
  bool found;
  RowBitmapWord *word = rowwords_insert(rbm->words, rowno / 64, &found);

  if (!found)
    word->bits = 0;
  word->bits |= UINT64CONST(1) << (rowno % 64);
}

/* Set in a every bit set in b. */
void rbm_union(RowBitmap *a, const RowBitmap *b) {
  rowwords_iterator i;
  RowBitmapWord *bword;

  rowwords_start_iterate(b->words, &i);
  while ((bword = rowwords_iterate(b->words, &i)) != NULL) {
    bool found;
    RowBitmapWord *aword = rowwords_insert(a->words, bword->wordno, &found);

    aword->bits = found ? aword->bits | bword->bits : bword->bits;
  }
}

/* Clear in a every bit not set in b. */
void rbm_intersect(RowBitmap *a, const RowBitmap *b) {
  rowwords_iterator i;
  RowBitmapWord *aword;

  /* simplehash allows deleting the element an iteration is on. */
  rowwords_start_iterate(a->words, &i);
  while ((aword = rowwords_iterate(a->words, &i)) != NULL) {
    RowBitmapWord *bword = rowwords_lookup(b->words, aword->wordno);

    if (bword != NULL)
      aword->bits &= bword->bits;
    if (bword == NULL || aword->bits == 0)
      rowwords_delete_item(a->words, aword);
  }
}

/* Whether no bit is set. */
bool rbm_is_empty(const RowBitmap *rbm) { return rbm->words->members == 0; }

/* Order two word places, for qsort. */
static int wordno_cmp(const void *a, const void *b) {
  uint64 x = *(const uint64 *)a, y = *(const uint64 *)b;

  return x < y ? -1 : x > y;
}

/*
 * Begin reporting the bitmap's rownos, in ascending order.  The bitmap must
 * not change until the iteration ends.
 */
RowBitmapIterator *rbm_begin_iterate(RowBitmap *rbm) {
  RowBitmapIterator *iter =
      (RowBitmapIterator *)palloc0(sizeof(RowBitmapIterator));
  int n = (int)rbm->words->members;
  rowwords_iterator i;
  RowBitmapWord *word;

  iter->wordnos = (uint64 *)palloc(Max(n, 1) * sizeof(uint64));
  iter->bits = (uint64 *)palloc(Max(n, 1) * sizeof(uint64));
  rowwords_start_iterate(rbm->words, &i);
  while ((word = rowwords_iterate(rbm->words, &i)) != NULL)
    iter->wordnos[iter->nwords++] = word->wordno;
  qsort(iter->wordnos, n, sizeof(uint64), wordno_cmp);
  for (int k = 0; k < n; k++)
    iter->bits[k] = rowwords_lookup(rbm->words, iter->wordnos[k])->bits;
  return iter;
}

/* The next rowno into *rowno; false when there are no more. */
bool rbm_iterate(RowBitmapIterator *iter, uint64 *rowno) {
  while (iter->next < iter->nwords) {
    uint64 *bits = &iter->bits[iter->next];

    if (*bits != 0) {
      int b = pg_rightmost_one_pos64(*bits);

      *bits &= *bits - 1;
      *rowno = iter->wordnos[iter->next] * 64 + b;
      return true;
    }
    iter->next++;
  }
  return false;
}

/* End an iteration. */
void rbm_end_iterate(RowBitmapIterator *iter) {
  pfree(iter->wordnos);
  pfree(iter->bits);
  pfree(iter);
}

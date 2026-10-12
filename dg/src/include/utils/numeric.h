/*-------------------------------------------------------------------------
 *
 * numeric.h
 *	  numeric, a decimal of at most 38 digits (DECIMAL.md)
 *
 * Original coding 1998, Jan Wieck.  Heavily revised 2003, Tom Lane.
 *
 * Copyright (c) 1998-2025, PostgreSQL Global Development Group
 *
 * src/include/utils/numeric.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_NUMERIC_H_
#define _PG_NUMERIC_H_

#include "common/pg_prng.h"
#include "fmgr.h"

/* forward declaration to avoid node.h include */
struct Node;

/*
 * numeric is a decimal of at most 38 digits (DECIMAL.md): a typmod's
 * precision is 1 to 38, its scale 0 to the precision, and a value's scale
 * at most 38.
 */
#define NUMERIC_MAX_PRECISION		38

#define NUMERIC_MIN_SCALE			0
#define NUMERIC_MAX_SCALE			38

/*
 * Limits on the scales chosen for calculation results
 */
#define NUMERIC_MAX_DISPLAY_SCALE	NUMERIC_MAX_SCALE
#define NUMERIC_MIN_DISPLAY_SCALE	0

/* A bound on the decimal weights a result's scale is estimated from */
#define NUMERIC_MAX_RESULT_SCALE	2000

/*
 * For inherently inexact calculations such as division and square root,
 * we try to get at least this many significant digits; the idea is to
 * deliver a result no worse than float8 would.
 */
#define NUMERIC_MIN_SIG_DIGITS		16

/*
 * A numeric value: the number times 10^scale, a 128-bit two's complement
 * integer in two halves, so that it needs no 16-byte alignment, and its
 * scale.  32 bytes, passed by reference; there is no NaN or infinity.
 */
typedef struct decimal_t {
  uint64 lo;    /* the integer's low 64 bits */
  int64 hi;     /* its high 64 bits */
  int16 scale;  /* 0 to NUMERIC_MAX_SCALE */
  int16 pad[7]; /* zero */
} decimal_t;

typedef decimal_t *Numeric;

/*
 * fmgr interface macros
 */

static inline Numeric DatumGetNumeric(Datum X) {
  return (Numeric)DatumGetPointer(X);
}

static inline Numeric DatumGetNumericCopy(Datum X) {
  Numeric copy = (Numeric)palloc(sizeof(decimal_t));

  memcpy(copy, DatumGetPointer(X), sizeof(decimal_t));
  return copy;
}

static inline Datum NumericGetDatum(Numeric X) { return PointerGetDatum(X); }

#define PG_GETARG_NUMERIC(n)	  DatumGetNumeric(PG_GETARG_DATUM(n))
#define PG_GETARG_NUMERIC_COPY(n) DatumGetNumericCopy(PG_GETARG_DATUM(n))
#define PG_RETURN_NUMERIC(x)	  return NumericGetDatum(x)

/*
 * Utility functions in decimal.c
 */
extern bool numeric_is_nan(Numeric num);
extern bool numeric_is_inf(Numeric num);
extern int32 numeric_maximum_size(int32 typmod);
extern char *numeric_out_sci(Numeric num, int scale);
extern char *numeric_normalize(Numeric num);

extern Numeric int64_to_numeric(int64 val);
extern Numeric int64_div_fast_to_numeric(int64 val1, int log10val2);

extern Numeric numeric_add_opt_error(Numeric num1, Numeric num2,
                                     bool *have_error);
extern Numeric numeric_sub_opt_error(Numeric num1, Numeric num2,
                                     bool *have_error);
extern Numeric numeric_mul_opt_error(Numeric num1, Numeric num2,
                                     bool *have_error);
extern Numeric numeric_div_opt_error(Numeric num1, Numeric num2,
                                     bool *have_error);
extern Numeric numeric_mod_opt_error(Numeric num1, Numeric num2,
                                     bool *have_error);
extern int32 numeric_int4_opt_error(Numeric num, bool *have_error);
extern int64 numeric_int8_opt_error(Numeric num, bool *have_error);
extern int32 make_numeric_typmod_safe(int32 precision, int32 scale,
                                      struct Node *escontext);

extern Numeric random_numeric(pg_prng_state *state, Numeric rmin, Numeric rmax);

#endif /* _PG_NUMERIC_H_ */

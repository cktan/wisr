/*-------------------------------------------------------------------------
 *
 * numeric.h
 *	  numeric, a decimal of at most 36 digits (DECIMAL.md)
 *
 * Original coding 1998, Jan Wieck.  Heavily revised 2003, Tom Lane.
 *
 * Copyright (c) 1998-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/utils/numeric.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _PG_NUMERIC_H_
#define _PG_NUMERIC_H_

#include "catalog/pg_type_d.h"
#include "common/pg_prng.h"
#include "fmgr.h"

/* forward declaration to avoid node.h include */
struct Node;

/*
 * numeric is a decimal of at most 36 digits (DECIMAL.md): a typmod's
 * precision is 1 to 36, its scale 0 to the precision, and a value's scale
 * at most 36.
 */
#define NUMERIC_MAX_PRECISION		36

#define NUMERIC_MIN_SCALE			0
#define NUMERIC_MAX_SCALE			36

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
 * A numeric value, sm's decimal128 bit for bit (sm/sm2/TYPE.md section
 * 3.4): a 128-bit two's complement word, in two halves so that it needs no
 * 16-byte alignment, holding the number times 10^scale shifted left past
 * the scale, (value << 6) | scale.  16 bytes, passed by reference; there
 * is no NaN or infinity.
 */
typedef struct decimal_t {
  uint64 lo; /* the word's low 64 bits */
  int64 hi;  /* its high 64 bits */
} decimal_t;

typedef decimal_t *Numeric;

/*
 * The OID a client knows numeric by, PostgreSQL's: digres's decimals go
 * to a client and come from one under it (DECIMAL.md section 2.1).
 */
#define NUMERIC_WIRE_OID 1700

/* The OID a client knows numeric[] by, PostgreSQL's. */
#define NUMERIC_ARRAY_WIRE_OID 1231

/*
 * The OID a client is told for type typid: numeric's for either decimal,
 * decimal64's being numeric's own, and numeric[]'s for an array of either.
 */
static inline Oid numeric_wire_oid(Oid typid) {
  if (typid == DECIMAL128OID)
    return NUMERIC_WIRE_OID;
  if (typid == DECIMAL64ARRAYOID)
    return NUMERIC_ARRAY_WIRE_OID;
  return typid;
}

/*
 * The type a client's OID typid names: numeric's names decimal128, which
 * holds any value a client sends (DECIMAL.md section 2.1).
 */
static inline Oid numeric_oid_from_wire(Oid typid) {
  return typid == NUMERIC_WIRE_OID ? DECIMAL128OID : typid;
}

/* The bits of a numeric's word below its value, which hold its scale. */
#define NUMERIC_SCALE_BITS 6

/* num's word, (value << 6) | scale. */
static inline __int128 decimal_word(const decimal_t *num) {
  return (__int128)(((unsigned __int128)(uint64)num->hi << 64) | num->lo);
}

/* num's value, the number times 10^scale. */
static inline __int128 decimal_value(const decimal_t *num) {
  return decimal_word(num) >> NUMERIC_SCALE_BITS;
}

/* num's scale. */
static inline int decimal_scale(const decimal_t *num) {
  return (int)(num->lo & ((1 << NUMERIC_SCALE_BITS) - 1));
}

/* Set num to v at scale, |v| below 10^36. */
static inline void decimal_set(decimal_t *num, __int128 v, int scale) {
  unsigned __int128 w =
      (unsigned __int128)v * ((unsigned __int128)1 << NUMERIC_SCALE_BITS) +
      (unsigned __int128)scale;

  num->lo = (uint64)w;
  num->hi = (int64)(w >> 64);
}

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
extern bool numeric_to_decimal64(Numeric num, Datum *out);
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

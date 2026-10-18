/*-------------------------------------------------------------------------
 *
 * decimal.c
 *	  numeric, as a decimal of at most 36 digits (DECIMAL.md).
 *
 * A value is sm's decimal128, a 128-bit word holding the number times
 * 10^scale shifted past its scale (utils/numeric.h): there is no NaN and
 * no infinity, and a result past 36 digits loses places after the point,
 * or is an error.  What PostgreSQL's numeric did with arbitrary precision
 * this does exactly, in a 512-bit integer where 128 bits do not reach, and
 * rounds to 36 digits; ln, exp, log and a power to a fraction are computed
 * in long double.  The scale of each result is the one PostgreSQL's numeric
 * would give it, as far as 36 digits allow.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <string>

#include "catalog/pg_type.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "common/int128.h"
#include "funcapi.h"
#include "lib/stringinfo.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "nodes/supportnodes.h"
#include "optimizer/optimizer.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/float.h"
#include "utils/fmgrprotos.h"
#include "utils/numeric.h"
#include "utils/pg_lsn.h"
#include "utils/sortsupport.h"

typedef __int128 i128;
typedef unsigned __int128 u128;

/* ----------------------------------------------------------------------
 * A 512-bit magnitude: what 38-digit values reach in a product, a square,
 * a sum of squares or a dividend scaled up for a quotient.
 * ----------------------------------------------------------------------
 */

#define MAG_LIMBS 8

/* A 512-bit unsigned integer, least significant limb first. */
typedef struct mag_t {
  uint64 limb[MAG_LIMBS];
} mag_t;

/* The magnitude v. */
static mag_t mag_of(u128 v) {
  mag_t m = {};

  m.limb[0] = (uint64)v;
  m.limb[1] = (uint64)(v >> 64);
  return m;
}

/* Whether m is zero. */
static bool mag_is_zero(const mag_t &m) {
  for (int i = 0; i < MAG_LIMBS; i++)
    if (m.limb[i] != 0)
      return false;
  return true;
}

/* -1, 0 or 1 as a is below, equal to or above b. */
static int mag_cmp(const mag_t &a, const mag_t &b) {
  for (int i = MAG_LIMBS - 1; i >= 0; i--)
    if (a.limb[i] != b.limb[i])
      return a.limb[i] < b.limb[i] ? -1 : 1;
  return 0;
}

/* a + b; overflow past 512 bits is an error. */
static mag_t mag_add(const mag_t &a, const mag_t &b) {
  mag_t r;
  u128 carry = 0;

  for (int i = 0; i < MAG_LIMBS; i++) {
    u128 s = (u128)a.limb[i] + b.limb[i] + carry;

    r.limb[i] = (uint64)s;
    carry = s >> 64;
  }
  if (carry)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("value overflows numeric format")));
  return r;
}

/* a - b, for a not below b. */
static mag_t mag_sub(const mag_t &a, const mag_t &b) {
  mag_t r;
  uint64 borrow = 0;

  for (int i = 0; i < MAG_LIMBS; i++) {
    u128 d = (u128)a.limb[i] - b.limb[i] - borrow;

    r.limb[i] = (uint64)d;
    borrow = (d >> 64) ? 1 : 0;
  }
  return r;
}

/* a * b; overflow past 512 bits is an error. */
static mag_t mag_mul(const mag_t &a, const mag_t &b) {
  uint64 r[2 * MAG_LIMBS] = {};
  mag_t out;

  for (int i = 0; i < MAG_LIMBS; i++) {
    u128 carry = 0;

    if (a.limb[i] == 0)
      continue;
    for (int j = 0; j < MAG_LIMBS; j++) {
      u128 t = (u128)a.limb[i] * b.limb[j] + r[i + j] + carry;

      r[i + j] = (uint64)t;
      carry = t >> 64;
    }
    for (int k = i + MAG_LIMBS; carry != 0; k++) {
      u128 t = (u128)r[k] + carry;

      r[k] = (uint64)t;
      carry = t >> 64;
    }
  }
  for (int i = MAG_LIMBS; i < 2 * MAG_LIMBS; i++)
    if (r[i] != 0)
      ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                      errmsg("value overflows numeric format")));
  memcpy(out.limb, r, sizeof(out.limb));
  return out;
}

/* a * m, a small multiplier; overflow is an error. */
static mag_t mag_mul_small(const mag_t &a, uint64 m) {
  return mag_mul(a, mag_of(m));
}

/* a / d, the remainder in *rem, for a small divisor d. */
static mag_t mag_div_small(const mag_t &a, uint64 d, uint64 *rem) {
  mag_t q;
  u128 r = 0;

  for (int i = MAG_LIMBS - 1; i >= 0; i--) {
    u128 cur = (r << 64) | a.limb[i];

    q.limb[i] = (uint64)(cur / d);
    r = cur % d;
  }
  *rem = (uint64)r;
  return q;
}

/* The number of significant bits of m. */
static int mag_bits(const mag_t &m) {
  for (int i = MAG_LIMBS - 1; i >= 0; i--)
    if (m.limb[i] != 0)
      return i * 64 + 64 - __builtin_clzll(m.limb[i]);
  return 0;
}

/* m shifted left by one bit, the bit in shifted in at the bottom. */
static void mag_shl1(mag_t &m, int in) {
  for (int i = MAG_LIMBS - 1; i > 0; i--)
    m.limb[i] = (m.limb[i] << 1) | (m.limb[i - 1] >> 63);
  m.limb[0] = (m.limb[0] << 1) | (uint64)in;
}

/* a / d, the remainder in *rem; d is not zero.  Long division by bits. */
static mag_t mag_divmod(const mag_t &a, const mag_t &d, mag_t *rem) {
  mag_t q = {};
  mag_t r = {};

  for (int bit = mag_bits(a) - 1; bit >= 0; bit--) {
    mag_shl1(r, (int)((a.limb[bit / 64] >> (bit % 64)) & 1));
    mag_shl1(q, 0);
    if (mag_cmp(r, d) >= 0) {
      r = mag_sub(r, d);
      q.limb[0] |= 1;
    }
  }
  if (rem != NULL)
    *rem = r;
  return q;
}

/* a * 10, at compile time: for the table of powers of ten. */
static constexpr mag_t mag_mul10_const(const mag_t &a) {
  mag_t r{};
  u128 carry = 0;

  for (int i = 0; i < MAG_LIMBS; i++) {
    u128 t = (u128)a.limb[i] * 10 + carry;

    r.limb[i] = (uint64)t;
    carry = t >> 64;
  }
  return r;
}

/* 10^0 to 10^154, the most 512 bits hold, made at compile time. */
static constexpr struct pow10_table_t {
  mag_t p[155];
  constexpr pow10_table_t() : p{} {
    p[0].limb[0] = 1;
    for (int i = 1; i < 155; i++)
      p[i] = mag_mul10_const(p[i - 1]);
  }
} pow10_table{};

/* 10^n, for n from 0 to 154. */
static const mag_t &mag_pow10(int n) {
  if (n < 0 || n > 154)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("value overflows numeric format")));
  return pow10_table.p[n];
}

/* The number of decimal digits of m; 0 for zero. */
static int mag_digits(const mag_t &m) {
  int n = 0;

  while (n < 155 && mag_cmp(m, mag_pow10(n)) >= 0)
    n++;
  return n;
}

/* The floor of the square root of m, by Newton's iteration. */
static mag_t mag_isqrt(const mag_t &m) {
  mag_t x, y;

  if (mag_is_zero(m))
    return m;
  /* A first guess at or above the root: 2^ceil(bits / 2). */
  x = mag_of(0);
  {
    int b = (mag_bits(m) + 1) / 2;

    x.limb[b / 64] = (uint64)1 << (b % 64);
  }
  for (;;) {
    uint64 r;

    y = mag_div_small(mag_add(x, mag_divmod(m, x, NULL)), 2, &r);
    if (mag_cmp(y, x) >= 0)
      return x;
    x = y;
  }
}

/* ----------------------------------------------------------------------
 * A wide decimal: a sign, a 512-bit magnitude and a scale, for exact
 * intermediate results.
 * ----------------------------------------------------------------------
 */

/* A value, exact: (neg ? -1 : 1) * mag / 10^scale. */
typedef struct wide_t {
  mag_t mag;
  bool neg;
  int scale;
} wide_t;

/* The wide value of an integer v at scale 0. */
static wide_t wide_of_int(i128 v) {
  wide_t w;

  w.neg = v < 0;
  w.mag = mag_of(v < 0 ? (u128)0 - (u128)v : (u128)v);
  w.scale = 0;
  return w;
}

/* Whether w is zero. */
static bool wide_is_zero(const wide_t &w) { return mag_is_zero(w.mag); }

/* w at scale s, s not below its own: exact. */
static wide_t wide_scale_up(const wide_t &w, int s) {
  wide_t r = w;

  if (s > w.scale) {
    r.mag = mag_mul(w.mag, mag_pow10(s - w.scale));
    r.scale = s;
  }
  return r;
}

/*
 * w at scale s: places past s rounded half away from zero, or, if trunc,
 * dropped.  A scale above w's own adds zeros.
 */
static wide_t wide_round(const wide_t &w, int s, bool trunc) {
  wide_t r = w;
  mag_t rem;
  int drop;

  if (s >= w.scale)
    return wide_scale_up(w, s);
  drop = w.scale - s;
  if (drop > 154) {
    r.mag = mag_of(0);
  } else {
    const mag_t &div = mag_pow10(drop);

    r.mag = mag_divmod(w.mag, div, &rem);
    /* Half away from zero: the remainder at least half the divisor. */
    if (!trunc && mag_cmp(mag_mul_small(rem, 2), div) >= 0)
      r.mag = mag_add(r.mag, mag_of(1));
  }
  r.scale = s;
  if (mag_is_zero(r.mag))
    r.neg = false;
  return r;
}

/* -1, 0 or 1 as a is below, equal to or above b. */
static int wide_cmp(const wide_t &a, const wide_t &b) {
  int s = Max(a.scale, b.scale);
  wide_t x = wide_scale_up(a, s), y = wide_scale_up(b, s);
  int c;

  if (wide_is_zero(x) && wide_is_zero(y))
    return 0;
  if (x.neg != y.neg)
    return x.neg ? -1 : 1;
  c = mag_cmp(x.mag, y.mag);
  return x.neg ? -c : c;
}

/* a + b, or a - b if sub, exact, at the larger scale. */
static wide_t wide_add(const wide_t &a, const wide_t &b, bool sub) {
  int s = Max(a.scale, b.scale);
  wide_t x = wide_scale_up(a, s), y = wide_scale_up(b, s);
  wide_t r;

  if (sub)
    y.neg = !y.neg;
  r.scale = s;
  if (x.neg == y.neg) {
    r.mag = mag_add(x.mag, y.mag);
    r.neg = x.neg;
  } else if (mag_cmp(x.mag, y.mag) >= 0) {
    r.mag = mag_sub(x.mag, y.mag);
    r.neg = x.neg;
  } else {
    r.mag = mag_sub(y.mag, x.mag);
    r.neg = y.neg;
  }
  if (mag_is_zero(r.mag))
    r.neg = false;
  return r;
}

/* a * b, exact, at the sum of their scales. */
static wide_t wide_mul(const wide_t &a, const wide_t &b) {
  wide_t r;

  r.mag = mag_mul(a.mag, b.mag);
  r.neg = a.neg != b.neg && !mag_is_zero(r.mag);
  r.scale = a.scale + b.scale;
  return r;
}

/*
 * a / b at scale rscale, rounded half away from zero, or truncated if
 * trunc; b is not zero.
 */
static wide_t wide_div(const wide_t &a, const wide_t &b, int rscale,
                       bool trunc) {
  /* a.mag / 10^as / (b.mag / 10^bs) * 10^rs = a.mag * 10^(rs+bs-as) / b.mag */
  int e = rscale + b.scale - a.scale;
  mag_t num = a.mag, den = b.mag, rem;
  wide_t r;

  if (e >= 0)
    num = mag_mul(num, mag_pow10(e));
  else
    den = mag_mul(den, mag_pow10(-e));
  r.mag = mag_divmod(num, den, &rem);
  if (!trunc && mag_cmp(mag_mul_small(rem, 2), den) >= 0)
    r.mag = mag_add(r.mag, mag_of(1));
  r.neg = a.neg != b.neg && !mag_is_zero(r.mag);
  r.scale = rscale;
  return r;
}

/* ----------------------------------------------------------------------
 * The value: a 128-bit integer and its scale (utils/numeric.h).
 * ----------------------------------------------------------------------
 */

#define DEC_MAX_DIGITS NUMERIC_MAX_PRECISION

/* The value of num, the number times 10^scale. */
static i128 dec_value(const decimal_t *num) { return decimal_value(num); }

/* The scale of num. */
static int dec_scale(const decimal_t *num) { return decimal_scale(num); }

/* A new value of v at scale, palloc'd. */
static Numeric dec_make(i128 v, int scale) {
  Numeric res = (Numeric)palloc(sizeof(decimal_t));

  decimal_set(res, v, scale);
  return res;
}

/* The wide value of num. */
static wide_t dec_wide(const decimal_t *num) {
  wide_t w = wide_of_int(dec_value(num));

  w.scale = dec_scale(num);
  return w;
}

/*
 * w as a value, or false with *have_error set if it has more than 36
 * digits before the point: places past the point are rounded away, half
 * away from zero, until it has at most 36 digits.
 */
static bool dec_pack(const wide_t &w, Numeric *out, bool *have_error) {
  wide_t r = w;
  int digits;
  u128 mag;

  if (r.scale > NUMERIC_MAX_SCALE)
    r = wide_round(r, NUMERIC_MAX_SCALE, false);
  digits = mag_digits(r.mag);
  if (digits > DEC_MAX_DIGITS) {
    int drop = digits - DEC_MAX_DIGITS;

    if (drop > r.scale)
      goto overflow;
    r = wide_round(r, r.scale - drop, false);
    /* Rounding up may add a digit: 99.95 to 100.0. */
    if (mag_digits(r.mag) > DEC_MAX_DIGITS) {
      if (r.scale == 0)
        goto overflow;
      r = wide_round(r, r.scale - 1, false);
    }
  }
  mag = ((u128)r.mag.limb[1] << 64) | r.mag.limb[0];
  *out = dec_make(r.neg ? -(i128)mag : (i128)mag, r.scale);
  return true;

overflow:
  if (have_error == NULL)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("value overflows numeric format")));
  *have_error = true;
  *out = NULL;
  return false;
}

/* w as a value; more than 36 digits before the point is an error. */
static Numeric dec_from_wide(const wide_t &w) {
  Numeric res;

  dec_pack(w, &res, NULL);
  return res;
}

/* The sign of num: -1, 0 or 1. */
static int dec_sign(const decimal_t *num) {
  i128 v = dec_value(num);

  return v < 0 ? -1 : (v > 0 ? 1 : 0);
}

/*
 * PostgreSQL's view of w in base 10,000, as its numeric held it: the
 * weight of its first nonzero base-10,000 digit, that digit, the next, and
 * whether there are more after the first.  Its scale rules are written in
 * these.  Zero has weight 0 and digit 0.
 */
typedef struct nbase_t {
  int weight;
  int first;
  int second;
  int ndigits; /* 1 if nothing follows the first digit, else 2 */
} nbase_t;

/* w's base-10,000 view. */
static nbase_t dec_nbase(const wide_t &w) {
  nbase_t nb = {0, 0, 0, 1};
  int e, shift;
  mag_t head, rest;

  if (wide_is_zero(w))
    return nb;
  /* e is the decimal exponent of the leading digit. */
  e = mag_digits(w.mag) - 1 - w.scale;
  nb.weight = e >= 0 ? e / 4 : -((-e + 3) / 4);
  /* head is the value / 10000^weight, scaled to keep 8 more digits. */
  shift = w.scale + 4 * nb.weight - 8;
  if (shift >= 0)
    head = mag_divmod(w.mag, mag_pow10(shift), &rest);
  else {
    head = mag_mul(w.mag, mag_pow10(-shift));
    rest = mag_of(0);
  }
  {
    uint64 r1, r2;
    mag_t q = mag_div_small(head, 100000000, &r1);

    nb.first = (int)q.limb[0];
    nb.second = (int)(r1 / 10000);
    r2 = r1 % 10000;
    nb.ndigits = (r1 != 0 || !mag_is_zero(rest) || r2 != 0) ? 2 : 1;
  }
  return nb;
}

/* ----------------------------------------------------------------------
 * Text
 * ----------------------------------------------------------------------
 */

/* The digits of w's magnitude, at least one. */
static std::string mag_text(const mag_t &m) {
  char buf[160];
  int n = 0;
  mag_t x = m;

  do {
    uint64 rem;

    x = mag_div_small(x, 10, &rem);
    buf[n++] = (char)('0' + rem);
  } while (!mag_is_zero(x));
  std::string s(n, '0');
  for (int i = 0; i < n; i++)
    s[i] = buf[n - 1 - i];
  return s;
}

/* w as PostgreSQL prints a numeric: every place of its scale, no exponent. */
static char *wide_text(const wide_t &w) {
  std::string digits = mag_text(w.mag);
  std::string out;

  if ((int)digits.size() <= w.scale)
    digits.insert(0, w.scale - digits.size() + 1, '0');
  if (w.neg && !wide_is_zero(w))
    out += '-';
  out += digits.substr(0, digits.size() - w.scale);
  if (w.scale > 0) {
    out += '.';
    out += digits.substr(digits.size() - w.scale);
  }
  return pstrdup(out.c_str());
}

/*
 * Parse a decimal number at cp -- digits, which may be separated by single
 * underscores, a point, more digits and an exponent -- into *w, setting
 * *endp past it.  False with an error saved in escontext if it is not one,
 * or is out of range.
 */
static bool parse_decimal(const char *str, const char *cp, wide_t *w,
                          const char **endp, Node *escontext) {
  mag_t mag = mag_of(0);
  int scale = 0;
  int sig = 0;
  bool have_dp = false, have_digit = false;
  long exponent = 0;

  for (;;) {
    if (isdigit((unsigned char)*cp)) {
      /* Leading zeros are not significant digits. */
      if (sig > 0 || *cp != '0')
        sig++;
      if (sig > 154)
        goto out_of_range;
      mag = mag_add(mag_mul_small(mag, 10), mag_of(*cp - '0'));
      if (have_dp)
        scale++;
      have_digit = true;
      cp++;
    } else if (*cp == '.' && !have_dp) {
      have_dp = true;
      cp++;
      /* A point must follow a digit or be followed by one. */
      if (!have_digit && !isdigit((unsigned char)*cp))
        goto invalid_syntax;
    } else if (*cp == '_' && have_digit && isdigit((unsigned char)cp[-1]) &&
               isdigit((unsigned char)cp[1])) {
      cp++;
    } else
      break;
  }
  if (!have_digit)
    goto invalid_syntax;
  if (*cp == 'e' || *cp == 'E') {
    const char *ep = cp + 1;
    bool eneg = false;

    if (*ep == '+' || *ep == '-')
      eneg = *ep++ == '-';
    if (!isdigit((unsigned char)*ep))
      goto invalid_syntax;
    while (isdigit((unsigned char)*ep)) {
      exponent = exponent * 10 + (*ep - '0');
      if (exponent > 1000)
        goto out_of_range;
      ep++;
      if (*ep == '_' && isdigit((unsigned char)ep[1]))
        ep++;
    }
    if (eneg)
      exponent = -exponent;
    cp = ep;
  }
  /* The exponent moves the point: up to the scale, then zeros. */
  scale -= (int)exponent;
  if (scale < 0 && mag_digits(mag) - scale > 154)
    goto out_of_range;
  if (scale < 0) {
    mag = mag_mul(mag, mag_pow10(-scale));
    scale = 0;
  }
  w->mag = mag;
  w->neg = false;
  w->scale = scale;
  *endp = cp;
  return true;

invalid_syntax:
  ereturn(escontext, false,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", "numeric", str)));
out_of_range:
  ereturn(escontext, false,
          (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
           errmsg("value overflows numeric format")));
}

/*
 * Parse an integer in base 16, 8 or 2 at cp, after its prefix, with
 * underscores between digits, into *w, setting *endp past it.
 */
static bool parse_nondecimal(const char *str, const char *cp, int base,
                             wide_t *w, const char **endp, Node *escontext) {
  mag_t mag = mag_of(0);
  bool have_digit = false;

  for (;;) {
    int d;

    if (isxdigit((unsigned char)*cp))
      d = isdigit((unsigned char)*cp)
              ? *cp - '0'
              : (tolower((unsigned char)*cp) - 'a' + 10);
    else if (*cp == '_' && have_digit && isxdigit((unsigned char)cp[1])) {
      cp++;
      continue;
    } else
      break;
    if (d >= base)
      break;
    mag = mag_add(mag_mul_small(mag, base), mag_of(d));
    if (mag_digits(mag) > 154)
      ereturn(escontext, false,
              (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
               errmsg("value overflows numeric format")));
    have_digit = true;
    cp++;
  }
  if (!have_digit)
    ereturn(
        escontext, false,
        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
         errmsg("invalid input syntax for type %s: \"%s\"", "numeric", str)));
  w->mag = mag;
  w->neg = false;
  w->scale = 0;
  *endp = cp;
  return true;
}

/* ----------------------------------------------------------------------
 * Typmods: PostgreSQL's ((precision << 16) | scale) + VARHDRSZ, with
 * 1 <= precision <= 36 and 0 <= scale <= precision.
 * ----------------------------------------------------------------------
 */

/* Whether typmod names a precision and scale. */
static bool is_valid_numeric_typmod(int32 typmod) {
  return typmod >= (int32)VARHDRSZ;
}

/* typmod's precision. */
static int numeric_typmod_precision(int32 typmod) {
  return ((typmod - VARHDRSZ) >> 16) & 0xffff;
}

/* typmod's scale. */
static int numeric_typmod_scale(int32 typmod) {
  return (((typmod - VARHDRSZ) & 0x7ff) ^ 1024) - 1024;
}

/*
 * w rounded to typmod's scale and checked against its precision, as an
 * assignment to numeric(p, s) does; false with the error saved in
 * escontext if it has too many digits before the point.
 */
static bool apply_typmod(wide_t *w, int32 typmod, Node *escontext) {
  int precision, scale, maxdigits, digits;

  if (!is_valid_numeric_typmod(typmod))
    return true;
  precision = numeric_typmod_precision(typmod);
  scale = numeric_typmod_scale(typmod);
  maxdigits = precision - scale;
  *w = wide_round(*w, scale, false);
  /* Digits before the point, of the rounded value. */
  digits = mag_digits(w->mag) - w->scale;
  if (digits > maxdigits)
    ereturn(escontext, false,
            (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
             errmsg("numeric field overflow"),
             errdetail("A field with precision %d, scale %d must round to an "
                       "absolute value less than %s%d.",
                       precision, scale,
                       /* Display 10^0 as 1 */
                       maxdigits ? "10^" : "", maxdigits ? maxdigits : 1)));
  return true;
}

/* ----------------------------------------------------------------------
 * Input and output
 * ----------------------------------------------------------------------
 */

/*
 * w with places after the point that are zero dropped, down to scale
 * floor at least: exact.
 */
static wide_t wide_strip_zeros(const wide_t &w, int floor) {
  wide_t r = w;

  while (r.scale > floor) {
    uint64 rem;
    mag_t q = mag_div_small(r.mag, 10, &rem);

    if (rem != 0)
      break;
    r.mag = q;
    r.scale--;
  }
  return r;
}

/* The error for a value of more than 36 significant digits. */
static bool too_many_digits(Node *escontext) {
  ereturn(
      escontext, false,
      (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
       errmsg("value overflows numeric format"),
       errdetail("A numeric value has at most %d digits.", DEC_MAX_DIGITS)));
}

/*
 * numeric_in() -
 *
 *	Input function for numeric: a decimal number, or an integer in base 16,
 *	8 or 2 after 0x, 0o or 0b.  NaN and infinity are not values.  A value of
 *	more than 36 significant digits is refused, not rounded, unless a typmod
 *	rounds it.
 */
Datum numeric_in(PG_FUNCTION_ARGS) {
  char *str = PG_GETARG_CSTRING(0);
  int32 typmod = PG_GETARG_INT32(2);
  Node *escontext = fcinfo->context;
  const char *cp = str;
  bool neg = false;
  wide_t w;
  Numeric res;
  bool have_error = false;

  /* Skip leading spaces */
  while (*cp && isspace((unsigned char)*cp))
    cp++;
  if (*cp == '+')
    cp++;
  else if (*cp == '-') {
    neg = true;
    cp++;
  }
  if (!isdigit((unsigned char)*cp) && *cp != '.') {
    if (pg_strncasecmp(cp, "NaN", 3) == 0 || pg_strncasecmp(cp, "inf", 3) == 0)
      ereturn(
          escontext, (Datum)0,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", "numeric", str),
           errdetail("numeric has no NaN and no infinity.")));
    ereturn(
        escontext, (Datum)0,
        (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
         errmsg("invalid input syntax for type %s: \"%s\"", "numeric", str)));
  }
  if (cp[0] == '0' && strchr("xXoObB", cp[1]) != NULL && cp[1] != '\0') {
    int base = (cp[1] == 'x' || cp[1] == 'X')   ? 16
               : (cp[1] == 'o' || cp[1] == 'O') ? 8
                                                : 2;

    if (!parse_nondecimal(str, cp + 2, base, &w, &cp, escontext))
      PG_RETURN_NULL();
  } else if (!parse_decimal(str, cp, &w, &cp, escontext))
    PG_RETURN_NULL();
  /* Should be nothing left but spaces. */
  while (*cp) {
    if (!isspace((unsigned char)*cp))
      ereturn(
          escontext, (Datum)0,
          (errcode(ERRCODE_INVALID_TEXT_REPRESENTATION),
           errmsg("invalid input syntax for type %s: \"%s\"", "numeric", str)));
    cp++;
  }
  w.neg = neg && !wide_is_zero(w);
  if (!apply_typmod(&w, typmod, escontext))
    PG_RETURN_NULL();
  /*
   * Zeros at the end, past the point, are dropped one by one while the
   * value has more than 36 digits or places: no other digit may be.
   */
  while ((mag_digits(w.mag) > DEC_MAX_DIGITS || w.scale > NUMERIC_MAX_SCALE) &&
         w.scale > 0) {
    wide_t t = wide_strip_zeros(w, w.scale - 1);

    if (t.scale == w.scale)
      break; /* the last place is not a zero */
    w = t;
  }
  if (mag_digits(w.mag) > DEC_MAX_DIGITS || w.scale > NUMERIC_MAX_SCALE) {
    too_many_digits(escontext);
    PG_RETURN_NULL();
  }
  if (!dec_pack(w, &res, &have_error)) {
    too_many_digits(escontext);
    PG_RETURN_NULL();
  }
  PG_RETURN_NUMERIC(res);
}

/* Output function for numeric: every place of its scale, no exponent. */
Datum numeric_out(PG_FUNCTION_ARGS) {
  PG_RETURN_CSTRING(wide_text(dec_wide(PG_GETARG_NUMERIC(0))));
}

/*
 * num in scientific notation, its significand rounded to scale places:
 * 1234 at scale 1 is 1.2e+03.  Zero's exponent is 0.
 */
char *numeric_out_sci(Numeric num, int scale) {
  wide_t w = dec_wide(num);
  int exponent = wide_is_zero(w) ? 0 : mag_digits(w.mag) - 1 - w.scale;
  wide_t sig = w;
  char *s;
  char *str;
  size_t len;

  if (scale < 0)
    scale = 0;
  /* The significand is w / 10^exponent: move the point. */
  sig.scale = w.scale + exponent;
  if (sig.scale < 0) {
    sig.mag = mag_mul(sig.mag, mag_pow10(-sig.scale));
    sig.scale = 0;
  }
  s = wide_text(wide_round(sig, scale, false));
  len = strlen(s) + 13;
  str = (char *)palloc(len);
  snprintf(str, len, "%se%+03d", s, exponent);
  pfree(s);
  return str;
}

/* num's text with no zeros after the point, nor the point if bare. */
char *numeric_normalize(Numeric num) {
  return wide_text(wide_strip_zeros(dec_wide(num), 0));
}

/* The largest number of digits a base-10,000 value of 36 digits has. */
#define DEC_MAX_NBASE_DIGITS 21

/*
 * numeric_recv - converts external binary format to numeric: PostgreSQL's,
 * base-10,000 digits with a weight, a sign and a display scale.
 */
Datum numeric_recv(PG_FUNCTION_ARGS) {
  StringInfo buf = (StringInfo)PG_GETARG_POINTER(0);
  int32 typmod = PG_GETARG_INT32(2);
  int ndigits, weight, sign, dscale;
  wide_t w;

  ndigits = (uint16)pq_getmsgint(buf, sizeof(uint16));
  weight = (int16)pq_getmsgint(buf, sizeof(int16));
  sign = (uint16)pq_getmsgint(buf, sizeof(uint16));
  if (sign != 0x0000 && sign != 0x4000) {
    if (sign == 0xC000 || sign == 0xD000 || sign == 0xF000)
      ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                      errmsg("numeric has no NaN and no infinity")));
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("invalid sign in external \"numeric\" value")));
  }
  dscale = (uint16)pq_getmsgint(buf, sizeof(uint16));
  if ((dscale & 0x3FFF) != dscale)
    ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                    errmsg("invalid scale in external \"numeric\" value")));
  w.mag = mag_of(0);
  w.neg = sign == 0x4000;
  for (int i = 0; i < ndigits; i++) {
    int d = (int16)pq_getmsgint(buf, sizeof(int16));

    if (d < 0 || d >= 10000)
      ereport(ERROR, (errcode(ERRCODE_INVALID_BINARY_REPRESENTATION),
                      errmsg("invalid digit in external \"numeric\" value")));
    if (i >= 38)
      ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                      errmsg("value overflows numeric format")));
    w.mag = mag_add(mag_mul_small(w.mag, 10000), mag_of(d));
  }
  /* The digits' value is that integer times 10000^(weight - ndigits + 1). */
  w.scale = 4 * (ndigits - 1 - weight);
  if (w.scale < 0) {
    w.mag = mag_mul(w.mag, mag_pow10(-w.scale));
    w.scale = 0;
  }
  /* A display scale hiding digits truncates them, as PostgreSQL's does. */
  w = wide_round(w, Min(dscale, NUMERIC_MAX_SCALE + 1), true);
  if (w.scale < dscale && dscale <= NUMERIC_MAX_SCALE)
    w = wide_scale_up(w, dscale);
  if (wide_is_zero(w))
    w.neg = false;
  (void)apply_typmod(&w, typmod, NULL);
  PG_RETURN_NUMERIC(dec_from_wide(w));
}

/* numeric_send - converts numeric to PostgreSQL's binary format. */
Datum numeric_send(PG_FUNCTION_ARGS) {
  wide_t w = dec_wide(PG_GETARG_NUMERIC(0));
  StringInfoData buf;
  int digits[DEC_MAX_NBASE_DIGITS + 2];
  int n = 0, first, last, weight;
  int s4 = (w.scale + 3) / 4 * 4;
  mag_t m = mag_mul(w.mag, mag_pow10(s4 - w.scale));

  /* Base-10,000 digits, least significant first, of m / 10^s4. */
  while (!mag_is_zero(m)) {
    uint64 rem;

    m = mag_div_small(m, 10000, &rem);
    digits[n++] = (int)rem;
  }
  /* The most significant is digits[n - 1], of weight n - 1 - s4 / 4. */
  weight = n - 1 - s4 / 4;
  last = 0;
  while (last < n && digits[last] == 0)
    last++;
  first = n - 1;
  pq_begintypsend(&buf);
  if (n == 0) {
    pq_sendint16(&buf, 0);
    pq_sendint16(&buf, 0);
  } else {
    pq_sendint16(&buf, first - last + 1);
    pq_sendint16(&buf, weight);
  }
  pq_sendint16(&buf, w.neg ? 0x4000 : 0x0000);
  pq_sendint16(&buf, w.scale);
  for (int i = first; n > 0 && i >= last; i--)
    pq_sendint16(&buf, digits[i]);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/*
 * numeric_support() -
 *
 * Planner support function for the numeric() length coercion function.
 *
 * Flatten calls that solely represent increases in allowable precision.
 * Scale changes mutate every datum, so they are unoptimizable.
 */
Datum numeric_support(PG_FUNCTION_ARGS) {
  Node *rawreq = (Node *)PG_GETARG_POINTER(0);
  Node *ret = NULL;

  if (IsA(rawreq, SupportRequestSimplify)) {
    SupportRequestSimplify *req = (SupportRequestSimplify *)rawreq;
    FuncExpr *expr = req->fcall;
    Node *typmod;

    Assert(list_length(expr->args) >= 2);
    typmod = (Node *)lsecond(expr->args);
    if (IsA(typmod, Const) && !((Const *)typmod)->constisnull) {
      Node *source = (Node *)linitial(expr->args);
      int32 old_typmod = exprTypmod(source);
      int32 new_typmod = DatumGetInt32(((Const *)typmod)->constvalue);
      int32 old_scale = numeric_typmod_scale(old_typmod);
      int32 new_scale = numeric_typmod_scale(new_typmod);
      int32 old_precision = numeric_typmod_precision(old_typmod);
      int32 new_precision = numeric_typmod_precision(new_typmod);

      if (!is_valid_numeric_typmod(new_typmod) ||
          (is_valid_numeric_typmod(old_typmod) && new_scale == old_scale &&
           new_precision >= old_precision))
        ret = relabel_to_typmod(source, new_typmod);
    }
  }
  PG_RETURN_POINTER(ret);
}

/*
 * numeric() -
 *
 *	The length coercion of numeric: num rounded to typmod's scale, and
 *	checked against its precision.
 */
Datum numeric(PG_FUNCTION_ARGS) {
  wide_t w = dec_wide(PG_GETARG_NUMERIC(0));

  (void)apply_typmod(&w, PG_GETARG_INT32(1), NULL);
  PG_RETURN_NUMERIC(dec_from_wide(w));
}

/*
 * Validate a numeric precision and scale and pack them into a typmod, with
 * soft error handling: 1 to 36 digits, a scale from 0 to the precision.
 */
int32 make_numeric_typmod_safe(int32 precision, int32 scale, Node *escontext) {
  if (precision < 1 || precision > NUMERIC_MAX_PRECISION)
    ereturn(escontext, -1,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("NUMERIC precision %d must be between 1 and %d", precision,
                    NUMERIC_MAX_PRECISION)));
  if (scale < 0 || scale > precision)
    ereturn(escontext, -1,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("NUMERIC scale %d must be between %d and precision %d",
                    scale, 0, precision)));
  return ((precision << 16) | (scale & 0x7ff)) + VARHDRSZ;
}

/* The typmod of numeric(p) or numeric(p, s). */
Datum numerictypmodin(PG_FUNCTION_ARGS) {
  ArrayType *ta = PG_GETARG_ARRAYTYPE_P(0);
  int32 *tl;
  int n;
  int32 typmod;

  tl = ArrayGetIntegerTypmods(ta, &n);
  if (n == 2)
    typmod = make_numeric_typmod_safe(tl[0], tl[1], NULL);
  else if (n == 1)
    typmod = make_numeric_typmod_safe(tl[0], 0, NULL); /* scale defaults to 0 */
  else {
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("invalid NUMERIC type modifier")));
    typmod = 0; /* keep compiler quiet */
  }
  PG_RETURN_INT32(typmod);
}

/* A numeric typmod's text: "(p,s)". */
Datum numerictypmodout(PG_FUNCTION_ARGS) {
  int32 typmod = PG_GETARG_INT32(0);
  char *res = (char *)palloc(64);

  if (is_valid_numeric_typmod(typmod))
    snprintf(res, 64, "(%d,%d)", numeric_typmod_precision(typmod),
             numeric_typmod_scale(typmod));
  else
    *res = '\0';
  PG_RETURN_CSTRING(res);
}

/* The largest size a value of typmod takes: every value is a decimal_t. */
int32 numeric_maximum_size(int32 typmod) { return sizeof(decimal_t); }

/* Whether num is NaN: no value is. */
bool numeric_is_nan(Numeric num) { return false; }

/* Whether num is an infinity: no value is. */
bool numeric_is_inf(Numeric num) { return false; }

/* ----------------------------------------------------------------------
 * Sign, rounding and the like
 * ----------------------------------------------------------------------
 */

/* |num|, at its scale. */
Datum numeric_abs(PG_FUNCTION_ARGS) {
  wide_t w = dec_wide(PG_GETARG_NUMERIC(0));

  w.neg = false;
  PG_RETURN_NUMERIC(dec_from_wide(w));
}

/* -num, at its scale. */
Datum numeric_uminus(PG_FUNCTION_ARGS) {
  wide_t w = dec_wide(PG_GETARG_NUMERIC(0));

  w.neg = !w.neg && !wide_is_zero(w);
  PG_RETURN_NUMERIC(dec_from_wide(w));
}

/* +num: a copy. */
Datum numeric_uplus(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(DatumGetNumericCopy(PG_GETARG_DATUM(0)));
}

/* -1, 0 or 1 as num is negative, zero or positive. */
Datum numeric_sign(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(dec_make(dec_sign(PG_GETARG_NUMERIC(0)), 0));
}

/*
 * num rounded to scale places, half away from zero; a negative scale
 * rounds to tens, hundreds and so on, and gives scale 0.
 */
Datum numeric_round(PG_FUNCTION_ARGS) {
  wide_t w = dec_wide(PG_GETARG_NUMERIC(0));
  int32 scale = PG_GETARG_INT32(1);

  scale = Max(scale, -154);
  scale = Min(scale, NUMERIC_MAX_SCALE);
  if (scale >= 0)
    PG_RETURN_NUMERIC(dec_from_wide(wide_round(w, scale, false)));
  w = wide_round(w, 0, false);
  w = wide_round(wide_strip_zeros(w, 0), 0, false);
  {
    wide_t r = w;

    /* Round at 10^-scale: as a value of scale -scale, then back up. */
    r.scale = -scale;
    r = wide_round(r, 0, false);
    r = wide_scale_up(r, -scale);
    r.scale = 0;
    PG_RETURN_NUMERIC(dec_from_wide(r));
  }
}

/* num truncated to scale places, as numeric_round(). */
Datum numeric_trunc(PG_FUNCTION_ARGS) {
  wide_t w = dec_wide(PG_GETARG_NUMERIC(0));
  int32 scale = PG_GETARG_INT32(1);

  scale = Max(scale, -154);
  scale = Min(scale, NUMERIC_MAX_SCALE);
  if (scale >= 0)
    PG_RETURN_NUMERIC(dec_from_wide(wide_round(w, scale, true)));
  w = wide_round(w, 0, true);
  {
    wide_t r = w;

    r.scale = -scale;
    r = wide_round(r, 0, true);
    r = wide_scale_up(r, -scale);
    r.scale = 0;
    PG_RETURN_NUMERIC(dec_from_wide(r));
  }
}

/* The smallest integer not below w, or with floor the largest not above. */
static wide_t wide_ceil_floor(const wide_t &w, bool floor) {
  wide_t t = wide_round(w, 0, true);

  /* Truncation moved toward zero; past the value for one sign. */
  if (wide_cmp(t, w) != 0 && w.neg == floor)
    t = wide_add(t, wide_of_int(floor ? -1 : 1), false);
  return t;
}

/* The smallest integer not below num, at scale 0. */
Datum numeric_ceil(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      dec_from_wide(wide_ceil_floor(dec_wide(PG_GETARG_NUMERIC(0)), false)));
}

/* The largest integer not above num, at scale 0. */
Datum numeric_floor(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      dec_from_wide(wide_ceil_floor(dec_wide(PG_GETARG_NUMERIC(0)), true)));
}

/* num's scale. */
Datum numeric_scale(PG_FUNCTION_ARGS) {
  PG_RETURN_INT32(dec_scale(PG_GETARG_NUMERIC(0)));
}

/* The fewest places num needs: its scale with no zeros at the end. */
Datum numeric_min_scale(PG_FUNCTION_ARGS) {
  PG_RETURN_INT32(wide_strip_zeros(dec_wide(PG_GETARG_NUMERIC(0)), 0).scale);
}

/* num at the fewest places it needs. */
Datum numeric_trim_scale(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      dec_from_wide(wide_strip_zeros(dec_wide(PG_GETARG_NUMERIC(0)), 0)));
}

/* ----------------------------------------------------------------------
 * generate_series and width_bucket
 * ----------------------------------------------------------------------
 */

/* Where a numeric generate_series stands. */
typedef struct series_fctx_t {
  wide_t current;
  wide_t stop;
  wide_t step;
} series_fctx_t;

/* generate_series(start, stop): by 1. */
Datum generate_series_numeric(PG_FUNCTION_ARGS) {
  return generate_series_step_numeric(fcinfo);
}

/* generate_series(start, stop, step), step not zero. */
Datum generate_series_step_numeric(PG_FUNCTION_ARGS) {
  series_fctx_t *fctx;
  FuncCallContext *funcctx;

  if (SRF_IS_FIRSTCALL()) {
    MemoryContext oldcontext;
    wide_t step = wide_of_int(1);

    if (PG_NARGS() == 3) {
      step = dec_wide(PG_GETARG_NUMERIC(2));
      if (wide_is_zero(step))
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("step size cannot equal zero")));
    }
    funcctx = SRF_FIRSTCALL_INIT();
    oldcontext = MemoryContextSwitchTo(funcctx->multi_call_memory_ctx);
    fctx = (series_fctx_t *)palloc(sizeof(series_fctx_t));
    fctx->current = dec_wide(PG_GETARG_NUMERIC(0));
    fctx->stop = dec_wide(PG_GETARG_NUMERIC(1));
    fctx->step = step;
    funcctx->user_fctx = fctx;
    MemoryContextSwitchTo(oldcontext);
  }
  funcctx = SRF_PERCALL_SETUP();
  fctx = (series_fctx_t *)funcctx->user_fctx;
  if ((!fctx->step.neg && wide_cmp(fctx->current, fctx->stop) <= 0) ||
      (fctx->step.neg && wide_cmp(fctx->current, fctx->stop) >= 0)) {
    Numeric result = dec_from_wide(fctx->current);

    fctx->current = wide_add(fctx->current, fctx->step, false);
    SRF_RETURN_NEXT(funcctx, NumericGetDatum(result));
  }
  SRF_RETURN_DONE(funcctx);
}

/* Planner support for generate_series of numerics: its rows. */
Datum generate_series_numeric_support(PG_FUNCTION_ARGS) {
  Node *rawreq = (Node *)PG_GETARG_POINTER(0);
  Node *ret = NULL;

  if (IsA(rawreq, SupportRequestRows)) {
    SupportRequestRows *req = (SupportRequestRows *)rawreq;

    if (is_funcclause(req->node)) /* be paranoid */
    {
      List *args = ((FuncExpr *)req->node)->args;
      Node *arg1, *arg2, *arg3;

      arg1 = estimate_expression_value(req->root, (Node *)linitial(args));
      arg2 = estimate_expression_value(req->root, (Node *)lsecond(args));
      arg3 = list_length(args) >= 3
                 ? estimate_expression_value(req->root, (Node *)lthird(args))
                 : NULL;
      if ((IsA(arg1, Const) && ((Const *)arg1)->constisnull) ||
          (IsA(arg2, Const) && ((Const *)arg2)->constisnull) ||
          (arg3 != NULL && IsA(arg3, Const) && ((Const *)arg3)->constisnull)) {
        req->rows = 0;
        ret = (Node *)req;
      } else if (IsA(arg1, Const) && IsA(arg2, Const) &&
                 (arg3 == NULL || IsA(arg3, Const))) {
        wide_t start = dec_wide(DatumGetNumeric(((Const *)arg1)->constvalue));
        wide_t stop = dec_wide(DatumGetNumeric(((Const *)arg2)->constvalue));
        wide_t step =
            arg3 ? dec_wide(DatumGetNumeric(((Const *)arg3)->constvalue))
                 : wide_of_int(1);

        if (!wide_is_zero(step)) {
          wide_t res = wide_add(stop, start, true);

          if (!wide_is_zero(res) && step.neg != res.neg)
            req->rows = 0;
          else
            req->rows =
                strtod(wide_text(wide_div(res, step, 0, true)), NULL) + 1;
          ret = (Node *)req;
        }
      }
    }
  }
  PG_RETURN_POINTER(ret);
}

/*
 * width_bucket(operand, bound1, bound2, count): which of count equal
 * buckets between the bounds operand falls in; 0 before them, count + 1
 * after.
 */
Datum width_bucket_numeric(PG_FUNCTION_ARGS) {
  wide_t operand = dec_wide(PG_GETARG_NUMERIC(0));
  wide_t bound1 = dec_wide(PG_GETARG_NUMERIC(1));
  wide_t bound2 = dec_wide(PG_GETARG_NUMERIC(2));
  int32 count = PG_GETARG_INT32(3);
  int32 result;
  int c = wide_cmp(bound1, bound2);

  if (count <= 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
                    errmsg("count must be greater than zero")));
  if (c == 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_WIDTH_BUCKET_FUNCTION),
                    errmsg("lower bound cannot equal upper bound")));
  if (c < 0 ? wide_cmp(operand, bound1) < 0 : wide_cmp(operand, bound1) > 0)
    result = 0;
  else if (c < 0 ? wide_cmp(operand, bound2) >= 0
                 : wide_cmp(operand, bound2) <= 0) {
    if (pg_add_s32_overflow(count, 1, &result))
      ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                      errmsg("integer out of range")));
  } else {
    /* floor((operand - low) * count / (high - low)) + 1, all positive */
    wide_t num = c < 0 ? wide_add(operand, bound1, true)
                       : wide_add(bound1, operand, true);
    wide_t den =
        c < 0 ? wide_add(bound2, bound1, true) : wide_add(bound1, bound2, true);
    wide_t q = wide_div(wide_mul(num, wide_of_int(count)), den, 0, true);

    result = (int32)q.mag.limb[0] + 1;
  }
  PG_RETURN_INT32(result);
}

/* ----------------------------------------------------------------------
 * Comparison and hashing
 * ----------------------------------------------------------------------
 */

/* -1, 0 or 1 as a is below, equal to or above b, at any scales. */
static int dec_cmp(const decimal_t *a, const decimal_t *b) {
  i128 va = dec_value(a), vb = dec_value(b);

  if (dec_scale(a) == dec_scale(b))
    return va < vb ? -1 : (va > vb ? 1 : 0);
  return wide_cmp(dec_wide(a), dec_wide(b));
}

/* The comparator of a sort of numerics. */
static int numeric_fast_cmp(Datum x, Datum y, SortSupport ssup) {
  return dec_cmp(DatumGetNumeric(x), DatumGetNumeric(y));
}

/* Sort support: a plain comparator, no abbreviation. */
Datum numeric_sortsupport(PG_FUNCTION_ARGS) {
  SortSupport ssup = (SortSupport)PG_GETARG_POINTER(0);

  ssup->comparator = numeric_fast_cmp;
  PG_RETURN_VOID();
}

/* btree comparison: -1, 0 or 1. */
Datum numeric_cmp(PG_FUNCTION_ARGS) {
  PG_RETURN_INT32(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)));
}

/* a = b, at any scales: 1.5 = 1.50. */
Datum numeric_eq(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) == 0);
}

/* a <> b. */
Datum numeric_ne(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) != 0);
}

/* a > b. */
Datum numeric_gt(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) > 0);
}

/* a >= b. */
Datum numeric_ge(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) >= 0);
}

/* a < b. */
Datum numeric_lt(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) < 0);
}

/* a <= b. */
Datum numeric_le(PG_FUNCTION_ARGS) {
  PG_RETURN_BOOL(dec_cmp(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1)) <= 0);
}

/*
 * in_range support for a window's RANGE frame: whether val is within
 * offset before (sub) or after base, as less says.
 */
Datum in_range_numeric_numeric(PG_FUNCTION_ARGS) {
  wide_t val = dec_wide(PG_GETARG_NUMERIC(0));
  wide_t base = dec_wide(PG_GETARG_NUMERIC(1));
  wide_t offset = dec_wide(PG_GETARG_NUMERIC(2));
  bool sub = PG_GETARG_BOOL(3);
  bool less = PG_GETARG_BOOL(4);
  wide_t sum;

  if (offset.neg)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PRECEDING_OR_FOLLOWING_SIZE),
             errmsg("invalid preceding or following size in window function")));
  sum = wide_add(base, offset, sub);
  PG_RETURN_BOOL(less ? wide_cmp(val, sum) <= 0 : wide_cmp(val, sum) >= 0);
}

/*
 * The bytes a value hashes by: its integer and scale with no zeros at the
 * end, so that equal values, 1.5 and 1.50, hash alike.
 */
typedef struct hash_key_t {
  i128 value;
  int16 scale;
} hash_key_t;

/* num's hash key. */
static hash_key_t dec_hash_key(const decimal_t *num) {
  hash_key_t k;
  i128 v = dec_value(num);
  int s = dec_scale(num);

  memset(&k, 0, sizeof(k));
  while (s > 0 && v % 10 == 0) {
    v /= 10;
    s--;
  }
  if (v == 0)
    s = 0;
  k.value = v;
  k.scale = (int16)s;
  return k;
}

/* hash_numeric: equal values, whatever their scales, hash alike. */
Datum hash_numeric(PG_FUNCTION_ARGS) {
  hash_key_t k = dec_hash_key(PG_GETARG_NUMERIC(0));

  return hash_any((const unsigned char *)&k, sizeof(k));
}

/* hash_numeric_extended: as hash_numeric, with a seed. */
Datum hash_numeric_extended(PG_FUNCTION_ARGS) {
  hash_key_t k = dec_hash_key(PG_GETARG_NUMERIC(0));

  return hash_any_extended((const unsigned char *)&k, sizeof(k),
                           PG_GETARG_INT64(1));
}

/* ----------------------------------------------------------------------
 * Arithmetic
 * ----------------------------------------------------------------------
 */

/*
 * The scale of a quotient, PostgreSQL's: at least 16 significant digits,
 * not below either operand's scale, at most 36.
 */
static int select_div_scale(const wide_t &a, const wide_t &b) {
  nbase_t na = dec_nbase(a), nb = dec_nbase(b);
  int qweight = na.weight - nb.weight;
  int rscale;

  if (na.first <= nb.first)
    qweight--;
  rscale = NUMERIC_MIN_SIG_DIGITS - qweight * 4;
  rscale = Max(rscale, a.scale);
  rscale = Max(rscale, b.scale);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  return rscale;
}

/* The error of a division by zero, or false with *have_error set. */
static bool division_by_zero(bool *have_error) {
  if (have_error == NULL)
    ereport(ERROR,
            (errcode(ERRCODE_DIVISION_BY_ZERO), errmsg("division by zero")));
  *have_error = true;
  return false;
}

/* num1 + num2, or NULL with *have_error set past 36 digits. */
Numeric numeric_add_opt_error(Numeric num1, Numeric num2, bool *have_error) {
  Numeric res;

  if (dec_scale(num1) == dec_scale(num2)) {
    i128 r;

    if (!__builtin_add_overflow(dec_value(num1), dec_value(num2), &r)) {
      wide_t w = wide_of_int(r);

      w.scale = dec_scale(num1);
      return dec_pack(w, &res, have_error) ? res : NULL;
    }
  }
  return dec_pack(wide_add(dec_wide(num1), dec_wide(num2), false), &res,
                  have_error)
             ? res
             : NULL;
}

/* num1 - num2, or NULL with *have_error set past 36 digits. */
Numeric numeric_sub_opt_error(Numeric num1, Numeric num2, bool *have_error) {
  Numeric res;

  return dec_pack(wide_add(dec_wide(num1), dec_wide(num2), true), &res,
                  have_error)
             ? res
             : NULL;
}

/*
 * num1 * num2 at the sum of their scales, places past 36 digits rounded
 * away; NULL with *have_error set if the integer part is past 36 digits.
 */
Numeric numeric_mul_opt_error(Numeric num1, Numeric num2, bool *have_error) {
  Numeric res;

  return dec_pack(wide_mul(dec_wide(num1), dec_wide(num2)), &res, have_error)
             ? res
             : NULL;
}

/* num1 / num2 at PostgreSQL's scale; NULL with *have_error on an error. */
Numeric numeric_div_opt_error(Numeric num1, Numeric num2, bool *have_error) {
  wide_t a = dec_wide(num1), b = dec_wide(num2);
  Numeric res;

  if (wide_is_zero(b)) {
    division_by_zero(have_error);
    return NULL;
  }
  return dec_pack(wide_div(a, b, select_div_scale(a, b), false), &res,
                  have_error)
             ? res
             : NULL;
}

/* num1 mod num2, at the larger scale, the sign of num1. */
Numeric numeric_mod_opt_error(Numeric num1, Numeric num2, bool *have_error) {
  wide_t a = dec_wide(num1), b = dec_wide(num2);
  int s = Max(a.scale, b.scale);
  wide_t r;
  Numeric res;

  if (wide_is_zero(b)) {
    division_by_zero(have_error);
    return NULL;
  }
  a = wide_scale_up(a, s);
  b = wide_scale_up(b, s);
  mag_divmod(a.mag, b.mag, &r.mag);
  r.neg = a.neg && !mag_is_zero(r.mag);
  r.scale = s;
  return dec_pack(r, &res, have_error) ? res : NULL;
}

/* num1 + num2. */
Datum numeric_add(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      numeric_add_opt_error(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1), NULL));
}

/* num1 - num2. */
Datum numeric_sub(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      numeric_sub_opt_error(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1), NULL));
}

/* num1 * num2. */
Datum numeric_mul(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      numeric_mul_opt_error(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1), NULL));
}

/* num1 / num2. */
Datum numeric_div(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      numeric_div_opt_error(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1), NULL));
}

/* div(num1, num2): the quotient truncated to an integer, scale 0. */
Datum numeric_div_trunc(PG_FUNCTION_ARGS) {
  wide_t a = dec_wide(PG_GETARG_NUMERIC(0)), b = dec_wide(PG_GETARG_NUMERIC(1));

  if (wide_is_zero(b))
    division_by_zero(NULL);
  PG_RETURN_NUMERIC(dec_from_wide(wide_div(a, b, 0, true)));
}

/* num1 % num2. */
Datum numeric_mod(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(
      numeric_mod_opt_error(PG_GETARG_NUMERIC(0), PG_GETARG_NUMERIC(1), NULL));
}

/* num + 1, at its scale. */
Datum numeric_inc(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(dec_from_wide(
      wide_add(dec_wide(PG_GETARG_NUMERIC(0)), wide_of_int(1), false)));
}

/* The smaller of two values: a copy of it. */
Datum numeric_smaller(PG_FUNCTION_ARGS) {
  Numeric a = PG_GETARG_NUMERIC(0), b = PG_GETARG_NUMERIC(1);

  PG_RETURN_NUMERIC(
      DatumGetNumericCopy(NumericGetDatum(dec_cmp(a, b) <= 0 ? a : b)));
}

/* The larger of two values: a copy of it. */
Datum numeric_larger(PG_FUNCTION_ARGS) {
  Numeric a = PG_GETARG_NUMERIC(0), b = PG_GETARG_NUMERIC(1);

  PG_RETURN_NUMERIC(
      DatumGetNumericCopy(NumericGetDatum(dec_cmp(a, b) >= 0 ? a : b)));
}

/* The greatest common divisor of two magnitudes. */
static mag_t mag_gcd(mag_t a, mag_t b) {
  while (!mag_is_zero(b)) {
    mag_t r;

    mag_divmod(a, b, &r);
    a = b;
    b = r;
  }
  return a;
}

/*
 * gcd(num1, num2): the largest value both are whole multiples of, at the
 * larger scale; never negative.
 */
Datum numeric_gcd(PG_FUNCTION_ARGS) {
  wide_t a = dec_wide(PG_GETARG_NUMERIC(0)), b = dec_wide(PG_GETARG_NUMERIC(1));
  int s = Max(a.scale, b.scale);
  wide_t r;

  a = wide_scale_up(a, s);
  b = wide_scale_up(b, s);
  r.mag = mag_gcd(a.mag, b.mag);
  r.neg = false;
  r.scale = s;
  PG_RETURN_NUMERIC(dec_from_wide(r));
}

/*
 * lcm(num1, num2): the smallest value that is a whole multiple of both, at
 * the larger scale; zero if either is.
 */
Datum numeric_lcm(PG_FUNCTION_ARGS) {
  wide_t a = dec_wide(PG_GETARG_NUMERIC(0)), b = dec_wide(PG_GETARG_NUMERIC(1));
  int s = Max(a.scale, b.scale);
  wide_t r;

  a = wide_scale_up(a, s);
  b = wide_scale_up(b, s);
  r.neg = false;
  r.scale = s;
  if (wide_is_zero(a) || wide_is_zero(b))
    r.mag = mag_of(0);
  else
    r.mag = mag_mul(mag_divmod(a.mag, mag_gcd(a.mag, b.mag), NULL), b.mag);
  PG_RETURN_NUMERIC(dec_from_wide(r));
}

/* factorial(n), for n of at most 33, the last whose factorial fits. */
Datum numeric_fac(PG_FUNCTION_ARGS) {
  int64 num = PG_GETARG_INT64(0);
  mag_t r = mag_of(1);

  if (num < 0)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("factorial of a negative number is undefined")));
  if (num > 100)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("value overflows numeric format")));
  for (int64 i = 2; i <= num; i++)
    r = mag_mul_small(r, (uint64)i);
  PG_RETURN_NUMERIC(dec_from_wide(wide_t{r, false, 0}));
}

/* ----------------------------------------------------------------------
 * Square root, logarithms and powers
 * ----------------------------------------------------------------------
 */

/* w as a long double. */
static long double wide_ld(const wide_t &w) {
  return strtold(wide_text(w), NULL);
}

/*
 * x, a finite long double, as a value at scale rscale; past 36 digits
 * before the point is an error.
 */
static Numeric dec_of_ld(long double x, int rscale) {
  char buf[512];
  wide_t w;
  const char *end;
  bool neg = x < 0;

  if (!std::isfinite(x) || fabsl(x) >= 1e40L)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("value overflows numeric format")));
  snprintf(buf, sizeof(buf), "%.*Lf", rscale, fabsl(x));
  parse_decimal(buf, buf, &w, &end, NULL);
  w.neg = neg && !wide_is_zero(w);
  return dec_from_wide(w);
}

/* The square root of num, at PostgreSQL's scale for it, rounded. */
Datum numeric_sqrt(PG_FUNCTION_ARGS) {
  wide_t arg = dec_wide(PG_GETARG_NUMERIC(0));
  nbase_t nb = dec_nbase(arg);
  int sweight, rscale, e;
  mag_t m;
  wide_t r;

  if (arg.neg)
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
                    errmsg("cannot take square root of a negative number")));
  /* At least 16 significant digits, not below the argument's scale. */
  sweight = nb.weight * 4 / 2 + 1;
  rscale = NUMERIC_MIN_SIG_DIGITS - sweight;
  rscale = Max(rscale, arg.scale);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  /* floor(sqrt(x) * 10^(rscale + 1)), from x * 10^(2 * (rscale + 1)). */
  e = 2 * (rscale + 1) - arg.scale;
  m = e >= 0 ? mag_mul(arg.mag, mag_pow10(e))
             : mag_divmod(arg.mag, mag_pow10(-e), NULL);
  r.mag = mag_isqrt(m);
  r.neg = false;
  r.scale = rscale + 1;
  PG_RETURN_NUMERIC(dec_from_wide(wide_round(r, rscale, false)));
}

/*
 * The decimal weight of ln(w), as PostgreSQL estimates it to choose a
 * result scale; w is positive.
 */
static int estimate_ln_dweight(const wide_t &w) {
  wide_t lo = wide_of_int(9), hi = wide_of_int(11);

  lo.scale = 1;
  hi.scale = 1;
  if (w.neg || wide_is_zero(w))
    return 0;
  if (wide_cmp(w, lo) >= 0 && wide_cmp(w, hi) <= 0) {
    /* 0.9 <= w <= 1.1: ln(w) is about w - 1. */
    wide_t x = wide_add(w, wide_of_int(1), true);

    if (wide_is_zero(x))
      return 0;
    return mag_digits(x.mag) - 1 - x.scale;
  } else {
    nbase_t nb = dec_nbase(w);
    int digits = nb.first;
    int dweight = nb.weight * 4;
    double ln_var;

    if (nb.ndigits > 1) {
      digits = digits * 10000 + nb.second;
      dweight -= 4;
    }
    ln_var = log((double)digits) + dweight * 2.302585092994046;
    return (int)log10(fabs(ln_var));
  }
}

/* The error of a logarithm of zero or a negative number, if w is one. */
static void check_log_argument(const wide_t &w) {
  if (wide_is_zero(w))
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
                    errmsg("cannot take logarithm of zero")));
  if (w.neg)
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_LOG),
                    errmsg("cannot take logarithm of a negative number")));
}

/* e raised to num, at PostgreSQL's scale for it. */
Datum numeric_exp(PG_FUNCTION_ARGS) {
  wide_t arg = dec_wide(PG_GETARG_NUMERIC(0));
  long double x = wide_ld(arg);
  double val = (double)x * 0.434294481903252; /* the result's dweight */
  int rscale;

  val = Max(val, -NUMERIC_MAX_RESULT_SCALE);
  val = Min(val, NUMERIC_MAX_RESULT_SCALE);
  rscale = NUMERIC_MIN_SIG_DIGITS - (int)val;
  rscale = Max(rscale, arg.scale);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  PG_RETURN_NUMERIC(dec_of_ld(expl(x), rscale));
}

/* The natural logarithm of num, at PostgreSQL's scale for it. */
Datum numeric_ln(PG_FUNCTION_ARGS) {
  wide_t arg = dec_wide(PG_GETARG_NUMERIC(0));
  int rscale;

  check_log_argument(arg);
  rscale = NUMERIC_MIN_SIG_DIGITS - estimate_ln_dweight(arg);
  rscale = Max(rscale, arg.scale);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  PG_RETURN_NUMERIC(dec_of_ld(logl(wide_ld(arg)), rscale));
}

/* log(base, num): the logarithm of num to base, at PostgreSQL's scale. */
Datum numeric_log(PG_FUNCTION_ARGS) {
  wide_t base = dec_wide(PG_GETARG_NUMERIC(0));
  wide_t num = dec_wide(PG_GETARG_NUMERIC(1));
  long double ln_base;
  int result_dweight, rscale;

  check_log_argument(base);
  check_log_argument(num);
  result_dweight = estimate_ln_dweight(num) - estimate_ln_dweight(base);
  rscale = NUMERIC_MIN_SIG_DIGITS - result_dweight;
  rscale = Max(rscale, base.scale);
  rscale = Max(rscale, num.scale);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  ln_base = logl(wide_ld(base));
  if (ln_base == 0)
    division_by_zero(NULL);
  PG_RETURN_NUMERIC(dec_of_ld(logl(wide_ld(num)) / ln_base, rscale));
}

/*
 * base ^ exp for an integer exp, exact where its digits fit 512 bits, and
 * rounded to PostgreSQL's scale for it.
 */
static Numeric power_int(const wide_t &base, int64 exp, int exp_scale) {
  nbase_t nb = dec_nbase(base);
  double f;
  int rscale;
  mag_t m = mag_of(1);
  wide_t r;
  uint64 n = exp < 0 ? (uint64)0 - (uint64)exp : (uint64)exp;

  /* The approximate decimal weight of the result, PostgreSQL's way. */
  if (!wide_is_zero(base)) {
    double d = nb.first;
    int p = nb.weight * 4;

    if (nb.ndigits > 1) {
      d = d * 10000 + nb.second;
      p -= 4;
    }
    f = (double)exp * (log10(d) + p);
  } else
    f = 0;
  if (f > DEC_MAX_DIGITS)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("value overflows numeric format")));
  rscale = NUMERIC_MIN_SIG_DIGITS - (int)Max(f, -1000.0);
  rscale = Max(rscale, base.scale);
  rscale = Max(rscale, exp_scale);
  rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
  rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
  if (wide_is_zero(base)) {
    if (exp < 0)
      ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
                      errmsg("zero raised to a negative power is undefined")));
    r = wide_of_int(exp == 0 ? 1 : 0);
    return dec_from_wide(wide_scale_up(r, rscale));
  }
  /* Exact if base's magnitude to n has at most 150 digits. */
  if ((double)n * mag_digits(base.mag) > 150.0) {
    long double x = powl(wide_ld(base), (long double)exp);

    return dec_of_ld(x, rscale);
  }
  for (uint64 i = 0; i < n; i++)
    m = mag_mul(m, base.mag);
  r.mag = m;
  r.neg = base.neg && (n & 1);
  r.scale = (int)(base.scale * n);
  if (exp < 0)
    return dec_from_wide(wide_div(wide_of_int(1), r, rscale, false));
  return dec_from_wide(wide_round(r, rscale, false));
}

/* num1 ^ num2, at PostgreSQL's scale for it. */
Datum numeric_power(PG_FUNCTION_ARGS) {
  wide_t base = dec_wide(PG_GETARG_NUMERIC(0));
  wide_t exp = dec_wide(PG_GETARG_NUMERIC(1));
  wide_t exp_int = wide_round(exp, 0, true);
  bool exp_is_int = wide_cmp(exp_int, exp) == 0;
  long double ln_num, val;
  int rscale;

  if (wide_is_zero(base) && exp.neg)
    ereport(ERROR, (errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
                    errmsg("zero raised to a negative power is undefined")));
  if (base.neg && !exp_is_int)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_ARGUMENT_FOR_POWER_FUNCTION),
             errmsg("a negative number raised to a non-integer power yields "
                    "a complex result")));
  if (exp_is_int && mag_digits(exp_int.mag) <= 9)
    PG_RETURN_NUMERIC(power_int(
        base, (exp.neg ? -1 : 1) * (int64)exp_int.mag.limb[0], exp.scale));
  if (wide_is_zero(base))
    PG_RETURN_NUMERIC(
        dec_from_wide(wide_scale_up(wide_of_int(0), NUMERIC_MIN_SIG_DIGITS)));
  {
    wide_t abs_base = base;

    abs_base.neg = false;
    ln_num = logl(wide_ld(abs_base)) * wide_ld(exp);
    if (fabsl(ln_num) > NUMERIC_MAX_RESULT_SCALE * 3.01L) {
      if (ln_num > 0)
        ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                        errmsg("value overflows numeric format")));
      PG_RETURN_NUMERIC(dec_from_wide(
          wide_scale_up(wide_of_int(0), NUMERIC_MAX_DISPLAY_SCALE)));
    }
    val = ln_num * 0.434294481903252L; /* the result's dweight */
    rscale = NUMERIC_MIN_SIG_DIGITS - (int)val;
    rscale = Max(rscale, base.scale);
    rscale = Max(rscale, exp.scale);
    rscale = Max(rscale, NUMERIC_MIN_DISPLAY_SCALE);
    rscale = Min(rscale, NUMERIC_MAX_DISPLAY_SCALE);
    {
      long double x = expl(ln_num);

      /* A negative base has an integer exponent: odd keeps the sign. */
      if (base.neg && (exp_int.mag.limb[0] & 1))
        x = -x;
      PG_RETURN_NUMERIC(dec_of_ld(x, rscale));
    }
  }
}

/* ----------------------------------------------------------------------
 * Casts
 * ----------------------------------------------------------------------
 */

/* val as a numeric of scale 0. */
Numeric int64_to_numeric(int64 val) { return dec_make(val, 0); }

/*
 * val1 / 10^log10val2, exactly, at scale log10val2: for a fraction kept as
 * an integer, as extract() does.
 */
Numeric int64_div_fast_to_numeric(int64 val1, int log10val2) {
  if (log10val2 < 0) {
    wide_t w = wide_of_int(val1);

    w.mag = mag_mul(w.mag, mag_pow10(-log10val2));
    return dec_from_wide(w);
  }
  return dec_make(val1, log10val2);
}

/* int4 to numeric. */
Datum int4_numeric(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(int64_to_numeric(PG_GETARG_INT32(0)));
}

/* int8 to numeric. */
Datum int8_numeric(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(int64_to_numeric(PG_GETARG_INT64(0)));
}

/* int2 to numeric. */
Datum int2_numeric(PG_FUNCTION_ARGS) {
  PG_RETURN_NUMERIC(int64_to_numeric(PG_GETARG_INT16(0)));
}

/*
 * num rounded to an integer, half away from zero, if it lies within lo and
 * hi; else false.
 */
static bool dec_to_int(const decimal_t *num, i128 lo, i128 hi, i128 *out) {
  wide_t w = wide_round(dec_wide(num), 0, false);
  u128 mag;
  i128 v;

  if (mag_digits(w.mag) > 38)
    return false;
  mag = ((u128)w.mag.limb[1] << 64) | w.mag.limb[0];
  v = w.neg ? -(i128)mag : (i128)mag;
  if (v < lo || v > hi)
    return false;
  *out = v;
  return true;
}

/* num as an int4, or false with *have_error set out of its range. */
int32 numeric_int4_opt_error(Numeric num, bool *have_error) {
  i128 v;

  if (!dec_to_int(num, PG_INT32_MIN, PG_INT32_MAX, &v)) {
    if (have_error) {
      *have_error = true;
      return 0;
    }
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("integer out of range")));
  }
  return (int32)v;
}

/* num as an int8, or false with *have_error set out of its range. */
int64 numeric_int8_opt_error(Numeric num, bool *have_error) {
  i128 v;

  if (!dec_to_int(num, PG_INT64_MIN, PG_INT64_MAX, &v)) {
    if (have_error) {
      *have_error = true;
      return 0;
    }
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("bigint out of range")));
  }
  return (int64)v;
}

/* numeric to int4, rounded. */
Datum numeric_int4(PG_FUNCTION_ARGS) {
  PG_RETURN_INT32(numeric_int4_opt_error(PG_GETARG_NUMERIC(0), NULL));
}

/* numeric to int8, rounded. */
Datum numeric_int8(PG_FUNCTION_ARGS) {
  PG_RETURN_INT64(numeric_int8_opt_error(PG_GETARG_NUMERIC(0), NULL));
}

/* numeric to int2, rounded. */
Datum numeric_int2(PG_FUNCTION_ARGS) {
  i128 v;

  if (!dec_to_int(PG_GETARG_NUMERIC(0), PG_INT16_MIN, PG_INT16_MAX, &v))
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("smallint out of range")));
  PG_RETURN_INT16((int16)v);
}

/* The error of a float with no numeric value: NaN or an infinity. */
static void float_not_numeric(double val) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("cannot convert %s to numeric",
                         std::isnan(val) ? "NaN" : "infinity")));
}

/* A float's text, digits of it, as a numeric. */
static Numeric float_text_numeric(const char *buf) {
  return DatumGetNumeric(DirectFunctionCall3(numeric_in, CStringGetDatum(buf),
                                             ObjectIdGetDatum(InvalidOid),
                                             Int32GetDatum(-1)));
}

/* float8 to numeric: its 15 significant digits, as PostgreSQL takes them. */
Datum float8_numeric(PG_FUNCTION_ARGS) {
  float8 val = PG_GETARG_FLOAT8(0);
  char buf[DBL_DIG + 100];

  if (std::isnan(val) || std::isinf(val))
    float_not_numeric(val);
  snprintf(buf, sizeof(buf), "%.*g", DBL_DIG, val);
  PG_RETURN_NUMERIC(float_text_numeric(buf));
}

/* float4 to numeric: its 6 significant digits. */
Datum float4_numeric(PG_FUNCTION_ARGS) {
  float4 val = PG_GETARG_FLOAT4(0);
  char buf[FLT_DIG + 100];

  if (std::isnan(val) || std::isinf(val))
    float_not_numeric(val);
  snprintf(buf, sizeof(buf), "%.*g", FLT_DIG, (double)val);
  PG_RETURN_NUMERIC(float_text_numeric(buf));
}

/* numeric to float8, by its text. */
Datum numeric_float8(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT8(strtod(wide_text(dec_wide(PG_GETARG_NUMERIC(0))), NULL));
}

/* numeric to float8, for the planner: never an error. */
Datum numeric_float8_no_overflow(PG_FUNCTION_ARGS) {
  return numeric_float8(fcinfo);
}

/* numeric to float4, by its text. */
Datum numeric_float4(PG_FUNCTION_ARGS) {
  PG_RETURN_FLOAT4(strtof(wide_text(dec_wide(PG_GETARG_NUMERIC(0))), NULL));
}

/* numeric to pg_lsn: rounded to an integer, from 0 to 2^64 - 1. */
Datum numeric_pg_lsn(PG_FUNCTION_ARGS) {
  wide_t w = wide_round(dec_wide(PG_GETARG_NUMERIC(0)), 0, false);

  if (w.neg || mag_bits(w.mag) > 64)
    ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE),
                    errmsg("pg_lsn out of range")));
  PG_RETURN_LSN((XLogRecPtr)w.mag.limb[0]);
}

/* ----------------------------------------------------------------------
 * random(min, max)
 * ----------------------------------------------------------------------
 */

/*
 * A random value from rmin to rmax, both included, uniformly, at the
 * larger of their scales.
 */
Numeric random_numeric(pg_prng_state *state, Numeric rmin, Numeric rmax) {
  wide_t lo = dec_wide(rmin), hi = dec_wide(rmax);
  int s = Max(lo.scale, hi.scale);
  wide_t range;
  mag_t r;
  int bits;

  lo = wide_scale_up(lo, s);
  hi = wide_scale_up(hi, s);
  if (wide_cmp(lo, hi) > 0)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("lower bound must be less than or equal to upper bound")));
  range = wide_add(hi, lo, true);
  bits = mag_bits(range.mag);
  /* Draw bits as wide as the range, again while the draw is past it. */
  do {
    r = mag_of(0);
    for (int i = 0; i * 64 < bits; i++)
      r.limb[i] = pg_prng_uint64(state);
    if (bits % 64 != 0)
      r.limb[(bits - 1) / 64] &= ((uint64)1 << (bits % 64)) - 1;
  } while (mag_cmp(r, range.mag) > 0);
  return dec_from_wide(wide_add(lo, wide_t{r, false, s}, false));
}

/* ----------------------------------------------------------------------
 * Aggregates: sum, avg, variance and standard deviation, of numeric and of
 * the integers, accumulated exactly.
 * ----------------------------------------------------------------------
 */

/* An aggregate's state: its count, and the sums of its values and squares. */
typedef struct dec_agg_t {
  bool calc_sumX2; /* whether squares are summed, for variance */
  MemoryContext agg_context;
  int64 N;               /* values counted */
  wide_t sumX;           /* their sum, exact */
  wide_t sumX2;          /* the sum of their squares, exact */
  int max_scale;         /* the largest scale among them */
  int64 max_scale_count; /* how many have it */
} dec_agg_t;

/* A new state in the aggregate's memory context. */
static dec_agg_t *make_agg_state(FunctionCallInfo fcinfo, bool calc_sumX2) {
  MemoryContext agg_context;
  dec_agg_t *state;

  if (!AggCheckCallContext(fcinfo, &agg_context))
    elog(ERROR, "aggregate function called in non-aggregate context");
  state = (dec_agg_t *)MemoryContextAllocZero(agg_context, sizeof(dec_agg_t));
  state->calc_sumX2 = calc_sumX2;
  state->agg_context = agg_context;
  state->sumX = wide_of_int(0);
  state->sumX2 = wide_of_int(0);
  return state;
}

/* Count x into state. */
static void agg_accum(dec_agg_t *state, const wide_t &x) {
  state->N++;
  if (x.scale > state->max_scale) {
    state->max_scale = x.scale;
    state->max_scale_count = 1;
  } else if (x.scale == state->max_scale)
    state->max_scale_count++;
  state->sumX = wide_add(state->sumX, x, false);
  if (state->calc_sumX2)
    state->sumX2 = wide_add(state->sumX2, wide_mul(x, x), false);
}

/*
 * Take x out of state, for a moving aggregate; false if the state can no
 * longer say its sum's scale, the last value of the largest scale gone,
 * and the aggregate must be computed again.
 */
static bool agg_discard(dec_agg_t *state, const wide_t &x) {
  if (x.scale == state->max_scale) {
    if (state->max_scale_count > 1 || state->max_scale == 0)
      state->max_scale_count--;
    else if (state->N == 1) {
      state->max_scale = 0;
      state->max_scale_count = 0;
    } else
      return false;
  }
  state->N--;
  state->sumX = wide_add(state->sumX, x, true);
  if (state->calc_sumX2)
    state->sumX2 = wide_add(state->sumX2, wide_mul(x, x), true);
  if (state->N == 0) {
    state->sumX = wide_of_int(0);
    state->sumX2 = wide_of_int(0);
  }
  return true;
}

/* The state argument 0, or a new one if it is null. */
static dec_agg_t *agg_state_arg(FunctionCallInfo fcinfo, bool calc_sumX2) {
  return PG_ARGISNULL(0) ? make_agg_state(fcinfo, calc_sumX2)
                         : (dec_agg_t *)PG_GETARG_POINTER(0);
}

/* Accumulate a numeric, summing squares too: variance and stddev. */
Datum numeric_accum(PG_FUNCTION_ARGS) {
  dec_agg_t *state = agg_state_arg(fcinfo, true);

  if (!PG_ARGISNULL(1))
    agg_accum(state, dec_wide(PG_GETARG_NUMERIC(1)));
  PG_RETURN_POINTER(state);
}

/* Accumulate a numeric for sum and avg. */
Datum numeric_avg_accum(PG_FUNCTION_ARGS) {
  dec_agg_t *state = agg_state_arg(fcinfo, false);

  if (!PG_ARGISNULL(1))
    agg_accum(state, dec_wide(PG_GETARG_NUMERIC(1)));
  PG_RETURN_POINTER(state);
}

/* Take a numeric out of a moving aggregate; null to compute it again. */
Datum numeric_accum_inv(PG_FUNCTION_ARGS) {
  dec_agg_t *state;

  if (PG_ARGISNULL(0))
    elog(ERROR, "numeric_accum_inv called with NULL state");
  state = (dec_agg_t *)PG_GETARG_POINTER(0);
  if (!PG_ARGISNULL(1) && !agg_discard(state, dec_wide(PG_GETARG_NUMERIC(1))))
    PG_RETURN_NULL();
  PG_RETURN_POINTER(state);
}

/* Accumulate an integer, argument 1, of the call's type. */
static Datum int_accum(FunctionCallInfo fcinfo, int64 value, bool calc_sumX2) {
  dec_agg_t *state = agg_state_arg(fcinfo, calc_sumX2);

  if (!PG_ARGISNULL(1))
    agg_accum(state, wide_of_int(value));
  PG_RETURN_POINTER(state);
}

/* Accumulate an int2 for variance and stddev. */
Datum int2_accum(PG_FUNCTION_ARGS) {
  return int_accum(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT16(1), true);
}

/* Accumulate an int4 for variance and stddev. */
Datum int4_accum(PG_FUNCTION_ARGS) {
  return int_accum(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT32(1), true);
}

/* Accumulate an int8 for variance and stddev. */
Datum int8_accum(PG_FUNCTION_ARGS) {
  return int_accum(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT64(1), true);
}

/* Accumulate an int8 for sum and avg. */
Datum int8_avg_accum(PG_FUNCTION_ARGS) {
  return int_accum(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT64(1), false);
}

/* Take an integer out of a moving aggregate. */
static Datum int_accum_inv(FunctionCallInfo fcinfo, int64 value) {
  dec_agg_t *state;

  if (PG_ARGISNULL(0))
    elog(ERROR, "integer accumulator inverse called with NULL state");
  state = (dec_agg_t *)PG_GETARG_POINTER(0);
  if (!PG_ARGISNULL(1))
    (void)agg_discard(state, wide_of_int(value));
  PG_RETURN_POINTER(state);
}

/* Take an int2 out of a moving variance. */
Datum int2_accum_inv(PG_FUNCTION_ARGS) {
  return int_accum_inv(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT16(1));
}

/* Take an int4 out of a moving variance. */
Datum int4_accum_inv(PG_FUNCTION_ARGS) {
  return int_accum_inv(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT32(1));
}

/* Take an int8 out of a moving variance. */
Datum int8_accum_inv(PG_FUNCTION_ARGS) {
  return int_accum_inv(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT64(1));
}

/* Take an int8 out of a moving sum or avg. */
Datum int8_avg_accum_inv(PG_FUNCTION_ARGS) {
  return int_accum_inv(fcinfo, PG_ARGISNULL(1) ? 0 : PG_GETARG_INT64(1));
}

/* Combine two states, for a partial aggregate. */
static Datum agg_combine(FunctionCallInfo fcinfo) {
  dec_agg_t *state1, *state2;
  MemoryContext agg_context, old;

  if (!AggCheckCallContext(fcinfo, &agg_context))
    elog(ERROR, "aggregate function called in non-aggregate context");
  state1 = PG_ARGISNULL(0) ? NULL : (dec_agg_t *)PG_GETARG_POINTER(0);
  state2 = PG_ARGISNULL(1) ? NULL : (dec_agg_t *)PG_GETARG_POINTER(1);
  if (state2 == NULL)
    PG_RETURN_POINTER(state1);
  if (state1 == NULL) {
    old = MemoryContextSwitchTo(agg_context);
    state1 = (dec_agg_t *)palloc(sizeof(dec_agg_t));
    *state1 = *state2;
    state1->agg_context = agg_context;
    MemoryContextSwitchTo(old);
    PG_RETURN_POINTER(state1);
  }
  state1->N += state2->N;
  state1->sumX = wide_add(state1->sumX, state2->sumX, false);
  if (state1->calc_sumX2)
    state1->sumX2 = wide_add(state1->sumX2, state2->sumX2, false);
  if (state2->max_scale > state1->max_scale) {
    state1->max_scale = state2->max_scale;
    state1->max_scale_count = state2->max_scale_count;
  } else if (state2->max_scale == state1->max_scale)
    state1->max_scale_count += state2->max_scale_count;
  PG_RETURN_POINTER(state1);
}

/* Combine numeric variance states. */
Datum numeric_combine(PG_FUNCTION_ARGS) { return agg_combine(fcinfo); }

/* Combine numeric sum and avg states. */
Datum numeric_avg_combine(PG_FUNCTION_ARGS) { return agg_combine(fcinfo); }

/* Combine integer variance states. */
Datum numeric_poly_combine(PG_FUNCTION_ARGS) { return agg_combine(fcinfo); }

/* Combine int8 sum and avg states. */
Datum int8_avg_combine(PG_FUNCTION_ARGS) { return agg_combine(fcinfo); }

/* Write w to buf. */
static void send_wide(StringInfo buf, const wide_t &w) {
  pq_sendbyte(buf, w.neg ? 1 : 0);
  pq_sendint32(buf, w.scale);
  for (int i = 0; i < MAG_LIMBS; i++)
    pq_sendint64(buf, (int64)w.mag.limb[i]);
}

/* Read a wide value from buf. */
static wide_t recv_wide(StringInfo buf) {
  wide_t w;

  w.neg = pq_getmsgbyte(buf) != 0;
  w.scale = pq_getmsgint(buf, 4);
  for (int i = 0; i < MAG_LIMBS; i++)
    w.mag.limb[i] = (uint64)pq_getmsgint64(buf);
  return w;
}

/* A state as bytes, for a partial aggregate's worker. */
static Datum agg_serialize(FunctionCallInfo fcinfo) {
  dec_agg_t *state;
  StringInfoData buf;

  if (!AggCheckCallContext(fcinfo, NULL))
    elog(ERROR, "aggregate function called in non-aggregate context");
  state = (dec_agg_t *)PG_GETARG_POINTER(0);
  pq_begintypsend(&buf);
  pq_sendbyte(&buf, state->calc_sumX2 ? 1 : 0);
  pq_sendint64(&buf, state->N);
  pq_sendint32(&buf, state->max_scale);
  pq_sendint64(&buf, state->max_scale_count);
  send_wide(&buf, state->sumX);
  send_wide(&buf, state->sumX2);
  PG_RETURN_BYTEA_P(pq_endtypsend(&buf));
}

/* A state from its bytes. */
static Datum agg_deserialize(FunctionCallInfo fcinfo) {
  bytea *sstate;
  StringInfoData buf;
  dec_agg_t *state;

  if (!AggCheckCallContext(fcinfo, NULL))
    elog(ERROR, "aggregate function called in non-aggregate context");
  sstate = PG_GETARG_BYTEA_PP(0);
  initReadOnlyStringInfo(&buf, VARDATA_ANY(sstate), VARSIZE_ANY_EXHDR(sstate));
  state = (dec_agg_t *)palloc0(sizeof(dec_agg_t));
  state->calc_sumX2 = pq_getmsgbyte(&buf) != 0;
  state->N = pq_getmsgint64(&buf);
  state->max_scale = pq_getmsgint(&buf, 4);
  state->max_scale_count = pq_getmsgint64(&buf);
  state->sumX = recv_wide(&buf);
  state->sumX2 = recv_wide(&buf);
  pq_getmsgend(&buf);
  PG_RETURN_POINTER(state);
}

/* Serialize a numeric variance state. */
Datum numeric_serialize(PG_FUNCTION_ARGS) { return agg_serialize(fcinfo); }

/* Deserialize a numeric variance state. */
Datum numeric_deserialize(PG_FUNCTION_ARGS) { return agg_deserialize(fcinfo); }

/* Serialize a numeric sum and avg state. */
Datum numeric_avg_serialize(PG_FUNCTION_ARGS) { return agg_serialize(fcinfo); }

/* Deserialize a numeric sum and avg state. */
Datum numeric_avg_deserialize(PG_FUNCTION_ARGS) {
  return agg_deserialize(fcinfo);
}

/* Serialize an integer variance state. */
Datum numeric_poly_serialize(PG_FUNCTION_ARGS) { return agg_serialize(fcinfo); }

/* Deserialize an integer variance state. */
Datum numeric_poly_deserialize(PG_FUNCTION_ARGS) {
  return agg_deserialize(fcinfo);
}

/* Serialize an int8 sum and avg state. */
Datum int8_avg_serialize(PG_FUNCTION_ARGS) { return agg_serialize(fcinfo); }

/* Deserialize an int8 sum and avg state. */
Datum int8_avg_deserialize(PG_FUNCTION_ARGS) { return agg_deserialize(fcinfo); }

/* The state of a final function, argument 0, or NULL. */
static dec_agg_t *final_state(FunctionCallInfo fcinfo) {
  return PG_ARGISNULL(0) ? NULL : (dec_agg_t *)PG_GETARG_POINTER(0);
}

/* sum: the sum, or null of no values; past 36 digits is an error. */
static Datum agg_sum(FunctionCallInfo fcinfo) {
  dec_agg_t *state = final_state(fcinfo);

  if (state == NULL || state->N == 0)
    PG_RETURN_NULL();
  PG_RETURN_NUMERIC(dec_from_wide(state->sumX));
}

/* avg: the sum over the count, at division's scale, or null. */
static Datum agg_avg(FunctionCallInfo fcinfo) {
  dec_agg_t *state = final_state(fcinfo);
  wide_t n;

  if (state == NULL || state->N == 0)
    PG_RETURN_NULL();
  n = wide_of_int(state->N);
  PG_RETURN_NUMERIC(dec_from_wide(
      wide_div(state->sumX, n, select_div_scale(state->sumX, n), false)));
}

/*
 * The variance, or with !variance the standard deviation, of the sample if
 * sample, else of the population, as PostgreSQL computes it exactly:
 * (N * sumX2 - sumX^2) / (N * (N - 1)), or over N * N.  Null with too few
 * values.
 */
static Datum agg_stddev(FunctionCallInfo fcinfo, bool variance, bool sample) {
  dec_agg_t *state = final_state(fcinfo);
  wide_t n, num, den, var;
  int rscale;

  if (state == NULL || state->N == 0 || (sample && state->N <= 1))
    PG_RETURN_NULL();
  n = wide_of_int(state->N);
  /* N * sumX2 - sumX * sumX, both at twice the sum's scale */
  num =
      wide_add(wide_scale_up(wide_mul(n, state->sumX2), 2 * state->sumX.scale),
               wide_mul(state->sumX, state->sumX), true);
  if (wide_is_zero(num) || num.neg)
    PG_RETURN_NUMERIC(dec_make(0, 0));
  den = wide_mul(n, wide_of_int(sample ? state->N - 1 : state->N));
  rscale = select_div_scale(num, den);
  var = wide_div(num, den, rscale, false);
  if (!variance) {
    /* The root of the rounded variance, rounded at the same scale. */
    int e = 2 * (rscale + 1) - var.scale;
    wide_t r;

    r.mag = mag_isqrt(e >= 0 ? mag_mul(var.mag, mag_pow10(e))
                             : mag_divmod(var.mag, mag_pow10(-e), NULL));
    r.neg = false;
    r.scale = rscale + 1;
    var = wide_round(r, rscale, false);
  }
  PG_RETURN_NUMERIC(dec_from_wide(var));
}

/* avg(numeric). */
Datum numeric_avg(PG_FUNCTION_ARGS) { return agg_avg(fcinfo); }

/* sum(numeric). */
Datum numeric_sum(PG_FUNCTION_ARGS) { return agg_sum(fcinfo); }

/* var_samp of numeric or int8. */
Datum numeric_var_samp(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, true, true);
}

/* stddev_samp of numeric or int8. */
Datum numeric_stddev_samp(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, false, true);
}

/* var_pop of numeric or int8. */
Datum numeric_var_pop(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, true, false);
}

/* stddev_pop of numeric or int8. */
Datum numeric_stddev_pop(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, false, false);
}

/* sum(int8). */
Datum numeric_poly_sum(PG_FUNCTION_ARGS) { return agg_sum(fcinfo); }

/* avg(int8). */
Datum numeric_poly_avg(PG_FUNCTION_ARGS) { return agg_avg(fcinfo); }

/* var_samp of int2 or int4. */
Datum numeric_poly_var_samp(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, true, true);
}

/* stddev_samp of int2 or int4. */
Datum numeric_poly_stddev_samp(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, false, true);
}

/* var_pop of int2 or int4. */
Datum numeric_poly_var_pop(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, true, false);
}

/* stddev_pop of int2 or int4. */
Datum numeric_poly_stddev_pop(PG_FUNCTION_ARGS) {
  return agg_stddev(fcinfo, false, false);
}

/* ----------------------------------------------------------------------
 * Sums and averages of int2 and int4, in int8: PostgreSQL's own.
 * ----------------------------------------------------------------------
 */

/*
 * SUM transition functions for integer datatypes.
 *
 * To avoid overflow, we use accumulators wider than the input datatype.
 * A Numeric accumulator is needed for int8 input; for int4 and int2
 * inputs, we use int8 accumulators which should be sufficient for practical
 * purposes.  (The latter two therefore don't really belong in this file,
 * but we keep them here anyway.)
 *
 * Because SQL defines the SUM() of no values to be NULL, not zero,
 * the initial condition of the transition data value needs to be NULL. This
 * means we can't rely on ExecAgg to automatically insert the first non-null
 * data value into the transition data: it doesn't know how to do the type
 * conversion.  The upshot is that these routines have to be marked non-strict
 * and handle substitution of the first non-null input themselves.
 *
 * Note: these functions are used only in plain aggregation mode.
 * In moving-aggregate mode, we use intX_avg_accum and intX_avg_accum_inv.
 */

Datum int2_sum(PG_FUNCTION_ARGS) {
  int64 newval;

  if (PG_ARGISNULL(0)) {
    /* No non-null input seen so far... */
    if (PG_ARGISNULL(1))
      PG_RETURN_NULL(); /* still no non-null */
    /* This is the first non-null input. */
    newval = (int64)PG_GETARG_INT16(1);
    PG_RETURN_INT64(newval);
  }

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to avoid palloc overhead. If not, we need to return
   * the new value of the transition variable. (If int8 is pass-by-value,
   * then of course this is useless as well as incorrect, so just ifdef it
   * out.)
   */
#ifndef USE_FLOAT8_BYVAL /* controls int8 too */
  if (AggCheckCallContext(fcinfo, NULL)) {
    int64 *oldsum = (int64 *)PG_GETARG_POINTER(0);

    /* Leave the running sum unchanged in the new input is null */
    if (!PG_ARGISNULL(1))
      *oldsum = *oldsum + (int64)PG_GETARG_INT16(1);

    PG_RETURN_POINTER(oldsum);
  } else
#endif
  {
    int64 oldsum = PG_GETARG_INT64(0);

    /* Leave sum unchanged if new input is null. */
    if (PG_ARGISNULL(1))
      PG_RETURN_INT64(oldsum);

    /* OK to do the addition. */
    newval = oldsum + (int64)PG_GETARG_INT16(1);

    PG_RETURN_INT64(newval);
  }
}

/* sum(int4): an int8 sum, null of no values. */
Datum int4_sum(PG_FUNCTION_ARGS) {
  int64 newval;

  if (PG_ARGISNULL(0)) {
    /* No non-null input seen so far... */
    if (PG_ARGISNULL(1))
      PG_RETURN_NULL(); /* still no non-null */
    /* This is the first non-null input. */
    newval = (int64)PG_GETARG_INT32(1);
    PG_RETURN_INT64(newval);
  }

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to avoid palloc overhead. If not, we need to return
   * the new value of the transition variable. (If int8 is pass-by-value,
   * then of course this is useless as well as incorrect, so just ifdef it
   * out.)
   */
#ifndef USE_FLOAT8_BYVAL /* controls int8 too */
  if (AggCheckCallContext(fcinfo, NULL)) {
    int64 *oldsum = (int64 *)PG_GETARG_POINTER(0);

    /* Leave the running sum unchanged in the new input is null */
    if (!PG_ARGISNULL(1))
      *oldsum = *oldsum + (int64)PG_GETARG_INT32(1);

    PG_RETURN_POINTER(oldsum);
  } else
#endif
  {
    int64 oldsum = PG_GETARG_INT64(0);

    /* Leave sum unchanged if new input is null. */
    if (PG_ARGISNULL(1))
      PG_RETURN_INT64(oldsum);

    /* OK to do the addition. */
    newval = oldsum + (int64)PG_GETARG_INT32(1);

    PG_RETURN_INT64(newval);
  }
}

/*
 * Note: this function is obsolete, it's no longer used for SUM(int8).
 */
Datum int8_sum(PG_FUNCTION_ARGS) {
  Numeric oldsum;

  if (PG_ARGISNULL(0)) {
    /* No non-null input seen so far... */
    if (PG_ARGISNULL(1))
      PG_RETURN_NULL(); /* still no non-null */
    /* This is the first non-null input. */
    PG_RETURN_NUMERIC(int64_to_numeric(PG_GETARG_INT64(1)));
  }

  /*
   * Note that we cannot special-case the aggregate case here, as we do for
   * int2_sum and int4_sum: numeric is of variable size, so we cannot modify
   * our first parameter in-place.
   */

  oldsum = PG_GETARG_NUMERIC(0);

  /* Leave sum unchanged if new input is null. */
  if (PG_ARGISNULL(1))
    PG_RETURN_NUMERIC(oldsum);

  /* OK to do the addition. */
  PG_RETURN_DATUM(DirectFunctionCall2(
      numeric_add, NumericGetDatum(oldsum),
      NumericGetDatum(int64_to_numeric(PG_GETARG_INT64(1)))));
}

/*
 * Routines for avg(int2) and avg(int4).  The transition datatype
 * is a two-element int8 array, holding count and sum.
 *
 * These functions are also used for sum(int2) and sum(int4) when
 * operating in moving-aggregate mode, since for correct inverse transitions
 * we need to count the inputs.
 */

typedef struct Int8TransTypeData {
  int64 count;
  int64 sum;
} Int8TransTypeData;

/* Accumulate an int2 into an avg state: an int8 array of count and sum. */
Datum int2_avg_accum(PG_FUNCTION_ARGS) {
  ArrayType *transarray;
  int16 newval = PG_GETARG_INT16(1);
  Int8TransTypeData *transdata;

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to reduce palloc overhead. Otherwise we need to make
   * a copy of it before scribbling on it.
   */
  if (AggCheckCallContext(fcinfo, NULL))
    transarray = PG_GETARG_ARRAYTYPE_P(0);
  else
    transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);
  transdata->count++;
  transdata->sum += newval;

  PG_RETURN_ARRAYTYPE_P(transarray);
}

/* Accumulate an int4 into an avg state: an int8 array of count and sum. */
Datum int4_avg_accum(PG_FUNCTION_ARGS) {
  ArrayType *transarray;
  int32 newval = PG_GETARG_INT32(1);
  Int8TransTypeData *transdata;

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to reduce palloc overhead. Otherwise we need to make
   * a copy of it before scribbling on it.
   */
  if (AggCheckCallContext(fcinfo, NULL))
    transarray = PG_GETARG_ARRAYTYPE_P(0);
  else
    transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);
  transdata->count++;
  transdata->sum += newval;

  PG_RETURN_ARRAYTYPE_P(transarray);
}

/* Combine two int2 or int4 avg states. */
Datum int4_avg_combine(PG_FUNCTION_ARGS) {
  ArrayType *transarray1;
  ArrayType *transarray2;
  Int8TransTypeData *state1;
  Int8TransTypeData *state2;

  if (!AggCheckCallContext(fcinfo, NULL))
    elog(ERROR, "aggregate function called in non-aggregate context");

  transarray1 = PG_GETARG_ARRAYTYPE_P(0);
  transarray2 = PG_GETARG_ARRAYTYPE_P(1);

  if (ARR_HASNULL(transarray1) ||
      ARR_SIZE(transarray1) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  if (ARR_HASNULL(transarray2) ||
      ARR_SIZE(transarray2) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  state1 = (Int8TransTypeData *)ARR_DATA_PTR(transarray1);
  state2 = (Int8TransTypeData *)ARR_DATA_PTR(transarray2);

  state1->count += state2->count;
  state1->sum += state2->sum;

  PG_RETURN_ARRAYTYPE_P(transarray1);
}

/* Take an int2 out of a moving avg state. */
Datum int2_avg_accum_inv(PG_FUNCTION_ARGS) {
  ArrayType *transarray;
  int16 newval = PG_GETARG_INT16(1);
  Int8TransTypeData *transdata;

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to reduce palloc overhead. Otherwise we need to make
   * a copy of it before scribbling on it.
   */
  if (AggCheckCallContext(fcinfo, NULL))
    transarray = PG_GETARG_ARRAYTYPE_P(0);
  else
    transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);
  transdata->count--;
  transdata->sum -= newval;

  PG_RETURN_ARRAYTYPE_P(transarray);
}

/* Take an int4 out of a moving avg state. */
Datum int4_avg_accum_inv(PG_FUNCTION_ARGS) {
  ArrayType *transarray;
  int32 newval = PG_GETARG_INT32(1);
  Int8TransTypeData *transdata;

  /*
   * If we're invoked as an aggregate, we can cheat and modify our first
   * parameter in-place to reduce palloc overhead. Otherwise we need to make
   * a copy of it before scribbling on it.
   */
  if (AggCheckCallContext(fcinfo, NULL))
    transarray = PG_GETARG_ARRAYTYPE_P(0);
  else
    transarray = PG_GETARG_ARRAYTYPE_P_COPY(0);

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");

  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);
  transdata->count--;
  transdata->sum -= newval;

  PG_RETURN_ARRAYTYPE_P(transarray);
}

/* avg of int2 or int4: the sum over the count, as numeric, or null. */
Datum int8_avg(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  Int8TransTypeData *transdata;
  Datum countd, sumd;

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");
  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);

  /* SQL defines AVG of no values to be NULL */
  if (transdata->count == 0)
    PG_RETURN_NULL();

  countd = NumericGetDatum(int64_to_numeric(transdata->count));
  sumd = NumericGetDatum(int64_to_numeric(transdata->sum));

  PG_RETURN_DATUM(DirectFunctionCall2(numeric_div, sumd, countd));
}

/*
 * SUM(int2) and SUM(int4) both return int8, so we can use this
 * final function for both.
 */
Datum int2int4_sum(PG_FUNCTION_ARGS) {
  ArrayType *transarray = PG_GETARG_ARRAYTYPE_P(0);
  Int8TransTypeData *transdata;

  if (ARR_HASNULL(transarray) ||
      ARR_SIZE(transarray) !=
          ARR_OVERHEAD_NONULLS(1) + sizeof(Int8TransTypeData))
    elog(ERROR, "expected 2-element int8 array");
  transdata = (Int8TransTypeData *)ARR_DATA_PTR(transarray);

  /* SQL defines SUM of no values to be NULL */
  if (transdata->count == 0)
    PG_RETURN_NULL();

  PG_RETURN_DATUM(Int64GetDatumFast(transdata->sum));
}

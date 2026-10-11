/*
 * decimal.cc -- decimal64 and decimal128 (TYPE.md §3.4,
 * SM2_DESIGN.md §2.2): after DuckDB's decimal, a two's complement
 * integer scaled by 10^s, little-endian, 8 bytes for decimal64 and
 * 16 for decimal128, with (precision, scale) in the typmod rather
 * than the datum. sm3 maps SQL's decimal(p, s) and numeric(p, s)
 * to one of the two.
 *
 * The typmod is PostgreSQL's numeric typmod, ((p << 16) | s) + 4,
 * with 0 <= s <= p and 1 <= p <= 18 for decimal64, 38 for
 * decimal128; there is no bare form, so -1 is refused. The text is
 * what PostgreSQL prints for numeric(p, s): exactly s fraction
 * digits, a minus sign only on a value below zero. As for the
 * other types, in accepts only text that prints back unchanged,
 * and a value needing more than p digits is refused.
 *
 * Both types share one implementation over a 128-bit integer,
 * parameterized by the datum's width W; an 8-byte datum is
 * sign-extended when read.
 */

#include "native.h"
#include "sm2.h"

#include <cstdint>
#include <string>

namespace {

/* A decimal typmod's precision and scale. */
struct numtype_t {
  int precision;
  int scale;
};

/* The name of the decimal type whose datum is W bytes. */
template <size_t W> constexpr const char *decimal_name() {
  return W == 8 ? "decimal64" : "decimal128";
}

/* The most digits a W-byte decimal holds. */
template <size_t W> constexpr int max_precision() { return W == 8 ? 18 : 38; }

/* Decode typmod; false if it is not a (p, s) a W-byte decimal holds. */
template <size_t W> bool decode_typmod(int32_t typmod, numtype_t &t) {
  if (typmod < 4) {
    return false;
  }
  int p = (typmod - 4) >> 16;
  int s = (typmod - 4) & 0xFFFF;
  if (p < 1 || p > max_precision<W>() || s > p) {
    return false;
  }
  t = {p, s};
  return true;
}

/* True if typmod is a (p, s) a W-byte decimal holds. */
template <size_t W> bool decimal_typmod_valid(int32_t typmod) {
  numtype_t t;
  return decode_typmod<W>(typmod, t);
}

/* Fail with SM_ERR_INVALID: typmod is not valid for a W-byte decimal. */
template <size_t W> int bad_typmod(int32_t typmod) {
  return sm1::seterr(sm1::SM_ERR_INVALID,
                     std::string("invalid typmod for type ") +
                         decimal_name<W>() + ": " + std::to_string(typmod));
}

/* 10^n, for n <= 38. */
unsigned __int128 pow10(int n) {
  unsigned __int128 r = 1;
  while (n-- > 0) {
    r *= 10;
  }
  return r;
}

/* The magnitude of v, which may be the most negative value. */
unsigned __int128 magnitude(__int128 v) {
  return v < 0 ? -static_cast<unsigned __int128>(v)
               : static_cast<unsigned __int128>(v);
}

/* A datum's value: 8 or 16 bytes, little-endian, sign-extended. */
__int128 load(std::string_view datum) {
  if (datum.size() == 8) {
    return sm2::load_le<int64_t>(datum);
  }
  uint64_t lo = sm2::load_le<uint64_t>(datum.substr(0, 8));
  uint64_t hi = sm2::load_le<uint64_t>(datum.substr(8, 8));
  return static_cast<__int128>(static_cast<unsigned __int128>(hi) << 64 | lo);
}

/* Replace datum with v's low width bytes, little-endian. */
void store(__int128 v, size_t width, std::string &datum) {
  sm2::store_le(static_cast<uint64_t>(v), datum);
  if (width == 16) {
    std::string hi;
    sm2::store_le(
        static_cast<uint64_t>(static_cast<unsigned __int128>(v) >> 64), hi);
    datum += hi;
  }
}

/* Print v at scale s, as PostgreSQL prints numeric(p, s). */
std::string decimal_text(__int128 v, int scale) {
  unsigned __int128 mag = magnitude(v);
  std::string digits;
  do {
    digits.insert(digits.begin(), static_cast<char>('0' + mag % 10));
    mag /= 10;
  } while (mag != 0);
  // At least one digit before the point.
  if (digits.size() < static_cast<size_t>(scale) + 1) {
    digits.insert(0, scale + 1 - digits.size(), '0');
  }
  if (scale > 0) {
    digits.insert(digits.size() - scale, 1, '.');
  }
  return v < 0 ? "-" + digits : digits;
}

/* Parse a decimal's canonical text under typmod. */
template <size_t W>
int decimal_in(std::string_view text, int32_t typmod, std::string &datum) {
  numtype_t t;
  if (!decode_typmod<W>(typmod, t)) {
    return bad_typmod<W>(typmod);
  }
  // Read [-]digits[.digits] as an integer at the text's scale,
  // stopping at 39 digits, past any precision.
  size_t i = 0;
  bool neg = i < text.size() && text[i] == '-';
  i += neg;
  unsigned __int128 mag = 0;
  int int_digits = 0, frac_digits = -1;
  for (; i < text.size(); i++) {
    char c = text[i];
    if (c == '.' && frac_digits < 0 && int_digits > 0) {
      frac_digits = 0;
    } else if (c >= '0' && c <= '9' && int_digits + frac_digits < 39) {
      (frac_digits < 0 ? int_digits : frac_digits)++;
      mag = mag * 10 + (c - '0');
    } else {
      return sm2::bad_input(decimal_name<W>(), text);
    }
  }
  // Canonical text carries exactly the column's scale, and the
  // value must fit its precision.
  if (int_digits == 0 || frac_digits == 0 ||
      (frac_digits < 0 ? 0 : frac_digits) != t.scale ||
      mag >= pow10(t.precision)) {
    return sm2::bad_input(decimal_name<W>(), text);
  }
  __int128 v = neg ? -static_cast<__int128>(mag) : static_cast<__int128>(mag);
  // Canonical only if it prints back unchanged: no leading zero,
  // no "-0".
  if (decimal_text(v, t.scale) != text) {
    return sm2::bad_input(decimal_name<W>(), text);
  }
  store(v, W, datum);
  return 0;
}

/* Print a decimal datum under typmod. */
template <size_t W>
int decimal_out(std::string_view datum, int32_t typmod, std::string &text) {
  numtype_t t;
  if (!decode_typmod<W>(typmod, t)) {
    return bad_typmod<W>(typmod);
  }
  if (datum.size() != W) {
    return sm2::bad_datum(decimal_name<W>(), datum, W);
  }
  __int128 v = load(datum);
  if (magnitude(v) >= pow10(t.precision)) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       std::string("a ") + decimal_name<W>() +
                           " datum exceeds its precision " +
                           std::to_string(t.precision));
  }
  text = decimal_text(v, t.scale);
  return 0;
}

/* Order two decimal datums of one column: integer order. */
int decimal_cmp(std::string_view a, std::string_view b, int32_t) {
  __int128 x = load(a), y = load(b);
  return (x > y) - (x < y);
}

/*
 * A decimal's key (TYPE.md §3.4): the integer at the datum's
 * width, sign bit flipped, big-endian, which orders a column's
 * values, since the typmod fixes their scale.
 */
template <size_t W>
int decimal_key(std::string_view datum, int32_t typmod,
                const sm2::collation_t *, std::string &key) {
  numtype_t t;
  if (!decode_typmod<W>(typmod, t)) {
    return bad_typmod<W>(typmod);
  }
  if (datum.size() != W) {
    return sm2::bad_datum(decimal_name<W>(), datum, W);
  }
  __int128 v = load(datum);
  if constexpr (W == 8) {
    sm2::put_key_int<uint64_t>(static_cast<int64_t>(v), key);
  } else {
    sm2::put_key_int<unsigned __int128>(v, key);
  }
  return 0;
}

/* Read a decimal's key back into its datum. */
template <size_t W>
int decimal_unkey(std::string_view &key, int32_t typmod,
                  const sm2::collation_t *, std::string &datum) {
  numtype_t t;
  if (!decode_typmod<W>(typmod, t)) {
    return bad_typmod<W>(typmod);
  }
  __int128 v;
  bool ok;
  if constexpr (W == 8) {
    int64_t n;
    ok = sm2::get_key_int<uint64_t>(key, n);
    v = n;
  } else {
    ok = sm2::get_key_int<unsigned __int128>(key, v);
  }
  if (!ok) {
    return sm2::bad_key(decimal_name<W>());
  }
  store(v, W, datum);
  return 0;
}

} // namespace

namespace sm2 {

const type_t type_decimal64 = {.code = 17,
                               .name = "decimal64",
                               .len = 8,
                               .byval = true,
                               .align = 8,
                               .in = decimal_in<8>,
                               .out = decimal_out<8>,
                               .cmp = decimal_cmp,
                               .key_encode = decimal_key<8>,
                               .key_decode = decimal_unkey<8>,
                               .typmod_valid = decimal_typmod_valid<8>};

const type_t type_decimal128 = {.code = 18,
                                .name = "decimal128",
                                .len = 16,
                                .byval = false,
                                .align = 8,
                                .in = decimal_in<16>,
                                .out = decimal_out<16>,
                                .cmp = decimal_cmp,
                                .key_encode = decimal_key<16>,
                                .key_decode = decimal_unkey<16>,
                                .typmod_valid = decimal_typmod_valid<16>};

} // namespace sm2

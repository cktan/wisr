/*
 * datums.h -- datums of every type, generated for property tests
 * (gen.h's seed), shared by the key and tuple tests.
 */
#ifndef SM2_TEST_DATUMS_H
#define SM2_TEST_DATUMS_H

#include "gen.h"

#include "sm2.h"

#include <cstring>
#include <random>
#include <string>
#include <vector>

/* v's little-endian bytes. */
template <typename T> std::string le(T v) {
  std::string out(sizeof(T), '\0');
  memcpy(out.data(), &v, sizeof(T));
  return out;
}

/* The sign of an int. */
inline int sign(int v) { return (v > 0) - (v < 0); }

/* A column's type and typmod, for the generators below. */
struct typed_t {
  const sm2::type_t *type;
  int32_t typmod;
};

/* decimal's typmod for (p, s). */
inline int32_t decimal_typmod(int p, int s) { return ((p << 16) | s) + 4; }

/* Every type, decimal64 and decimal128 among them. */
inline const std::vector<typed_t> &all_types() {
  static const std::vector<typed_t> types = {
      {&sm2::type_bool, -1},
      {&sm2::type_int2, -1},
      {&sm2::type_int4, -1},
      {&sm2::type_int8, -1},
      {&sm2::type_float4, -1},
      {&sm2::type_float8, -1},
      {&sm2::type_decimal64, decimal_typmod(10, 2)},
      {&sm2::type_decimal128, decimal_typmod(30, 5)},
      {&sm2::type_text, -1},
      {&sm2::type_varchar, -1},
      {&sm2::type_bytea, -1},
      {&sm2::type_date, -1},
      {&sm2::type_time, -1},
      {&sm2::type_timestamp, -1},
      {&sm2::type_timestamptz, -1},
      {&sm2::type_interval, -1},
      {&sm2::type_uuid, -1},
  };
  return types;
}

/* A random value of 0 .. n-1, n > 0, in 128 bits. */
inline unsigned __int128 below(std::mt19937_64 &rng, unsigned __int128 n) {
  unsigned __int128 r = static_cast<unsigned __int128>(rng()) << 64 | rng();
  return r % n;
}

/*
 * A random datum of t, drawn to meet the boundaries and the ties
 * the type has: the integers' ends, the floats' zeros and NaNs,
 * the datetimes' infinities, intervals that PostgreSQL calls
 * equal, byte strings that are prefixes of one another.
 */
inline std::string gen_datum(const typed_t &t, std::mt19937_64 &rng) {
  const sm2::type_t *type = t.type;
  if (type == &sm2::type_bool) {
    return std::string(1, static_cast<char>(rng() % 2));
  }
  if (type == &sm2::type_int2) {
    return le(gen_int<int16_t>(rng));
  }
  if (type == &sm2::type_int4 || type == &sm2::type_date) {
    return le(gen_int<int32_t>(rng));
  }
  if (type == &sm2::type_int8 || type == &sm2::type_timestamp ||
      type == &sm2::type_timestamptz) {
    return le(gen_int<int64_t>(rng));
  }
  if (type == &sm2::type_time) {
    return le(static_cast<int64_t>(rng() % 86400000001));
  }
  if (type == &sm2::type_float4) {
    return le(gen_float<float>(rng));
  }
  if (type == &sm2::type_float8) {
    return le(gen_float<double>(rng));
  }
  if (type == &sm2::type_decimal64 || type == &sm2::type_decimal128) {
    int p = (t.typmod - 4) >> 16;
    unsigned __int128 limit = 1;
    for (int i = 0; i < p; i++) {
      limit *= 10;
    }
    unsigned __int128 mag = rng() % 3 == 0 ? rng() % 3 : below(rng, limit);
    __int128 v =
        rng() % 2 ? -static_cast<__int128>(mag) : static_cast<__int128>(mag);
    return p <= 18 ? le(static_cast<int64_t>(v)) : le(v);
  }
  if (type == &sm2::type_text || type == &sm2::type_varchar) {
    return gen_utf8(rng);
  }
  if (type == &sm2::type_bytea) {
    std::string s;
    for (size_t n = rng() % 6; n > 0; n--) {
      s += static_cast<char>(rng() % 3 == 0   ? 0
                             : rng() % 3 == 0 ? 0xff
                                              : rng());
    }
    return s;
  }
  if (type == &sm2::type_interval) {
    // Small components collide under PostgreSQL's flattening.
    if (rng() % 2) {
      int32_t month = static_cast<int32_t>(rng() % 5) - 2;
      int32_t day = static_cast<int32_t>(rng() % 121) - 60;
      int64_t time = (static_cast<int64_t>(rng() % 5) - 2) * 86400000000;
      return le(time) + le(day) + le(month);
    }
    return le(gen_int<int64_t>(rng)) + le(gen_int<int32_t>(rng)) +
           le(gen_int<int32_t>(rng));
  }
  // uuid: random bytes, with shared prefixes now and then.
  std::string s = le(rng()) + le(rng());
  if (rng() % 3 == 0) {
    s.replace(0, 8, 8, '\0');
  }
  return s;
}

#endif /* SM2_TEST_DATUMS_H */

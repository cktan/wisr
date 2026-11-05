/*
 * gen.h -- generators for sm2's property tests (TYPE.md §9,
 * "Testing"). Every property runs from one seed: a fixed default
 * that the environment variable WISR_SEED overrides, which a
 * property prints when it fails, so any failure replays exactly.
 */
#ifndef SM2_TEST_GEN_H
#define SM2_TEST_GEN_H

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iterator>
#include <limits>
#include <random>
#include <string>
#include <vector>

/* The seed every property runs from: WISR_SEED, or a fixed default. */
inline uint64_t test_seed() {
  static const uint64_t seed = [] {
    const char *s = getenv("WISR_SEED");
    return s ? strtoull(s, nullptr, 0) : 20260924ULL;
  }();
  return seed;
}

/* How many random cases a property runs, after its boundaries. */
inline constexpr int property_runs = 10000;

/* T's boundary values, in ascending order. */
template <typename T> std::vector<T> int_boundaries() {
  using lim = std::numeric_limits<T>;
  return {lim::min(), static_cast<T>(lim::min() + 1), -1,        0,
          1,          static_cast<T>(lim::max() - 1), lim::max()};
}

/*
 * A random T: a boundary a quarter of the time, a small value a
 * quarter of the time, and anything in T's range otherwise.
 */
template <typename T> T gen_int(std::mt19937_64 &rng) {
  switch (rng() % 4) {
  case 0: {
    std::vector<T> b = int_boundaries<T>();
    return b[rng() % b.size()];
  }
  case 1:
    return static_cast<T>(static_cast<int64_t>(rng() % 201) - 100);
  default:
    return static_cast<T>(rng());
  }
}

/* F's boundary values: zeros, infinities, NaN and the range's ends. */
template <typename F> std::vector<F> float_boundaries() {
  using lim = std::numeric_limits<F>;
  return {lim::quiet_NaN(),
          -lim::infinity(),
          lim::lowest(),
          -1,
          -lim::denorm_min(),
          static_cast<F>(-0.0),
          0,
          lim::denorm_min(),
          lim::min(),
          lim::epsilon(),
          1,
          lim::max(),
          lim::infinity()};
}

/*
 * A random F: a boundary a quarter of the time, a small integer
 * or a short fraction a quarter of the time, and any bit pattern
 * otherwise -- NaNs with payloads and subnormals included.
 */
template <typename F> F gen_float(std::mt19937_64 &rng) {
  using bits_t = std::conditional_t<sizeof(F) == 4, uint32_t, uint64_t>;
  switch (rng() % 4) {
  case 0: {
    std::vector<F> b = float_boundaries<F>();
    return b[rng() % b.size()];
  }
  case 1:
    return static_cast<F>(static_cast<int64_t>(rng() % 2001) - 1000) /
           static_cast<F>(rng() % 2 ? 1 : 8);
  default:
    return std::bit_cast<F>(static_cast<bits_t>(rng()));
  }
}

/* A code point's UTF-8 bytes. */
inline std::string utf8(uint32_t cp) {
  std::string s;
  if (cp < 0x80) {
    s += static_cast<char>(cp);
  } else if (cp < 0x800) {
    s += static_cast<char>(0xC0 | cp >> 6);
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    s += static_cast<char>(0xE0 | cp >> 12);
    s += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    s += static_cast<char>(0xF0 | cp >> 18);
    s += static_cast<char>(0x80 | (cp >> 12 & 0x3F));
    s += static_cast<char>(0x80 | (cp >> 6 & 0x3F));
    s += static_cast<char>(0x80 | (cp & 0x3F));
  }
  return s;
}

/*
 * A random valid UTF-8 string of up to 20 code points: mostly
 * ASCII, with every length of sequence, the edges of each range
 * included.
 */
inline std::string gen_utf8(std::mt19937_64 &rng) {
  static const uint32_t edges[] = {0x01,   0x7F,   0x80,   0x7FF,   0x800,
                                   0xD7FF, 0xE000, 0xFFFF, 0x10000, 0x10FFFF};
  std::string s;
  for (size_t n = rng() % 21; n > 0; n--) {
    uint32_t cp;
    switch (rng() % 4) {
    case 0:
      cp = edges[rng() % std::size(edges)];
      break;
    case 1:
      do {
        cp = 1 + rng() % 0x10FFFF;
      } while (cp >= 0xD800 && cp <= 0xDFFF);
      break;
    default:
      cp = 0x20 + rng() % 0x5F;
    }
    s += utf8(cp);
  }
  return s;
}

#endif /* SM2_TEST_GEN_H */

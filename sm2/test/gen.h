/*
 * gen.h -- generators for sm2's property tests (PLAN.md,
 * "Testing"). Every property runs from one seed: a fixed default
 * that the environment variable WISR_SEED overrides, which a
 * property prints when it fails, so any failure replays exactly.
 */
#ifndef SM2_TEST_GEN_H
#define SM2_TEST_GEN_H

#include <bit>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <random>
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

#endif /* SM2_TEST_GEN_H */

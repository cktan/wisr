/*
 * test_sort.cc -- the sort of records by key (include/sm0_sort.h).
 */

#include "doctest.h"

#include "sm0.h"
#include "sm0_sort.h"
#include "sm_sys.h"

#include <algorithm>
#include <random>
#include <string>
#include <utility>
#include <vector>

namespace {

using record_t = std::pair<std::string, std::string>;

/* Sort records, and give back what the sort gives, in order. */
std::vector<record_t> sorted(const std::vector<record_t> &in) {
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open("unused", 1 << 20, false, s) == 0);
  for (const record_t &r : in) {
    REQUIRE(sm0::sort_put(s, r.first, r.second) == 0);
  }
  REQUIRE(sm0::sort_done(s) == 0);
  std::vector<record_t> out;
  std::string_view k, v;
  while (0 == sm0::sort_next(s, k, v)) {
    out.emplace_back(std::string(k), std::string(v));
  }
  sm0::sort_free(s);
  return out;
}

/* Check the sort gives in's keys as std::sort orders them. */
void check_sort(std::vector<record_t> in) {
  std::vector<record_t> got = sorted(in);
  std::stable_sort(
      in.begin(), in.end(),
      [](const record_t &a, const record_t &b) { return a.first < b.first; });
  REQUIRE(got.size() == in.size());
  for (size_t i = 0; i < in.size(); i++) {
    REQUIRE(got[i].first == in[i].first);
  }
}

/* Records whose value is their key, so a test can check they match. */
std::vector<record_t> records(const std::vector<std::string> &keys) {
  std::vector<record_t> out;
  for (const std::string &k : keys) {
    out.emplace_back(k, "v" + k);
  }
  return out;
}

/* n random keys of length up to len, from an alphabet of few bytes. */
std::vector<std::string> random_keys(size_t n, size_t len, unsigned seed) {
  std::mt19937 rng(seed);
  std::vector<std::string> out(n);
  for (std::string &k : out) {
    k.resize(rng() % (len + 1));
    for (char &c : k) {
      c = static_cast<char>(rng() % 4);
    }
  }
  return out;
}

} // namespace

TEST_CASE("a sort orders keys as std::sort does") {
  // Verify the order on the shapes of input that take different paths.
  // 1. Random keys of many lengths, zero bytes among them.
  // 2. Keys already in order, which skip the sort.
  // 3. Keys in reverse order.
  // 4. Every key equal.
  std::vector<std::string> keys = random_keys(20000, 20, 1);
  check_sort(records(keys));

  std::sort(keys.begin(), keys.end());
  check_sort(records(keys));

  std::reverse(keys.begin(), keys.end());
  check_sort(records(keys));

  check_sort(records(std::vector<std::string>(5000, "same")));
}

TEST_CASE("the common prefix is skipped, however long") {
  // Verify the abbreviation's offset leaves the order right.
  // 1. Keys sharing 40 bytes differ past them.
  // 2. Keys sharing exactly 8 bytes, then differing.
  // 3. One key that is the prefix itself, so the offset is its end.
  std::string long_prefix(40, 'p');
  std::vector<std::string> keys;
  for (std::string &k : random_keys(5000, 20, 2)) {
    keys.push_back(long_prefix + k);
  }
  check_sort(records(keys));

  keys.clear();
  for (std::string &k : random_keys(5000, 12, 3)) {
    keys.push_back("12345678" + k);
  }
  check_sort(records(keys));

  keys.push_back("12345678");
  check_sort(records(keys));
}

TEST_CASE("a short key's padding does not make it equal a longer key") {
  // Verify a key and that key plus zero bytes stay distinct and ordered.
  // 1. Keys that abbreviate alike, as the padding is zeros.
  // 2. A key past the abbreviation's 8 bytes differs only in length.
  // 3. An empty key sorts first.
  using namespace std::string_literals;
  check_sort(records({"ab\0\0"s, "ab"s, "ab\0"s, "ab\0\0\0"s, "ac"s}));
  check_sort(records({"0123456789"s, "0123456789\0"s, "012345678"s}));
  check_sort(records({"b"s, ""s, "a"s}));
}

TEST_CASE("a record's value comes back with its key") {
  // Verify the record is whole after the sort moved its item.
  // 1. Sort records whose value names their key, some values empty.
  // 2. A value and a key of over 127 bytes take a longer length.
  std::vector<record_t> in = {
      {"b", "vb"}, {"a", ""}, {std::string(200, 'c'), std::string(300, 'x')}};
  std::vector<record_t> got = sorted(in);
  REQUIRE(got.size() == 3);
  CHECK(got[0] == record_t("a", ""));
  CHECK(got[1] == record_t("b", "vb"));
  CHECK(got[2] == record_t(std::string(200, 'c'), std::string(300, 'x')));
}

TEST_CASE("a unique sort fails on two equal keys, and only then") {
  // Verify SM_ERR_DUPKEY at the end of the input.
  // 1. Two equal keys among others fail the sort.
  // 2. Keys that differ only past the abbreviation pass.
  // 3. A key and that key plus a zero byte pass.
  auto run = [](const std::vector<std::string> &keys) {
    sm0::sort_t *s = nullptr;
    REQUIRE(sm0::sort_open("unused", 1 << 20, true, s) == 0);
    for (const std::string &k : keys) {
      REQUIRE(sm0::sort_put(s, k, "") == 0);
    }
    int rc = sm0::sort_done(s);
    sm0::sort_free(s);
    return rc;
  };
  using namespace std::string_literals;
  CHECK(run({"b", "a", "c", "a"}) == -1);
  CHECK(sm0::errnum() == SM_ERR_DUPKEY);
  CHECK(run({"x0123456789a", "x0123456789b"}) == 0);
  CHECK(run({"ab"s, "ab\0"s}) == 0);
}

TEST_CASE("a sort is used in order") {
  // Verify the calls that come too soon or too late.
  // 1. An empty sort ends at once.
  // 2. sort_next before sort_done is refused.
  // 3. sort_put and sort_done after sort_done are refused.
  // 4. sort_open needs a directory and a budget.
  // 5. sort_free takes nullptr, and a sort not finished.
  sm0::sort_t *s = nullptr;
  std::string_view k, v;
  REQUIRE(sm0::sort_open("unused", 1, false, s) == 0);
  CHECK(sm0::sort_next(s, k, v) == -1);
  REQUIRE(sm0::sort_done(s) == 0);
  CHECK(sm0::sort_next(s, k, v) == 1);
  CHECK(sm0::sort_put(s, "k", "v") == -1);
  CHECK(sm0::sort_done(s) == -1);
  sm0::sort_free(s);

  sm0::sort_t *none = nullptr;
  CHECK(sm0::sort_open(nullptr, 1, false, none) == -1);
  CHECK(sm0::sort_open("unused", 0, false, none) == -1);
  CHECK(none == nullptr);
  sm0::sort_free(nullptr);

  REQUIRE(sm0::sort_open("unused", 1, false, s) == 0);
  REQUIRE(sm0::sort_put(s, "k", "v") == 0);
  sm0::sort_free(s);
}

TEST_CASE("an interrupt stops a sort") {
  // Verify a pending interrupt the thread takes fails the sort.
  // 1. Interrupt the thread, with the bit in its wait mask.
  // 2. sort_put's first look fails with SM_ERR_INTERRUPTED.
  // 3. With the bit taken, the sort goes on.
  // 4. A bit outside the wait mask stops nothing.
  constexpr uint32_t bit = 1u << 7;
  sm0::sort_t *s = nullptr;
  REQUIRE(sm0::sort_open("unused", 1, false, s) == 0);
  uint32_t mask = sm_wait_mask();
  sm_set_wait_mask(bit);
  sm_interrupt_self(bit);
  CHECK(sm0::sort_put(s, "k", "v") == -1);
  CHECK(sm0::errnum() == SM_ERR_INTERRUPTED);
  CHECK(sm_take(bit) == bit);
  CHECK(sm0::sort_put(s, "k", "v") == 0);
  sm0::sort_free(s);

  REQUIRE(sm0::sort_open("unused", 1, false, s) == 0);
  sm_set_wait_mask(0);
  sm_interrupt_self(bit);
  CHECK(sm0::sort_put(s, "k", "v") == 0);
  CHECK(sm_take(bit) == bit);
  sm_set_wait_mask(mask);
  sm0::sort_free(s);
}

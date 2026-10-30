/*
 * test_page.cc -- a B+tree page (../BTREE.md §3, §13 step 1): its
 * bytes laid out and read where they lie, an entry's insert, a
 * split's halves, and a page that is not one refused.
 */

#include "doctest.h"

#include "../src/tree/page.h"
#include "sm1.h"

#include <map>
#include <random>
#include <string>
#include <vector>

using namespace sm2::tree;

namespace {

/* A key of random bytes, 1 to max_len long. */
std::string random_key(std::mt19937_64 &rng, size_t max_len) {
  std::uniform_int_distribution<size_t> len(1, max_len);
  std::uniform_int_distribution<int> byte(0, 255);
  std::string key(len(rng), '\0');
  for (char &c : key) {
    c = static_cast<char>(byte(rng));
  }
  return key;
}

/* The page bytes laid out, opened as a view; REQUIREs it opens. */
page_view_t view_of(const std::string &bytes) {
  page_view_t view;
  REQUIRE(0 == view.open(bytes));
  return view;
}

/* Whether view holds exactly expected's entries, in key order. */
void check_entries(const page_view_t &view,
                   const std::map<std::string, std::string> &expected) {
  REQUIRE(expected.size() == view.count());
  size_t i = 0;
  for (const auto &[key, value] : expected) {
    CHECK(key == view.key(i));
    CHECK(value == view.value(i));
    i++;
  }
}

} // namespace

TEST_CASE("a page holds its entries in key order, read where they lie") {
  // Verify inserts at random against a sorted map, through the bytes.
  // 1. Insert random keys, with values, until the page has no room.
  // 2. A key inserted again is no change, and is reported so.
  // 3. The bytes read back give every entry in key order, and the
  //    search of the slots finds each key and the gaps between.
  // 4. Its header round-trips, and its size is page_size's.
  std::mt19937_64 rng(1);
  for (int round = 0; round < 50; round++) {
    page_t page;
    page.level = 0;
    page.version = 7 + round;
    page.right = 1000 + round;
    std::map<std::string, std::string> expected;
    for (;;) {
      std::string key = random_key(rng, 40);
      std::string value(round % 3, 'v');
      int rc = page_insert(page, key, value);
      REQUIRE(rc >= 0);
      if (2 == rc) {
        break;
      }
      if (1 == rc) {
        CHECK(expected.count(key) == 1);
        continue;
      }
      expected[key] = value;
      REQUIRE(1 == page_insert(page, key, "other"));
    }
    std::string bytes = page_bytes(page);
    CHECK(page_size(page) == bytes.size());
    CHECK(bytes.size() <= page_limit);
    page_view_t view = view_of(bytes);
    CHECK(0 == view.level());
    CHECK(7u + round == view.version());
    CHECK(1000u + round == view.right());
    CHECK_FALSE(view.has_high());
    check_entries(view, expected);
    // Each key is found where it is; the key just past it, its
    // bytes and a 0, is placed at the next entry.
    size_t i = 0;
    for (const auto &[key, value] : expected) {
      CHECK(i == view.lower_bound(key));
      CHECK(i + 1 == view.lower_bound(key + std::string(1, '\0')));
      i++;
    }
    CHECK(view.count() == view.lower_bound(std::string(64, '\xff')));
    // Decoded again, the page is the same page.
    page_t again;
    view.load(again);
    CHECK(bytes == page_bytes(again));
  }
}

TEST_CASE("a split's halves are the whole, in order, each under the limit") {
  // Verify page_split at every size of page, by bytes.
  // 1. Fill pages of 2 entries up to full, of random lengths.
  // 2. Split each: the halves' entries, together, are the whole's,
  //    and every key of the left is below every key of the right.
  // 3. The left's high key is the right's first key; the right takes
  //    the left's high key and right link, the left none.
  // 4. Neither half is empty, and each is under the limit.
  std::mt19937_64 rng(2);
  for (int round = 0; round < 200; round++) {
    page_t page;
    page.level = round % 2;
    page.flags = (round % 4 >= 2) ? page_has_high : 0;
    if (0 != (page.flags & page_has_high)) {
      page.high = std::string(50, '\xff');
    }
    page.right = 99;
    std::map<std::string, std::string> whole;
    size_t want = 2 + round % 150;
    while (whole.size() < want) {
      std::string key = random_key(rng, 1 + round % 300);
      int rc = page_insert(page, key, "x");
      REQUIRE(rc >= 0);
      if (2 == rc) {
        break;
      }
      if (0 == rc) {
        whole[key] = "x";
      }
    }
    if (whole.size() < 2) {
      continue;
    }
    page_t right;
    std::string old_high = page.high;
    uint8_t old_flags = page.flags;
    page_split(page, right);
    REQUIRE_FALSE(page.entries.empty());
    REQUIRE_FALSE(right.entries.empty());
    CHECK(page.entries.size() + right.entries.size() == whole.size());
    CHECK(page.entries.back().first < right.entries.front().first);
    CHECK(page.high == right.entries.front().first);
    CHECK(0 != (page.flags & page_has_high));
    CHECK((old_flags & page_has_high) == (right.flags & page_has_high));
    CHECK(old_high == right.high);
    CHECK(99u == right.right);
    CHECK(0u == page.right);
    CHECK(page.level == right.level);
    CHECK(page_size(page) <= page_limit);
    CHECK(page_size(right) <= page_limit);
    // The halves, laid out and read back, hold the whole.
    std::map<std::string, std::string> left_part(
        whole.begin(), whole.find(right.entries.front().first));
    std::map<std::string, std::string> right_part(
        whole.find(right.entries.front().first), whole.end());
    check_entries(view_of(page_bytes(page)), left_part);
    check_entries(view_of(page_bytes(right)), right_part);
  }
}

TEST_CASE("an entry over a third of its page is refused") {
  // Verify the entry limit, which keeps two entries a side at a split.
  // 1. An entry of exactly a third of the limit is taken.
  // 2. One byte more is refused with SM_ERR_KEYTOOLONG, and the page
  //    is not changed.
  page_t page;
  size_t max = entry_max(page_limit);
  std::string key(max - entry_size(0, 0), 'k');
  CHECK(0 == page_insert(page, key, ""));
  std::string longer(max - entry_size(0, 0) + 1, 'l');
  CHECK(-1 == page_insert(page, longer, ""));
  CHECK(sm1::SM_ERR_KEYTOOLONG == sm1::errnum());
  CHECK(1u == page.entries.size());
}

TEST_CASE("an inner page descends by the last entry at or below a key") {
  // Verify child(), the descent of an inner page.
  // 1. An inner page of entries "" (below all), "f" and "p".
  // 2. A key below "f" descends by the first; "f" itself and a key
  //    between by the second; "p" and above by the third.
  // 3. A key at or past the high key is to move right.
  page_t page;
  page.level = 1;
  page.flags = page_has_high;
  page.high = "x";
  REQUIRE(0 == page_insert(page, "", "1"));
  REQUIRE(0 == page_insert(page, "f", "2"));
  REQUIRE(0 == page_insert(page, "p", "3"));
  std::string bytes = page_bytes(page);
  page_view_t view = view_of(bytes);
  CHECK(0u == view.child("a"));
  CHECK(1u == view.child("f"));
  CHECK(1u == view.child("g"));
  CHECK(2u == view.child("p"));
  CHECK(2u == view.child("w"));
  CHECK("3" == view.value(view.child("w")));
  CHECK_FALSE(view.past_high("w"));
  CHECK(view.past_high("x"));
  CHECK(view.past_high("y"));
}

TEST_CASE("bytes that are not a page are refused") {
  // Verify open()'s checks, so a damaged value is not read past.
  // 1. A page of three entries opens.
  // 2. Cut short anywhere, its bytes are refused with SM_ERR_CORRUPT.
  // 3. A slot pointing past the end is refused; so is a high key with
  //    no flag for it.
  page_t page;
  REQUIRE(0 == page_insert(page, "a", "1"));
  REQUIRE(0 == page_insert(page, "b", "2"));
  REQUIRE(0 == page_insert(page, "c", "3"));
  std::string bytes = page_bytes(page);
  page_view_t view;
  REQUIRE(0 == view.open(bytes));
  for (size_t n = 0; n < bytes.size(); n++) {
    CHECK(-1 == view.open(std::string_view(bytes).substr(0, n)));
    CHECK(sm1::SM_ERR_CORRUPT == sm1::errnum());
  }
  std::string bad = bytes;
  bad[page_header_size] = '\xff';
  bad[page_header_size + 1] = '\x7f';
  CHECK(-1 == view.open(bad));
  std::string flagless = bytes;
  flagless[20] = 1;
  CHECK(-1 == view.open(flagless));
}

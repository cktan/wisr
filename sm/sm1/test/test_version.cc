/*
 * test_version.cc -- the version codec, the wisr.store.v2
 * comparator and its prefix extractor (SM1_DESIGN.md §3.1,
 * ../VCHAIN.md §2).
 */

#include "doctest.h"

#include "sm0_codec.h"
#include "version.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace wisr;

TEST_CASE("version keys round-trip through encode and decode") {
  // Verify a version key decodes to the userkey, wts, and seq it encodes.
  // 1. Encode a userkey containing NUL and 0xFF bytes.
  // 2. Decode it and compare every field.
  std::string uk("a\0\xff", 3);
  std::string k = version_key(uk, 0x0102030405060708ull, 42);
  CHECK(k.size() == 1 + uk.size() + version_tail_size);
  CHECK(k[0] == key_class_versioned);
  version_t v;
  version_decode_key(k, &v);
  CHECK(v.userkey == rocksdb::Slice(uk));
  CHECK(v.wts == 0x0102030405060708ull);
  CHECK(v.seq == 42);
}

TEST_CASE("version values round-trip, stamped or not") {
  // Verify flags, xid or stamp, and payload decode as encoded.
  // 1. Decode an unstamped value: xid, then the payload.
  // 2. Decode a tombstone with no payload.
  // 3. Decode a stamped value: commit_ts, then the payload.
  // 4. Reject an empty value and a truncated xid or stamp.
  // 5. Pin the exact bytes of both forms.
  // Decoded slices borrow the value, so each value is kept in a
  // named string for as long as they are read.
  version_t v;
  std::string plain = version_value(0, 77, "hello");
  REQUIRE(version_decode_value(plain, &v));
  CHECK(v.flags == 0);
  CHECK(v.xid == 77);
  CHECK(v.commit_ts == 0);
  CHECK(v.payload == rocksdb::Slice("hello"));

  std::string tomb = version_value(version_tombstone, 5, "");
  REQUIRE(version_decode_value(tomb, &v));
  CHECK(v.flags == version_tombstone);
  CHECK(v.payload.empty());

  std::string stamped(1, static_cast<char>(version_stamped));
  sm0::put_be64(stamped, 12345);
  stamped += "hi";
  REQUIRE(version_decode_value(stamped, &v));
  CHECK(v.flags == version_stamped);
  CHECK(v.commit_ts == 12345);
  CHECK(v.xid == 0);
  CHECK(v.payload == rocksdb::Slice("hi"));

  // A short unstamped header is malformed too.
  std::string short_plain = plain.substr(0, 5);
  CHECK_FALSE(version_decode_value(short_plain, &v));

  // Golden bytes: flags, then the eight-byte xid or stamp, then payload.
  CHECK(version_value(0, 0x0102030405060708ull, "p") ==
        std::string("\x00\x01\x02\x03\x04\x05\x06\x07\x08p", 10));
  CHECK(stamped == std::string("\x02\x00\x00\x00\x00\x00\x00\x30\x39hi", 11));
  // Golden bytes of a key: class, userkey, ~wts, ~seq.
  CHECK(version_key("a", 1, 2) ==
        std::string("\0a\xff\xff\xff\xff\xff\xff\xff\xfe\xff\xff\xff\xfd", 14));

  CHECK_FALSE(version_decode_value("", &v));
  std::string bad(1, static_cast<char>(version_stamped));
  bad += "abc";
  CHECK_FALSE(version_decode_value(bad, &v));
}

TEST_CASE("the comparator orders userkeys by memcmp, versions newest first") {
  // Verify keys that are prefixes of one another sort by userkey.
  // 1. Encode several versions of "", "a", "a\0", "a\xff", "ab".
  // 2. Sort them with the comparator.
  // 3. Confirm userkeys come out in memcmp order, each contiguous.
  // 4. Confirm versions of one userkey run newest wts first,
  //    and within one wts, highest seq first.
  std::vector<std::string> uks = {"", "a", std::string("a\0", 2), "a\xff",
                                  "ab"};
  std::vector<std::string> keys;
  for (const std::string &uk : uks) {
    for (uint64_t wts : {5ull, 900ull, 70000ull}) {
      for (uint32_t seq : {1u, 2u}) {
        keys.push_back(version_key(uk, wts, seq));
      }
    }
  }
  const rocksdb::Comparator *cmp = version_comparator();
  std::sort(keys.begin(), keys.end(),
            [cmp](const std::string &a, const std::string &b) {
              return cmp->Compare(a, b) < 0;
            });

  std::vector<std::string> expect_uks = uks;
  std::sort(expect_uks.begin(), expect_uks.end());
  size_t i = 0;
  for (const std::string &uk : expect_uks) {
    for (uint64_t wts : {70000ull, 900ull, 5ull}) {
      for (uint32_t seq : {2u, 1u}) {
        version_t v;
        version_decode_key(keys[i++], &v);
        CHECK(v.userkey == rocksdb::Slice(uk));
        CHECK(v.wts == wts);
        CHECK(v.seq == seq);
      }
    }
  }
}

TEST_CASE("scan bounds bracket every version of their userkey") {
  // Verify all-00 and all-FF tails fall outside a key's versions.
  // 1. Build the 00 and FF bounds for "a".
  // 2. Confirm 00 < every version of "a" < FF.
  // 3. Confirm FF of "a" sorts before any version of "a\0".
  const rocksdb::Comparator *cmp = version_comparator();
  std::string lo = version_bound("a", '\x00');
  std::string hi = version_bound("a", '\xff');
  for (uint64_t wts : {1ull, 12345ull, ~0ull - 1}) {
    std::string k = version_key("a", wts, 1);
    CHECK(cmp->Compare(lo, k) < 0);
    CHECK(cmp->Compare(k, hi) < 0);
  }
  CHECK(cmp->Compare(hi, version_key(std::string("a\0", 2), 1, 1)) < 0);
}

TEST_CASE("the comparator orders keys by class, then within each class") {
  // Verify wisr.store.v2's class order and its two orders within.
  // 1. Every versioned key sorts before every current key, and every
  //    current key before every old key, whatever their bytes.
  // 2. Current keys sort bytewise, a key before the longer keys it
  //    starts.
  // 3. Old keys, be64 ids, sort as numbers.
  const rocksdb::Comparator *cmp = version_comparator();
  auto current = [](const std::string &k) {
    return std::string(1, key_class_current) + k;
  };
  auto old = [](uint64_t id) {
    std::string k(1, key_class_old);
    sm0::put_be64(k, id);
    return k;
  };
  std::string top = version_key(std::string(20, '\xff'), 1, 1);
  CHECK(cmp->Compare(top, current("")) < 0);
  CHECK(cmp->Compare(current(std::string(20, '\xff')), old(0)) < 0);
  CHECK(cmp->Compare(version_key("", 1, 1), current("a")) < 0);

  CHECK(cmp->Compare(current("a"), current(std::string("a\0", 2))) < 0);
  CHECK(cmp->Compare(current(std::string("a\0", 2)), current("ab")) < 0);
  CHECK(cmp->Compare(current("ab"), current("b")) < 0);
  CHECK(cmp->Compare(current("ab"), current("ab")) == 0);
  // Long enough to have a version tail, were it versioned: bytewise,
  // "a" and twelve FF bytes sorts after "ab" and twelve zeros.
  CHECK(cmp->Compare(current("a" + std::string(12, '\xff')),
                     current("ab" + std::string(12, '\0'))) > 0);

  CHECK(cmp->Compare(old(2), old(10)) < 0);
  CHECK(cmp->Compare(old(255), old(256)) < 0);
}

TEST_CASE("the prefix extractor keys a versioned key on its userkey, any "
          "other whole") {
  // Verify wisr.prefix.v2's domain and its prefixes.
  // 1. A versioned key's prefix is its class and userkey: every
  //    version of a userkey shares it.
  // 2. A current or an old key is its own prefix.
  // 3. An empty key, and a versioned key with no full tail, are out
  //    of its domain.
  auto px = version_prefix_extractor();
  std::string k1 = version_key("ab", 7, 1), k2 = version_key("ab", 9, 3);
  REQUIRE(px->InDomain(k1));
  CHECK(px->Transform(k1) == rocksdb::Slice(std::string("\0ab", 3)));
  CHECK(px->Transform(k1) == px->Transform(k2));

  std::string c = std::string(1, key_class_current) + "ab";
  std::string o(1, key_class_old);
  sm0::put_be64(o, 42);
  REQUIRE(px->InDomain(c));
  REQUIRE(px->InDomain(o));
  CHECK(px->Transform(c) == rocksdb::Slice(c));
  CHECK(px->Transform(o) == rocksdb::Slice(o));

  CHECK_FALSE(px->InDomain(""));
  CHECK_FALSE(px->InDomain(std::string("\0ab", 3)));
}

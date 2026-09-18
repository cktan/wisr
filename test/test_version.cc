/*
 * test_version.cc -- the version codec and the wisr.version.v1
 * comparator (DESIGN.md §3.1).
 */

#include "doctest.h"

#include "version.h"

#include <algorithm>
#include <string>
#include <vector>

using namespace wisr;

TEST_CASE("version keys round-trip through encode and decode")
{
    // Verify a version key decodes to the userkey, xid, and seq it encodes.
    // 1. Encode a userkey containing NUL and 0xFF bytes.
    // 2. Decode it and compare every field.
    std::string uk("a\0\xff", 3);
    std::string k = version_key(uk, 0x0102030405060708ull, 42);
    CHECK(k.size() == uk.size() + version_tail_size);
    version_t v;
    version_decode_key(k, &v);
    CHECK(v.userkey == rocksdb::Slice(uk));
    CHECK(v.xid == 0x0102030405060708ull);
    CHECK(v.seq == 42);
}

TEST_CASE("version values round-trip, stamped or not")
{
    // Verify flags, stamp, and payload decode as encoded.
    // 1. Decode an unstamped value with a payload.
    // 2. Decode a tombstone with no payload.
    // 3. Reject an empty value and a truncated stamp.
    version_t v;
    REQUIRE(version_decode_value(version_value(0, "hello"), &v));
    CHECK(v.flags == 0);
    CHECK(v.payload == rocksdb::Slice("hello"));

    REQUIRE(version_decode_value(version_value(version_tombstone, ""), &v));
    CHECK(v.flags == version_tombstone);
    CHECK(v.payload.empty());

    CHECK_FALSE(version_decode_value("", &v));
    std::string bad(1, static_cast<char>(version_stamped));
    bad += "abc";
    CHECK_FALSE(version_decode_value(bad, &v));
}

TEST_CASE("the comparator orders userkeys by memcmp, versions newest first")
{
    // Verify keys that are prefixes of one another sort by userkey.
    // 1. Encode several versions of "", "a", "a\0", "a\xff", "ab".
    // 2. Sort them with the comparator.
    // 3. Confirm userkeys come out in memcmp order, each contiguous.
    // 4. Confirm versions of one userkey run newest xid first,
    //    and within one xid, highest seq first.
    std::vector<std::string> uks = {"", "a", std::string("a\0", 2), "a\xff",
                                    "ab"};
    std::vector<std::string> keys;
    for (const std::string& uk : uks) {
        for (uint64_t xid : {5ull, 900ull, 70000ull}) {
            for (uint32_t seq : {1u, 2u}) {
                keys.push_back(version_key(uk, xid, seq));
            }
        }
    }
    const rocksdb::Comparator* cmp = version_comparator();
    std::sort(keys.begin(), keys.end(),
              [cmp](const std::string& a, const std::string& b) {
                  return cmp->Compare(a, b) < 0;
              });

    std::vector<std::string> expect_uks = uks;
    std::sort(expect_uks.begin(), expect_uks.end());
    size_t i = 0;
    for (const std::string& uk : expect_uks) {
        for (uint64_t xid : {70000ull, 900ull, 5ull}) {
            for (uint32_t seq : {2u, 1u}) {
                version_t v;
                version_decode_key(keys[i++], &v);
                CHECK(v.userkey == rocksdb::Slice(uk));
                CHECK(v.xid == xid);
                CHECK(v.seq == seq);
            }
        }
    }
}

TEST_CASE("scan bounds bracket every version of their userkey")
{
    // Verify all-00 and all-FF tails fall outside a key's versions.
    // 1. Build the 00 and FF bounds for "a".
    // 2. Confirm 00 < every version of "a" < FF.
    // 3. Confirm FF of "a" sorts before any version of "a\0".
    const rocksdb::Comparator* cmp = version_comparator();
    std::string lo = version_bound("a", '\x00');
    std::string hi = version_bound("a", '\xff');
    for (uint64_t xid : {1ull, 12345ull, ~0ull - 1}) {
        std::string k = version_key("a", xid, 1);
        CHECK(cmp->Compare(lo, k) < 0);
        CHECK(cmp->Compare(k, hi) < 0);
    }
    CHECK(cmp->Compare(hi, version_key(std::string("a\0", 2), 1, 1)) < 0);
}

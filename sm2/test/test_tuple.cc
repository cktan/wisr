/*
 * test_tuple.cc -- the tuple codec (SM2_DESIGN.md §4, §8.2
 * step 3): its bytes, its round trip, the schema changes it must
 * survive, and the tuples it must refuse. No database is involved:
 * the codec touches sm1 nowhere.
 */

#include "datums.h"
#include "doctest.h"
#include "gen.h"

#include "sm1.h"
#include "sm2.h"

#include <optional>
#include <random>
#include <string>
#include <vector>

namespace {

/* An attribute of type, live, null when missing. */
sm2::attribute_t attr(const sm2::type_t &type, int32_t typmod = -1) {
  return {&type, typmod, false, std::nullopt};
}

/* A varlena holding datum: how a variable-width datum is stored. */
std::string vl(std::string_view datum) { return sm2::make_varlena(datum); }

/* datum as the codec takes attribute a's: a varlena if variable. */
std::string codec_form(const sm2::attribute_t &a, const std::string &datum) {
  return sm2::datum_width(a.type, a.typmod) < 0 ? vl(datum) : datum;
}

/* Encode row under schema; it must encode. */
std::string encode(const std::vector<sm2::attribute_t> &schema,
                   const std::vector<std::optional<std::string>> &row) {
  std::vector<sm2::datum_t> datums;
  for (const auto &d : row) {
    datums.push_back({d ? std::string_view(*d) : std::string_view(), !d});
  }
  std::string tuple;
  REQUIRE(sm2::tuple_encode(schema, datums, tuple) == 0);
  return tuple;
}

/* Every attribute of tuple under schema, as owned strings. */
std::vector<std::optional<std::string>>
deform(const std::vector<sm2::attribute_t> &schema, std::string_view tuple) {
  std::vector<std::optional<std::string_view>> views;
  REQUIRE_MESSAGE(sm2::tuple_deform(schema, tuple, views) == 0, sm1::errmsg());
  std::vector<std::optional<std::string>> out;
  for (const auto &v : views) {
    out.push_back(v ? std::optional<std::string>(*v) : std::nullopt);
  }
  return out;
}

/* tuple's bytes in hex, for the golden tests' messages. */
std::string hex(std::string_view s) {
  static const char digits[] = "0123456789abcdef";
  std::string out;
  for (unsigned char c : s) {
    out += digits[c >> 4];
    out += digits[c & 15];
  }
  return out;
}

} // namespace

TEST_CASE("a tuple's bytes are the layout SM2_DESIGN.md §4.1 fixes") {
  // Pin the format with two golden tuples, so a change to it is a
  // change to this test.
  // 1. (int4 7, text "hi", int8 null, bool true): flags 0x01 for a
  //    null, natts 4, bitmap 0x04 for attribute 2, the fixed part
  //    int4 then bool with the null int8 skipped, one end offset,
  //    3, then "hi" as a varlena with PostgreSQL's 1-byte header,
  //    (3 << 1) | 1.
  // 2. (int4 7, text ""): no nulls, so no bitmap; an empty text
  //    is its header alone, 0x03, ending at offset 1.
  std::vector<sm2::attribute_t> schema = {
      attr(sm2::type_int4), attr(sm2::type_text), attr(sm2::type_int8),
      attr(sm2::type_bool)};
  std::string t = encode(
      schema, {le(int32_t{7}), vl("hi"), std::nullopt, std::string(1, '\1')});
  CHECK(hex(t) == "010404"
                  "07000000"
                  "01"
                  "0300"
                  "076869");
  std::vector<sm2::attribute_t> two = {attr(sm2::type_int4),
                                       attr(sm2::type_text)};
  t = encode(two, {le(int32_t{7}), vl("")});
  CHECK(hex(t) == "0002"
                  "07000000"
                  "0100"
                  "03");
}

TEST_CASE("every tuple round trips, and any one column decodes alone") {
  // Verify the codec by property, from test_seed(), over random
  // schemas of every type, decimal at both widths.
  // 1. Every row, nulls included, encodes, and tuple_deform gives
  //    it back.
  // 2. tuple_decode of each attribute alone agrees with deform.
  // 3. A row with no nulls carries no null bitmap: its has-nulls
  //    flag is clear.
  INFO("seed ", test_seed());
  std::mt19937_64 rng(test_seed());
  const std::vector<typed_t> &types = all_types();
  for (int round = 0; round < 300; round++) {
    std::vector<sm2::attribute_t> schema;
    for (size_t n = 1 + rng() % 12; n > 0; n--) {
      const typed_t &t = types[rng() % types.size()];
      schema.push_back({t.type, t.typmod, false, std::nullopt});
    }
    for (int r = 0; r < 20; r++) {
      std::vector<std::optional<std::string>> row;
      bool any_null = false;
      for (const auto &a : schema) {
        if (rng() % 4 == 0) {
          row.push_back(std::nullopt);
          any_null = true;
        } else {
          row.push_back(codec_form(a, gen_datum({a.type, a.typmod}, rng)));
        }
      }
      std::string tuple = encode(schema, row);
      CHECK(deform(schema, tuple) == row);
      for (size_t i = 0; i < schema.size(); i++) {
        std::optional<std::string_view> d;
        REQUIRE(sm2::tuple_decode(schema, tuple, i, d) == 0);
        CHECK(d.has_value() == row[i].has_value());
        if (d && row[i]) {
          CHECK(*d == *row[i]);
        }
      }
      CHECK(((tuple[0] & 0x01) != 0) == any_null);
    }
  }
}

TEST_CASE("variable offsets widen to 4 bytes only past 64 KB") {
  // Verify the wide-offsets flag, chosen per tuple.
  // 1. 65535 bytes of variable data, a 65531-byte bytea and its
  //    4-byte header, keep 2-byte offsets.
  // 2. One byte more sets the flag, 0x04, and still round trips.
  std::vector<sm2::attribute_t> schema = {attr(sm2::type_bytea),
                                          attr(sm2::type_int4)};
  for (size_t len : {size_t{65531}, size_t{65532}}) {
    CAPTURE(len);
    std::vector<std::optional<std::string>> row = {vl(std::string(len, 'x')),
                                                   le(int32_t{-1})};
    std::string t = encode(schema, row);
    CHECK(((t[0] & 0x04) != 0) == (len > 65531));
    CHECK(deform(schema, t) == row);
  }
}

TEST_CASE("a row reads on after ADD COLUMN, as null or its missing value") {
  // Verify natts: columns appended after a row was written.
  // 1. A row written under (int4, text) reads under a schema with
  //    two more attributes: its own two, then std::nullopt for the
  //    one added with no value, then the other's missing value.
  // 2. tuple_decode of an added attribute says the same.
  std::vector<sm2::attribute_t> before = {attr(sm2::type_int4),
                                          attr(sm2::type_text)};
  std::string t = encode(before, {le(int32_t{3}), vl("row")});
  std::vector<sm2::attribute_t> after = before;
  after.push_back(attr(sm2::type_int8));
  after.push_back({&sm2::type_int4, -1, false, le(int32_t{42})});
  std::vector<std::optional<std::string>> want = {
      le(int32_t{3}), vl("row"), std::nullopt, le(int32_t{42})};
  CHECK(deform(after, t) == want);
  std::optional<std::string_view> d;
  REQUIRE(sm2::tuple_decode(after, t, 3, d) == 0);
  REQUIRE(d);
  CHECK(*d == le(int32_t{42}));
  REQUIRE(sm2::tuple_decode(after, t, 2, d) == 0);
  CHECK_FALSE(d);
}

TEST_CASE("a dropped column keeps its place and reads as null") {
  // Verify DROP COLUMN.
  // 1. A row written while (int4, text, int4) were all live reads
  //    under the schema with the text dropped: the text as
  //    std::nullopt, the int4s unmoved.
  // 2. A new row stores the dropped attribute as null; a value
  //    for it is SM_ERR_INVALID.
  std::vector<sm2::attribute_t> live = {
      attr(sm2::type_int4), attr(sm2::type_text), attr(sm2::type_int4)};
  std::string old = encode(live, {le(int32_t{1}), vl("gone"), le(int32_t{2})});
  std::vector<sm2::attribute_t> dropped = live;
  dropped[1].dropped = true;
  std::vector<std::optional<std::string>> want = {le(int32_t{1}), std::nullopt,
                                                  le(int32_t{2})};
  CHECK(deform(dropped, old) == want);
  std::string fresh =
      encode(dropped, {le(int32_t{1}), std::nullopt, le(int32_t{2})});
  CHECK(deform(dropped, fresh) == want);
  std::string ignored;
  std::vector<sm2::datum_t> with_value = {
      {le(int32_t{1}), false}, {vl("x"), false}, {le(int32_t{2}), false}};
  CHECK(sm2::tuple_encode(dropped, with_value, ignored) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

TEST_CASE("a malformed tuple is refused as corrupt") {
  // Verify the checks tuple_deform and tuple_decode make.
  // 1. Every proper prefix of a valid tuple, and the tuple with a
  //    byte added, is SM_ERR_CORRUPT: the parts must fit exactly.
  // 2. So is a reserved flag, more attributes than the schema has,
  //    an end offset past the next one's, a varlena tagged as a
  //    TOAST pointer, which sm2 cannot read yet, and one with a
  //    reserved tag.
  std::vector<sm2::attribute_t> schema = {
      attr(sm2::type_int4), attr(sm2::type_text), attr(sm2::type_int8),
      attr(sm2::type_bytea)};
  std::string good =
      encode(schema, {le(int32_t{7}), vl("abc"), std::nullopt, vl("de")});
  std::vector<std::optional<std::string_view>> out;
  auto refused = [&](std::string_view t) {
    CAPTURE(hex(t));
    CHECK(sm2::tuple_deform(schema, t, out) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
    std::optional<std::string_view> d;
    CHECK(sm2::tuple_decode(schema, t, 0, d) == -1);
  };
  for (size_t n = 0; n < good.size(); n++) {
    refused(good.substr(0, n));
  }
  refused(good + "!");
  std::string bad = good;
  bad[0] = static_cast<char>(bad[0] | 0x80);
  refused(bad);
  bad = good;
  bad[0] = static_cast<char>(bad[0] | 0x02);
  refused(bad);
  bad = good;
  bad[1] = 5; // natts past the schema's 4
  refused(bad);
  // The offsets, 4 then 7, follow the header, the bitmap and the
  // 4-byte fixed part: make the first end past the second.
  bad = good;
  size_t offsets = 3 + 4;
  REQUIRE(bad[offsets] == 4);
  bad[offsets] = 8;
  refused(bad);
  // The first varlena, "abc" with header 0x09, follows the two
  // offsets: make it a TOAST pointer, a compressed 4-byte header,
  // and a 1-byte header one byte too long.
  size_t first = offsets + 4;
  REQUIRE(good[first] == '\x09');
  for (char h : {sm2::varlena_toast, '\x02', '\x0b'}) {
    bad = good;
    bad[first] = h;
    refused(bad);
  }
}

TEST_CASE("the codec refuses arguments that do not fit the schema") {
  // Verify tuple_encode and tuple_decode's SM_ERR_INVALID cases.
  // 1. Fewer or more datums than attributes, and a fixed-width
  //    datum of the wrong width, leave the tuple as it was.
  // 2. A variable datum with no header, a TOAST pointer, a
  //    compressed one, or a header that does not match its
  //    length.
  // 3. A decimal attribute with a typmod decimal refuses.
  // 4. An ordinal past the schema.
  std::vector<sm2::attribute_t> schema = {attr(sm2::type_int4),
                                          attr(sm2::type_text)};
  std::string tuple = "keep";
  std::vector<sm2::datum_t> one = {{le(int32_t{1}), false}};
  CHECK(sm2::tuple_encode(schema, one, tuple) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  std::vector<sm2::datum_t> wide = {{"12345", false}, {vl("t"), false}};
  CHECK(sm2::tuple_encode(schema, wide, tuple) == -1);
  CHECK(tuple == "keep");
  // A variable datum must be a varlena this codec can store.
  std::string toast(1, sm2::varlena_toast);
  std::string compressed("\x12\0\0\0", 4), too_short = "\x09"
                                                       "ab";
  for (std::string_view v :
       {std::string_view(), std::string_view(toast),
        std::string_view(compressed), std::string_view(too_short)}) {
    std::vector<sm2::datum_t> row = {{le(int32_t{1}), false}, {v, false}};
    CHECK(sm2::tuple_encode(schema, row, tuple) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  }
  CHECK(tuple == "keep");
  std::vector<sm2::attribute_t> bad_typmod = {attr(sm2::type_decimal, 3)};
  std::vector<sm2::datum_t> d8 = {{le(int64_t{1}), false}};
  CHECK(sm2::tuple_encode(bad_typmod, d8, tuple) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  std::string good = encode(schema, {le(int32_t{1}), vl("t")});
  std::optional<std::string_view> d;
  CHECK(sm2::tuple_decode(schema, good, 2, d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

TEST_CASE("a varlena is PostgreSQL's, in its shortest form") {
  // Verify the varlena helpers of sm2.h against PostgreSQL's
  // little-endian headers (varatt.h).
  // 1. Up to 126 bytes take the 1-byte header, (len << 1) | 1, the
  //    length counting the header: "abc" is 09 61 62 63, and 126
  //    bytes start ff.
  // 2. 127 bytes and more take the 4-byte header, len << 2: 127
  //    bytes start 0c 02 00 00, and 200 bytes 30 03 00 00.
  // 3. varlena_payload takes either header off; varlena_is_toast
  //    is true only for PostgreSQL's TOAST pointer byte, 0x01.
  // 4. The codec stores a 4-byte header on short data in the
  //    1-byte form.
  std::string v = sm2::make_varlena("abc");
  CHECK(hex(v) == "09616263");
  CHECK(sm2::varlena_payload(v) == "abc");
  CHECK(hex(sm2::make_varlena(std::string(126, 'x'))).substr(0, 2) == "ff");
  CHECK(hex(sm2::make_varlena(std::string(127, 'x'))).substr(0, 8) ==
        "0c020000");
  std::string long_v = sm2::make_varlena(std::string(200, 'y'));
  CHECK(hex(long_v.substr(0, 4)) == "30030000");
  CHECK(sm2::varlena_payload(long_v) == std::string(200, 'y'));
  CHECK_FALSE(sm2::varlena_is_toast(v));
  CHECK(sm2::varlena_is_toast(std::string(1, sm2::varlena_toast)));
  CHECK_FALSE(sm2::varlena_is_toast(""));
  std::vector<sm2::attribute_t> schema = {attr(sm2::type_text)};
  std::string four("\x1c\0\0\0abc", 7);
  CHECK(sm2::varlena_payload(four) == "abc");
  std::string t = encode(schema, {four});
  CHECK(deform(schema, t) ==
        std::vector<std::optional<std::string>>{sm2::make_varlena("abc")});
}

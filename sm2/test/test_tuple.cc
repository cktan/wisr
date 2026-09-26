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

/* row, in codec form, as the Datums tup_encode takes. */
values_t datums_of(const std::vector<sm2::attribute_t> &schema,
                   const std::vector<std::optional<std::string>> &row) {
  return values_of(types_of(schema), row, false);
}

/* Encode row under schema; it must encode. */
std::string encode(const std::vector<sm2::attribute_t> &schema,
                   const std::vector<std::optional<std::string>> &row) {
  values_t v = datums_of(schema, row);
  std::string tuple;
  REQUIRE_MESSAGE(sm2::tup_encode(schema, v.d(), v.null(), tuple) == 0,
                  sm1::errmsg());
  return tuple;
}

/* Every attribute of tuple under schema, as owned strings. */
std::vector<std::optional<std::string>>
deform(const std::vector<sm2::attribute_t> &schema, std::string_view tuple) {
  std::vector<std::optional<std::string>> out;
  REQUIRE_MESSAGE(bytes_of(schema, tuple, out) == 0, sm1::errmsg());
  return out;
}

/*
 * Attribute k of tuple under desc, as deform gives it; returns
 * what tup_decode does.
 */
int decode_bytes(const sm2::tup_desc_t &desc, std::string_view tuple, size_t k,
                 std::optional<std::string> &out) {
  sm2::datum_t v;
  bool isnull;
  out.reset();
  if (sm2::tup_decode(desc, tuple, k, v, isnull) != 0) {
    return -1;
  }
  if (isnull) {
    return 0;
  }
  std::string_view b;
  if (sm2::datum_bytes(desc.schema[k].type, desc.schema[k].typmod, v, b) != 0) {
    return -1;
  }
  out = std::string(b);
  return 0;
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
  //    null, natts 4, bitmap 0x04 for attribute 2, then the fixed
  //    part in schema order -- the int4, the text's slot, 10, where
  //    its varlena starts in the tuple, and the bool, the null int8
  //    taking nothing -- then "hi" as a varlena with PostgreSQL's
  //    1-byte header, (3 << 1) | 1.
  // 2. (int4 7, text ""): no nulls, so no bitmap; the text's slot
  //    is 8, and an empty text is its header alone, 0x03.
  std::vector<sm2::attribute_t> schema = {
      attr(sm2::type_int4), attr(sm2::type_text), attr(sm2::type_int8),
      attr(sm2::type_bool)};
  std::string t = encode(
      schema, {le(int32_t{7}), vl("hi"), std::nullopt, std::string(1, '\1')});
  CHECK(hex(t) == "010404"
                  "07000000"
                  "0a00"
                  "01"
                  "076869");
  std::vector<sm2::attribute_t> two = {attr(sm2::type_int4),
                                       attr(sm2::type_text)};
  t = encode(two, {le(int32_t{7}), vl("")});
  CHECK(hex(t) == "0002"
                  "07000000"
                  "0800"
                  "03");
}

TEST_CASE("every tuple round trips, and any one column decodes alone") {
  // Verify the codec by property, from test_seed(), over random
  // schemas of every type, decimal at both widths.
  // 1. Every row, nulls included, encodes, and tup_deform gives
  //    it back.
  // 2. tup_decode of each attribute alone agrees with deform.
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
        std::optional<std::string> d;
        REQUIRE(decode_bytes(schema, tuple, i, d) == 0);
        CHECK(d.has_value() == row[i].has_value());
        if (d && row[i]) {
          CHECK(*d == *row[i]);
        }
      }
      CHECK(((tuple[0] & 0x01) != 0) == any_null);
    }
  }
}

TEST_CASE("a tuple is at most 65535 bytes, so its slots are 2 bytes") {
  // Verify the tuple length limit, which is what keeps every slot
  // at 2 bytes.
  // 1. A tuple of exactly 65535 bytes -- a 2-byte header, the
  //    bytea's slot and the int4, 6 bytes, and a 65523-byte bytea
  //    with its 4-byte header -- encodes with no flag set and
  //    round trips.
  // 2. One byte more fails with SM_ERR_ROWTOOLONG and leaves the
  //    tuple as it was.
  // 3. A reader refuses, as corrupt, a tuple past 65535 bytes and
  //    one with the retired wide-slots flag, 0x04.
  std::vector<sm2::attribute_t> schema = {attr(sm2::type_bytea),
                                          attr(sm2::type_int4)};
  std::vector<std::optional<std::string>> row = {vl(std::string(65523, 'x')),
                                                 le(int32_t{-1})};
  std::string t = encode(schema, row);
  CHECK(t.size() == sm2::max_tup_len);
  CHECK(t[0] == 0);
  CHECK(deform(schema, t) == row);

  std::string big = vl(std::string(65524, 'x')), four = le(int32_t{-1});
  values_t over = datums_of(schema, {big, four});
  std::string kept = "keep";
  CHECK(sm2::tup_encode(schema, over.d(), over.null(), kept) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_ROWTOOLONG);
  CHECK(kept == "keep");

  std::vector<std::optional<std::string>> views;
  CHECK(bytes_of(schema, t + "x", views) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
  std::string flagged = t;
  flagged[0] = 0x04;
  CHECK(bytes_of(schema, flagged, views) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
}

TEST_CASE("a row reads on after ADD COLUMN, as null or its missing value") {
  // Verify natts: columns appended after a row was written.
  // 1. A row written under (int4, text) reads under a schema with
  //    two more attributes: its own two, then std::nullopt for the
  //    one added with no value, then the other's missing value.
  // 2. tup_decode of an added attribute says the same.
  std::vector<sm2::attribute_t> before = {attr(sm2::type_int4),
                                          attr(sm2::type_text)};
  std::string t = encode(before, {le(int32_t{3}), vl("row")});
  std::vector<sm2::attribute_t> after = before;
  after.push_back(attr(sm2::type_int8));
  after.push_back({&sm2::type_int4, -1, false, le(int32_t{42})});
  std::vector<std::optional<std::string>> want = {
      le(int32_t{3}), vl("row"), std::nullopt, le(int32_t{42})};
  CHECK(deform(after, t) == want);
  std::optional<std::string> d;
  REQUIRE(decode_bytes(after, t, 3, d) == 0);
  REQUIRE(d);
  CHECK(*d == le(int32_t{42}));
  REQUIRE(decode_bytes(after, t, 2, d) == 0);
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
  values_t with_value =
      datums_of(dropped, {le(int32_t{1}), vl("x"), le(int32_t{2})});
  CHECK(sm2::tup_encode(dropped, with_value.d(), with_value.null(), ignored) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

TEST_CASE("a malformed tuple is refused as corrupt") {
  // Verify the checks tup_deform makes on the whole tuple, and the
  // narrower ones tup_decode makes on what it reads.
  // 1. Every proper prefix of a valid tuple, and the tuple with a
  //    byte added, is SM_ERR_CORRUPT to deform: the parts must fit
  //    exactly. Decoding the last attribute, whose varlena ends
  //    the tuple, refuses every prefix too.
  // 2. A reserved flag, or more attributes than the schema has, is
  //    refused by both, whichever attribute is decoded.
  // 3. A slot that does not point to its varlena, and a varlena
  //    that is a TOAST pointer or compressed, which sm2 cannot read
  //    yet, are refused by deform and by decoding that attribute.
  // 4. A varlena whose header claims a byte too many is refused by
  //    deform; decode, which does not check the slots after it,
  //    reads it as it claims.
  std::vector<sm2::attribute_t> schema = {
      attr(sm2::type_int4), attr(sm2::type_text), attr(sm2::type_int8),
      attr(sm2::type_bytea)};
  std::string good =
      encode(schema, {le(int32_t{7}), vl("abc"), std::nullopt, vl("de")});
  std::vector<std::optional<std::string>> out;
  std::optional<std::string> d;
  // Refused by deform, and by decoding attribute k, if one is named.
  auto refused = [&](std::string_view t, std::optional<size_t> k) {
    CAPTURE(hex(t));
    CHECK(bytes_of(schema, t, out) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
    if (k) {
      CAPTURE(*k);
      CHECK(decode_bytes(schema, t, *k, d) == -1);
      CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
    }
  };
  for (size_t n = 0; n < good.size(); n++) {
    refused(good.substr(0, n), 3);
  }
  refused(good + "!", std::nullopt);
  std::string bad = good;
  bad[0] = static_cast<char>(bad[0] | 0x80);
  for (size_t k = 0; k < schema.size(); k++) {
    refused(bad, k);
  }
  bad = good;
  bad[0] = static_cast<char>(bad[0] | 0x02);
  refused(bad, 0);
  bad = good;
  bad[1] = 5; // natts past the schema's 4
  refused(bad, 0);
  // The fixed part follows the 3-byte header and bitmap: the int4,
  // then the text's slot, 11, where "abc" starts, just past the
  // fixed part's 8 bytes. Point it a byte further on.
  bad = good;
  size_t slot = 3 + 4;
  REQUIRE(bad[slot] == 11);
  bad[slot] = 12;
  refused(bad, 1);
  // The first varlena, "abc" with header 0x09: make it a TOAST
  // pointer, a compressed 4-byte header, and a 1-byte header one
  // byte too long.
  size_t first = 11;
  REQUIRE(good[first] == '\x09');
  for (char h : {sm2::varlena_toast, '\x02'}) {
    bad = good;
    bad[first] = h;
    refused(bad, 1);
  }
  bad = good;
  bad[first] = '\x0b';
  refused(bad, std::nullopt);
  REQUIRE(decode_bytes(schema, bad, 1, d) == 0);
  CHECK(d == std::string_view(bad).substr(first, 5));
}

TEST_CASE("the codec refuses arguments that do not fit the schema") {
  // Verify tup_encode and tup_decode's SM_ERR_INVALID cases.
  // 1. Fewer datums than attributes leaves the tuple as it was.
  // 2. A variable datum with a null pointer, a TOAST pointer, or a
  //    compressed one. (A datum_t has no length, so a datum of the
  //    wrong width, or a varlena longer than its buffer, cannot be
  //    told; the header is trusted, as in PostgreSQL.)
  // 3. A decimal64 attribute with a typmod decimal64 refuses.
  // 4. An ordinal past the schema.
  std::vector<sm2::attribute_t> schema = {attr(sm2::type_int4),
                                          attr(sm2::type_text)};
  std::string tuple = "keep";
  sm2::datum_t one[1] = {sm2::datum_from_int32(1)};
  bool no[2] = {false, false};
  CHECK(sm2::tup_encode(schema, one, std::span<const bool>(no, 1), tuple) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(tuple == "keep");
  // A variable datum must be a varlena this codec can store.
  std::string toast(1, sm2::varlena_toast);
  std::string compressed("\x12\0\0\0", 4);
  const char *bad_ptrs[] = {nullptr, toast.data(), compressed.data()};
  for (const char *v : bad_ptrs) {
    sm2::datum_t row[2] = {sm2::datum_from_int32(1), sm2::datum_from_ptr(v)};
    CHECK(sm2::tup_encode(schema, row, no, tuple) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  }
  CHECK(tuple == "keep");
  std::vector<sm2::attribute_t> bad_typmod = {attr(sm2::type_decimal64, 3)};
  sm2::datum_t d8[1] = {sm2::datum_from_int64(1)};
  CHECK(sm2::tup_encode(bad_typmod, d8, std::span<const bool>(no, 1), tuple) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  std::string good = encode(schema, {le(int32_t{1}), vl("t")});
  std::optional<std::string> d;
  CHECK(decode_bytes(schema, good, 2, d) == -1);
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

TEST_CASE(
    "tuples encode, deform, and decode via PostgreSQL datum_t and isnull") {
  // Verify the datum_t form of the codec writes the same layout
  // and reads back what it wrote, by value and by reference.
  // 1. Encode an int4, a text, a null int8 and a bool, and check
  //    the bytes.
  // 2. Deform it back into Datums and null flags.
  // 3. Decode each attribute on its own.
  // 4. Read an attribute added after the row as its missing
  //    value, by deform and by decode.
  std::vector<sm2::attribute_t> schema = {
      attr(sm2::type_int4), attr(sm2::type_text), attr(sm2::type_int8),
      attr(sm2::type_bool)};
  std::string vl_hi = vl("hi");
  sm2::datum_t datums[4] = {sm2::datum_from_int32(7),
                            sm2::datum_from_ptr(vl_hi.data()), 0,
                            sm2::datum_from_bool(true)};
  bool isnull[4] = {false, false, true, false};
  std::string t;
  REQUIRE(sm2::tup_encode(schema, datums, isnull, t) == 0);
  CHECK(hex(t) == "010404070000000a0001076869");

  // 2. Deform back to datum_t and isnull.
  sm2::datum_t out_d[4];
  bool out_null[4];
  REQUIRE(sm2::tup_deform(schema, t, out_d, out_null) == 0);
  CHECK_FALSE(out_null[0]);
  CHECK(sm2::datum_to_int32(out_d[0]) == 7);
  CHECK_FALSE(out_null[1]);
  const char *text_ptr = reinterpret_cast<const char *>(out_d[1]);
  CHECK(sm2::varlena_payload(std::string_view(text_ptr, 3)) == "hi");
  CHECK(out_null[2]);
  CHECK_FALSE(out_null[3]);
  CHECK(sm2::datum_to_bool(out_d[3]) == true);

  // 3. Decode each attribute individually.
  sm2::datum_t val;
  bool is_null;
  REQUIRE(sm2::tup_decode(schema, t, 0, val, is_null) == 0);
  CHECK_FALSE(is_null);
  CHECK(sm2::datum_to_int32(val) == 7);

  REQUIRE(sm2::tup_decode(schema, t, 1, val, is_null) == 0);
  CHECK_FALSE(is_null);
  CHECK(sm2::varlena_payload(
            std::string_view(reinterpret_cast<const char *>(val), 3)) == "hi");

  REQUIRE(sm2::tup_decode(schema, t, 2, val, is_null) == 0);
  CHECK(is_null);

  REQUIRE(sm2::tup_decode(schema, t, 3, val, is_null) == 0);
  CHECK_FALSE(is_null);
  CHECK(sm2::datum_to_bool(val) == true);

  // 4. Missing attribute decoding.
  std::string missing_val = le(int64_t{9999});
  std::vector<sm2::attribute_t> expanded = schema;
  expanded.push_back({&sm2::type_int8, -1, false, missing_val});

  sm2::datum_t exp_d[5];
  bool exp_null[5];
  REQUIRE(sm2::tup_deform(expanded, t, exp_d, exp_null) == 0);
  CHECK_FALSE(exp_null[4]);
  CHECK(sm2::datum_to_int64(exp_d[4]) == 9999);

  REQUIRE(sm2::tup_decode(expanded, t, 4, val, is_null) == 0);
  CHECK_FALSE(is_null);
  CHECK(sm2::datum_to_int64(val) == 9999);
}

TEST_CASE("tup_check accepts what the codec writes, and nothing else") {
  // Verify tup_check refuses, as SM_ERR_INVALID, any tuple
  // tup_encode could not have written under the schema.
  // 1. An encoded tuple passes.
  // 2. Malformed bytes fail.
  // 3. A tuple with fewer attributes than the schema fails.
  // 4. A non-null value under a dropped attribute fails.
  std::vector<sm2::attribute_t> schema = {attr(sm2::type_int4),
                                          attr(sm2::type_text)};
  std::string vl_hi = vl("hi");
  sm2::datum_t datums[2] = {sm2::datum_from_int32(7),
                            sm2::datum_from_ptr(vl_hi.data())};
  bool isnull[2] = {false, false};
  std::string t;
  REQUIRE(sm2::tup_encode(schema, datums, isnull, t) == 0);
  CHECK(sm2::tup_check(schema, t) == 0);

  CHECK(sm2::tup_check(schema, "garbage") == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
  CHECK(sm2::tup_check(schema, t.substr(0, t.size() - 1)) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);

  std::vector<sm2::attribute_t> wider = schema;
  wider.push_back(attr(sm2::type_int8));
  CHECK(sm2::tup_check(wider, t) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);

  std::vector<sm2::attribute_t> dropped = schema;
  dropped[0].dropped = true;
  CHECK(sm2::tup_check(dropped, t) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

TEST_CASE("a wide row decodes each attribute alone, across bitmap words") {
  // Verify off[] less the widths of earlier nulls finds every entry
  // when the null bitmap spans more than one 64-bit word.
  // 1. Build a 150-attribute schema of int4, text, int8 and bool in
  //    turn, and one tup_desc_t for it, reused for every call.
  // 2. Encode a row with nulls every third attribute and around
  //    the word edges, 63 to 65 and 127 to 129.
  // 3. Decode each attribute alone; each matches the full deform.
  // 4. A desc over a schema with a refused typmod fails every call
  //    with SM_ERR_INVALID.
  const sm2::type_t *types[] = {&sm2::type_int4, &sm2::type_text,
                                &sm2::type_int8, &sm2::type_bool};
  std::vector<sm2::attribute_t> schema;
  for (size_t i = 0; i < 150; i++) {
    schema.push_back(attr(*types[i % 4]));
  }
  sm2::tup_desc_t desc(schema);

  std::vector<std::optional<std::string>> row;
  for (size_t i = 0; i < schema.size(); i++) {
    bool null = i % 3 == 0 || (i >= 63 && i <= 65) || (i >= 127 && i <= 129);
    if (null) {
      row.push_back(std::nullopt);
    } else if (types[i % 4] == &sm2::type_int4) {
      row.push_back(le(static_cast<int32_t>(i)));
    } else if (types[i % 4] == &sm2::type_text) {
      row.push_back(vl("t" + std::to_string(i)));
    } else if (types[i % 4] == &sm2::type_int8) {
      row.push_back(le(static_cast<int64_t>(i) * 1000));
    } else {
      row.push_back(std::string(1, static_cast<char>(i % 2)));
    }
  }
  std::string t = encode(schema, row);
  REQUIRE(deform(schema, t) == row);

  for (size_t k = 0; k < schema.size(); k++) {
    CAPTURE(k);
    std::optional<std::string> d;
    REQUIRE(decode_bytes(desc, t, k, d) == 0);
    CHECK(d.has_value() == row[k].has_value());
    if (d && row[k]) {
      CHECK(*d == *row[k]);
    }
  }

  std::vector<sm2::attribute_t> refused = {attr(sm2::type_decimal64, 3)};
  sm2::tup_desc_t bad(refused);
  std::optional<std::string> d;
  CHECK(decode_bytes(bad, t, 0, d) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_INVALID);
}

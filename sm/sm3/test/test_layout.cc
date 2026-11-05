/*
 * test_layout.cc -- the catalog's keys and blobs (../CATALOG.md §3.3,
 * ../CATALOG.md §3.5).
 */

#include "catalog.h"
#include "doctest.h"

#include <algorithm>
#include <string>
#include <vector>

namespace {

/* A key built by f; it must build. */
template <typename F> std::string key_of(F f) {
  std::string k;
  REQUIRE(f(k) == 0);
  return k;
}

/* A table of three columns and two indexes, every field set. */
sm3::table_rec_t sample_table() {
  sm3::table_rec_t t;
  t.nspoid = 2;
  t.name = "orders";
  t.storeid = 1001;
  t.id_seq = "orders_rowno";
  t.next_relid = 3;
  t.columns = {
      {"id", 36, -1, true, false, std::nullopt, std::nullopt, std::nullopt, 0,
       0, std::nullopt},
      {"note", 64, -1, false, false, sm2::make_varlena("n/a"), 1000,
       std::nullopt, 0, 0, std::nullopt},
      {"gone", 27, -1, false, true, std::nullopt, std::nullopt, std::nullopt, 0,
       0, std::nullopt},
  };
  t.indexes = {
      {1100,
       "orders_pkey",
       1,
       true,
       {{1, false, false, std::nullopt}},
       {},
       {},
       {}},
      {1101,
       "orders_note",
       2,
       false,
       {{2, true, true, 1000}, {1, false, false, std::nullopt}},
       {3},
       {},
       {}},
  };
  return t;
}

} // namespace

TEST_CASE("catalog keys group by kind and order by their columns") {
  // Verify the key layout of ../CATALOG.md §3.3.
  // 1. Every key of a kind begins with that kind's prefix, and no
  //    other kind's; META's key is its kind alone.
  // 2. Entity keys order by oid, and NAME keys by namespace, then
  //    name, bytewise.
  // 3. entity_oid gives back the oid of an entity key, and refuses
  //    a key of another kind or one that is not a key.
  // 4. A table's columns have keys of their own, in attnum order,
  //    and its indexes in oid order, each kind apart from the
  //    tables' keys and from another table's parts; part_ids gives
  //    back the table and the place.
  using sm3::kind_t;
  std::string table_p =
      key_of([](auto &k) { return sm3::kind_prefix(kind_t::table, k); });
  std::string name_p =
      key_of([](auto &k) { return sm3::kind_prefix(kind_t::name, k); });
  std::string t5 =
      key_of([](auto &k) { return sm3::entity_key(kind_t::table, 5, k); });
  std::string t1000 =
      key_of([](auto &k) { return sm3::entity_key(kind_t::table, 1000, k); });
  std::string c5 =
      key_of([](auto &k) { return sm3::entity_key(kind_t::collation, 5, k); });
  CHECK(t5.starts_with(table_p));
  CHECK(t1000.starts_with(table_p));
  CHECK_FALSE(c5.starts_with(table_p));
  CHECK(t5 < t1000);
  CHECK(key_of([](auto &k) { return sm3::meta_key(k); }) ==
        key_of([](auto &k) { return sm3::kind_prefix(kind_t::meta, k); }));

  std::string n2a = key_of([](auto &k) { return sm3::name_key(2, "a", k); });
  std::string n2b = key_of([](auto &k) { return sm3::name_key(2, "b", k); });
  std::string n2ab = key_of([](auto &k) { return sm3::name_key(2, "ab", k); });
  std::string n3a = key_of([](auto &k) { return sm3::name_key(3, "a", k); });
  CHECK(n2a.starts_with(name_p));
  CHECK(n2a < n2ab);
  CHECK(n2ab < n2b);
  CHECK(n2b < n3a);

  uint64_t oid = 0;
  REQUIRE(sm3::entity_oid(kind_t::table, t1000, oid) == 0);
  CHECK(oid == 1000);
  CHECK(sm3::entity_oid(kind_t::table, c5, oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
  CHECK(sm3::entity_oid(kind_t::table, "junk", oid) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);

  auto part = [](kind_t kind, uint64_t table, uint64_t n) {
    return key_of([&](auto &k) { return sm3::part_key(kind, table, n, k); });
  };
  std::string column_p =
      key_of([](auto &k) { return sm3::kind_prefix(kind_t::column, k); });
  std::string index_p =
      key_of([](auto &k) { return sm3::kind_prefix(kind_t::index, k); });
  CHECK(part(kind_t::column, 1000, 0).starts_with(column_p));
  CHECK_FALSE(part(kind_t::column, 1000, 0).starts_with(table_p));
  CHECK(part(kind_t::index, 1000, 1100).starts_with(index_p));
  CHECK(part(kind_t::column, 1000, 0) < part(kind_t::column, 1000, 1));
  CHECK(part(kind_t::column, 1000, 9) < part(kind_t::column, 1000, 10));
  CHECK(part(kind_t::column, 1000, 300) < part(kind_t::column, 1001, 0));
  CHECK(part(kind_t::index, 1000, 1100) < part(kind_t::index, 1000, 1101));
  uint64_t table = 0, n = 0;
  REQUIRE(sm3::part_ids(kind_t::column, part(kind_t::column, 1000, 7), table,
                        n) == 0);
  CHECK(table == 1000);
  CHECK(n == 7);
  CHECK(sm3::part_ids(kind_t::column, part(kind_t::index, 1000, 7), table, n) ==
        -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
  CHECK(sm3::part_ids(kind_t::column, t5, table, n) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
}

TEST_CASE("every catalog entity round trips through its value") {
  // Verify the blob codec of ../CATALOG.md §3.5.
  // 1. META, a NAME value with and without its table, a table's
  //    own fields, each of its columns -- nullable, with a missing
  //    value, dropped -- each of its two indexes, an index's state,
  //    valid and failed with its reason, a namespace and a collation
  //    each decode to what was encoded.
  // 2. A value cut short, or with a byte added, is SM_ERR_CORRUPT.
  std::string v;
  int32_t version = 0;
  REQUIRE(sm3::encode_meta(sm3::format_version, v) == 0);
  REQUIRE(sm3::decode_meta(v, version) == 0);
  CHECK(version == sm3::format_version);

  for (const sm3::name_rec_t &n :
       {sm3::name_rec_t{sm3::kind_t::table, 1000, std::nullopt},
        sm3::name_rec_t{sm3::kind_t::table, 1101, 1000}}) {
    sm3::name_rec_t back{};
    REQUIRE(sm3::encode_name(n, v) == 0);
    REQUIRE(sm3::decode_name(v, back) == 0);
    CHECK(back.kind == n.kind);
    CHECK(back.oid == n.oid);
    CHECK(back.table == n.table);
  }

  // A table's value is its own fields: its parts have their own.
  sm3::table_rec_t t = sample_table(), back;
  std::string blob;
  REQUIRE(sm3::encode_table(t, v) == 0);
  REQUIRE(sm3::decode_table(v, back) == 0);
  CHECK(back.nspoid == t.nspoid);
  CHECK(back.name == t.name);
  CHECK(back.storeid == t.storeid);
  CHECK(back.id_seq == t.id_seq);
  CHECK(back.next_relid == t.next_relid);
  CHECK(back.columns.empty());
  CHECK(back.indexes.empty());
  for (const sm3::column_rec_t &c : t.columns) {
    sm3::column_rec_t c_back;
    REQUIRE(sm3::encode_column(c, v) == 0);
    REQUIRE(sm3::decode_column(v, c_back) == 0);
    CHECK(c_back == c);
  }
  for (const sm3::index_rec_t &x : t.indexes) {
    sm3::index_rec_t x_back;
    REQUIRE(sm3::encode_index(x, blob) == 0);
    REQUIRE(sm3::decode_index(blob, x_back) == 0);
    CHECK(x_back == x);
  }

  for (const sm3::index_status_t &st :
       {sm3::index_status_t{sm3::index_state_t::valid, sm1::SM_OK, {}, {}},
        sm3::index_status_t{
            sm3::index_state_t::failed, sm1::SM_ERR_DUPKEY, "two rows", {}}}) {
    sm3::index_status_t st_back;
    REQUIRE(sm3::encode_index_state(st, v) == 0);
    REQUIRE(sm3::decode_index_state(v, st_back) == 0);
    CHECK(st_back.state == st.state);
    CHECK(st_back.error == st.error);
    CHECK(st_back.reason == st.reason);
  }

  sm3::nspace_rec_t ns{"public"}, ns_back;
  REQUIRE(sm3::encode_nspace(ns, v) == 0);
  REQUIRE(sm3::decode_nspace(v, ns_back) == 0);
  CHECK(ns_back.name == "public");

  sm3::collation_rec_t co{"ci", "und-u-ks-level2", false, "153.80"}, co_back;
  REQUIRE(sm3::encode_collation(co, v) == 0);
  REQUIRE(sm3::decode_collation(v, co_back) == 0);
  CHECK(co_back.name == co.name);
  CHECK(co_back.locale == co.locale);
  CHECK(co_back.deterministic == co.deterministic);
  CHECK(co_back.icu_version == co.icu_version);

  for (size_t n : {size_t{0}, size_t{1}, blob.size() / 2, blob.size() - 1}) {
    CAPTURE(n);
    sm3::index_rec_t x_back;
    CHECK(sm3::decode_index(blob.substr(0, n), x_back) == -1);
    CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
  }
  sm3::index_rec_t x_back;
  CHECK(sm3::decode_index(blob + "x", x_back) == -1);
  CHECK(sm1::errnum() == sm1::SM_ERR_CORRUPT);
}

TEST_CASE("a part written before a field was appended reads it as missing") {
  // Verify that a part evolves as a tuple does (../CATALOG.md §3.5).
  // 1. Write a collation part under its schema.
  // 2. Read it under that schema with one attribute appended, whose
  //    missing value is set: the new attribute reads as that value,
  //    and the old ones as written.
  // 3. With no missing value, the appended attribute reads as null.
  sm3::collation_rec_t co{"c", "en", true, "1.0"};
  std::string blob;
  REQUIRE(sm3::encode_collation(co, blob) == 0);

  std::vector<sm2::attribute_t> v2 = sm3::part_schema(sm3::part_t::collation);
  v2.push_back({&sm2::type_int4, -1, false, std::string("\x2a\0\0\0", 4)});
  sm2::tup_desc_t desc(v2);
  sm3::row_t row;
  size_t pos = 0;
  REQUIRE(sm3::read_part(blob, pos, desc, row) == 0);
  CHECK(pos == blob.size());
  CHECK_FALSE(row.isnull[4]);
  CHECK(sm2::datum_to_int32(row.values[4]) == 42);
  CHECK(sm2::datum_to_bool(row.values[2]));

  v2.back().missing = std::nullopt;
  sm2::tup_desc_t bare(v2);
  pos = 0;
  REQUIRE(sm3::read_part(blob, pos, bare, row) == 0);
  CHECK(row.isnull[4]);
}

/*
 * key.cc -- the catalog's keys (CATALOG.md §2): a kind, then the
 * kind's own columns, encoded ascending by sm2's encode_key, so a
 * kind's keys are contiguous and every key of one kind orders by
 * its columns.
 */

#include "catalog.h"

#include <cstring>
#include <string>

namespace {

/* A key column of type, ascending, nulls last, bytewise. */
sm2::key_column_t col(const sm2::type_t &type) {
  return {&type, -1, nullptr, false, false};
}

/* The columns of each key shape: the kind alone, then more. */
const sm2::key_column_t kind_cols[] = {col(sm2::type_int2)};
const sm2::key_column_t name_cols[] = {col(sm2::type_int2), col(sm2::type_int8),
                                       col(sm2::type_text)};
const sm2::key_column_t entity_cols[] = {col(sm2::type_int2),
                                         col(sm2::type_int8)};

/* Encode datums, none null, under cols into key. */
int encode(std::span<const sm2::key_column_t> cols,
           std::span<const sm2::datum_t> datums, std::string &key) {
  bool isnull[3] = {false, false, false};
  key.clear();
  return sm2::encode_key(cols, datums,
                         std::span<const bool>(isnull, datums.size()), key);
}

/* kind as the datum a key's first column holds. */
sm2::datum_t kind_datum(sm3::kind_t kind) {
  return sm2::datum_from_int16(static_cast<int16_t>(kind));
}

} // namespace

namespace sm3 {

/* META's key: its kind alone. */
int meta_key(std::string &key) { return kind_prefix(kind_t::meta, key); }

/* The key of a name in a namespace. */
int name_key(uint64_t nspoid, std::string_view name, std::string &key) {
  return catch_nomem([&] {
    std::string text = sm2::make_varlena(name);
    sm2::datum_t d[3] = {kind_datum(kind_t::name),
                         sm2::datum_from_int64(static_cast<int64_t>(nspoid)),
                         sm2::datum_from_ptr(text.data())};
    return encode(name_cols, d, key);
  });
}

/* The namespace a NAME key is in. */
int name_nspoid(std::string_view key, uint64_t &nspoid) {
  return catch_nomem([&] {
    std::vector<std::optional<std::string>> cols;
    if (sm2::decode_key(name_cols, key, cols) != 0 || !cols[1]) {
      return sm1::seterr(sm1::SM_ERR_CORRUPT, "catalog: not a name key");
    }
    // The column comes back as its native bytes, little-endian.
    int64_t n;
    std::memcpy(&n, cols[1]->data(), sizeof(n));
    nspoid = static_cast<uint64_t>(n);
    return 0;
  });
}

/* The key of an entity by its oid. */
int entity_key(kind_t kind, uint64_t oid, std::string &key) {
  return catch_nomem([&] {
    sm2::datum_t d[2] = {kind_datum(kind),
                         sm2::datum_from_int64(static_cast<int64_t>(oid))};
    return encode(entity_cols, d, key);
  });
}

/* The prefix of every key of a kind. */
int kind_prefix(kind_t kind, std::string &prefix) {
  return catch_nomem([&] {
    sm2::datum_t d[1] = {kind_datum(kind)};
    return encode(kind_cols, d, prefix);
  });
}

/* The oid an entity key names. */
int entity_oid(kind_t kind, std::string_view key, uint64_t &oid) {
  return catch_nomem([&] {
    std::vector<std::optional<std::string>> cols;
    if (sm2::decode_key(entity_cols, key, cols) != 0 || !cols[0] || !cols[1]) {
      return sm1::seterr(sm1::SM_ERR_CORRUPT, "catalog: not an entity key");
    }
    // Each column comes back as its native bytes, little-endian.
    int16_t k;
    int64_t o;
    std::memcpy(&k, cols[0]->data(), sizeof(k));
    std::memcpy(&o, cols[1]->data(), sizeof(o));
    if (k != static_cast<int16_t>(kind)) {
      return sm1::seterr(sm1::SM_ERR_CORRUPT, "catalog: a key of another kind");
    }
    oid = static_cast<uint64_t>(o);
    return 0;
  });
}

} // namespace sm3

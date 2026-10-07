/*
 * catalog.h -- sm3's catalog as it is kept (CATALOG.md §2,
 * §3): the keys of its entries, and the blobs they hold. Each
 * entry lives in sm2's catalog key-value store; this is the
 * layout only, with no store, no lock and no memory of its own.
 */
#ifndef SM3_CATALOG_H
#define SM3_CATALOG_H

#include "sm2.h"
#include "sm3.h"

#include <cstdint>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sm3 {

/*
 * A catalog key's kind, its first column. The values are frozen:
 * they are on disk.
 */
enum class kind_t : int16_t {
  meta = 1,      /* the catalog's format version */
  name = 2,      /* a name, and what it names */
  nspace = 3,    /* a namespace */
  table = 4,     /* a table, its columns and indexes */
  collation = 5, /* a collation */
  index = 6,     /* an index: only named, kept in its table's blob */
  lock = 7,      /* a table's schema lock: locked, never written */
};

/*
 * Run f, turning an allocation failure into SM_ERR_NOMEM, as sm2's
 * entry points do: no exception leaves an sm3 call.
 */
template <typename F> int catch_nomem(F f) {
  try {
    return f();
  } catch (const std::bad_alloc &) {
    return sm1::seterr(sm1::SM_ERR_NOMEM, "out of memory");
  }
}

/* The catalog's format version, which META holds. */
constexpr int32_t format_version = 1;

/* ---- keys (CATALOG.md §2) ---- */

/* META's key. */
int meta_key(std::string &key);

/* The key of name in the namespace nspoid. */
int name_key(uint64_t nspoid, std::string_view name, std::string &key);

/* The key of the entity oid of kind: a namespace, table or collation. */
int entity_key(kind_t kind, uint64_t oid, std::string &key);

/* The prefix every key of kind begins with, for a scan of them all. */
int kind_prefix(kind_t kind, std::string &prefix);

/*
 * The oid an entity key of kind names; fails with SM_ERR_CORRUPT
 * if key is not one.
 */
int entity_oid(kind_t kind, std::string_view key, uint64_t &oid);

/* ---- records: what the blobs hold (CATALOG.md §3) ---- */

/* What a NAME key names. */
struct name_rec_t {
  kind_t kind;
  uint64_t oid;
  std::optional<uint64_t> table; /* an index's table */
};

/* A column, as its table's blob holds it. */
struct column_rec_t {
  std::string name;
  uint8_t type_code;
  int32_t typmod;
  bool not_null;
  bool dropped;
  std::optional<std::string> missing; /* its bytes; a varlena if variable */
  std::optional<uint64_t> collation;
};

/* One key column of an index; attnum is the column's 0-based place. */
struct index_key_rec_t {
  int32_t attnum;
  bool desc;
  bool nulls_first;
  std::optional<uint64_t> collation;
  uint8_t type_code = 0; /* an expression's, attnum -1 (TYPE.md §3) */
  int32_t typmod = -1;   /* an expression's */
};

/* An index, as its table's blob holds it. */
struct index_rec_t {
  uint64_t oid;
  std::string name;
  uint16_t relid;
  bool unique;
  std::vector<index_key_rec_t> keys;
  std::vector<int32_t> includes; /* 0-based attnums */
  index_state_t state = index_state_t::valid;
  sm1::errnum_t error = sm1::SM_OK; /* why its build failed */
  std::string reason;               /* that error's message */
  std::string eval;                 /* the host's blob; empty: plain */
  std::vector<int32_t> reads;       /* the columns it reads, 0-based */
};

/* A table, its columns and its indexes: one blob. */
struct table_rec_t {
  uint64_t nspoid;
  std::string name;
  uint64_t storeid;
  std::string rowno_seq;
  uint16_t next_relid;
  std::vector<column_rec_t> columns;
  std::vector<index_rec_t> indexes;
};

/* A namespace's blob. */
struct nspace_rec_t {
  std::string name;
};

/* A collation's blob. */
struct collation_rec_t {
  std::string name;
  std::string locale;
  bool deterministic;
  std::string icu_version;
};

/*
 * Encode a record into its value, or decode a value into its
 * record. A decode fails with SM_ERR_CORRUPT on a value that is
 * not one, leaving the record unspecified.
 */
int encode_meta(int32_t version, std::string &value);
int decode_meta(std::string_view value, int32_t &version);
int encode_name(const name_rec_t &rec, std::string &value);
int decode_name(std::string_view value, name_rec_t &rec);
int encode_table(const table_rec_t &rec, std::string &blob);
int decode_table(std::string_view blob, table_rec_t &rec);
int encode_nspace(const nspace_rec_t &rec, std::string &blob);
int decode_nspace(std::string_view blob, nspace_rec_t &rec);
int encode_collation(const collation_rec_t &rec, std::string &blob);
int decode_collation(std::string_view blob, collation_rec_t &rec);

/* ---- parts: a blob's tuples, for the codec and its tests ---- */

/* The layout of each part of a blob (CATALOG.md §3). */
enum class part_t {
  meta,
  name,
  table,
  column,
  index,
  index_key,
  include,
  nspace,
  collation,
};

/* A part's schema, and its layout, which the codec encodes by. */
const std::vector<sm2::attribute_t> &part_schema(part_t part);
const sm2::tup_desc_t &part_desc(part_t part);

/*
 * One part read back: its values and null flags, which view the
 * blob it was read from.
 */
struct row_t {
  std::vector<sm2::datum_t> values;
  std::unique_ptr<bool[]> isnull;
};

/*
 * Read the part at pos in blob under desc, and move pos past it:
 * a varint length, then an sm2 tuple of that length. A part
 * written under an older schema -- fewer attributes -- reads the
 * rest as their missing values. Fails with SM_ERR_CORRUPT.
 */
int read_part(std::string_view blob, size_t &pos, const sm2::tup_desc_t &desc,
              row_t &row);

} // namespace sm3

#endif /* SM3_CATALOG_H */

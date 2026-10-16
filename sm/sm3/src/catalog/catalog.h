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
  table = 4,     /* a table: its own fields */
  collation = 5, /* a collation */
  index = 6,     /* an index, keyed by its table's oid and its own */
  /* 7 was a table's oid lock, now an sm1 LOCKTYPE_OID lock
     and no key at all; the value stays retired. */
  column = 8,      /* a column, keyed by its table's oid and its attnum */
  index_state = 9, /* an index's state, keyed as its index is */
  stats = 10,      /* a table's statistics (SM3_DESIGN.md §6.6) */
  sequence = 11,   /* a sequence: its namespace, name and counter */
  constraint = 12, /* a constraint, keyed by its table's oid and its own */
};

/* The catalog's format version, which META holds. */
constexpr int32_t format_version = 4;

/* ---- keys (CATALOG.md §2) ---- */

/* META's key. */
int meta_key(std::string &key);

/* The key of name in the namespace nspoid. */
int name_key(uint64_t nspoid, std::string_view name, std::string &key);

/* The key of the entity oid of kind: a namespace, table or collation. */
int entity_key(kind_t kind, uint64_t oid, std::string &key);

/*
 * The key of a part of the table table_oid: a column, with n its
 * attnum, or an index, with n its oid. A table's parts of one kind
 * are together and in the order of n.
 */
int part_key(kind_t kind, uint64_t table_oid, uint64_t n, std::string &key);

/*
 * The table and the n a part key of kind names; fails with
 * SM_ERR_CORRUPT if key is not one.
 */
int part_ids(kind_t kind, std::string_view key, uint64_t &table_oid,
             uint64_t &n);

/* The namespace the NAME key is in. */
int name_nspoid(std::string_view key, uint64_t &nspoid);

/*
 * The key of one of the table table_oid's statistics entries
 * (SM3_DESIGN.md §6.6): (0, 0) its own, the table's counts and
 * sizes; (0, attnum + 1) a column's; (index oid, place + 1) an
 * index expression's. A table's entries sort in that order.
 */
int stats_key(uint64_t table_oid, uint64_t index_oid, uint64_t n,
              std::string &key);

/* The prefix of every statistics entry of the table table_oid. */
int stats_prefix(uint64_t table_oid, std::string &prefix);

/* The prefix of every NAME key in the namespace nspoid. */
int name_prefix(uint64_t nspoid, std::string &prefix);

/* The name a NAME key holds. */
int name_of_key(std::string_view key, std::string &name);

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

/* A namespace's blob. */
struct nspace_rec_t {
  std::string name;
};

/* A sequence's blob: where it is, and how its counter was made. */
struct sequence_rec_t {
  uint64_t nspoid;
  std::string name;
  uint64_t start;
  uint64_t increment;
  /* The table whose column owns it, OWNED BY, and the column's place. */
  std::optional<uint64_t> owner;
  int32_t owner_attnum = -1;
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
/* A table's value holds its own fields; its columns and indexes
   have a key and a value each. */
int encode_table(const table_rec_t &rec, std::string &blob);
int decode_table(std::string_view blob, table_rec_t &rec);
int encode_column(const column_rec_t &rec, std::string &blob);
int decode_column(std::string_view blob, column_rec_t &rec);
int encode_index(const index_rec_t &rec, std::string &blob);
int decode_index(std::string_view blob, index_rec_t &rec);
int encode_index_state(const index_status_t &rec, std::string &blob);
int decode_index_state(std::string_view blob, index_status_t &rec);
int encode_nspace(const nspace_rec_t &rec, std::string &blob);
int decode_nspace(std::string_view blob, nspace_rec_t &rec);
int encode_collation(const collation_rec_t &rec, std::string &blob);
int decode_collation(std::string_view blob, collation_rec_t &rec);
int encode_sequence(const sequence_rec_t &rec, std::string &blob);
int decode_sequence(std::string_view blob, sequence_rec_t &rec);
/* A constraint's value: a foreign key's (FOREIGNKEY.md §3). */
int encode_constraint(const fkey_rec_t &rec, std::string &blob);
int decode_constraint(std::string_view blob, fkey_rec_t &rec);

/*
 * A table's statistics, as its STATS entries hold them
 * (SM3_DESIGN.md §6.6): the table's own entry, its counts, sizes
 * and indexes, with stats left empty; and one statistic's entry,
 * typed by what it describes, a column or an index expression of
 * def. Encoding a statistic fails with SM_ERR_INVALID if def has no
 * such one; decoding the table's leaves out an index def no longer
 * has, and decoding a statistic sets kept to false for one whose
 * column is dropped or whose index is gone.
 */
int encode_stats_table(const stats_t &stats, std::string &blob);
int decode_stats_table(std::string_view blob, const table_def_t &def,
                       stats_t &stats);
int encode_stat(const stat_t &stat, const table_def_t &def, std::string &blob);
int decode_stat(std::string_view blob, const table_def_t &def, stat_t &stat,
                bool &kept);

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
  index_state,
  sequence,
  stats_table,
  stats_index,
  statistic,
  constraint,
  fkey,
  fkey_pair,
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

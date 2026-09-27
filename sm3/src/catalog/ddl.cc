/*
 * ddl.cc -- changes to the catalog (CATALOG.md §5): cat_init,
 * and so far cat_create_table. Each writes its entries through
 * sm2's catalog key-value store, in the session's transaction, so
 * it commits and rolls back with the transaction's other writes.
 */

#include "catalog.h"
#include "sm3.h"

#include <set>
#include <string>

namespace {

using kv_ptr = std::unique_ptr<sm2::catkv_t, decltype(&sm2::catkv_close)>;

/* The sequence oids come from; they start at 1000 (CATALOG.md §1). */
constexpr const char *oid_seq = "sm3.oid";
constexpr uint64_t first_oid = 1000;

/*
 * Names of namespaces live in namespace 0, which is no namespace:
 * every name is in one, and a namespace's is in this.
 */
constexpr uint64_t nsp_of_namespaces = 0;

/* Open session's catalog store into kv. */
int open_kv(sm1::session_t *session, kv_ptr &kv) {
  sm2::catkv_t *raw = nullptr;
  if (sm2::catkv_open(session, raw) != 0) {
    return -1;
  }
  kv.reset(raw);
  return 0;
}

/* Insert name's NAME entry in nspoid, saying it names rec. */
int insert_name(sm2::catkv_t *kv, uint64_t nspoid, std::string_view name,
                const sm3::name_rec_t &rec) {
  std::string key, value;
  if (sm3::name_key(nspoid, name, key) != 0 ||
      sm3::encode_name(rec, value) != 0) {
    return -1;
  }
  return sm2::catkv_insert(kv, key, value);
}

/* Insert the entity oid of kind, holding blob. */
int insert_entity(sm2::catkv_t *kv, sm3::kind_t kind, uint64_t oid,
                  const std::string &blob) {
  std::string key;
  if (sm3::entity_key(kind, oid, key) != 0) {
    return -1;
  }
  return sm2::catkv_insert(kv, key, blob);
}

/* Refuse columns the table cannot be laid out by. */
int check_columns(std::span<const sm3::column_spec_t> columns) {
  std::set<std::string> seen;
  for (const sm3::column_spec_t &c : columns) {
    const sm2::type_t *type = sm2::type_by_code(c.type_code);
    if (c.name.empty() || !seen.insert(c.name).second) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "create table: a column without a name, or two "
                         "with one: " +
                             c.name);
    }
    if (!type || sm2::datum_width(type, c.typmod) == 0) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "create table: column " + c.name +
                             " has a type code sm2 lacks, or a typmod its "
                             "type refuses");
    }
  }
  return 0;
}

} // namespace

namespace sm3 {

/* Make a new database's catalog; see sm3.h. */
int cat_init(sm1::session_t *session) {
  return catch_nomem([&] {
    kv_ptr kv(nullptr, &sm2::catkv_close);
    std::string key, value;
    std::string_view v;
    if (open_kv(session, kv) != 0 || meta_key(key) != 0) {
      return -1;
    }
    // A database is initialized once.
    int rc = sm2::catkv_get(kv.get(), key, v);
    if (rc != 1) {
      return rc == 0 ? sm1::seterr(sm1::SM_ERR_EXISTS,
                                   "catalog: the database has one already")
                     : -1;
    }
    if (encode_meta(format_version, value) != 0 ||
        sm2::catkv_insert(kv.get(), key, value) != 0) {
      return -1;
    }
    // Oids for everything made from now on.
    if (sm1::mkseq(session, oid_seq, first_oid, 1) != 0) {
      return -1;
    }
    // The built-in namespaces, at their fixed oids.
    for (auto [oid, name] :
         {std::pair<uint64_t, const char *>{nsp_pg_catalog, "pg_catalog"},
          {nsp_public, "public"}}) {
      std::string blob;
      if (insert_name(kv.get(), nsp_of_namespaces, name,
                      {kind_t::nspace, oid, std::nullopt}) != 0 ||
          encode_nspace({name}, blob) != 0 ||
          insert_entity(kv.get(), kind_t::nspace, oid, blob) != 0) {
        return -1;
      }
    }
    return 0;
  });
}

/* Create a table; see sm3.h. */
int cat_create_table(sm1::session_t *session, uint64_t nspoid,
                     std::string_view name,
                     std::span<const column_spec_t> columns, uint64_t &oid) {
  return catch_nomem([&] {
    if (name.empty()) {
      return sm1::seterr(sm1::SM_ERR_INVALID, "create table: no name");
    }
    if (check_columns(columns) != 0) {
      return -1;
    }
    kv_ptr kv(nullptr, &sm2::catkv_close);
    std::string key, blob;
    std::string_view v;
    if (open_kv(session, kv) != 0 ||
        entity_key(kind_t::nspace, nspoid, key) != 0) {
      return -1;
    }
    int rc = sm2::catkv_get(kv.get(), key, v);
    if (rc != 0) {
      return rc == 1 ? sm1::seterr(sm1::SM_ERR_NOTFOUND,
                                   "create table: no namespace " +
                                       std::to_string(nspoid))
                     : -1;
    }
    // The oid first: the name's entry says what it names.
    uint64_t t;
    if (sm1::nextseq(session, oid_seq, t) != 0) {
      return -1;
    }
    // The name next, so a taken one fails before a store is made.
    if (insert_name(kv.get(), nspoid, name, {kind_t::table, t, std::nullopt}) !=
        0) {
      return -1;
    }
    table_rec_t rec{
        nspoid, std::string(name), 0, "sm3.rowno." + std::to_string(t), 1, {},
        {}};
    // The table's store and its rowno sequence (DESIGN.md §5).
    if (sm1::mkstore(session, rec.storeid) != 0 ||
        sm1::mkseq(session, rec.rowno_seq, 1, 1) != 0) {
      return -1;
    }
    for (const column_spec_t &c : columns) {
      rec.columns.push_back({c.name, c.type_code, c.typmod, c.not_null, false,
                             std::nullopt, std::nullopt});
    }
    if (encode_table(rec, blob) != 0 ||
        insert_entity(kv.get(), kind_t::table, t, blob) != 0) {
      return -1;
    }
    oid = t;
    return 0;
  });
}

} // namespace sm3

/*
 * ddl.cc -- the catalog's writes (CATALOG.md §5): cat_init, and
 * the puts and dels of its records. Each writes its entries
 * through sm2's catalog key-value store, in the session's
 * transaction, so it commits and rolls back with the transaction's
 * other writes. What DDL does besides -- stores, sequences, index
 * entries -- is not here (ddl/ddl.cc).
 */

#include "internal.h"

#include <algorithm>
#include <map>
#include <string>

namespace {

using kv_ptr = std::unique_ptr<sm2::catkv_t, decltype(&sm2::catkv_close)>;

/* The sequence oids come from; they start at first_oid (CATALOG.md §1). */
constexpr const char *oid_seq = "sm3.oid";

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

/* Fail with SM_ERR_INVALID for a DDL call's argument. */
int invalid(const std::string &why) {
  return sm1::seterr(sm1::SM_ERR_INVALID, why);
}

/* Fail with SM_ERR_DUPKEY: name is another object's. */
int taken(const std::string &name) {
  return sm1::seterr(sm1::SM_ERR_DUPKEY, "the name " + name + " is taken");
}

/*
 * Take the lock of the namespace nspoid in mode -- its oid's, as a
 * table's oid lock is its table's -- and check it is there. A
 * creator takes it shared and a drop exclusive, so a drop waits
 * out the creators and they it; and the check reads the catalog
 * in memory, which has every commit by then, where the
 * transaction's snapshot may not (CATALOG.md §5). name is set to
 * the namespace's.
 */
int use_nspace(sm3::session_t *session, uint64_t nspoid, sm1::lockmode_t mode,
               std::string &name) {
  // No namespace: a temporary table's, which never comes this way
  // (put_temp_table), and nothing else is temporary yet.
  if (nspoid == sm3::nsp_temp) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND, "namespace 0 does not exist");
  }
  // Held to the end of the transaction, as a table-oid lock is.
  if (sm3::lock_table(session->sm1, nspoid, mode) != 0) {
    return -1;
  }
  return sm3::cat_get_namespace_name(session, nspoid, name)
             ? 0
             : sm1::seterr(sm1::SM_ERR_NOTFOUND, "namespace " +
                                                     std::to_string(nspoid) +
                                                     " does not exist");
}

/*
 * Take the table oid's table-oid lock X and set rec to its record as
 * session sees it. The record is read from the catalog in memory:
 * under the lock no other DDL can change it, and memory has every
 * commit by the time the lock is granted, where the transaction's
 * snapshot may be older (CATALOG.md §5).
 */
int latest_table(sm3::session_t *session, uint64_t oid, sm3::table_rec_t &rec) {
  std::shared_ptr<const sm3::table_def_t> d = sm3::cat_get_table(session, oid);
  // A temporary table is the session's alone: it has no users to
  // wait out, and its record is the one the session sees.
  if (d && d->nspoid == sm3::nsp_temp) {
    rec = d->rec;
    return 0;
  }
  // Held to the end of the transaction: DDL waits out its users.
  if (sm3::lock_table(session->sm1, oid, sm1::SM_LOCKMODE_EX) != 0) {
    return -1;
  }
  d = sm3::cat_get_table(session, oid);
  if (!d) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "table " + std::to_string(oid) + " does not exist");
  }
  rec = d->rec;
  return 0;
}

/*
 * Read key under an X lock on it, for a write to follow: the
 * newest committed version, which at READ COMMITTED is a
 * read_latest that also lets this transaction overwrite a version
 * newer than its snapshot; at REPEATABLE READ the lock has already
 * failed on one. Returns as catkv_get does.
 */
int latest_key(sm3::session_t *session, sm2::catkv_t *kv,
               const std::string &key) {
  std::string_view v;
  if (sm2::catkv_lock(kv, key, sm1::SM_LOCKMODE_EX) != 0) {
    return -1;
  }
  return session->level == sm1::xctlevel_t::read_committed
             ? sm2::catkv_get_latest(kv, key, v)
             : sm2::catkv_get(kv, key, v);
}

/* Write key's value, whether the key is there or not. */
int put_key(sm3::session_t *session, sm2::catkv_t *kv, const std::string &key,
            std::string_view value) {
  int rc = latest_key(session, kv, key);
  if (rc < 0) {
    return -1;
  }
  return rc == 0 ? sm2::catkv_update(kv, key, value)
                 : sm2::catkv_insert(kv, key, value);
}

/* Erase key, which must be there. */
int del_key(sm3::session_t *session, sm2::catkv_t *kv, const std::string &key) {
  int rc = latest_key(session, kv, key);
  if (rc != 0) {
    return rc < 0 ? -1
                  : sm1::seterr(sm1::SM_ERR_CORRUPT,
                                "catalog: an entry to erase is not there");
  }
  return sm2::catkv_erase(kv, key);
}

/* Write the table oid's own fields from rec. */
int put_table_fields(sm3::session_t *session, sm2::catkv_t *kv, uint64_t oid,
                     const sm3::table_rec_t &rec) {
  std::string key, blob;
  if (sm3::entity_key(sm3::kind_t::table, oid, key) != 0 ||
      sm3::encode_table(rec, blob) != 0) {
    return -1;
  }
  return put_key(session, kv, key, blob);
}

/* Write column attnum of the table oid. */
int put_column(sm3::session_t *session, sm2::catkv_t *kv, uint64_t oid,
               size_t attnum, const sm3::column_rec_t &c) {
  std::string key, blob;
  if (sm3::part_key(sm3::kind_t::column, oid, attnum, key) != 0 ||
      sm3::encode_column(c, blob) != 0) {
    return -1;
  }
  return put_key(session, kv, key, blob);
}

/* Write the index x of the table oid. */
int put_index(sm3::session_t *session, sm2::catkv_t *kv, uint64_t oid,
              const sm3::index_rec_t &x) {
  std::string key, blob;
  if (sm3::part_key(sm3::kind_t::index, oid, x.oid, key) != 0 ||
      sm3::encode_index(x, blob) != 0) {
    return -1;
  }
  return put_key(session, kv, key, blob);
}

/* Write the foreign key fk of the table oid, its child. */
int put_fkey(sm3::session_t *session, sm2::catkv_t *kv, uint64_t oid,
             const sm3::fkey_rec_t &fk) {
  std::string key, blob;
  if (sm3::part_key(sm3::kind_t::constraint, oid, fk.oid, key) != 0 ||
      sm3::encode_constraint(fk, blob) != 0) {
    return -1;
  }
  return put_key(session, kv, key, blob);
}

/*
 * Erase the state of the index x of the table oid, if it has one:
 * its entry, and in session's view. A pending index has none.
 */
int del_index_state(sm3::session_t *session, sm2::catkv_t *kv, uint64_t oid,
                    uint64_t x) {
  std::string key;
  if (sm3::part_key(sm3::kind_t::index_state, oid, x, key) != 0) {
    return -1;
  }
  int rc = latest_key(session, kv, key);
  if (rc < 0 || (rc == 0 && sm2::catkv_erase(kv, key) != 0)) {
    return -1;
  }
  sm3::overlay_state(session, x, std::nullopt);
  return 0;
}

/* Erase the part n of kind of the table oid. */
int del_part(sm3::session_t *session, sm2::catkv_t *kv, sm3::kind_t kind,
             uint64_t oid, uint64_t n) {
  std::string key;
  if (sm3::part_key(kind, oid, n, key) != 0) {
    return -1;
  }
  return del_key(session, kv, key);
}

/* Erase name's NAME entry in nspoid. */
int erase_name(sm2::catkv_t *kv, uint64_t nspoid, std::string_view name) {
  std::string key;
  if (sm3::name_key(nspoid, name, key) != 0) {
    return -1;
  }
  return sm2::catkv_erase(kv, key);
}

/* Erase key, if it is there, as of the newest commit and under its lock X. */
int erase_if_there(sm3::session_t *session, sm2::catkv_t *kv,
                   const std::string &key) {
  int rc = latest_key(session, kv, key);
  if (rc < 0) {
    return -1;
  }
  return rc == 0 ? sm2::catkv_erase(kv, key) : 0;
}

/*
 * Every statistics key the table oid of rec may have: its own, each
 * column's, dropped or not, and each index expression's.
 */
int stats_keys(uint64_t oid, const sm3::table_rec_t &rec,
               std::vector<std::string> &keys) {
  keys.clear();
  std::string k;
  if (sm3::stats_key(oid, 0, 0, k) != 0) {
    return -1;
  }
  keys.push_back(k);
  for (size_t i = 0; i < rec.columns.size(); i++) {
    if (sm3::stats_key(oid, 0, i + 1, k) != 0) {
      return -1;
    }
    keys.push_back(k);
  }
  for (const sm3::index_rec_t &x : rec.indexes) {
    for (size_t p = 0; p < x.keys.size(); p++) {
      if (x.keys[p].attnum < 0) {
        if (sm3::stats_key(oid, x.oid, p + 1, k) != 0) {
          return -1;
        }
        keys.push_back(k);
      }
    }
  }
  return 0;
}

/*
 * Build the definition rec describes, as s sees its collations, and
 * record it in s's overlay as the table oid's.
 */
int record_table(sm3::session_t *s, uint64_t oid, const sm3::table_rec_t &rec,
                 bool temp = false) {
  std::shared_ptr<const sm3::table_def_t> def;
  sm3::collation_lookup_t lookup = [s](uint64_t c,
                                       const sm2::collation_t *&out) {
    return sm3::session_collation(s, c, out);
  };
  if (sm3::build_table(lookup, oid, rec, def) != 0) {
    return -1;
  }
  sm3::overlay_table(s, oid, std::move(def), temp);
  return 0;
}

/*
 * cat_put_table for a temporary table (TEMPTABLE.md §4): the same
 * checks, made against the session's own temporary tables, and the
 * record kept in its overlay alone. Nothing is written to the
 * catalog's store and nothing is locked. names is what rec takes.
 */
int put_temp_table(sm3::session_t *session, uint64_t oid,
                   const sm3::table_rec_t &rec,
                   const std::map<std::string, sm3::name_rec_t> &names) {
  // A name is taken once among the session's temporary tables, their
  // indexes and its temporary views; the table's own are its to keep
  // or change.
  for (const auto &[name, what] : names) {
    if (sm3::temp_name_taken(session, name, oid)) {
      return taken(name);
    }
  }
  std::shared_ptr<const sm3::table_def_t> old =
      sm3::cat_get_table(session, oid);
  if (!old) {
    if (record_table(session, oid, rec, true) != 0) {
      return -1;
    }
    sm3::overlay_name(session, rec.nspoid, rec.name, oid);
    return 0;
  }
  if (rec.columns.size() < old->rec.columns.size()) {
    return invalid("put table: a column keeps its place, dropped or not");
  }
  if (old->nspoid != rec.nspoid) {
    return invalid("put table: a table does not change its namespace");
  }
  // An index no longer in the record goes, and its state with it.
  for (const sm3::index_def_t &x : old->indexes) {
    bool kept = false;
    for (const sm3::index_rec_t &i : rec.indexes) {
      kept |= i.oid == x.oid;
    }
    if (!kept) {
      sm3::overlay_state(session, x.oid, std::nullopt, true);
    }
  }
  if (record_table(session, oid, rec, true) != 0) {
    return -1;
  }
  if (old->name != rec.name) {
    sm3::overlay_name(session, rec.nspoid, old->name, std::nullopt);
    sm3::overlay_name(session, rec.nspoid, rec.name, oid);
  }
  return 0;
}

} // namespace

namespace sm3 {

/* Make a new database's catalog; see sm3.h. */
int cat_init(sm1::env_t *env, std::string_view dbname,
             const sm1::options_t *opt) {
  return in_own_txn(env, dbname, opt, [&](sm1::session_t *session) {
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

/* Take the next oid; see sm3.h. */
int cat_new_oid(session_t *session, uint64_t &oid) {
  uint64_t next = 0;
  if (sm1::nextseq(session->sm1, oid_seq, next) != 0) {
    return -1;
  }
  // The sequence goes on past last_oid; the oids do not.
  if (next > last_oid) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "the oids are used up: " + std::to_string(next) +
                           " is past " + std::to_string(last_oid));
  }
  oid = next;
  return 0;
}

/* Store a new namespace; see sm3.h. */
int cat_put_namespace(session_t *session, uint64_t oid, std::string_view name) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string blob;
  // The name first, so a taken one fails before the namespace
  // is written.
  if (open_kv(session->sm1, kv) != 0 ||
      insert_name(kv.get(), nsp_of_namespaces, name,
                  {kind_t::nspace, oid, std::nullopt}) != 0 ||
      encode_nspace({std::string(name)}, blob) != 0 ||
      insert_entity(kv.get(), kind_t::nspace, oid, blob) != 0) {
    return -1;
  }
  overlay_nspace(session, oid, name);
  return 0;
}

/* Erase a namespace; see sm3.h. */
int cat_del_namespace(session_t *session, uint64_t oid) {
  if (oid == nsp_pg_catalog || oid == nsp_public) {
    return invalid("drop namespace: a built-in namespace cannot be dropped");
  }
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string name, key;
  std::string_view v;
  // Exclusive: this waits out every transaction creating in it.
  if (open_kv(session->sm1, kv) != 0 ||
      use_nspace(session, oid, sm1::SM_LOCKMODE_EX, name) != 0) {
    return -1;
  }
  if (nspace_in_use(session, oid)) {
    return sm1::seterr(sm1::SM_ERR_INUSE,
                       "drop namespace: " + name + " is not empty");
  }
  // Its record under an X lock on its key, as a table's is read:
  // the newest committed one, which this may then erase.
  if (entity_key(kind_t::nspace, oid, key) != 0 ||
      sm2::catkv_lock(kv.get(), key, sm1::SM_LOCKMODE_EX) != 0) {
    return -1;
  }
  int rc = session->level == sm1::xctlevel_t::read_committed
               ? sm2::catkv_get_latest(kv.get(), key, v)
               : sm2::catkv_get(kv.get(), key, v);
  if (rc != 0) {
    return rc == 1 ? sm1::seterr(sm1::SM_ERR_NOTFOUND, "namespace " +
                                                           std::to_string(oid) +
                                                           " does not exist")
                   : -1;
  }
  if (sm2::catkv_erase(kv.get(), key) != 0 ||
      erase_name(kv.get(), nsp_of_namespaces, name) != 0) {
    return -1;
  }
  overlay_nspace(session, oid, std::nullopt);
  return 0;
}

/* Read a table's record to change it; see sm3.h. */
int cat_get_table_rec(session_t *session, uint64_t oid, table_rec_t &rec) {
  return latest_table(session, oid, rec);
}

/* Store a table's record, new or changed; see sm3.h. */
int cat_put_table(session_t *session, uint64_t oid, const table_rec_t &rec) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string blob;
  // The names rec takes, and what each names.
  std::map<std::string, name_rec_t> names;
  names[rec.name] = {kind_t::table, oid, std::nullopt};
  for (size_t i = 1; i < rec.indexes.size(); i++) {
    // The order they are stored in, and so the order they load in.
    if (rec.indexes[i - 1].oid >= rec.indexes[i].oid) {
      return invalid("put table: indexes out of the order of their oids");
    }
  }
  for (const index_rec_t &x : rec.indexes) {
    // Two of its own objects cannot share one.
    if (!names.emplace(x.name, name_rec_t{kind_t::index, x.oid, oid}).second) {
      return taken(x.name);
    }
  }
  // A temporary table is kept in the session, not in the store.
  if (rec.nspoid == nsp_temp) {
    return put_temp_table(session, oid, rec, names);
  }
  if (!cat_get_table(session, oid)) {
    // A new table. Its namespace must be there, and is held
    // shared, so that it is not dropped under this.
    if (open_kv(session->sm1, kv) != 0 ||
        use_nspace(session, rec.nspoid, sm1::SM_LOCKMODE_SH, blob) != 0) {
      return -1;
    }
    // The names first, so a taken one fails before the record
    // is written.
    for (const auto &[name, what] : names) {
      if (insert_name(kv.get(), rec.nspoid, name, what) != 0) {
        return -1;
      }
    }
    // Its own fields, then a key for each column and each index.
    if (put_table_fields(session, kv.get(), oid, rec) != 0) {
      return -1;
    }
    for (size_t i = 0; i < rec.columns.size(); i++) {
      if (put_column(session, kv.get(), oid, i, rec.columns[i]) != 0) {
        return -1;
      }
    }
    for (const index_rec_t &x : rec.indexes) {
      if (put_index(session, kv.get(), oid, x) != 0) {
        return -1;
      }
    }
    for (const fkey_rec_t &fk : rec.fkeys) {
      if (put_fkey(session, kv.get(), oid, fk) != 0) {
        return -1;
      }
    }
    if (record_table(session, oid, rec) != 0) {
      return -1;
    }
    overlay_name(session, rec.nspoid, rec.name, oid);
    return 0;
  }
  table_rec_t old;
  if (open_kv(session->sm1, kv) != 0 || latest_table(session, oid, old) != 0) {
    return -1;
  }
  if (rec.columns.size() < old.columns.size()) {
    return invalid("put table: a column keeps its place, dropped or not");
  }
  if (old.nspoid != rec.nspoid) {
    return invalid("put table: a table does not change its namespace");
  }
  std::map<std::string, name_rec_t> had;
  had[old.name] = {kind_t::table, oid, std::nullopt};
  for (const index_rec_t &x : old.indexes) {
    had[x.name] = {kind_t::index, x.oid, oid};
  }
  // The new names first, so a taken one leaves the old in place.
  for (const auto &[name, what] : names) {
    auto h = had.find(name);
    if (h != had.end()) {
      // Kept, unless it now names another of the table's objects.
      if (h->second.oid != what.oid) {
        return taken(name);
      }
      continue;
    }
    if (insert_name(kv.get(), rec.nspoid, name, what) != 0) {
      return -1;
    }
  }
  for (const auto &[name, what] : had) {
    if (!names.count(name) && erase_name(kv.get(), rec.nspoid, name) != 0) {
      return -1;
    }
  }
  // Only what changed is written: the table's own fields, a
  // column, an index.
  if ((old.name != rec.name || old.storeid != rec.storeid ||
       old.rowno_seq != rec.rowno_seq || old.next_relid != rec.next_relid) &&
      put_table_fields(session, kv.get(), oid, rec) != 0) {
    return -1;
  }
  for (size_t i = 0; i < rec.columns.size(); i++) {
    if ((i >= old.columns.size() || !(rec.columns[i] == old.columns[i])) &&
        put_column(session, kv.get(), oid, i, rec.columns[i]) != 0) {
      return -1;
    }
  }
  auto index_of = [](const table_rec_t &r, uint64_t x) {
    return std::find_if(r.indexes.begin(), r.indexes.end(),
                        [x](const index_rec_t &i) { return i.oid == x; });
  };
  for (const index_rec_t &x : rec.indexes) {
    auto was = index_of(old, x.oid);
    if ((was == old.indexes.end() || !(x == *was)) &&
        put_index(session, kv.get(), oid, x) != 0) {
      return -1;
    }
  }
  for (const index_rec_t &x : old.indexes) {
    // An index no longer in the record goes, and its state with it.
    if (index_of(rec, x.oid) == rec.indexes.end() &&
        (del_part(session, kv.get(), kind_t::index, oid, x.oid) != 0 ||
         del_index_state(session, kv.get(), oid, x.oid) != 0)) {
      return -1;
    }
    // And its expressions' statistics, if it was analyzed.
    for (size_t p = 0;
         index_of(rec, x.oid) == rec.indexes.end() && p < x.keys.size(); p++) {
      std::string k;
      if (x.keys[p].attnum < 0 && (stats_key(oid, x.oid, p + 1, k) != 0 ||
                                   erase_if_there(session, kv.get(), k) != 0)) {
        return -1;
      }
    }
  }
  // A foreign key new or changed is written, and one gone erased;
  // referenced_by is derived, and written nowhere.
  auto fkey_of = [](const table_rec_t &r, uint64_t f) {
    return std::find_if(r.fkeys.begin(), r.fkeys.end(),
                        [f](const fkey_rec_t &k) { return k.oid == f; });
  };
  for (const fkey_rec_t &fk : rec.fkeys) {
    auto was = fkey_of(old, fk.oid);
    if ((was == old.fkeys.end() || !(fk == *was)) &&
        put_fkey(session, kv.get(), oid, fk) != 0) {
      return -1;
    }
  }
  for (const fkey_rec_t &fk : old.fkeys) {
    if (fkey_of(rec, fk.oid) == rec.fkeys.end() &&
        del_part(session, kv.get(), kind_t::constraint, oid, fk.oid) != 0) {
      return -1;
    }
  }
  // A column dropped now takes its statistics with it.
  for (size_t i = 0; i < old.columns.size(); i++) {
    std::string k;
    if (!old.columns[i].dropped && rec.columns[i].dropped &&
        (stats_key(oid, 0, i + 1, k) != 0 ||
         erase_if_there(session, kv.get(), k) != 0)) {
      return -1;
    }
  }
  if (record_table(session, oid, rec) != 0) {
    return -1;
  }
  if (old.name != rec.name) {
    overlay_name(session, rec.nspoid, old.name, std::nullopt);
    overlay_name(session, rec.nspoid, rec.name, oid);
  }
  return 0;
}

/* Store an index's state, under the table's lock IX; see sm3.h. */
int cat_put_index_state(session_t *session, uint64_t table_oid,
                        uint64_t index_oid, const index_status_t &status) {
  // As a writer of the table takes it: readers and writers go
  // on, and DDL on the table is kept out until this commits.
  std::shared_ptr<const table_def_t> d;
  if (cat_lock_table(session, table_oid, sm1::SM_LOCKMODE_IX, d) != 0) {
    return -1;
  }
  bool there = false;
  for (const index_def_t &x : d->indexes) {
    there |= x.oid == index_oid;
  }
  if (!there) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "index state: no index " + std::to_string(index_oid));
  }
  // A state only ever leaves pending: that is what makes one seen
  // late safe (SM3_DESIGN.md §3.3). Setting a pending index
  // pending changes nothing.
  index_state_t now = cat_get_index_state(session, index_oid).state;
  if (now != index_state_t::pending) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index state: index " + std::to_string(index_oid) +
                           " is no longer pending, and its state is set "
                           "once");
  }
  if (status.state == index_state_t::pending) {
    return 0;
  }
  // Only a failed index keeps why.
  index_status_t st{status.state, sm1::SM_OK, {}, {}};
  if (status.state == index_state_t::failed) {
    st.error = status.error;
    st.reason = status.reason;
    st.dup = status.dup;
  }
  // A temporary table's index: its state is the session's to keep.
  if (d->nspoid == nsp_temp) {
    overlay_state(session, index_oid, std::move(st), true);
    return 0;
  }
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string key, blob;
  if (open_kv(session->sm1, kv) != 0 ||
      part_key(kind_t::index_state, table_oid, index_oid, key) != 0) {
    return -1;
  }
  // Under the key's lock, so that of two transactions setting one
  // index's state the second finds the first's and is refused.
  int rc = latest_key(session, kv.get(), key);
  if (rc < 0) {
    return -1;
  }
  if (rc == 0) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "index state: index " + std::to_string(index_oid) +
                           " is no longer pending, and its state is set "
                           "once");
  }
  if (encode_index_state(st, blob) != 0 ||
      sm2::catkv_insert(kv.get(), key, blob) != 0) {
    return -1;
  }
  overlay_state(session, index_oid, std::move(st));
  return 0;
}

/* Erase a table's record and its names; see sm3.h. */
int cat_del_table(session_t *session, uint64_t oid) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  table_rec_t rec;
  std::string key;
  if (open_kv(session->sm1, kv) != 0 || latest_table(session, oid, rec) != 0) {
    return -1;
  }
  // A temporary table has no entry in the store: it goes from the
  // session's view, with its name and its indexes' states.
  if (rec.nspoid == nsp_temp) {
    for (const index_rec_t &x : rec.indexes) {
      overlay_state(session, x.oid, std::nullopt, true);
    }
    overlay_table(session, oid, nullptr, true);
    overlay_name(session, rec.nspoid, rec.name, std::nullopt);
    return 0;
  }
  // Its names, then its record: each column, each index, and
  // its own fields.
  if (erase_name(kv.get(), rec.nspoid, rec.name) != 0) {
    return -1;
  }
  for (const index_rec_t &x : rec.indexes) {
    if (erase_name(kv.get(), rec.nspoid, x.name) != 0 ||
        del_part(session, kv.get(), kind_t::index, oid, x.oid) != 0 ||
        del_index_state(session, kv.get(), oid, x.oid) != 0) {
      return -1;
    }
  }
  for (size_t i = 0; i < rec.columns.size(); i++) {
    if (del_part(session, kv.get(), kind_t::column, oid, i) != 0) {
      return -1;
    }
  }
  for (const fkey_rec_t &fk : rec.fkeys) {
    if (del_part(session, kv.get(), kind_t::constraint, oid, fk.oid) != 0) {
      return -1;
    }
  }
  // Its statistics, every entry it may have, if it was analyzed.
  std::vector<std::string> keys;
  if (stats_keys(oid, rec, keys) != 0) {
    return -1;
  }
  for (const std::string &k : keys) {
    if (erase_if_there(session, kv.get(), k) != 0) {
      return -1;
    }
  }
  if (entity_key(kind_t::table, oid, key) != 0 ||
      del_key(session, kv.get(), key) != 0) {
    return -1;
  }
  overlay_table(session, oid, nullptr);
  overlay_stats(session, oid, nullptr);
  overlay_name(session, rec.nspoid, rec.name, std::nullopt);
  return 0;
}

/* Store a new collation; see sm3.h. */
int cat_put_collation(session_t *session, uint64_t oid, uint64_t nspoid,
                      std::string_view name, std::string_view locale,
                      bool deterministic) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string blob;
  if (open_kv(session->sm1, kv) != 0 ||
      use_nspace(session, nspoid, sm1::SM_LOCKMODE_SH, blob) != 0) {
    return -1;
  }
  // Opening it checks the locale and gives the version to record.
  sm2::collation_t *raw = nullptr;
  if (sm2::collation_open(locale, deterministic, "", raw) != 0) {
    return -1;
  }
  std::unique_ptr<sm2::collation_t, decltype(&sm2::collation_close)> c(
      raw, &sm2::collation_close);
  std::string version(sm2::collation_version(c.get()));
  if (insert_name(kv.get(), nspoid, name,
                  {kind_t::collation, oid, std::nullopt}) != 0 ||
      encode_collation(
          {std::string(name), std::string(locale), deterministic, version},
          blob) != 0 ||
      insert_entity(kv.get(), kind_t::collation, oid, blob) != 0) {
    return -1;
  }
  // The overlay holds it open, for this transaction's DDL to use.
  overlay_collation(session, oid, nspoid, c.release());
  return 0;
}

/* Record a table's statistics; see sm3.h. */
int cat_put_stats(session_t *session, uint64_t oid, const stats_t &stats) {
  std::shared_ptr<const table_def_t> def = cat_get_table(session, oid);
  if (def && def->nspoid == nsp_temp) {
    return invalid("statistics are not kept for a temporary table yet");
  }
  // A reader's table-oid lock, so a drop of the table waits for this
  // transaction, and this sees its definition as it stays.
  if (lock_table(session->sm1, oid, sm1::SM_LOCKMODE_IS) != 0) {
    return -1;
  }
  def = cat_get_table(session, oid);
  if (!def) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                       "table " + std::to_string(oid) + " does not exist");
  }
  // The table's own entry and each statistic's, by key.
  std::map<std::string, std::string> want;
  std::string key, blob;
  if (stats_key(oid, 0, 0, key) != 0 || encode_stats_table(stats, blob) != 0) {
    return -1;
  }
  want[key] = blob;
  for (const stat_t &st : stats.stats) {
    if (encode_stat(st, *def, blob) != 0 ||
        stats_key(oid, st.index.value_or(0), st.position + 1, key) != 0) {
      return -1;
    }
    if (!want.emplace(key, blob).second) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "statistics: two of one column or expression");
    }
  }
  // Its own entry's lock X first: a second analyze queues here.
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::vector<std::string> keys;
  if (cat_lock_stats(session, oid) != 0 || open_kv(session->sm1, kv) != 0 ||
      stats_keys(oid, def->rec, keys) != 0) {
    return -1;
  }
  // Each entry the table may have is written, or erased if this
  // analyze has none for it.
  for (const std::string &k : keys) {
    auto w = want.find(k);
    if (w != want.end() ? put_key(session, kv.get(), k, w->second) != 0
                        : erase_if_there(session, kv.get(), k) != 0) {
      return -1;
    }
  }
  overlay_stats(session, oid, std::make_shared<const stats_t>(stats));
  return 0;
}

namespace {

/*
 * Take the table oid's STATS entry lock X, waiting for it or not: 0,
 * or 1 with nothing taken if it would have waited, or -1.
 */
int lock_stats(session_t *session, uint64_t oid, bool wait) {
  std::shared_ptr<const table_def_t> def = cat_get_table(session, oid);
  if (def && def->nspoid == nsp_temp) {
    return invalid("statistics are not kept for a temporary table yet");
  }
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string key;
  if (open_kv(session->sm1, kv) != 0 || stats_key(oid, 0, 0, key) != 0) {
    return -1;
  }
  return wait ? sm2::catkv_lock(kv.get(), key, sm1::SM_LOCKMODE_EX)
              : sm2::catkv_trylock(kv.get(), key, sm1::SM_LOCKMODE_EX);
}

} // namespace

/* Lock a table's STATS entry; see internal.h. */
int cat_lock_stats(session_t *session, uint64_t oid) {
  return lock_stats(session, oid, true);
}

/* Lock a table's STATS entry if it is free now; see internal.h. */
int cat_trylock_stats(session_t *session, uint64_t oid) {
  return lock_stats(session, oid, false);
}

/* A table's statistics, loaded on first use; see sm3.h. */
int cat_get_stats(session_t *session, uint64_t oid,
                  std::shared_ptr<const stats_t> &out) {
  // This transaction's own, the last it recorded.
  for (auto it = session->stats.rbegin(); it != session->stats.rend(); ++it) {
    if (it->oid == oid) {
      out = it->stats;
      return 0;
    }
  }
  catalog_t *cat = session->cat;
  {
    std::shared_lock lock(cat->mu);
    auto it = cat->stats.find(oid);
    if (it != cat->stats.end()) {
      out = it->second;
      return 0;
    }
  }
  // Not asked about yet: load it, typed by the definition session
  // sees, in a READ COMMITTED transaction of the catalog's own, so
  // the caller's takes no lock and records no read.
  std::shared_ptr<const table_def_t> def = cat_get_table(session, oid);
  // A temporary table has none, and the store nothing to load.
  if (def && def->nspoid == nsp_temp) {
    out = nullptr;
    return 0;
  }
  if (!def) {
    out = nullptr;
    return 0;
  }
  std::shared_ptr<const stats_t> loaded;
  if (in_own_txn(cat->env, cat->dbname, nullptr, [&](sm1::session_t *s) {
        kv_ptr kv(nullptr, &sm2::catkv_close);
        std::string prefix, head;
        sm2::catkv_scan_t *c = nullptr;
        if (open_kv(s, kv) != 0 || stats_prefix(oid, prefix) != 0 ||
            stats_key(oid, 0, 0, head) != 0 ||
            sm2::catkv_scan(kv.get(), prefix, c) != 0) {
          return -1;
        }
        std::unique_ptr<sm2::catkv_scan_t, decltype(&sm2::catkv_scanend)> guard(
            c, &sm2::catkv_scanend);
        // The table's own entry sorts first, then each statistic's.
        auto st = std::make_shared<stats_t>();
        bool have = false;
        std::string_view k, v;
        int rc;
        while ((rc = sm2::catkv_scannxt(c, k, v)) == 0) {
          if (k == head) {
            if (decode_stats_table(v, *def, *st) != 0) {
              return -1;
            }
            have = true;
            continue;
          }
          stat_t one;
          bool kept = false;
          if (decode_stat(v, *def, one, kept) != 0) {
            return -1;
          }
          if (kept) {
            st->stats.push_back(std::move(one));
          }
        }
        if (rc != 1) {
          return -1;
        }
        if (have) {
          loaded = std::move(st);
        }
        return 0;
      }) != 0) {
    return -1;
  }
  // A commit that published newer ones meanwhile keeps its own.
  std::unique_lock lock(cat->mu);
  out = cat->stats.try_emplace(oid, std::move(loaded)).first->second;
  return 0;
}

/* A sequence's counter; see internal.h. */
std::string sequence_counter(uint64_t oid) {
  return "sm3.seq." + std::to_string(oid);
}

/* Store a new sequence and make its counter; see sm3.h. */
int cat_put_sequence(session_t *session, uint64_t oid, uint64_t nspoid,
                     std::string_view name, uint64_t start,
                     uint64_t increment) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string blob;
  // Shared: a drop of the namespace waits this transaction out.
  if (open_kv(session->sm1, kv) != 0 ||
      use_nspace(session, nspoid, sm1::SM_LOCKMODE_SH, blob) != 0) {
    return -1;
  }
  // The name first, so a taken one fails before anything is made.
  if (insert_name(kv.get(), nspoid, name,
                  {kind_t::sequence, oid, std::nullopt}) != 0 ||
      encode_sequence({nspoid, std::string(name), start, increment}, blob) !=
          0 ||
      insert_entity(kv.get(), kind_t::sequence, oid, blob) != 0 ||
      sm1::mkseq(session->sm1, sequence_counter(oid), start, increment) != 0) {
    return -1;
  }
  overlay_sequence(session, oid, nspoid);
  return 0;
}

/* A sequence's record, to change it; see internal.h. */
int cat_get_sequence_rec(session_t *session, uint64_t oid, bool lock,
                         sequence_rec_t &rec) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string key;
  std::string_view v;
  if (open_kv(session->sm1, kv) != 0 ||
      entity_key(kind_t::sequence, oid, key) != 0) {
    return -1;
  }
  // Under an X lock on its key, as cat_del_sequence reads it.
  if (lock && sm2::catkv_lock(kv.get(), key, sm1::SM_LOCKMODE_EX) != 0) {
    return -1;
  }
  int rc = lock && session->level == sm1::xctlevel_t::read_committed
               ? sm2::catkv_get_latest(kv.get(), key, v)
               : sm2::catkv_get(kv.get(), key, v);
  if (rc != 0) {
    return rc == 1 ? sm1::seterr(sm1::SM_ERR_NOTFOUND, "sequence " +
                                                           std::to_string(oid) +
                                                           " does not exist")
                   : -1;
  }
  return decode_sequence(v, rec);
}

/* Replace a sequence's record; see internal.h. */
int cat_put_sequence_rec(session_t *session, uint64_t oid,
                         const sequence_rec_t &rec) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string key;
  std::string blob;
  if (open_kv(session->sm1, kv) != 0 ||
      entity_key(kind_t::sequence, oid, key) != 0 ||
      encode_sequence(rec, blob) != 0) {
    return -1;
  }
  return put_key(session, kv.get(), key, blob);
}

/* Erase a sequence and its counter; see sm3.h. */
int cat_del_sequence(session_t *session, uint64_t oid) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string key;
  std::string_view v;
  // Its record under an X lock on its key, as a namespace's is
  // read: the newest committed one, which this may then erase.
  if (open_kv(session->sm1, kv) != 0 ||
      entity_key(kind_t::sequence, oid, key) != 0 ||
      sm2::catkv_lock(kv.get(), key, sm1::SM_LOCKMODE_EX) != 0) {
    return -1;
  }
  int rc = session->level == sm1::xctlevel_t::read_committed
               ? sm2::catkv_get_latest(kv.get(), key, v)
               : sm2::catkv_get(kv.get(), key, v);
  if (rc != 0) {
    return rc == 1 ? sm1::seterr(sm1::SM_ERR_NOTFOUND, "sequence " +
                                                           std::to_string(oid) +
                                                           " does not exist")
                   : -1;
  }
  sequence_rec_t rec;
  if (decode_sequence(v, rec) != 0 || sm2::catkv_erase(kv.get(), key) != 0 ||
      erase_name(kv.get(), rec.nspoid, rec.name) != 0 ||
      sm1::rmseq(session->sm1, sequence_counter(oid)) != 0) {
    return -1;
  }
  overlay_sequence(session, oid, std::nullopt);
  return 0;
}

/* A sequence by name, read in the transaction; see sm3.h. */
int cat_get_sequence(session_t *session, uint64_t nspoid, std::string_view name,
                     uint64_t &oid) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string key;
  std::string_view v;
  if (open_kv(session->sm1, kv) != 0 || name_key(nspoid, name, key) != 0) {
    return -1;
  }
  int rc = sm2::catkv_get(kv.get(), key, v);
  if (rc != 0) {
    return rc;
  }
  name_rec_t rec;
  if (decode_name(v, rec) != 0) {
    return -1;
  }
  // The name may be a table's or an index's.
  if (rec.kind != kind_t::sequence) {
    return 1;
  }
  oid = rec.oid;
  return 0;
}

/* A sequence's namespace and name by its oid; see sm3.h. */
int cat_get_sequence_name(session_t *session, uint64_t oid, uint64_t &nspoid,
                          std::string &name) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string key;
  std::string_view v;
  if (open_kv(session->sm1, kv) != 0 ||
      entity_key(kind_t::sequence, oid, key) != 0) {
    return -1;
  }
  int rc = sm2::catkv_get(kv.get(), key, v);
  if (rc != 0) {
    return rc;
  }
  sequence_rec_t rec;
  if (decode_sequence(v, rec) != 0) {
    return -1;
  }
  nspoid = rec.nspoid;
  name = std::move(rec.name);
  return 0;
}

/* A sequence by its oid, read in the transaction; see sm3.h. */
int cat_get_sequence_def(session_t *session, uint64_t oid,
                         sequence_def_t &def) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string key;
  std::string_view v;
  if (open_kv(session->sm1, kv) != 0 ||
      entity_key(kind_t::sequence, oid, key) != 0) {
    return -1;
  }
  int rc = sm2::catkv_get(kv.get(), key, v);
  if (rc != 0) {
    return rc;
  }
  sequence_rec_t rec;
  if (decode_sequence(v, rec) != 0) {
    return -1;
  }
  def = {rec.nspoid,    std::move(rec.name), rec.start,
         rec.increment, rec.owner,           rec.owner_attnum};
  return 0;
}

/* The sequences in a namespace, read in the transaction; see sm3.h. */
int cat_list_sequences(session_t *session, uint64_t nspoid,
                       std::vector<std::string> &names) {
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string prefix;
  sm2::catkv_scan_t *c = nullptr;
  if (open_kv(session->sm1, kv) != 0 || name_prefix(nspoid, prefix) != 0 ||
      sm2::catkv_scan(kv.get(), prefix, c) != 0) {
    return -1;
  }
  std::unique_ptr<sm2::catkv_scan_t, decltype(&sm2::catkv_scanend)> guard(
      c, &sm2::catkv_scanend);
  names.clear();
  std::string_view k, v;
  int rc;
  // The keys come in name order, every kind's names together.
  while ((rc = sm2::catkv_scannxt(c, k, v)) == 0) {
    name_rec_t rec;
    if (decode_name(v, rec) != 0) {
      return -1;
    }
    if (rec.kind == kind_t::sequence) {
      std::string name;
      if (name_of_key(k, name) != 0) {
        return -1;
      }
      names.push_back(std::move(name));
    }
  }
  return rc == 1 ? 0 : -1;
}

/* Store a view's record, new or changed; see internal.h. */
int cat_put_view(session_t *session, uint64_t oid, const view_def_t &def,
                 const view_def_t *old) {
  // A temporary view is kept in the session, not in the store, and
  // its name among the session's temporary names (VIEW.md §12.2).
  if (def.nspoid == nsp_temp) {
    if (temp_name_taken(session, def.name, oid)) {
      return taken(def.name);
    }
    overlay_view(session, oid, std::make_shared<const view_def_t>(def), true);
    return 0;
  }
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string key, blob;
  if (open_kv(session->sm1, kv) != 0 ||
      entity_key(kind_t::view, oid, key) != 0 || encode_view(def, blob) != 0) {
    return -1;
  }
  if (!old) {
    // Shared: a drop of the namespace waits this transaction out.
    // The name first, so a taken one fails before the record is
    // written.
    if (use_nspace(session, def.nspoid, sm1::SM_LOCKMODE_SH, key) != 0 ||
        insert_name(kv.get(), def.nspoid, def.name,
                    {kind_t::view, oid, std::nullopt}) != 0 ||
        entity_key(kind_t::view, oid, key) != 0 ||
        sm2::catkv_insert(kv.get(), key, blob) != 0) {
      return -1;
    }
  } else {
    if (old->name != def.name &&
        (insert_name(kv.get(), def.nspoid, def.name,
                     {kind_t::view, oid, std::nullopt}) != 0 ||
         erase_name(kv.get(), old->nspoid, old->name) != 0)) {
      return -1;
    }
    if (put_key(session, kv.get(), key, blob) != 0) {
      return -1;
    }
  }
  overlay_view(session, oid, std::make_shared<const view_def_t>(def));
  return 0;
}

/* Erase a view's record and its name; see internal.h. */
int cat_del_view(session_t *session, uint64_t oid, const view_def_t &def) {
  // A temporary view has no entry in the store.
  if (def.nspoid == nsp_temp) {
    overlay_view(session, oid, nullptr, true);
    return 0;
  }
  kv_ptr kv(nullptr, &sm2::catkv_close);
  std::string key;
  if (open_kv(session->sm1, kv) != 0 ||
      entity_key(kind_t::view, oid, key) != 0 ||
      del_key(session, kv.get(), key) != 0 ||
      erase_name(kv.get(), def.nspoid, def.name) != 0) {
    return -1;
  }
  overlay_view(session, oid, nullptr);
  return 0;
}

/* Take a table's oid lock; see internal.h. */
int lock_table(sm1::session_t *s, uint64_t oid, sm1::lockmode_t mode) {
  // The name is the oid, big-endian: it is persisted in a
  // prepared transaction's lock set, so host order will not do.
  std::string key(8, '\0');
  for (int i = 0; i < 8; i++) {
    key[i] = static_cast<char>(oid >> (56 - 8 * i));
  }
  /*
   * An sm1 OID lock: a name in no store, which nothing writes,
   * so the lock never fails on a version newer than the caller's
   * snapshot and a reader that waited out DDL gets the new
   * definition. Held to the end of the transaction, so DDL waits
   * out its users (SM3_DESIGN.md §2).
   */
  return sm1::lock(s, sm1::LOCKTYPE_OID, 0, key, mode);
}

/* Take a table's oid lock if it is free now; see internal.h. */
int lock_table_now(sm1::session_t *s, uint64_t oid, sm1::lockmode_t mode) {
  std::string key(8, '\0');
  for (int i = 0; i < 8; i++) {
    key[i] = static_cast<char>(oid >> (56 - 8 * i));
  }
  // lock_table's lock, asked for without waiting.
  return sm1::trylock(s, sm1::LOCKTYPE_OID, 0, key, mode);
}

} // namespace sm3

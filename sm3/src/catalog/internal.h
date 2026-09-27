/*
 * internal.h -- the catalog in memory (CATALOG.md §4, §7): the
 * shared catalog, a session's overlay on it, and what load.cc,
 * ddl.cc and session.cc share. Internal to sm3.
 */
#ifndef SM3_INTERNAL_H
#define SM3_INTERNAL_H

#include "catalog.h"
#include "sm3.h"

#include <functional>
#include <map>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

/*
 * The shared catalog: every committed definition, read by every
 * session. mu guards the maps; a change replaces a definition, it
 * never edits one in place.
 */
struct sm3::catalog_t {
  mutable std::shared_mutex mu;
  std::map<uint64_t, std::shared_ptr<const table_def_t>> tables;
  std::map<std::pair<uint64_t, std::string>, uint64_t> table_names;
  std::map<uint64_t, std::string> nspaces;
  std::map<uint64_t, sm2::collation_t *> collations; /* owned */

  /* Close every collation the catalog holds. */
  ~catalog_t() {
    for (auto &[oid, c] : collations) {
      sm2::collation_close(c);
    }
  }
};

namespace sm3 {

/*
 * One change a transaction's DDL made, tagged with the savepoint
 * depth it was made at: 0 before any, d after savepoint d.
 */
struct table_change_t {
  int depth;
  uint64_t oid;
  std::shared_ptr<const table_def_t> def; /* nullptr: dropped */
};
struct name_change_t {
  int depth;
  std::pair<uint64_t, std::string> name;
  std::optional<uint64_t> oid; /* nullopt: erased */
};
struct collation_change_t {
  int depth;
  uint64_t oid;
  sm2::collation_t *collation; /* owned until published */
};

} // namespace sm3

/*
 * A session: an sm1 session, the shared catalog it reads, and its
 * open transaction's overlay -- the changes its DDL made, which
 * only it sees until they are published at commit.
 */
struct sm3::session_t {
  sm1::session_t *sm1;
  catalog_t *cat;
  int depth = 0; /* the savepoint depth changes are made at */
  std::vector<table_change_t> tables;
  std::vector<name_change_t> names;
  std::vector<collation_change_t> collations;
};

namespace sm3 {

/* The collation oid names, as session sees it; false if unknown. */
using collation_lookup_t =
    std::function<bool(uint64_t, const sm2::collation_t *&)>;

/*
 * Build the definition of table oid from its record, resolving its
 * collations with lookup. Fails with SM_ERR_CORRUPT if the record
 * names a type, collation or column that is not there.
 */
int build_table(const collation_lookup_t &lookup, uint64_t oid,
                const table_rec_t &rec,
                std::shared_ptr<const table_def_t> &out);

/* The collation oid, as session sees it; false if unknown. */
bool session_collation(const session_t *s, uint64_t oid,
                       const sm2::collation_t *&out);

/* Record changes in session's overlay, at its current depth. */
void overlay_table(session_t *s, uint64_t oid,
                   std::shared_ptr<const table_def_t> def);
void overlay_name(session_t *s, uint64_t nspoid, std::string_view name,
                  std::optional<uint64_t> oid);
void overlay_collation(session_t *s, uint64_t oid, sm2::collation_t *collation);

} // namespace sm3

#endif /* SM3_INTERNAL_H */

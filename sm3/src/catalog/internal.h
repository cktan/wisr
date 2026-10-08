/*
 * internal.h -- the catalog in memory (CATALOG.md §4, §7): the
 * shared catalog, a session's overlay on it, and what load.cc,
 * ddl.cc and session.cc share. Internal to sm3.
 */
#ifndef SM3_INTERNAL_H
#define SM3_INTERNAL_H

#include "catalog.h"
#include "sm3.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace sm3 {

/*
 * The hash of a (nspoid, name) key, for the catalog's table_names.
 * The nspoid is mixed into the name's hash, so one name in two
 * namespaces lands in two buckets.
 */
struct tblname_hash_t {
  size_t operator()(const std::pair<uint64_t, std::string> &k) const {
    size_t h = std::hash<std::string>{}(k.second);
    h ^= std::hash<uint64_t>{}(k.first) + 0x9e3779b97f4a7c15ULL + (h << 6) +
         (h >> 2);
    return h;
  }
};

} // namespace sm3

/*
 * The shared catalog: every committed definition, read by every
 * session. mu guards the maps; a change replaces a definition, it
 * never edits one in place.
 */
struct sm3::catalog_t {
  mutable std::shared_mutex mu;
  std::unordered_map<uint64_t, std::shared_ptr<const table_def_t>> tables;
  std::unordered_map<std::pair<uint64_t, std::string>, uint64_t, tblname_hash_t>
      table_names;
  std::unordered_map<uint64_t, std::string> nspaces;
  std::unordered_map<uint64_t, sm2::collation_t *> collations; /* owned */
  /* The namespace of each collation, for a namespace's drop to ask. */
  std::unordered_map<uint64_t, uint64_t> collation_nsp;

  /*
   * How many times a transaction's DDL has been published, and, for
   * an index that became valid since the catalog was loaded, the
   * count at which it did (SM3_DESIGN.md §3). A session notes the
   * count before it takes a snapshot; an index valid at or before
   * that count is filled as far as that snapshot can see.
   */
  std::atomic<uint64_t> seq{0};
  std::unordered_map<uint64_t, uint64_t> valid_seq; /* index oid -> count */

  /* The host's evaluator of partial and expression indexes; set once. */
  index_eval_fn eval;

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
  uint64_t nspoid;
  sm2::collation_t *collation; /* owned until published */
};
struct nspace_change_t {
  int depth;
  uint64_t oid;
  std::optional<std::string> name; /* nullopt: dropped */
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
  sm1::xctlevel_t level; /* the open transaction's, set by begin */
  int depth = 0;         /* the savepoint depth changes are made at */
  std::vector<table_change_t> tables;
  std::vector<name_change_t> names;
  std::vector<collation_change_t> collations;
  std::vector<nspace_change_t> nspaces;
  bool in_txn = false;      /* between begin and commit or abort */
  uint64_t snap_seq = 0;    /* cat->seq, noted before the snapshot */
  bool stmt_pinned = false; /* stmt_begin was called: statement snapshots */
};

namespace sm3 {

/*
 * Run f on a session of its own on dbname, opened with opt, in a
 * READ COMMITTED transaction of its own: committed if f returns 0,
 * aborted if not, and the session closed either way, with the
 * error as it was met.
 */
template <typename F>
int in_own_txn(sm1::env_t *env, std::string_view dbname,
               const sm1::options_t *opt, F f) {
  sm1::session_t *s = nullptr;
  // A session of its own, so that the caller needs none.
  if (sm1::opendb(env, dbname, s, opt) != 0) {
    return -1;
  }
  if (sm1::begin(s, sm1::xctlevel_t::read_committed) != 0 || f(s) != 0 ||
      sm1::commit(s) != 0) {
    // Keep the error as it was met: the abort and the close may not.
    sm1::errnum_t error = sm1::errnum();
    std::string reason = sm1::errmsg();
    sm1::abort(s);
    sm1::closedb(s);
    return sm1::seterr(error, reason);
  }
  return sm1::closedb(s);
}

/* The collation oid names, as session sees it; false if unknown. */
using collation_lookup_t =
    std::function<bool(uint64_t, const sm2::collation_t *&)>;

/*
 * Whether session may scan the index oid of the table it sees as
 * def (sm3.h, tbl_index_usable).
 */
bool index_usable(const session_t *session, const table_def_t &def,
                  uint64_t oid);

/*
 * Build the definition of table oid from its record, resolving its
 * collations with lookup. Fails with SM_ERR_CORRUPT if the record
 * names a type, collation or column that is not there.
 */
int build_table(const collation_lookup_t &lookup, uint64_t oid,
                const table_rec_t &rec,
                std::shared_ptr<const table_def_t> &out);

/*
 * Take the schema lock of the table oid in mode (SM3_DESIGN.md
 * §2): sm1's row lock on its LOCK key, which no one writes, so
 * the lock waits on DDL but never fails as a stale read would.
 */
int lock_table(sm1::session_t *s, uint64_t oid, sm1::lockmode_t mode);

/* The collation oid, as session sees it; false if unknown. */
bool session_collation(const session_t *s, uint64_t oid,
                       const sm2::collation_t *&out);

/* Record changes in session's overlay, at its current depth. */
void overlay_table(session_t *s, uint64_t oid,
                   std::shared_ptr<const table_def_t> def);
void overlay_name(session_t *s, uint64_t nspoid, std::string_view name,
                  std::optional<uint64_t> oid);
void overlay_collation(session_t *s, uint64_t oid, uint64_t nspoid,
                       sm2::collation_t *collation);
void overlay_nspace(session_t *s, uint64_t oid,
                    std::optional<std::string_view> name);

/*
 * Whether there is a namespace oid as session sees it, its name
 * set in name; and whether a table or a collation is in it. Both
 * read the catalog in memory, which holds every commit published
 * so far, not what the transaction's snapshot shows (CATALOG.md
 * §5).
 */
bool session_nspace(const session_t *s, uint64_t oid, std::string &name);
bool nspace_in_use(const session_t *s, uint64_t oid);

} // namespace sm3

#endif /* SM3_INTERNAL_H */

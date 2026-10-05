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
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace sm3 {

/*
 * The catalog's builder (SM3_DESIGN.md §3): a queue of indexes to
 * build and the workers building them. mu guards all of it, and is
 * taken after the catalog's own mutex, never before.
 */
struct builder_t {
  std::mutex mu;
  std::condition_variable cv; /* a build ended, or the catalog changed */
  std::function<sm1::session_t *()> open; /* empty: not started */
  size_t max_workers = 0;
  size_t batch_rows = 0;
  std::deque<std::pair<uint64_t, uint64_t>> queue; /* table, index */
  std::set<uint64_t> active; /* indexes queued or being built */
  size_t running = 0;        /* workers alive */
  bool stop = false;
  std::vector<std::thread> threads;
  std::vector<std::thread::id> finished; /* workers to join */
};

} // namespace sm3

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

  /*
   * How many times a transaction's DDL has been published, and, for
   * an index that became valid since the catalog was loaded, the
   * count at which it did (SM3_DESIGN.md §3). A session notes the
   * count before it takes a snapshot; an index valid at or before
   * that count is filled as far as that snapshot can see.
   */
  std::atomic<uint64_t> seq{0};
  std::map<uint64_t, uint64_t> valid_seq; /* index oid -> count */

  builder_t builder;

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
  sm1::xctlevel_t level; /* the open transaction's, set by begin */
  int depth = 0;         /* the savepoint depth changes are made at */
  std::vector<table_change_t> tables;
  std::vector<name_change_t> names;
  std::vector<collation_change_t> collations;
  bool in_txn = false;      /* between begin and commit or abort */
  uint64_t snap_seq = 0;    /* cat->seq, noted before the snapshot */
  bool stmt_pinned = false; /* stmt_begin was called: statement snapshots */
};

namespace sm3 {

/* The collation oid names, as session sees it; false if unknown. */
using collation_lookup_t =
    std::function<bool(uint64_t, const sm2::collation_t *&)>;

/*
 * Queue the build of a table's index, if the builder was started
 * and the index is not queued already, starting a worker if fewer
 * than the most allowed are alive. The caller may hold the
 * catalog's mutex.
 */
void build_enqueue(catalog_t *cat, uint64_t table, uint64_t index);

/* Wake whoever waits on an index's state: the catalog has changed. */
void build_notify(catalog_t *cat);

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
int lock_table(sm2::catkv_t *kv, uint64_t oid, sm1::lockmode_t mode);

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

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
#include <map>
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
  /*
   * Each index's table and its own oid, by its table's namespace and
   * its name, so a lookup by name reads no definition.
   */
  std::unordered_map<std::pair<uint64_t, std::string>,
                     std::pair<uint64_t, uint64_t>, tblname_hash_t>
      index_names;
  std::unordered_map<uint64_t, std::string> nspaces;
  std::unordered_map<uint64_t, sm2::collation_t *> collations; /* owned */
  /*
   * Each index's state, by its oid; one that has none is pending.
   * Kept by index and not in a definition, so that it changes
   * without the table's definition being replaced.
   */
  std::unordered_map<uint64_t, index_status_t> index_states;
  /* The namespace of each collation, for a namespace's drop to ask. */
  std::unordered_map<uint64_t, uint64_t> collation_nsp;
  /*
   * The namespace and name of each sequence by its oid, for the same,
   * and its oid by them, so a lookup either way reads no store: all
   * of a sequence that is kept in memory, its record and counter
   * being read from the catalog store in the transaction.
   */
  std::unordered_map<uint64_t, std::pair<uint64_t, std::string>>
      sequence_by_oid;
  std::unordered_map<std::pair<uint64_t, std::string>, uint64_t, tblname_hash_t>
      sequence_names;
  /*
   * Each view's definition, and its oid by its namespace and name
   * (VIEW.md §4). What reads a relation is found by a pass over
   * views: it is derived, and kept nowhere.
   */
  std::unordered_map<uint64_t, std::shared_ptr<const view_def_t>> views;
  std::unordered_map<std::pair<uint64_t, std::string>, uint64_t, tblname_hash_t>
      view_names;
  /*
   * The register of temporary readers (VIEW.md §12.3): every session's
   * committed temporary views, by oid, with the session that has each,
   * so that another session's drop finds what reads a permanent
   * relation. In memory alone, as the views are.
   */
  struct temp_reader_t {
    session_t *session;
    std::shared_ptr<const view_def_t> def;
  };
  std::unordered_map<uint64_t, temp_reader_t> temp_readers;

  /*
   * Each table's committed statistics, loaded on first use: nullptr
   * if it was never analyzed. A table not yet asked about has no
   * entry. A commit that recorded new ones replaces its entry.
   */
  std::unordered_map<uint64_t, std::shared_ptr<const stats_t>> stats;

  /* Where the catalog was opened from, for a load of its own. */
  sm1::env_t *env = nullptr;
  std::string dbname;

  /*
   * How many times a transaction's DDL has been published, and, for
   * an index that became valid since the catalog was loaded, the
   * count at which it did (SM3_DESIGN.md §3). A session notes the
   * count before it takes a snapshot; an index valid at or before
   * that count is filled as far as that snapshot can see.
   */
  std::atomic<uint64_t> seq{0};
  std::unordered_map<uint64_t, uint64_t> valid_seq; /* index oid -> count */
  /*
   * The count at which each table's definition, and its statistics,
   * were last published, for its version (cat_table_version); one
   * not changed since the catalog was loaded has none, 0. And the
   * count at which a namespace was last made or dropped, the
   * namespaces' epoch (cat_namespace_epoch).
   */
  std::unordered_map<uint64_t, uint64_t> table_seq; /* table oid -> count */
  std::unordered_map<uint64_t, uint64_t> stats_seq; /* table oid -> count */
  std::unordered_map<uint64_t, uint64_t> view_seq;  /* view oid -> count */
  std::atomic<uint64_t> nsp_seq{0};

  /* The host's evaluator of partial and expression indexes; set once. */
  index_eval_fn eval;
  bool toast = false; /* the host reads TOAST pointers (cat_set_toast) */

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
  bool temp = false;                      /* a temporary table's */
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
struct state_change_t {
  int depth;
  uint64_t index;
  std::optional<index_status_t> status; /* nullopt: none, the index gone */
  bool temp = false;                    /* a temporary table's index */
};
struct nspace_change_t {
  int depth;
  uint64_t oid;
  std::optional<std::string> name; /* nullopt: dropped */
};
struct sequence_change_t {
  int depth;
  uint64_t oid;
  std::optional<uint64_t> nspoid;        /* nullopt: dropped */
  std::pair<uint64_t, std::string> name; /* made or dropped */
};
struct view_change_t {
  int depth;
  uint64_t oid;
  std::shared_ptr<const view_def_t> def; /* nullptr: dropped */
  bool temp = false;                     /* a temporary view's */
  bool other = false; /* a drop of another session's temporary view */
};
struct stats_change_t {
  int depth;
  uint64_t oid;                         /* the table's */
  std::shared_ptr<const stats_t> stats; /* nullptr: the table dropped */
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
  std::vector<state_change_t> states;
  std::vector<sequence_change_t> sequences;
  std::vector<stats_change_t> stats;
  std::vector<view_change_t> views;
  bool in_txn = false;      /* between begin and commit or abort */
  uint64_t snap_seq = 0;    /* cat->seq, noted before the snapshot */
  bool stmt_pinned = false; /* stmt_begin was called: statement snapshots */

  /*
   * The session's temporary catalog (TEMPTABLE.md §4): its committed
   * temporary tables, their names, their indexes' states, and its
   * temporary views. A commit moves the overlay's temporary changes here, and
   * never into the shared catalog; nothing else reads or writes it.
   */
  std::unordered_map<uint64_t, std::shared_ptr<const table_def_t>> temp_tables;
  std::unordered_map<std::string, uint64_t> temp_names;
  std::unordered_map<uint64_t, index_status_t> temp_states;
  /* Its committed temporary views (VIEW.md §12.2), by oid. */
  std::unordered_map<uint64_t, std::shared_ptr<const view_def_t>> temp_views;
  /*
   * Its temporary views other sessions have dropped (VIEW.md §12.4),
   * for its next begin to drop: guarded by the catalog's mutex, and
   * has_doomed set while there are any.
   */
  std::vector<uint64_t> doomed;
  std::atomic<bool> has_doomed{false};

  /*
   * The pool of pins (../ROW.md §4.1): every pin made, freed at
   * session_close, so a stale ref always points to a pin; those
   * released, ready to take; those holding a read's or a write's
   * row; and the cursors'.
   */
  std::vector<sm3::pin_t *> pins;
  sm3::pin_t *pins_free = nullptr;
  sm3::pin_t *pins_held = nullptr;
  sm3::pin_t *pins_scan = nullptr;
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
 * Take the table-oid lock of the table oid in mode (SM3_DESIGN.md
 * §2): sm1's OID lock on the table's oid, a name nothing writes,
 * so the lock waits on DDL but never fails as a stale read would.
 * A namespace's lock is taken the same way, on its oid.
 */
int lock_table(sm1::session_t *s, uint64_t oid, sm1::lockmode_t mode);

/*
 * As lock_table, but never waiting: 0 with the lock held, 1 with
 * nothing taken if another transaction holds it in a mode that
 * conflicts, or -1.
 */
int lock_table_now(sm1::session_t *s, uint64_t oid, sm1::lockmode_t mode);

/*
 * Take the table oid's STATS entry lock X, to the end of the
 * transaction: an analyze takes it before it samples, so a second
 * waits for the first rather than both sampling.
 */
int cat_lock_stats(session_t *s, uint64_t oid);

/*
 * As cat_lock_stats, but never waiting: 0 with the lock held, 1 with
 * nothing taken if another analyze of the table holds it, or -1.
 */
int cat_trylock_stats(session_t *s, uint64_t oid);

/* The name of the sm1 sequence that counts for the sequence oid. */
std::string sequence_counter(uint64_t oid);

/*
 * The record of the sequence oid. To change it, lock: under an X
 * lock on its key to the end of the transaction, its newest
 * committed version, or the one s's transaction last put; else as
 * s's transaction sees it. No such sequence fails with
 * SM_ERR_NOTFOUND.
 */
int cat_get_sequence_rec(session_t *s, uint64_t oid, bool lock,
                         sequence_rec_t &rec);

/* Replace the record of the sequence oid, read by cat_get_sequence_rec. */
int cat_put_sequence_rec(session_t *s, uint64_t oid, const sequence_rec_t &rec);

/* The collation oid, as session sees it; false if unknown. */
bool session_collation(const session_t *s, uint64_t oid,
                       const sm2::collation_t *&out);

/*
 * The temporary tables as session sees them, by oid: its committed
 * ones with its transaction's changes over them.
 */
std::map<uint64_t, std::shared_ptr<const table_def_t>>
temp_tables_of(const session_t *s);

/*
 * Record changes in session's overlay, at its current depth; temp
 * marks one of a temporary table, which a commit keeps in the
 * session.
 */
void overlay_table(session_t *s, uint64_t oid,
                   std::shared_ptr<const table_def_t> def, bool temp = false);
/*
 * Fetch the columns atts of a row of table that are out of line,
 * in place: a TOAST pointer in row becomes a datum pointing at its
 * value, which own is given to keep. For a caller that shows a
 * row to the host's evaluator outside a write, as ANALYZE does.
 */
int tbl_fetch_cols(table_t *table, const std::vector<int32_t> &atts,
                   std::span<sm2::datum_t> row, std::span<const bool> isnull,
                   std::vector<std::string> &own);

/* The sm1 sequence the table oid draws its TOAST ids from. */
inline std::string toast_seq_name(uint64_t oid) {
  return "sm3.toast." + std::to_string(oid);
}

void overlay_name(session_t *s, uint64_t nspoid, std::string_view name,
                  std::optional<uint64_t> oid);
void overlay_collation(session_t *s, uint64_t oid, uint64_t nspoid,
                       sm2::collation_t *collation);
void overlay_nspace(session_t *s, uint64_t oid,
                    std::optional<std::string_view> name);
void overlay_state(session_t *s, uint64_t index,
                   std::optional<index_status_t> status, bool temp = false);
void overlay_sequence(session_t *s, uint64_t oid,
                      std::optional<uint64_t> nspoid, uint64_t name_nspoid,
                      std::string_view name);
void overlay_stats(session_t *s, uint64_t oid,
                   std::shared_ptr<const stats_t> stats);
void overlay_view(session_t *s, uint64_t oid,
                  std::shared_ptr<const view_def_t> def, bool temp = false);

/*
 * Whether name is one of session's temporary names, as it sees them: a
 * temporary table's, one of their indexes', or a temporary view's, but
 * those of the object oid.
 */
bool temp_name_taken(const session_t *s, std::string_view name, uint64_t oid);

/*
 * Another session's temporary view oid, from the register (VIEW.md
 * §12.3), unless s's transaction has dropped it; nullptr if none.
 */
std::shared_ptr<const view_def_t> other_view(const session_t *s, uint64_t oid);

/* Record the drop of another session's temporary view (VIEW.md §12.4). */
void overlay_other_drop(session_t *s, uint64_t oid);

/*
 * Store the view oid's record, def, new or changed, in s's
 * transaction: a new one takes its name and holds its namespace
 * shared, and a changed one, whose old record is old, moves its
 * name if def's differs. Nothing is checked of what it reads.
 */
int cat_put_view(session_t *s, uint64_t oid, const view_def_t &def,
                 const view_def_t *old);

/* Erase the view oid's record, def, and its name. */
int cat_del_view(session_t *s, uint64_t oid, const view_def_t &def);

/*
 * Whether there is a namespace oid as session sees it, its name
 * set in name; and whether a table, a collation, a sequence or a
 * view is in it. Both
 * read the catalog in memory, which holds every commit published
 * so far, not what the transaction's snapshot shows (CATALOG.md
 * §5).
 */
bool nspace_in_use(const session_t *s, uint64_t oid);

} // namespace sm3

#endif /* SM3_INTERNAL_H */

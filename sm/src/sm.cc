/*
 * sm.cc -- wisr's C API over sm1, sm2 and sm3 (include/sm.h).
 *
 * Each entry point converts its arguments, calls the level below
 * inside guard(), and converts the results back. guard() is the
 * stack's one catch-all (DESIGN.md section 5): no exception
 * leaves this file.
 */

#include "sm.h"

#include "sm1.h"
#include "sm2.h"
#include "sm3.h"

#include <bit>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

static_assert(static_cast<int>(sm1::xctlevel_t::read_committed) ==
              SM_XCT_READ_COMMITTED);
static_assert(static_cast<int>(sm1::xctlevel_t::repeatable_read) ==
              SM_XCT_REPEATABLE_READ);
static_assert(static_cast<int>(sm1::xctlevel_t::serializable) ==
              SM_XCT_SERIALIZABLE);
static_assert(sm3::nsp_pg_catalog == SM_NSP_PG_CATALOG);
static_assert(sm3::nsp_public == SM_NSP_PUBLIC);
static_assert(sm3::nsp_temp == SM_NSP_TEMP);
static_assert(static_cast<int>(sm3::index_state_t::pending) ==
              SM_INDEX_PENDING);
static_assert(static_cast<int>(sm3::index_state_t::valid) == SM_INDEX_VALID);
static_assert(static_cast<int>(sm3::index_state_t::failed) == SM_INDEX_FAILED);
static_assert(static_cast<int>(sm1::loglevel_t::error) == SM_LOG_ERROR);
static_assert(static_cast<int>(sm1::loglevel_t::warn) == SM_LOG_WARN);
static_assert(static_cast<int>(sm1::loglevel_t::info) == SM_LOG_INFO);
/*
 * sm.h's SM_DATUM_FROM_* macros set a by-value datum_t by value,
 * and sm2 reads its bytes from the low address; the two agree
 * only on a little-endian machine.
 */
static_assert(std::endian::native == std::endian::little);

struct sm_session_t;

/*
 * The hash of a database name, over std::string and
 * std::string_view alike: a lookup by the name the caller passed
 * copies nothing, as it did not under std::less<>.
 */
struct dbname_hash_t {
  using is_transparent = void;
  size_t operator()(std::string_view name) const {
    return std::hash<std::string_view>{}(name);
  }
};

/*
 * An environment as the wrapper holds it: sm1's, and the catalog
 * of each database a session has opened, loaded by the first and
 * shared by every session on it until detach or rmdb. mu guards
 * the catalogs and the sessions.
 */
struct sm_env_t {
  sm1::env_t *env;
  std::mutex mu;
  std::unordered_map<std::string, sm3::catalog_t *, dbname_hash_t,
                     std::equal_to<>>
      catalogs;
  std::unordered_set<sm_session_t *> sessions;
  sm_index_eval_fn eval = nullptr; /* the host's evaluator, set once */
  void *eval_ctx = nullptr;
  bool toast = false; /* the host reads TOAST pointers (sm_set_toast) */
};

/*
 * A session as the wrapper holds it: an sm3 session, over the sm1
 * one, whose transaction calls go through sm3 so its overlay of
 * DDL stays in step (sm3/CATALOG.md §4).
 */
struct sm_session_t {
  sm_env_t *env;
  sm1::session_t *sm1;
  sm3::session_t *sm3 = nullptr;
};

namespace {

/* The sm1 environment behind a C handle. */
sm1::env_t *env_of(sm_env_t *env) { return env->env; }

/* The sm1 session behind a C handle. */
sm1::session_t *session_of(sm_session_t *session) { return session->sm1; }

/* A C name as a view; NULL is the empty name, which sm1 refuses. */
std::string_view name_of(const char *name) {
  return name ? std::string_view(name) : std::string_view();
}

/* Record an out-of-memory error and return -1. */
int nomem() { return sm1::seterr(sm1::SM_ERR_NOMEM, "out of memory"); }

/*
 * The message of the error being kept across an abort, as large
 * as sm1's own message buffer.
 */
thread_local char t_kept[4096 + 256];

/*
 * Abort session's active transaction, keeping the calling
 * thread's error as it was: the abort fails harmlessly when the
 * engine has already aborted, and must not replace the error the
 * caller gets. A prepared transaction in doubt is left alone.
 */
void abort_keeping_error(sm_session_t *session) {
  // Keep the code and a copy of the message; nothing allocates.
  sm1::errnum_t code = sm1::errnum();
  const char *msg = sm1::errmsg();
  size_t n = strlen(msg);
  if (n >= sizeof(t_kept)) {
    n = sizeof(t_kept) - 1;
  }
  memcpy(t_kept, msg, n);
  t_kept[n] = '\0';
  // Abort in memory: it undoes what a half-done call wrote. A
  // prepared transaction is the coordinator's, never ours to undo.
  if (!sm1::in_doubt(session->sm1)) {
    sm3::abort(session->sm3);
  }
  sm1::seterr(code, std::string_view(t_kept, n));
}

/*
 * Run fn, a cleanup after a failure, keeping the calling thread's
 * error as it was: the error the caller gets is the failure's.
 */
template <class F> void keeping_error(F &&fn) {
  sm1::errnum_t code = sm1::errnum();
  std::string msg = sm1::errmsg();
  fn();
  sm1::seterr(code, msg);
}

/*
 * Run fn, the body of an entry point, and return what it returns.
 * An exception becomes -1 with SM_ERR_NOMEM or SM_ERR_EXCEPTION;
 * after either, from fn or from below, session's transaction is
 * aborted, since a data call may have written part of its change.
 */
template <class F> int guard(sm_session_t *session, F &&fn) noexcept {
  int rc;
  try {
    rc = fn();
  } catch (const std::bad_alloc &) {
    rc = nomem();
  } catch (const std::exception &e) {
    rc = sm1::seterr(sm1::SM_ERR_EXCEPTION, e.what());
  } catch (...) {
    rc = sm1::seterr(sm1::SM_ERR_EXCEPTION, "unknown exception");
  }
  if (rc == -1 && session != nullptr &&
      (sm1::errnum() == sm1::SM_ERR_NOMEM ||
       sm1::errnum() == sm1::SM_ERR_EXCEPTION)) {
    abort_keeping_error(session);
  }
  return rc;
}

/* The C log hook and its arg, as sm_set_log() installed them. */
struct log_hook_t {
  sm_log_fn fn;
  void *arg;
};

/* sm1's log hook: pass the call on to the C one in arg. */
void log_trampoline(void *arg, sm1::loglevel_t level,
                    const char *msg) noexcept {
  const log_hook_t *hook = static_cast<const log_hook_t *>(arg);
  hook->fn(hook->arg, static_cast<sm_loglevel_t>(level), msg);
}

/* The API's schema as sm2's, the missing values copied. */
std::vector<sm2::attribute_t>
schema_of(const std::vector<sm_attribute_t> &attrs) {
  std::vector<sm2::attribute_t> out;
  out.reserve(attrs.size());
  for (const sm_attribute_t &a : attrs) {
    out.push_back(
        {reinterpret_cast<const sm2::type_t *>(a.type), a.typmod, a.dropped,
         a.has_missing ? std::optional<std::string>(a.missing) : std::nullopt});
  }
  return out;
}

/* Fail with SM_ERR_INVALID: fn was handed NULL for arg. */
int null_arg(const char *fn, const char *arg) {
  return sm1::seterr(sm1::SM_ERR_INVALID,
                     std::string(fn) + ": " + arg + " is NULL");
}

/* One attribute of tuple under desc into *val and *isnull, for fn. */
int decode(const char *fn, const sm2::tup_desc_t &desc, const void *tuple,
           size_t len, size_t ordinal, sm_datum_t *val, bool *isnull) {
  if (!tuple || !val || !isnull) {
    return null_arg(fn, !tuple ? "tuple" : !val ? "val" : "isnull");
  }
  std::string_view v(static_cast<const char *>(tuple), len);
  return sm2::tup_decode(desc, v, ordinal, *val, *isnull);
}

/* Every attribute of tuple under desc into values and isnull, for fn. */
int deform(const char *fn, const sm2::tup_desc_t &desc, const void *tuple,
           size_t len, sm_datum_t *values, bool *isnull, size_t n) {
  if (!tuple || (n && !values) || (n && !isnull)) {
    return null_arg(fn, !tuple ? "tuple" : n && !values ? "values" : "isnull");
  }
  if (n != desc.schema.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       std::string(fn) + ": n is not the attribute count");
  }
  std::string_view v(static_cast<const char *>(tuple), len);
  return sm2::tup_deform(desc, v, std::span<sm2::datum_t>(values, n),
                         std::span<bool>(isnull, n));
}

/* A C view out-parameter pair, set from v. */
void view_out(std::string_view v, const void **data, size_t *len) {
  *data = v.data();
  *len = v.size();
}

} // namespace

namespace {

/* The sm2 cursor behind a C heap cursor. */
sm2::rel_scan_t *rel_scan_of(sm_rel_scan_t *scan) {
  return reinterpret_cast<sm2::rel_scan_t *>(scan);
}

/* The sm2 cursor behind a C index cursor. */
sm2::index_scan_t *idx_scan_of(sm_idx_scan_t *scan) {
  return reinterpret_cast<sm2::index_scan_t *>(scan);
}

/* True if bound b gives values but not their arrays. */
bool bad_bound(const sm_bound_t *b) {
  return b != nullptr && b->n != 0 && (!b->prefix || !b->isnull);
}

/* A C bound as sm2's, viewing its values and null flags. */
sm2::bound_t bound_of(const sm_bound_t &b) {
  return {std::span<const sm2::datum_t>(b.prefix, b.n),
          std::span<const bool>(b.isnull, b.n), b.inclusive};
}

} // namespace

namespace {

/* sm2's attribute as the API's, its missing value copied. */
sm_attribute_t api_attribute(const sm2::attribute_t &a) {
  return {reinterpret_cast<const sm_type_t *>(a.type), a.typmod, a.dropped,
          a.missing.has_value(), a.missing.value_or("")};
}

/*
 * sm3's definition d as the API's, which owns all it holds, with
 * each index's state as ss sees it. A pending index a worker is
 * filling is building.
 */
sm_table_def_t api_def(const sm3::session_t *ss, const sm3::table_def_t &d) {
  sm_table_def_t out{d.oid, d.nspoid, d.name, {}, {}, {}};
  out.version = sm3::cat_table_version(ss, d.oid);
  for (const sm3::column_def_t &c : d.columns) {
    out.columns.push_back({c.name, c.type->code, c.typmod, c.not_null,
                           c.dropped, c.default_expr, c.identity,
                           c.identity_seq});
  }
  for (const sm2::attribute_t &a : d.schema) {
    out.schema.push_back(api_attribute(a));
  }
  for (const sm3::index_def_t &x : d.indexes) {
    // Its state is kept by index, not in the definition.
    sm3::index_status_t st = sm3::cat_get_index_state(ss, x.oid);
    sm_index_state_t state =
        st.state == sm3::index_state_t::pending &&
                sm3::builder_building(sm3::session_cat(ss), x.oid)
            ? SM_INDEX_BUILDING
            : static_cast<sm_index_state_t>(st.state);
    sm_index_def_t i{
        x.oid, x.name, x.unique, x.key_attnums, {},     x.include_attnums,
        {},    state,  st.error, st.reason,     x.eval, x.reads};
    i.primary = x.primary;
    for (const sm2::key_column_t &k : x.key) {
      i.key.push_back({reinterpret_cast<const sm_type_t *>(k.type), k.typmod,
                       reinterpret_cast<const sm_collation_t *>(k.collation),
                       k.desc, k.nulls_first});
    }
    for (const sm2::attribute_t &a : x.include) {
      i.include.push_back(api_attribute(a));
    }
    out.indexes.push_back(std::move(i));
  }
  return out;
}

} // namespace

/* The last error raised on the calling thread. */
sm_errnum_t sm_errnum(void) { return sm1::errnum(); }

/* The last error's message. */
const char *sm_errmsg(void) { return sm1::errmsg(); }

/* The last error's code, named without its SM_ prefix. */
const char *sm_errname(void) { return sm1::errname(); }

/* 1 if the last error is retryable, else 0. */
int sm_retryable(void) { return sm1::retryable(); }

/* Clear the calling thread's error. */
void sm_reseterr(void) { sm1::reseterr(); }

/* Record the thread's error and return -1. */
int sm_seterr(sm_errnum_t code, const char *msg) {
  return sm1::seterr(code, name_of(msg));
}

/*
 * Install fn as the log hook, through log_trampoline. Each hook
 * installed is kept for the life of the process, since a thread
 * may still be calling the one it replaces.
 */
int sm_set_log(sm_log_fn fn, void *arg) {
  return guard(nullptr, [&] {
    if (fn == nullptr) {
      sm1::set_log(nullptr, nullptr);
      return 0;
    }
    log_hook_t *hook = new log_hook_t{fn, arg};
    // Install the hook process-wide.
    sm1::set_log(log_trampoline, hook);
    return 0;
  });
}

/* Make homedir a wisr home directory. */
int sm_init(const char *homedir) {
  return guard(nullptr, [&] { return sm1::init(name_of(homedir)); });
}

/*
 * Attach dbname's catalog to the builder, whose workers open sm1
 * sessions of their own on that database, and queue the indexes a
 * restart left pending: the builder is given its work, it does not
 * look for it.
 */
static int start_builder(sm_env_t *env, std::string_view dbname,
                         sm3::catalog_t *cat) {
  sm1::env_t *e = env->env;
  std::string name(dbname);
  if (sm3::builder_attach(cat, [e, name]() -> sm1::session_t * {
        sm1::session_t *s = nullptr;
        return sm1::opendb(e, name, s) == 0 ? s : nullptr;
      }) != 0) {
    return -1;
  }
  if (sm3::enqueue_pending_indexes(cat) != 0) {
    // Not half started: the caller is told it failed.
    keeping_error([&] { sm3::builder_detach(cat); });
    return -1;
  }
  return 0;
}

/*
 * Give cat the environment's evaluator, if it has one: the C
 * callback behind sm3's, shown the row's values as it was.
 */
static int set_evaluator(sm_env_t *env, sm3::catalog_t *cat) {
  if (!env->eval) {
    return 0;
  }
  sm_index_eval_fn fn = env->eval;
  void *ctx = env->eval_ctx;
  return sm3::cat_set_evaluator(
      cat,
      [fn, ctx](const sm3::table_def_t &table, const sm3::index_def_t &index,
                std::span<const sm2::datum_t> values,
                std::span<const bool> isnull, bool &belongs,
                std::vector<std::string> &out,
                std::span<bool> out_isnull) noexcept {
        return fn(ctx, table.oid, index.oid, index.eval, values.data(),
                  isnull.data(), values.size(), &belongs, out,
                  out_isnull.data());
      });
}

/* Attach homedir. */
int sm_attach(const char *homedir, sm_env_t **out) {
  return guard(nullptr, [&] {
    sm1::env_t *env = nullptr;
    // Attach: opens the root db and starts sm1's threads.
    if (sm1::attach(name_of(homedir), env) != 0) {
      return -1;
    }
    *out = new sm_env_t;
    (*out)->env = env;
    return 0;
  });
}

/*
 * Detach env. Once sm1 has, the sessions it closed and the
 * catalogs are freed with the handle.
 */
int sm_detach(sm_env_t *env, int force) {
  return guard(nullptr, [&] {
    if (env == nullptr) {
      return 0;
    }
    // Sessions still open: an unforced detach is refused by sm1,
    // and the builders stay as they are.
    if (!force && !env->sessions.empty()) {
      return sm1::detach(env->env, force);
    }
    // A forced detach abandons the open transactions; do it now, so
    // no worker is left waiting for a lock one of them holds.
    for (sm_session_t *s : env->sessions) {
      if (!sm1::in_doubt(s->sm1)) {
        keeping_error([&] { sm3::abort(s->sm3); });
      }
    }
    // Stop the builders first: their workers hold sm1 sessions,
    // which must be closed before the databases are.
    for (auto &[name, cat] : env->catalogs) {
      sm3::builder_detach(cat);
    }
    // Detach: closes the databases and stops sm1's threads.
    if (sm1::detach(env->env, force) != 0) {
      keeping_error([&] {
        for (auto &[name, cat] : env->catalogs) {
          start_builder(env, name, cat);
        }
      });
      return -1;
    }
    for (sm_session_t *s : env->sessions) {
      sm3::session_close(s->sm3);
      delete s;
    }
    for (auto &[name, cat] : env->catalogs) {
      sm3::cat_close(cat);
    }
    delete env;
    return 0;
  });
}

/* Give env its evaluator; see sm.h. */
int sm_set_index_evaluator(sm_env_t *env, sm_index_eval_fn fn, void *ctx) {
  return guard(nullptr, [&] {
    if (!env || !fn) {
      return null_arg("sm_set_index_evaluator", !env ? "env" : "fn");
    }
    std::lock_guard lock(env->mu);
    if (env->eval || !env->catalogs.empty()) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "sm_set_index_evaluator: one is set already, or a "
                         "database has been opened");
    }
    env->eval = fn;
    env->eval_ctx = ctx;
    return 0;
  });
}

/* Turn TOAST on or off for env, before it opens a database. */
int sm_set_toast(sm_env_t *env, int on) {
  return guard(nullptr, [&] {
    if (!env) {
      return null_arg("sm_set_toast", "env");
    }
    std::lock_guard lock(env->mu);
    if (!env->catalogs.empty()) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "sm_set_toast: a database has been opened");
    }
    env->toast = on != 0;
    return 0;
  });
}

/*
 * Create dbname, with opt's options or the defaults, and its
 * catalog; a database whose catalog cannot be made is removed.
 */
int sm_mkdb(sm_env_t *env, const char *dbname, const sm_options_t *opt) {
  return guard(nullptr, [&] {
    sm1::options_t o;
    if (opt != nullptr) {
      static_cast<sm_options_t &>(o) = *opt;
    }
    if (sm1::mkdb(env->env, name_of(dbname), &o) != 0) {
      return -1;
    }
    // Make its catalog, whole or not at all.
    if (sm3::cat_init(env->env, name_of(dbname), &o) != 0) {
      keeping_error([&] { sm1::rmdb(env->env, name_of(dbname)); });
      return -1;
    }
    return 0;
  });
}

/* Drop dbname, and the catalog loaded for it. */
int sm_rmdb(sm_env_t *env, const char *dbname) {
  return guard(nullptr, [&] {
    std::lock_guard lock(env->mu);
    auto it = env->catalogs.find(name_of(dbname));
    // Its builder's workers hold sessions on it: close them first.
    if (it != env->catalogs.end()) {
      sm3::builder_detach(it->second);
    }
    if (sm1::rmdb(env->env, name_of(dbname)) != 0) {
      if (it != env->catalogs.end()) {
        keeping_error([&] { start_builder(env, name_of(dbname), it->second); });
      }
      return -1;
    }
    if (it != env->catalogs.end()) {
      sm3::cat_close(it->second);
      env->catalogs.erase(it);
    }
    return 0;
  });
}

/* List the databases. */
int sm_lsdb(sm_env_t *env, std::vector<std::string> &names) {
  return guard(nullptr, [&] { return sm1::lsdb(env_of(env), names); });
}

/* The id sm_mkdb gave database dbname. */
int sm_dbid(sm_env_t *env, const char *dbname, uint64_t *out) {
  return guard(nullptr,
               [&] { return sm1::dbid(env_of(env), name_of(dbname), out); });
}

/*
 * Open a session on dbname, over its catalog: the first session
 * on the database loads it, in a transaction of its own.
 */
int sm_opendb(sm_env_t *env, const char *dbname, const sm_options_t *opt,
              sm_session_t **out) {
  return guard(nullptr, [&] {
    sm1::options_t o;
    if (opt != nullptr) {
      static_cast<sm_options_t &>(o) = *opt;
    }
    sm1::session_t *s = nullptr;
    // Open the session: the first one opens the database itself.
    if (sm1::opendb(env->env, name_of(dbname), s, &o) != 0) {
      return -1;
    }
    // Held while loading, so two first sessions load it once.
    std::lock_guard lock(env->mu);
    auto it = env->catalogs.find(name_of(dbname));
    sm3::catalog_t *cat = it == env->catalogs.end() ? nullptr : it->second;
    sm_session_t *session = nullptr;
    int rc = 0;
    if (cat == nullptr) {
      rc = sm3::cat_open(env->env, name_of(dbname), cat);
      // Build its indexes in the background from here on, and
      // queue what a restart left unbuilt; the evaluator first,
      // for a build may need it at once.
      if (rc == 0 && (set_evaluator(env, cat) != 0 ||
                      sm3::cat_set_toast(cat, env->toast) != 0 ||
                      start_builder(env, name_of(dbname), cat) != 0)) {
        sm3::cat_close(cat);
        cat = nullptr;
        rc = -1;
      }
      if (rc == 0) {
        env->catalogs.emplace(std::string(name_of(dbname)), cat);
      }
    }
    if (rc == 0) {
      session = new sm_session_t{env, s};
      rc = sm3::session_open(cat, s, session->sm3);
    }
    if (rc != 0) {
      delete session;
      keeping_error([&] { sm1::closedb(s); });
      return -1;
    }
    env->sessions.insert(session);
    *out = session;
    return 0;
  });
}

/* Close a session: its overlay, then sm1's. */
int sm_closedb(sm_session_t *session) {
  return guard(nullptr, [&] {
    if (session == nullptr) {
      return 0;
    }
    {
      std::lock_guard lock(session->env->mu);
      session->env->sessions.erase(session);
    }
    sm3::session_close(session->sm3);
    int rc = sm1::closedb(session->sm1);
    delete session;
    return rc;
  });
}

/* Begin a transaction at level, declared read-only or not. */
int sm_begin(sm_session_t *session, sm_xctlevel_t level, int read_only) {
  return guard(session, [&] {
    return sm3::begin(session->sm3, static_cast<sm1::xctlevel_t>(level),
                      read_only != 0);
  });
}

/* Commit the active transaction. */
int sm_commit(sm_session_t *session) {
  return guard(session, [&] { return sm3::commit(session->sm3); });
}

/* Abort the active transaction, or accept that it is already gone. */
int sm_abort(sm_session_t *session) {
  return guard(session, [&] {
    if (sm3::abort(session->sm3) != 0) {
      // The engine aborted it already, after a retryable error.
      return sm1::errnum() == sm1::SM_ERR_NOTRANS ? 0 : -1;
    }
    return 0;
  });
}

/* Begin a statement. */
int sm_stmt_begin(sm_session_t *session) {
  return guard(session, [&] { return sm3::stmt_begin(session->sm3); });
}

struct sm_stmt_t {
  sm3::stmt_t outer;
};

/* Begin a statement inside the running one, saving the outer. */
int sm_stmt_begin_inner(sm_session_t *session, sm_stmt_t **outer) {
  return guard(session, [&] {
    if (!session || !outer) {
      return null_arg("sm_stmt_begin_inner", !session ? "session" : "outer");
    }
    auto saved = std::make_unique<sm_stmt_t>();
    if (sm3::stmt_begin(session->sm3, saved->outer) != 0) {
      return -1;
    }
    *outer = saved.release();
    return 0;
  });
}

/* End an inner statement, resuming the outer, and free it. */
int sm_stmt_end(sm_session_t *session, sm_stmt_t *outer) {
  std::unique_ptr<sm_stmt_t> saved(outer);
  return guard(session, [&] {
    if (!session || !saved) {
      return null_arg("sm_stmt_end", !session ? "session" : "outer");
    }
    return sm3::stmt_end(session->sm3, saved->outer);
  });
}

/* QA only: make the session's lock requests fail where they would wait. */
int sm_set_qa_nowait(sm_session_t *session, bool on) {
  sm1::session_t *s = session_of(session);
  return guard(session, [&] { return sm1::set_qa_nowait(s, on); });
}

/* Give the session the host's number for it, for sm_waits_for. */
int sm_session_tag(sm_session_t *session, uint64_t tag) {
  return guard(nullptr, [&] {
    if (!session) {
      return null_arg("sm_session_tag", "session");
    }
    return sm1::set_tag(session_of(session), tag);
  });
}

/* Set how long the session waits before a deadlock check. */
int sm_set_deadlock_ms(sm_session_t *session, int ms) {
  return guard(nullptr, [&] {
    if (!session) {
      return null_arg("sm_set_deadlock_ms", "session");
    }
    return sm1::set_deadlock_ms(session_of(session), ms);
  });
}

/* The numbers of the sessions the session numbered tag waits for. */
int sm_waits_for(sm_env_t *env, uint64_t tag, std::vector<uint64_t> *out) {
  return guard(nullptr, [&] {
    if (!env) {
      return null_arg("sm_waits_for", "env");
    }
    return sm1::waits_for(env_of(env), tag, out);
  });
}

/* Set a savepoint and return its depth. */
int sm_savepoint(sm_session_t *session) {
  return guard(session, [&] { return sm3::savepoint(session->sm3); });
}

/* Roll back to the savepoint at depth. */
int sm_rollback(sm_session_t *session, int depth) {
  return guard(session, [&] { return sm3::rollback(session->sm3, depth); });
}

/* Forget the savepoint at depth. */
int sm_forget(sm_session_t *session, int depth) {
  return guard(session, [&] { return sm3::forget(session->sm3, depth); });
}

/* Prepare the active transaction as gid. */
int sm_prepare(sm_session_t *session, const char *gid) {
  return guard(session,
               [&] { return sm3::prepare(session->sm3, name_of(gid)); });
}

/* List the prepared transactions. */
int sm_list_prepared(sm_session_t *session, std::vector<sm_prepared_t> &out) {
  sm1::session_t *s = session_of(session);
  return guard(session, [&] {
    std::vector<sm1::prepared_t> list;
    if (sm1::list_prepared(s, list) != 0) {
      return -1;
    }
    out.clear();
    for (const sm1::prepared_t &p : list) {
      out.push_back({p.gid, p.prepared_at});
    }
    return 0;
  });
}

/* Commit the prepared transaction gid. */
int sm_commit_prepared(sm_session_t *session, const char *gid) {
  sm1::session_t *s = session_of(session);
  return guard(session, [&] { return sm1::commit_prepared(s, name_of(gid)); });
}

/* Abort the prepared transaction gid. */
int sm_abort_prepared(sm_session_t *session, const char *gid) {
  sm1::session_t *s = session_of(session);
  return guard(session, [&] { return sm1::abort_prepared(s, name_of(gid)); });
}

namespace {

/* Refuse, for fn, a sequence name sm3 keeps for its own. */
int reserved_seq(const char *fn, const char *name) {
  if (name && strncmp(name, "sm3.", 4) == 0) {
    return sm1::seterr(sm1::SM_ERR_INVALID, std::string(fn) + ": " + name +
                                                ": names beginning sm3. "
                                                "are sm3's own");
  }
  return 0;
}

} // namespace

/* Create a sequence. */
int sm_mkseq(sm_session_t *session, const char *name, uint64_t start,
             uint64_t increment) {
  sm1::session_t *s = session_of(session);
  return guard(session, [&] {
    if (reserved_seq("sm_mkseq", name) != 0) {
      return -1;
    }
    return sm1::mkseq(s, name_of(name), start, increment);
  });
}

/* Drop a sequence. */
int sm_rmseq(sm_session_t *session, const char *name) {
  sm1::session_t *s = session_of(session);
  return guard(session, [&] {
    if (reserved_seq("sm_rmseq", name) != 0) {
      return -1;
    }
    return sm1::rmseq(s, name_of(name));
  });
}

/* Hand out the sequence's next value. */
int sm_nextseq(sm_session_t *session, const char *name, uint64_t *out) {
  sm1::session_t *s = session_of(session);
  return guard(session, [&] {
    if (!out) {
      return sm1::seterr(sm1::SM_ERR_INVALID, "sm_nextseq: out is NULL");
    }
    if (reserved_seq("sm_nextseq", name) != 0) {
      return -1;
    }
    return sm1::nextseq(s, name_of(name), *out);
  });
}

/* The type with this frozen code, or NULL. */
const sm_type_t *sm_type(uint8_t code) {
  return reinterpret_cast<const sm_type_t *>(sm2::type_by_code(code));
}

/* A datum from a statistic value's bytes; see sm.h. */
sm_datum_t sm_datum_of_bytes(const sm_type_t *type, const std::string &bytes) {
  return sm3::datum_of_bytes(reinterpret_cast<const sm2::type_t *>(type),
                             bytes);
}

/* Parse a value's canonical text; see sm.h. */
int sm_type_in(const sm_type_t *type, int32_t typmod, const char *text,
               size_t len, sm_datum_t *val, std::string &buf) {
  return guard(nullptr, [&] {
    if (!type || (len && !text) || !val) {
      return null_arg("sm_type_in", !type          ? "type"
                                    : len && !text ? "text"
                                                   : "val");
    }
    const sm2::type_t *t = reinterpret_cast<const sm2::type_t *>(type);
    std::string datum;
    if (t->in(std::string_view(text, len), typmod, datum) != 0) {
      return -1;
    }
    *val = 0;
    buf.clear();
    // A by-value datum's bytes are the value itself.
    if (t->byval) {
      std::memcpy(val, datum.data(), datum.size());
      return 0;
    }
    // A by-reference one is bytes the value points at: a
    // variable-width datum's payload wrapped in its varlena.
    buf = t->len < 0 ? sm2::make_varlena(datum) : std::move(datum);
    *val = SM_DATUM_FROM_PTR(buf.data());
    return 0;
  });
}

/* Print a value as canonical text; see sm.h. */
int sm_type_out(const sm_type_t *type, int32_t typmod, sm_datum_t val,
                std::string &text) {
  return guard(nullptr, [&] {
    if (!type) {
      return null_arg("sm_type_out", "type");
    }
    const sm2::type_t *t = reinterpret_cast<const sm2::type_t *>(type);
    // The type's out takes a variable-width datum's payload alone.
    std::string_view bytes;
    if (sm2::datum_bytes(t, typmod, val, bytes) != 0) {
      return -1;
    }
    if (t->len < 0) {
      bytes = sm2::varlena_payload(bytes);
    }
    return t->out(bytes, typmod, text);
  });
}

/* Open a collation. */
int sm_collation_open(const char *locale, bool deterministic,
                      const char *expected_version, sm_collation_t **out) {
  return guard(nullptr, [&] {
    sm2::collation_t *c = nullptr;
    if (sm2::collation_open(name_of(locale), deterministic,
                            name_of(expected_version), c) != 0) {
      return -1;
    }
    *out = reinterpret_cast<sm_collation_t *>(c);
    return 0;
  });
}

/* Close a collation. */
void sm_collation_close(sm_collation_t *collation) {
  sm2::collation_close(reinterpret_cast<sm2::collation_t *>(collation));
}

/*
 * The collation's version. sm2 keeps it as a std::string, or
 * hands back a literal for byte order, so the view ends in NUL.
 */
const char *sm_collation_version(const sm_collation_t *collation) {
  return sm2::collation_version(
             reinterpret_cast<const sm2::collation_t *>(collation))
      .data();
}

/*
 * The next row. A cursor call writes nothing, so it passes no
 * session: there is no half-done change for an abort to undo.
 */
int sm_rel_scannxt(sm_rel_scan_t *scan, uint64_t *rowno, const void **tuple,
                   size_t *len) {
  return guard(nullptr, [&] {
    std::string_view v;
    int rc = sm2::rel_scannxt(rel_scan_of(scan), *rowno, v);
    if (rc == 0) {
      view_out(v, tuple, len);
    }
    return rc;
  });
}

/* Move a heap cursor forward to rowno. */
int sm_rel_scan_seek(sm_rel_scan_t *scan, uint64_t rowno) {
  return guard(nullptr,
               [&] { return sm2::rel_scan_seek(rel_scan_of(scan), rowno); });
}

/* Close a heap cursor. */
void sm_rel_scanend(sm_rel_scan_t *scan) {
  sm2::rel_scanend(rel_scan_of(scan));
}

/* Encode values and isnull into a tuple. */
int sm_tup_encode(const std::vector<sm_attribute_t> &schema,
                  const sm_datum_t *values, const bool *isnull,
                  std::string &tuple) {
  return guard(nullptr, [&] {
    size_t natts = schema.size();
    if ((natts && !values) || (natts && !isnull)) {
      return null_arg("sm_tup_encode", !values ? "values" : "isnull");
    }
    auto s = schema_of(schema);
    return sm2::tup_encode(s, std::span<const sm2::datum_t>(values, natts),
                           std::span<const bool>(isnull, natts), tuple);
  });
}

/* The next entry; no session, as sm_rel_scannxt. */
int sm_idx_scannxt(sm_idx_scan_t *scan, uint64_t *rowno, bool *cut,
                   const void **value, size_t *len) {
  return guard(nullptr, [&] {
    std::string_view v;
    int rc = sm2::idx_scannxt(idx_scan_of(scan), *rowno, *cut, v);
    if (rc == 0) {
      view_out(v, value, len);
    }
    return rc;
  });
}

/* Move an index cursor forward to lo. */
int sm_idx_scan_seek(sm_idx_scan_t *scan, const sm_bound_t *lo) {
  return guard(nullptr, [&] {
    if (!lo || bad_bound(lo)) {
      return null_arg("sm_idx_scan_seek", !lo ? "lo" : "lo's prefix or isnull");
    }
    return sm2::idx_scan_seek(idx_scan_of(scan), bound_of(*lo));
  });
}

/* The key columns of an index cursor's last entry; no session. */
int sm_idx_scan_key(sm_idx_scan_t *scan, sm_datum_t *values, bool *isnull,
                    size_t n) {
  return guard(nullptr, [&] {
    if (!scan || (n && (!values || !isnull))) {
      return null_arg("sm_idx_scan_key", !scan     ? "scan"
                                         : !values ? "values"
                                                   : "isnull");
    }
    return sm2::idx_scan_key(idx_scan_of(scan),
                             std::span<sm2::datum_t>(values, n),
                             std::span<bool>(isnull, n));
  });
}

/* Close an index cursor. */
void sm_idx_scanend(sm_idx_scan_t *scan) {
  sm2::idx_scanend(idx_scan_of(scan));
}

/* A table's oid by name, as the session sees it; 1 if none. */
/* The namespaces' epoch: 0 for a session with no catalog. */
uint64_t sm_cat_namespace_epoch(sm_session_t *session) {
  return session && session->sm3 ? sm3::cat_namespace_epoch(session->sm3) : 0;
}

/* Create a namespace. */
int sm_cat_create_namespace(sm_session_t *session, const char *name,
                            uint64_t *oid) {
  return guard(session, [&] {
    if (!oid) {
      return null_arg("sm_cat_create_namespace", "oid");
    }
    return sm3::create_namespace(session->sm3, name_of(name), *oid);
  });
}

/* Drop a namespace. */
int sm_cat_drop_namespace(sm_session_t *session, uint64_t oid) {
  return guard(session, [&] { return sm3::drop_namespace(session->sm3, oid); });
}

/* A namespace's oid by its name: 0, or 1 if there is none. */
int sm_cat_namespace_oid(sm_session_t *session, const char *name,
                         uint64_t *oid) {
  return guard(session, [&] {
    if (!oid) {
      return null_arg("sm_cat_namespace_oid", "oid");
    }
    return sm3::cat_get_namespace(session->sm3, name_of(name), *oid) ? 0 : 1;
  });
}

/* The name of the namespace oid: 0, or 1 if there is none. */
int sm_cat_namespace_name(sm_session_t *session, uint64_t oid,
                          std::string &name) {
  return guard(session, [&] {
    return sm3::cat_get_namespace_name(session->sm3, oid, name) ? 0 : 1;
  });
}

/* The namespaces' names, sorted. */
int sm_cat_list_namespaces(sm_session_t *session,
                           std::vector<std::string> &names) {
  return guard(session, [&] {
    names = sm3::cat_list_namespaces(session->sm3);
    return 0;
  });
}

int sm_cat_table_oid(sm_session_t *session, uint64_t nspoid, const char *name,
                     uint64_t *oid) {
  return guard(session, [&] {
    if (!oid) {
      return null_arg("sm_cat_table_oid", "oid");
    }
    auto def = sm3::cat_get_table(session->sm3, nspoid, name_of(name));
    if (!def) {
      return 1;
    }
    *oid = def->oid;
    return 0;
  });
}

/* The table names in a namespace, as the session sees them. */
int sm_cat_list_tables(sm_session_t *session, uint64_t nspoid,
                       std::vector<std::string> &names) {
  return guard(session, [&] {
    names = sm3::cat_list_tables(session->sm3, nspoid);
    return 0;
  });
}

/* A table's definition by name, with no lock; 1 if none. */
int sm_cat_table_def(sm_session_t *session, uint64_t nspoid, const char *name,
                     sm_table_def_t &out) {
  return guard(session, [&] {
    std::shared_ptr<const sm3::table_def_t> d =
        sm3::cat_get_table(session->sm3, nspoid, name_of(name));
    if (!d) {
      return 1;
    }
    out = api_def(session->sm3, *d);
    return 0;
  });
}

/* A table's definition by oid, with no lock; 1 if none. */
int sm_cat_table_def_oid(sm_session_t *session, uint64_t oid,
                         sm_table_def_t &out) {
  return guard(session, [&] {
    std::shared_ptr<const sm3::table_def_t> d =
        sm3::cat_get_table(session->sm3, oid);
    if (!d) {
      return 1;
    }
    out = api_def(session->sm3, *d);
    return 0;
  });
}

/* A table's version by oid, with no lock; 1 if none. */
int sm_cat_table_version_oid(sm_session_t *session, uint64_t oid,
                             uint64_t *version) {
  return guard(session, [&] {
    if (!version) {
      return null_arg("sm_cat_table_version_oid", "version");
    }
    if (!sm3::cat_get_table(session->sm3, oid)) {
      return 1;
    }
    *version = sm3::cat_table_version(session->sm3, oid);
    return 0;
  });
}

/* An index's oid and its table's by name; 1 if none. */
int sm_cat_index_oid(sm_session_t *session, uint64_t nspoid, const char *name,
                     uint64_t *table, uint64_t *index) {
  return guard(session, [&] {
    if (!table || !index) {
      return null_arg("sm_cat_index_oid", !table ? "table" : "index");
    }
    return sm3::cat_get_index(session->sm3, nspoid, name_of(name), *table,
                              *index)
               ? 0
               : 1;
  });
}

/* Create a sequence in the catalog. */
int sm_cat_create_sequence(sm_session_t *session, uint64_t nspoid,
                           const char *name, uint64_t start, uint64_t increment,
                           uint64_t *oid) {
  return guard(session, [&] {
    if (!oid) {
      return null_arg("sm_cat_create_sequence", "oid");
    }
    return sm3::create_sequence(session->sm3, nspoid, name_of(name), start,
                                increment, *oid);
  });
}

/* Drop a sequence from the catalog. */
int sm_cat_drop_sequence(sm_session_t *session, uint64_t oid) {
  return guard(session, [&] { return sm3::drop_sequence(session->sm3, oid); });
}

/* Make a table's column own a sequence, or none. */
int sm_cat_own_sequence(sm_session_t *session, uint64_t oid, uint64_t table,
                        int32_t attnum) {
  return guard(session, [&] {
    return sm3::own_sequence(session->sm3, oid, table, attnum);
  });
}

/* A sequence's oid by name: 0, or 1 if there is none. */
int sm_cat_sequence_oid(sm_session_t *session, uint64_t nspoid,
                        const char *name, uint64_t *oid) {
  return guard(session, [&] {
    if (!oid) {
      return null_arg("sm_cat_sequence_oid", "oid");
    }
    return sm3::cat_get_sequence(session->sm3, nspoid, name_of(name), *oid);
  });
}

/* A sequence's namespace and name by its oid: 0, or 1 if none. */
int sm_cat_sequence_name(sm_session_t *session, uint64_t oid, uint64_t *nspoid,
                         std::string &name) {
  return guard(session, [&] {
    if (!nspoid) {
      return null_arg("sm_cat_sequence_name", "nspoid");
    }
    return sm3::cat_get_sequence_name(session->sm3, oid, *nspoid, name);
  });
}

/* A sequence's definition. */
int sm_cat_sequence_def(sm_session_t *session, uint64_t oid,
                        sm_sequence_def_t *out) {
  return guard(session, [&] {
    if (!out) {
      return null_arg("sm_cat_sequence_def", "out");
    }
    sm3::sequence_def_t d;
    int rc = sm3::cat_get_sequence_def(session->sm3, oid, d);
    if (rc != 0) {
      return rc;
    }
    *out = {d.nspoid,
            d.name,
            d.start,
            d.increment,
            d.owner.value_or(0),
            d.owner ? d.owner_attnum : -1};
    return 0;
  });
}

/* The sequences' names in a namespace, sorted. */
int sm_cat_list_sequences(sm_session_t *session, uint64_t nspoid,
                          std::vector<std::string> &names) {
  return guard(session, [&] {
    return sm3::cat_list_sequences(session->sm3, nspoid, names);
  });
}

/* A sequence's next value. */
int sm_cat_nextval(sm_session_t *session, uint64_t oid, uint64_t *out) {
  return guard(session, [&] {
    if (!out) {
      return null_arg("sm_cat_nextval", "out");
    }
    return sm3::nextval(session->sm3, oid, *out);
  });
}

/* Wait for an index's build to end. */
int sm_cat_index_wait(sm_session_t *session, uint64_t table, uint64_t index) {
  return guard(nullptr, [&] {
    if (!session) {
      return null_arg("sm_cat_index_wait", "session");
    }
    return sm3::builder_wait(session->sm3, table, index);
  });
}

/*
 * A table as the wrapper holds it: the session, for the
 * catch-all's abort; sm3's handle; its definition as the API gives
 * it; and each index's value layout, built on first use.
 */
struct sm_tbl_t {
  sm_session_t *session;
  sm3::table_t *table = nullptr;
  sm_table_def_t def;
  std::unordered_map<uint64_t, sm2::tup_desc_t> value_descs;
  sm3::loader_t *loader = nullptr; /* the bulk load begun on the table */
};

namespace {

/* Check a write's row arguments, for fn; n must be the column count. */
int check_row(const char *fn, const sm_tbl_t *tbl, const sm_datum_t *values,
              const bool *isnull, size_t n) {
  if (!tbl || (n && !values) || (n && !isnull)) {
    return null_arg(fn, !tbl ? "tbl" : n && !values ? "values" : "isnull");
  }
  if (n != tbl->def.columns.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       std::string(fn) + ": n is not the attribute count");
  }
  return 0;
}

/* The value layout of tbl's index oid, or nullptr with the error set. */
const sm2::tup_desc_t *value_desc(sm_tbl_t *tbl, uint64_t index) {
  auto it = tbl->value_descs.find(index);
  if (it != tbl->value_descs.end()) {
    return &it->second;
  }
  std::span<const sm2::attribute_t> schema =
      sm3::tbl_idx_value_schema(tbl->table, index);
  if (schema.empty()) {
    sm1::seterr(sm1::SM_ERR_NOTFOUND,
                "table: no index " + std::to_string(index));
    return nullptr;
  }
  return &tbl->value_descs.emplace(index, sm2::tup_desc_t(schema))
              .first->second;
}

} // namespace

/* Lock a table and open its heap and indexes. */
int sm_tbl_open(sm_session_t *session, uint64_t oid, sm_lockmode_t mode,
                sm_tbl_t **out) {
  return guard(session, [&] {
    if (!session || !out) {
      return null_arg("sm_tbl_open", !session ? "session" : "out");
    }
    auto t = std::make_unique<sm_tbl_t>();
    t->session = session;
    if (sm3::tbl_open(session->sm3, oid, mode, t->table) != 0) {
      return -1;
    }
    t->def = api_def(session->sm3, sm3::tbl_def(t->table));
    *out = t.release();
    return 0;
  });
}

/* Open a table if its table-oid lock is free now: 0, 1, or -1. */
int sm_tbl_tryopen(sm_session_t *session, uint64_t oid, sm_lockmode_t mode,
                   sm_tbl_t **out) {
  return guard(session, [&] {
    if (!session || !out) {
      return null_arg("sm_tbl_tryopen", !session ? "session" : "out");
    }
    auto t = std::make_unique<sm_tbl_t>();
    t->session = session;
    int rc = sm3::tbl_tryopen(session->sm3, oid, mode, t->table);
    if (rc != 0) {
      return rc;
    }
    t->def = api_def(session->sm3, sm3::tbl_def(t->table));
    *out = t.release();
    return 0;
  });
}

/* Close a table; NULL is a no-op. */
void sm_tbl_close(sm_tbl_t *tbl) {
  if (tbl) {
    // A load never ended: its transaction's end has ended it.
    sm3::tbl_load_free(tbl->loader);
    sm3::tbl_close(tbl->table);
    delete tbl;
  }
}

/* The table's definition. */
const sm_table_def_t *sm_tbl_def(const sm_tbl_t *tbl) { return &tbl->def; }

/* Insert a row and its index entries. */
int sm_tbl_insert(sm_tbl_t *tbl, const sm_datum_t *values, const bool *isnull,
                  size_t n, uint64_t *rowno) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (check_row("sm_tbl_insert", tbl, values, isnull, n) != 0) {
      return -1;
    }
    if (!rowno) {
      return null_arg("sm_tbl_insert", "rowno");
    }
    return sm3::tbl_insert(tbl->table, {values, n}, {isnull, n}, *rowno);
  });
}

/* Insert a row unless an arbiter index has its key. */
int sm_tbl_insert_arbiter(sm_tbl_t *tbl, const sm_datum_t *values,
                          const bool *isnull, size_t n,
                          const uint64_t *arbiters, size_t narbiters,
                          uint64_t *rowno) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (check_row("sm_tbl_insert_arbiter", tbl, values, isnull, n) != 0) {
      return -1;
    }
    if (!rowno || (narbiters && !arbiters)) {
      return null_arg("sm_tbl_insert_arbiter", !rowno ? "rowno" : "arbiters");
    }
    return sm3::tbl_insert_arbiter(tbl->table, {values, n}, {isnull, n},
                                   {arbiters, narbiters}, *rowno);
  });
}

/* Begin a bulk load into the table. */
int sm_tbl_load_begin(sm_tbl_t *tbl) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl) {
      return null_arg("sm_tbl_load_begin", "tbl");
    }
    if (tbl->loader) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "sm_tbl_load_begin: a load is begun already");
    }
    return sm3::tbl_load_open(tbl->table, tbl->loader);
  });
}

/* Hand a row to the table's bulk load. */
int sm_tbl_load_add(sm_tbl_t *tbl, const sm_datum_t *values, const bool *isnull,
                    size_t n, uint64_t *rowno) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (check_row("sm_tbl_load_add", tbl, values, isnull, n) != 0) {
      return -1;
    }
    if (!rowno || !tbl->loader) {
      return !rowno ? null_arg("sm_tbl_load_add", "rowno")
                    : sm1::seterr(sm1::SM_ERR_INVALID,
                                  "sm_tbl_load_add: no load is begun");
    }
    return sm3::tbl_load_add(tbl->loader, {values, n}, {isnull, n}, *rowno);
  });
}

/* Finish the table's bulk load. */
int sm_tbl_load_end(sm_tbl_t *tbl) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl || !tbl->loader) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "sm_tbl_load_end: no load is begun");
    }
    // Freed by the close, whatever it returns.
    sm3::loader_t *loader = tbl->loader;
    tbl->loader = nullptr;
    return sm3::tbl_load_close(loader);
  });
}

/* Replace a row and fix its index entries. */
int sm_tbl_update(sm_tbl_t *tbl, uint64_t rowno, const sm_datum_t *values,
                  const bool *isnull, size_t n, std::string *old) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (check_row("sm_tbl_update", tbl, values, isnull, n) != 0) {
      return -1;
    }
    return sm3::tbl_update(tbl->table, rowno, {values, n}, {isnull, n}, old);
  });
}

/* Delete a row and its index entries. */
int sm_tbl_delete(sm_tbl_t *tbl, uint64_t rowno, std::string *old) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl) {
      return null_arg("sm_tbl_delete", "tbl");
    }
    return sm3::tbl_delete(tbl->table, rowno, old);
  });
}

/* Read a row. */
int sm_tbl_read(sm_tbl_t *tbl, uint64_t rowno, const void **tuple,
                size_t *len) {
  return guard(tbl->session, [&] {
    std::string_view v;
    int rc = sm3::tbl_read(tbl->table, rowno, v);
    if (rc == 0) {
      view_out(v, tuple, len);
    }
    return rc;
  });
}

/* Read the newest committed version of a row. */
int sm_tbl_read_latest(sm_tbl_t *tbl, uint64_t rowno, const void **tuple,
                       size_t *len) {
  return guard(tbl->session, [&] {
    std::string_view v;
    int rc = sm3::tbl_read_latest(tbl->table, rowno, v);
    if (rc == 0) {
      view_out(v, tuple, len);
    }
    return rc;
  });
}

/* Lock a row without writing it. */
int sm_tbl_lock(sm_tbl_t *tbl, uint64_t rowno, sm_lockmode_t mode) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl) {
      return null_arg("sm_tbl_lock", "tbl");
    }
    return sm3::tbl_lock(tbl->table, rowno, mode);
  });
}

/* Lock a row if it is free now. */
int sm_tbl_trylock(sm_tbl_t *tbl, uint64_t rowno, sm_lockmode_t mode) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl) {
      return null_arg("sm_tbl_trylock", "tbl");
    }
    return sm3::tbl_trylock(tbl->table, rowno, mode);
  });
}

/* Empty a table, and refresh the handle's definition. */
int sm_tbl_truncate(sm_tbl_t *tbl) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl) {
      return null_arg("sm_tbl_truncate", "tbl");
    }
    if (sm3::tbl_truncate(tbl->table) != 0) {
      return -1;
    }
    // The table now lives in another store.
    tbl->def = api_def(tbl->session->sm3, sm3::tbl_def(tbl->table));
    return 0;
  });
}

/* Open a cursor over a table's rows. */
int sm_tbl_scan(sm_tbl_t *tbl, uint64_t first, uint64_t last,
                sm_rel_scan_t **out) {
  return guard(tbl->session, [&] {
    sm2::rel_scan_t *scan = nullptr;
    if (sm3::tbl_scan(tbl->table, first, last, scan) != 0) {
      return -1;
    }
    *out = reinterpret_cast<sm_rel_scan_t *>(scan);
    return 0;
  });
}

/* Every attribute of a row's tuple. */
int sm_tbl_deform(sm_tbl_t *tbl, const void *tuple, size_t len,
                  sm_datum_t *values, bool *isnull, size_t n) {
  return guard(nullptr, [&] {
    if (!tbl) {
      return null_arg("sm_tbl_deform", "tbl");
    }
    return deform("sm_tbl_deform", sm3::tbl_def(tbl->table).desc, tuple, len,
                  values, isnull, n);
  });
}

/* One attribute of a row's tuple. */
int sm_tbl_decode(sm_tbl_t *tbl, const void *tuple, size_t len, size_t ordinal,
                  sm_datum_t *val, bool *isnull) {
  return guard(nullptr, [&] {
    if (!tbl) {
      return null_arg("sm_tbl_decode", "tbl");
    }
    return decode("sm_tbl_decode", sm3::tbl_def(tbl->table).desc, tuple, len,
                  ordinal, val, isnull);
  });
}

/* Read the value a TOAST pointer of the table names. */
int sm_tbl_fetch(sm_tbl_t *tbl, sm_datum_t val, std::string *out) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl || !val || !out) {
      return null_arg("sm_tbl_fetch", !tbl ? "tbl" : !val ? "val" : "out");
    }
    const char *v = static_cast<const char *>(sm2::datum_to_ptr(val));
    if (v[0] != sm2::varlena_toast) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "sm_tbl_fetch: not a TOAST pointer");
    }
    return sm3::tbl_fetch(tbl->table, {v, sm2::toast_ptr_len}, *out);
  });
}

/* Read the value a TOAST pointer names, by its store alone. */
int sm_toast_fetch(sm_session_t *session, sm_datum_t val, std::string *out) {
  return guard(session, [&] {
    if (!session || !val || !out) {
      return null_arg("sm_toast_fetch", !session ? "session"
                                        : !val   ? "val"
                                                 : "out");
    }
    const char *v = static_cast<const char *>(sm2::datum_to_ptr(val));
    if (v[0] != sm2::varlena_toast) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "sm_toast_fetch: not a TOAST pointer");
    }
    return sm2::toast_fetch(session_of(session), {v, sm2::toast_ptr_len}, *out);
  });
}

/* Open a cursor over one of a table's indexes. */
int sm_tbl_idx_scan(sm_tbl_t *tbl, uint64_t index, const sm_bound_t *lo,
                    const sm_bound_t *hi, bool *widened, sm_idx_scan_t **out) {
  return guard(tbl->session, [&] {
    if (bad_bound(lo) || bad_bound(hi)) {
      return null_arg("sm_tbl_idx_scan", "a bound's prefix or isnull");
    }
    sm2::bound_t lo_b, hi_b;
    if (lo != nullptr) {
      lo_b = bound_of(*lo);
    }
    if (hi != nullptr) {
      hi_b = bound_of(*hi);
    }
    sm2::index_scan_t *scan = nullptr;
    if (sm3::tbl_idx_scan(tbl->table, index, lo != nullptr ? &lo_b : nullptr,
                          hi != nullptr ? &hi_b : nullptr, scan,
                          widened) != 0) {
      return -1;
    }
    *out = reinterpret_cast<sm_idx_scan_t *>(scan);
    return 0;
  });
}

/* Whether the table's session may scan one of its indexes now. */
bool sm_tbl_index_usable(const sm_tbl_t *tbl, uint64_t index) {
  return sm3::tbl_index_usable(tbl->table, index);
}

/* The attribute count of an entry's value in one of a table's indexes. */
size_t sm_tbl_idx_ncols(const sm_tbl_t *tbl, uint64_t index) {
  return sm3::tbl_idx_value_schema(tbl->table, index).size();
}

/* Every attribute of an entry's value in one of a table's indexes. */
int sm_tbl_idx_deform(sm_tbl_t *tbl, uint64_t index, const void *value,
                      size_t len, sm_datum_t *values, bool *isnull, size_t n) {
  return guard(nullptr, [&] {
    if (!tbl) {
      return null_arg("sm_tbl_idx_deform", "tbl");
    }
    const sm2::tup_desc_t *desc = value_desc(tbl, index);
    if (!desc) {
      return -1;
    }
    return deform("sm_tbl_idx_deform", *desc, value, len, values, isnull, n);
  });
}

/* The heap's bytes, live. */
int sm_tbl_size(sm_tbl_t *tbl, uint64_t *disk, uint64_t *mem) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl || !disk || !mem) {
      return null_arg("sm_tbl_size", !tbl ? "tbl" : !disk ? "disk" : "mem");
    }
    return sm3::tbl_size(tbl->table, *disk, *mem);
  });
}

/* An index's bytes, live. */
int sm_tbl_idx_size(sm_tbl_t *tbl, uint64_t index, uint64_t *disk,
                    uint64_t *mem) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl || !disk || !mem) {
      return null_arg("sm_tbl_idx_size", !tbl ? "tbl" : !disk ? "disk" : "mem");
    }
    return sm3::tbl_idx_size(tbl->table, index, *disk, *mem);
  });
}

/* The last rowno handed out, live. */
int sm_tbl_rowno_limit(sm_tbl_t *tbl, uint64_t *n) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl || !n) {
      return null_arg("sm_tbl_rowno_limit", !tbl ? "tbl" : "n");
    }
    return sm3::tbl_rowno_limit(tbl->table, *n);
  });
}

/* The sorted runs a lookup may visit, live. */
int sm_tbl_probes(sm_tbl_t *tbl, uint64_t *n) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl || !n) {
      return null_arg("sm_tbl_probes", !tbl ? "tbl" : "n");
    }
    return sm3::tbl_probes(tbl->table, *n);
  });
}

/* Analyze a table. */
int sm_tbl_analyze(sm_tbl_t *tbl, int32_t target) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl) {
      return null_arg("sm_tbl_analyze", "tbl");
    }
    return sm3::tbl_analyze(tbl->table, target);
  });
}

/* Analyze a table if no other analyze of it holds it: 0, 1, or -1. */
int sm_tbl_tryanalyze(sm_tbl_t *tbl, int32_t target) {
  return guard(tbl ? tbl->session : nullptr, [&] {
    if (!tbl) {
      return null_arg("sm_tbl_tryanalyze", "tbl");
    }
    return sm3::tbl_tryanalyze(tbl->table, target);
  });
}

namespace {

/*
 * The statistics in sm.h's form, each built once from sm3's and shared:
 * by the address of sm3's, which a view keeps alive, so the address is
 * never another's while its view is held.  An expired view is built again.
 */
std::mutex g_stats_mu;
std::unordered_map<const sm3::stats_t *, std::weak_ptr<const sm_stats_t>>
    g_stats_views;

/* sm3's statistics in sm.h's form, with what they were built from. */
struct stats_view_t {
  std::shared_ptr<const sm3::stats_t> from;
  sm_stats_t view;
};

/* The view of st, built now if no holder has one. */
std::shared_ptr<const sm_stats_t>
stats_view(std::shared_ptr<const sm3::stats_t> st) {
  // One build at a time, so two callers share the first's.
  std::lock_guard<std::mutex> lock(g_stats_mu);
  auto it = g_stats_views.find(st.get());
  if (it != g_stats_views.end()) {
    if (auto held = it->second.lock()) {
      return held;
    }
  }
  auto v = std::make_shared<stats_view_t>();
  v->view = {st->target,
             st->run_length,
             st->rows_sampled,
             st->rowno_limit,
             st->live_rows,
             st->heap_disk,
             st->heap_mem,
             st->avg_row_bytes,
             {},
             {}};
  for (const sm3::stats_index_t &x : st->indexes) {
    v->view.indexes.push_back({x.oid, x.disk, x.mem, x.entries, x.belongs});
  }
  for (const sm3::stat_t &c : st->stats) {
    v->view.stats.push_back(
        {c.index.has_value(), c.index.value_or(0), c.position, c.null_frac,
         c.width, c.distinct, c.correlation.has_value(),
         c.correlation.value_or(0), c.mcv, c.mcv_freq, c.bounds});
  }
  v->from = std::move(st);
  std::shared_ptr<const sm_stats_t> out(v, &v->view);
  // Views no one holds any more go, so the map stays small.
  for (auto e = g_stats_views.begin(); e != g_stats_views.end();) {
    e = e->second.expired() ? g_stats_views.erase(e) : std::next(e);
  }
  g_stats_views[v->from.get()] = out;
  return out;
}

} // namespace

/* A table's statistics: 0, or 1 if never analyzed. */
int sm_tbl_stats(sm_session_t *session, uint64_t oid,
                 std::shared_ptr<const sm_stats_t> &out) {
  return guard(session, [&] {
    std::shared_ptr<const sm3::stats_t> st;
    if (sm3::cat_get_stats(session->sm3, oid, st) != 0) {
      return -1;
    }
    if (!st) {
      out = nullptr;
      return 1;
    }
    out = stats_view(std::move(st));
    return 0;
  });
}

/* Create a table in the session's transaction. */
int sm_cat_create_table(sm_session_t *session, uint64_t nspoid,
                        const char *name,
                        const std::vector<sm_column_spec_t> &columns,
                        uint64_t *oid) {
  return guard(session, [&] {
    if (!oid) {
      return null_arg("sm_cat_create_table", "oid");
    }
    std::vector<sm3::column_spec_t> cols;
    for (const sm_column_spec_t &c : columns) {
      cols.push_back(
          {std::string(name_of(c.name)), c.type_code, c.typmod, c.not_null,
           c.default_expr ? std::optional<std::string>(c.default_expr)
                          : std::nullopt,
           c.identity, c.identity_seq});
    }
    return sm3::create_table(session->sm3, nspoid, name_of(name), cols, *oid);
  });
}

/* Create a temporary table in the session's transaction. */
int sm_cat_create_temp_table(sm_session_t *session, const char *name,
                             const std::vector<sm_column_spec_t> &columns,
                             uint64_t *oid) {
  return guard(session, [&] {
    if (!oid) {
      return null_arg("sm_cat_create_temp_table", "oid");
    }
    std::vector<sm3::column_spec_t> cols;
    for (const sm_column_spec_t &c : columns) {
      cols.push_back(
          {std::string(name_of(c.name)), c.type_code, c.typmod, c.not_null,
           c.default_expr ? std::optional<std::string>(c.default_expr)
                          : std::nullopt,
           c.identity, c.identity_seq});
    }
    return sm3::create_temp_table(session->sm3, name_of(name), cols, *oid);
  });
}

/* Drop a table in the session's transaction. */
int sm_cat_drop_table(sm_session_t *session, uint64_t oid) {
  return guard(session, [&] { return sm3::drop_table(session->sm3, oid); });
}

/* Rename a table in the session's transaction. */
int sm_cat_rename_table(sm_session_t *session, uint64_t oid, const char *name) {
  return guard(session, [&] {
    return sm3::rename_table(session->sm3, oid, name_of(name));
  });
}

/* Append a column in the session's transaction. */
int sm_cat_add_column(sm_session_t *session, uint64_t oid,
                      const sm_column_spec_t &col, const sm_datum_t *missing) {
  return guard(session, [&] {
    // The missing value as sm2 holds it: the datum's bytes.
    std::optional<std::string_view> bytes;
    if (missing) {
      const sm2::type_t *type = sm2::type_by_code(col.type_code);
      std::string_view v;
      if (!type) {
        return sm1::seterr(sm1::SM_ERR_INVALID,
                           "add column: a type code sm2 lacks");
      }
      if (sm2::datum_bytes(type, col.typmod, *missing, v) != 0) {
        return -1;
      }
      bytes = v;
    }
    sm3::column_spec_t c{std::string(name_of(col.name)),
                         col.type_code,
                         col.typmod,
                         col.not_null,
                         col.default_expr
                             ? std::optional<std::string>(col.default_expr)
                             : std::nullopt,
                         col.identity,
                         col.identity_seq};
    return sm3::add_column(session->sm3, oid, c, bytes);
  });
}

/* Rename a column in the session's transaction. */
int sm_cat_rename_column(sm_session_t *session, uint64_t oid, const char *name,
                         const char *new_name) {
  return guard(session, [&] {
    return sm3::rename_column(session->sm3, oid, name_of(name),
                              name_of(new_name));
  });
}

/* Drop a column in the session's transaction. */
int sm_cat_drop_column(sm_session_t *session, uint64_t oid, const char *name) {
  return guard(session, [&] {
    return sm3::drop_column(session->sm3, oid, name_of(name));
  });
}

/* Create an index in the session's transaction. */
int sm_cat_create_index(sm_session_t *session, uint64_t table, const char *name,
                        const std::vector<sm_index_key_spec_t> &keys,
                        const std::vector<int32_t> &includes, bool unique,
                        const std::string &eval,
                        const std::vector<int32_t> &reads, uint64_t *oid,
                        bool primary) {
  return guard(session, [&] {
    if (!oid) {
      return null_arg("sm_cat_create_index", "oid");
    }
    std::vector<sm3::index_key_spec_t> k;
    for (const sm_index_key_spec_t &key : keys) {
      k.push_back({key.attnum, key.desc, key.nulls_first, std::nullopt,
                   key.type_code, key.typmod});
    }
    return sm3::create_index(session->sm3, table, name_of(name), k, includes,
                             unique, eval, reads, *oid, primary);
  });
}

/* Drop an index in the session's transaction. */
int sm_cat_drop_index(sm_session_t *session, uint64_t table, uint64_t index) {
  return guard(session,
               [&] { return sm3::drop_index(session->sm3, table, index); });
}

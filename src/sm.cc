/*
 * sm.cc -- wisr's C API over sm1 and sm2 (include/sm.h).
 *
 * Each entry point converts its arguments, calls the level below
 * inside guard(), and converts the results back. guard() is the
 * stack's one catch-all (DESIGN.md section 5): no exception
 * leaves this file.
 */

#include "sm.h"

#include "sm1.h"
#include "sm2.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

static_assert(static_cast<int>(sm1::xctlevel_t::read_committed) ==
              SM_XCT_READ_COMMITTED);
static_assert(static_cast<int>(sm1::xctlevel_t::repeatable_read) ==
              SM_XCT_REPEATABLE_READ);
static_assert(static_cast<int>(sm1::xctlevel_t::serializable) ==
              SM_XCT_SERIALIZABLE);
static_assert(static_cast<int>(sm1::loglevel_t::error) == SM_LOG_ERROR);
static_assert(static_cast<int>(sm1::loglevel_t::warn) == SM_LOG_WARN);
static_assert(static_cast<int>(sm1::loglevel_t::info) == SM_LOG_INFO);

namespace {

/* The sm1 environment behind a C handle. */
sm1::env_t *env_of(sm_env_t *env) {
  return reinterpret_cast<sm1::env_t *>(env);
}

/* The sm1 session behind a C handle. */
sm1::session_t *session_of(sm_session_t *session) {
  return reinterpret_cast<sm1::session_t *>(session);
}

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
void abort_keeping_error(sm1::session_t *session) {
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
  if (!sm1::in_doubt(session)) {
    sm1::abort(session);
  }
  sm1::seterr(code, std::string_view(t_kept, n));
}

/*
 * Run fn, the body of an entry point, and return what it returns.
 * An exception becomes -1 with SM_ERR_NOMEM or SM_ERR_EXCEPTION;
 * after either, from fn or from below, session's transaction is
 * aborted, since a data call may have written part of its change.
 */
template <class F> int guard(sm1::session_t *session, F &&fn) noexcept {
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
void log_trampoline(void *arg, sm1::loglevel_t level, const char *msg) {
  const log_hook_t *hook = static_cast<const log_hook_t *>(arg);
  hook->fn(hook->arg, static_cast<sm_loglevel_t>(level), msg);
}

/*
 * Copy strings into one malloc'd block: the n pointers first,
 * each pointing at its string's copy after them. Returns nullptr
 * for an empty list, or on running out of memory, which *failed
 * then says.
 */
char **pack_strings(const std::vector<std::string> &strings, bool *failed) {
  *failed = false;
  if (strings.empty()) {
    return nullptr;
  }
  size_t size = strings.size() * sizeof(char *);
  for (const std::string &s : strings) {
    size += s.size() + 1;
  }
  char **out = static_cast<char **>(malloc(size));
  if (out == nullptr) {
    *failed = true;
    return nullptr;
  }
  char *p = reinterpret_cast<char *>(out + strings.size());
  for (size_t i = 0; i < strings.size(); i++) {
    out[i] = p;
    memcpy(p, strings[i].data(), strings[i].size());
    p[strings[i].size()] = '\0';
    p += strings[i].size() + 1;
  }
  return out;
}

/* A C datum as sm2's: a view of its bytes, or null. */
sm2::datum_t datum_of(const sm_datum_t &d) {
  return {std::string_view(static_cast<const char *>(d.data), d.len), d.null};
}

/* Fill buf with sm2's datums for the n C datums at datums. */
std::span<const sm2::datum_t> datums_of(std::vector<sm2::datum_t> &buf,
                                        const sm_datum_t *datums, size_t n) {
  buf.clear();
  for (size_t i = 0; i < n; i++) {
    buf.push_back(datum_of(datums[i]));
  }
  return buf;
}

/* A C schema as sm2's, the missing values copied. */
std::vector<sm2::attribute_t> schema_of(const sm_attribute_t *attrs, size_t n) {
  std::vector<sm2::attribute_t> out;
  out.reserve(n);
  for (size_t i = 0; i < n; i++) {
    const sm_attribute_t &a = attrs[i];
    std::optional<std::string> missing;
    if (a.has_missing) {
      missing.emplace(static_cast<const char *>(a.missing), a.missing_len);
    }
    out.push_back({reinterpret_cast<const sm2::type_t *>(a.type), a.typmod,
                   a.dropped, std::move(missing)});
  }
  return out;
}

/* A decoded attribute as a C datum: a view, or null. */
sm_datum_t c_datum_of(const std::optional<std::string_view> &d) {
  return d ? sm_datum_t{d->data(), d->size(), false}
           : sm_datum_t{nullptr, 0, true};
}

/* One attribute of tuple under schema into *out. */
int decode(std::span<const sm2::attribute_t> schema, const void *tuple,
           size_t len, size_t ordinal, sm_datum_t *out) {
  std::optional<std::string_view> d;
  if (sm2::tuple_decode(schema,
                        std::string_view(static_cast<const char *>(tuple), len),
                        ordinal, d) != 0) {
    return -1;
  }
  *out = c_datum_of(d);
  return 0;
}

/* Every attribute of tuple under schema into out[0..n), via buf. */
int deform(std::span<const sm2::attribute_t> schema,
           std::vector<std::optional<std::string_view>> &buf, const void *tuple,
           size_t len, sm_datum_t *out, size_t n) {
  if (n != schema.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "deform: n is not the schema's attribute count");
  }
  if (sm2::tuple_deform(schema,
                        std::string_view(static_cast<const char *>(tuple), len),
                        buf) != 0) {
    return -1;
  }
  for (size_t i = 0; i < n; i++) {
    out[i] = c_datum_of(buf[i]);
  }
  return 0;
}

/* Fail with SM_ERR_INVALID: fn was handed NULL for arg. */
int null_arg(const char *fn, const char *arg) {
  return sm1::seterr(sm1::SM_ERR_INVALID,
                     std::string(fn) + ": " + arg + " is NULL");
}

/* A C view out-parameter pair, set from v. */
void view_out(std::string_view v, const void **data, size_t *len) {
  *data = v.data();
  *len = v.size();
}

} // namespace

/*
 * A heap rel as the wrapper holds it: the session, for the
 * catch-all's abort; the schema sm2 borrows, owned here; and the
 * buffers reused from row to row.
 */
struct sm_rel_t {
  sm1::session_t *session;
  std::vector<sm2::attribute_t> schema;
  sm2::rel_t *rel = nullptr;
  std::vector<sm2::datum_t> datums;
  std::vector<std::optional<std::string_view>> deformed;
};

/*
 * An index rel as the wrapper holds it: the session, the key
 * columns and INCLUDE attributes sm2 borrows, and the buffers
 * reused from entry to entry.
 */
struct sm_idx_t {
  sm1::session_t *session;
  std::vector<sm2::key_column_t> key;
  std::vector<sm2::attribute_t> include;
  sm2::index_t *index = nullptr;
  std::vector<sm2::datum_t> key_datums;
  std::vector<sm2::datum_t> include_datums;
  std::vector<std::optional<std::string_view>> deformed;
};

namespace {

/* The sm2 cursor behind a C heap cursor. */
sm2::rel_scan_t *rel_scan_of(sm_rel_scan_t *scan) {
  return reinterpret_cast<sm2::rel_scan_t *>(scan);
}

/* The sm2 cursor behind a C index cursor. */
sm2::index_scan_t *idx_scan_of(sm_idx_scan_t *scan) {
  return reinterpret_cast<sm2::index_scan_t *>(scan);
}

/* A C bound as sm2's, its datums converted into buf. */
sm2::bound_t bound_of(std::vector<sm2::datum_t> &buf, const sm_bound_t &b) {
  return {datums_of(buf, b.prefix, b.n), b.inclusive};
}

} // namespace

extern "C" {

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

/* Attach homedir. */
int sm_attach(const char *homedir, sm_env_t **out) {
  return guard(nullptr, [&] {
    sm1::env_t *env = nullptr;
    // Attach: opens the root db and starts sm1's threads.
    if (sm1::attach(name_of(homedir), env) != 0) {
      return -1;
    }
    *out = reinterpret_cast<sm_env_t *>(env);
    return 0;
  });
}

/* Detach env. */
int sm_detach(sm_env_t *env, int force) {
  return guard(nullptr, [&] { return sm1::detach(env_of(env), force); });
}

/* Create dbname, with opt's options or the defaults. */
int sm_mkdb(sm_env_t *env, const char *dbname, const sm_options_t *opt) {
  return guard(nullptr, [&] {
    sm1::options_t o;
    if (opt != nullptr) {
      static_cast<sm_options_t &>(o) = *opt;
    }
    return sm1::mkdb(env_of(env), name_of(dbname), &o);
  });
}

/* Drop dbname. */
int sm_rmdb(sm_env_t *env, const char *dbname) {
  return guard(nullptr,
               [&] { return sm1::rmdb(env_of(env), name_of(dbname)); });
}

/* List the databases into one block. */
int sm_lsdb(sm_env_t *env, char ***names, size_t *n) {
  return guard(nullptr, [&] {
    std::vector<std::string> list;
    if (sm1::lsdb(env_of(env), list) != 0) {
      return -1;
    }
    bool failed;
    char **out = pack_strings(list, &failed);
    if (failed) {
      return nomem();
    }
    *names = out;
    *n = list.size();
    return 0;
  });
}

/* Open a session on dbname. */
int sm_opendb(sm_env_t *env, const char *dbname, const sm_options_t *opt,
              sm_session_t **out) {
  return guard(nullptr, [&] {
    sm1::options_t o;
    if (opt != nullptr) {
      static_cast<sm_options_t &>(o) = *opt;
    }
    sm1::session_t *session = nullptr;
    // Open the session: the first one opens the database itself.
    if (sm1::opendb(env_of(env), name_of(dbname), session, &o) != 0) {
      return -1;
    }
    *out = reinterpret_cast<sm_session_t *>(session);
    return 0;
  });
}

/* Close a session. */
int sm_closedb(sm_session_t *session) {
  return guard(nullptr, [&] { return sm1::closedb(session_of(session)); });
}

/* Begin a transaction at level. */
int sm_begin(sm_session_t *session, sm_xctlevel_t level) {
  sm1::session_t *s = session_of(session);
  return guard(
      s, [&] { return sm1::begin(s, static_cast<sm1::xctlevel_t>(level)); });
}

/* Commit the active transaction. */
int sm_commit(sm_session_t *session) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] { return sm1::commit(s); });
}

/* Abort the active transaction, or accept that it is already gone. */
int sm_abort(sm_session_t *session) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] {
    if (sm1::abort(s) != 0) {
      // The engine aborted it already, after a retryable error.
      return sm1::errnum() == sm1::SM_ERR_NOTRANS ? 0 : -1;
    }
    return 0;
  });
}

/* Begin a statement. */
int sm_stmt_begin(sm_session_t *session) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] { return sm1::stmt_begin(s); });
}

/* Set a savepoint and return its depth. */
int sm_savepoint(sm_session_t *session) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] { return sm1::savepoint(s); });
}

/* Roll back to the savepoint at depth. */
int sm_rollback(sm_session_t *session, int depth) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] { return sm1::rollback(s, depth); });
}

/* Forget the savepoint at depth. */
int sm_forget(sm_session_t *session, int depth) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] { return sm1::forget(s, depth); });
}

/* Prepare the active transaction as gid. */
int sm_prepare(sm_session_t *session, const char *gid) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] { return sm1::prepare(s, name_of(gid)); });
}

/* List the prepared transactions into one block. */
int sm_list_prepared(sm_session_t *session, sm_prepared_t **out, size_t *n) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] {
    std::vector<sm1::prepared_t> list;
    if (sm1::list_prepared(s, list) != 0) {
      return -1;
    }
    *out = nullptr;
    *n = 0;
    if (list.empty()) {
      return 0;
    }
    // The entries first, then each gid's copy after them.
    size_t size = list.size() * sizeof(sm_prepared_t);
    for (const sm1::prepared_t &p : list) {
      size += p.gid.size() + 1;
    }
    sm_prepared_t *block = static_cast<sm_prepared_t *>(malloc(size));
    if (block == nullptr) {
      return nomem();
    }
    char *str = reinterpret_cast<char *>(block + list.size());
    for (size_t i = 0; i < list.size(); i++) {
      memcpy(str, list[i].gid.data(), list[i].gid.size());
      str[list[i].gid.size()] = '\0';
      block[i].gid = str;
      block[i].prepared_at = list[i].prepared_at;
      str += list[i].gid.size() + 1;
    }
    *out = block;
    *n = list.size();
    return 0;
  });
}

/* Commit the prepared transaction gid. */
int sm_commit_prepared(sm_session_t *session, const char *gid) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] { return sm1::commit_prepared(s, name_of(gid)); });
}

/* Abort the prepared transaction gid. */
int sm_abort_prepared(sm_session_t *session, const char *gid) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] { return sm1::abort_prepared(s, name_of(gid)); });
}

/* Release a block this API handed out. */
void sm_free(void *p) { free(p); }

/* Create a store. */
int sm_mkstore(sm_session_t *session, uint64_t *storeid) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] { return sm1::mkstore(s, *storeid); });
}

/* Drop a store. */
int sm_rmstore(sm_session_t *session, uint64_t storeid) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] { return sm1::rmstore(s, storeid); });
}

/* List the stores into one block. */
int sm_lsstore(sm_session_t *session, uint64_t **ids, size_t *n) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] {
    std::vector<uint64_t> list;
    if (sm1::lsstore(s, list) != 0) {
      return -1;
    }
    *ids = nullptr;
    *n = 0;
    if (list.empty()) {
      return 0;
    }
    uint64_t *block =
        static_cast<uint64_t *>(malloc(list.size() * sizeof(uint64_t)));
    if (block == nullptr) {
      return nomem();
    }
    memcpy(block, list.data(), list.size() * sizeof(uint64_t));
    *ids = block;
    *n = list.size();
    return 0;
  });
}

/* Create a sequence. */
int sm_mkseq(sm_session_t *session, const char *name, uint64_t start,
             uint64_t increment) {
  sm1::session_t *s = session_of(session);
  return guard(s,
               [&] { return sm1::mkseq(s, name_of(name), start, increment); });
}

/* Drop a sequence. */
int sm_rmseq(sm_session_t *session, const char *name) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] { return sm1::rmseq(s, name_of(name)); });
}

/* Hand out the sequence's next value. */
int sm_nextseq(sm_session_t *session, const char *name, uint64_t *out) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] {
    if (!out) {
      return sm1::seterr(sm1::SM_ERR_INVALID, "sm_nextseq: out is NULL");
    }
    return sm1::nextseq(s, name_of(name), *out);
  });
}

/* The type with this frozen code, or NULL. */
const sm_type_t *sm_type(uint8_t code) {
  return reinterpret_cast<const sm_type_t *>(sm2::type_by_code(code));
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

/* Open a heap rel, copying schema into the handle. */
int sm_rel_open(sm_session_t *session, uint64_t storeid, uint16_t prefix,
                const sm_attribute_t *schema, size_t n, const char *rowno_seq,
                sm_rel_t **out) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] {
    sm_rel_t *rel = new sm_rel_t;
    rel->session = s;
    rel->schema = schema_of(schema, n);
    if (sm2::rel_open(s, storeid, prefix, rel->schema, name_of(rowno_seq),
                      rel->rel) != 0) {
      delete rel;
      return -1;
    }
    *out = rel;
    return 0;
  });
}

/* Close a heap rel and free the handle. */
void sm_rel_close(sm_rel_t *rel) {
  if (rel != nullptr) {
    sm2::rel_close(rel->rel);
    delete rel;
  }
}

/* The number of attributes in the rel's schema. */
size_t sm_rel_ncols(const sm_rel_t *rel) { return rel->schema.size(); }

/* Insert an already-encoded tuple into the heap rel. */
int sm_rel_insert_tuple(sm_rel_t *rel, const void *tuple, size_t len,
                        uint64_t *rowno) {
  return guard(rel ? rel->session : nullptr, [&] {
    if (!rel || !tuple || !rowno) {
      return null_arg("sm_rel_insert_tuple", !rel     ? "rel"
                                             : !tuple ? "tuple"
                                                      : "rowno");
    }
    std::string_view v(static_cast<const char *>(tuple), len);
    return sm2::rel_insert(rel->rel, v, *rowno);
  });
}

/* Replace the row at rowno with an already-encoded tuple. */
int sm_rel_update_tuple(sm_rel_t *rel, uint64_t rowno, const void *tuple,
                        size_t len, const void **old, size_t *old_len) {
  return guard(rel ? rel->session : nullptr, [&] {
    if (!rel || !tuple || (old && !old_len)) {
      return null_arg("sm_rel_update_tuple", !rel     ? "rel"
                                             : !tuple ? "tuple"
                                                      : "old_len");
    }
    std::string_view v(static_cast<const char *>(tuple), len);
    std::string_view old_v;
    if (sm2::rel_update(rel->rel, rowno, v,
                        old != nullptr ? &old_v : nullptr) != 0) {
      return -1;
    }
    if (old != nullptr) {
      view_out(old_v, old, old_len);
    }
    return 0;
  });
}

/* Insert a row of values and isnull. */
int sm_rel_insert_values(sm_rel_t *rel, const sm_datum_val_t *values,
                         const bool *isnull, size_t n, uint64_t *rowno) {
  return guard(rel ? rel->session : nullptr, [&] {
    if (!rel || (n && !values) || (n && !isnull) || !rowno) {
      return null_arg("sm_rel_insert_values", !rel           ? "rel"
                                              : !rowno       ? "rowno"
                                              : n && !values ? "values"
                                                             : "isnull");
    }
    if (n != rel->schema.size()) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "sm_rel_insert_values: n is not rel schema count");
    }
    std::span<const sm2::Datum> val_span(values, n);
    std::span<const bool> null_span(isnull, n);
    return sm2::rel_insert(rel->rel, val_span, null_span, *rowno);
  });
}

/* Replace a row with values and isnull. */
int sm_rel_update_values(sm_rel_t *rel, uint64_t rowno,
                         const sm_datum_val_t *values, const bool *isnull,
                         size_t n, const void **old, size_t *old_len) {
  return guard(rel ? rel->session : nullptr, [&] {
    if (!rel || (n && !values) || (n && !isnull) || (old && !old_len)) {
      return null_arg("sm_rel_update_values", !rel           ? "rel"
                                              : n && !values ? "values"
                                              : n && !isnull ? "isnull"
                                                             : "old_len");
    }
    if (n != rel->schema.size()) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "sm_rel_update_values: n is not rel schema count");
    }
    std::span<const sm2::Datum> val_span(values, n);
    std::span<const bool> null_span(isnull, n);
    std::string_view old_v;
    if (sm2::rel_update(rel->rel, rowno, val_span, null_span,
                        old != nullptr ? &old_v : nullptr) != 0) {
      return -1;
    }
    if (old != nullptr) {
      view_out(old_v, old, old_len);
    }
    return 0;
  });
}

/* Insert a row. */
int sm_rel_insert(sm_rel_t *rel, const sm_datum_t *datums, size_t n,
                  uint64_t *rowno) {
  return guard(rel->session, [&] {
    return sm2::rel_insert(rel->rel, datums_of(rel->datums, datums, n), *rowno);
  });
}

/* Read a row as the transaction sees it. */
int sm_rel_read(sm_rel_t *rel, uint64_t rowno, const void **tuple,
                size_t *len) {
  return guard(rel->session, [&] {
    std::string_view v;
    int rc = sm2::rel_read(rel->rel, rowno, v);
    if (rc == 0) {
      view_out(v, tuple, len);
    }
    return rc;
  });
}

/* Read the newest committed version of a row. */
int sm_rel_read_latest(sm_rel_t *rel, uint64_t rowno, const void **tuple,
                       size_t *len) {
  return guard(rel->session, [&] {
    std::string_view v;
    int rc = sm2::rel_read_latest(rel->rel, rowno, v);
    if (rc == 0) {
      view_out(v, tuple, len);
    }
    return rc;
  });
}

/* Replace a row, handing back the old tuple if asked. */
int sm_rel_update(sm_rel_t *rel, uint64_t rowno, const sm_datum_t *datums,
                  size_t n, const void **old, size_t *old_len) {
  return guard(rel->session, [&] {
    std::string_view v;
    if (sm2::rel_update(rel->rel, rowno, datums_of(rel->datums, datums, n),
                        old != nullptr ? &v : nullptr) != 0) {
      return -1;
    }
    if (old != nullptr) {
      view_out(v, old, old_len);
    }
    return 0;
  });
}

/* Delete a row, handing back its tuple if asked. */
int sm_rel_delete(sm_rel_t *rel, uint64_t rowno, const void **old,
                  size_t *old_len) {
  return guard(rel->session, [&] {
    std::string_view v;
    if (sm2::rel_delete(rel->rel, rowno, old != nullptr ? &v : nullptr) != 0) {
      return -1;
    }
    if (old != nullptr) {
      view_out(v, old, old_len);
    }
    return 0;
  });
}

/* Open a cursor over rows first..last. */
int sm_rel_scan(sm_rel_t *rel, uint64_t first, uint64_t last,
                sm_rel_scan_t **out) {
  return guard(rel->session, [&] {
    sm2::rel_scan_t *scan = nullptr;
    if (sm2::rel_scan(rel->rel, first, last, scan) != 0) {
      return -1;
    }
    *out = reinterpret_cast<sm_rel_scan_t *>(scan);
    return 0;
  });
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

/* Encode values and isnull into a tuple buffer; sm_free(*tuple). */
int sm_tuple_encode(const sm_attribute_t *schema, size_t natts,
                    const sm_datum_val_t *values, const bool *isnull,
                    void **tuple, size_t *len) {
  return guard(nullptr, [&] {
    if ((natts && !schema) || (natts && !values) || (natts && !isnull) ||
        !tuple || !len) {
      return null_arg("sm_tuple_encode", natts && !schema   ? "schema"
                                         : natts && !values ? "values"
                                         : natts && !isnull ? "isnull"
                                         : !tuple           ? "tuple"
                                                            : "len");
    }
    auto s = schema_of(schema, natts);
    std::string out;
    std::span<const sm2::Datum> val_span(values, natts);
    std::span<const bool> null_span(isnull, natts);
    if (sm2::tuple_encode(s, val_span, null_span, out) != 0) {
      return -1;
    }
    void *buf = std::malloc(out.size());
    if (!buf) {
      return sm1::seterr(sm1::SM_ERR_NOMEM, "sm_tuple_encode: out of memory");
    }
    std::memcpy(buf, out.data(), out.size());
    *tuple = buf;
    *len = out.size();
    return 0;
  });
}

/* Deform tuple into values and isnull arrays of size n. */
int sm_rel_deform_values(sm_rel_t *rel, const void *tuple, size_t len,
                         sm_datum_val_t *values, bool *isnull, size_t n) {
  return guard(nullptr, [&] {
    if (!rel || !tuple || (n && !values) || (n && !isnull)) {
      return null_arg("sm_rel_deform_values", !rel           ? "rel"
                                              : !tuple       ? "tuple"
                                              : n && !values ? "values"
                                                             : "isnull");
    }
    if (n != rel->schema.size()) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "sm_rel_deform_values: n is not rel schema count");
    }
    std::string_view v(static_cast<const char *>(tuple), len);
    std::span<sm2::Datum> val_span(values, n);
    std::span<bool> null_span(isnull, n);
    return sm2::tuple_deform(rel->schema, v, val_span, null_span);
  });
}

/* Decode one attribute into val and isnull. */
int sm_rel_decode_value(sm_rel_t *rel, const void *tuple, size_t len,
                        size_t ordinal, sm_datum_val_t *val, bool *isnull) {
  return guard(nullptr, [&] {
    if (!rel || !tuple || !val || !isnull) {
      return null_arg("sm_rel_decode_value", !rel     ? "rel"
                                             : !tuple ? "tuple"
                                             : !val   ? "val"
                                                      : "isnull");
    }
    std::string_view v(static_cast<const char *>(tuple), len);
    return sm2::tuple_decode(rel->schema, v, ordinal, *val, *isnull);
  });
}

/* One attribute of a row's tuple. */
int sm_rel_decode(sm_rel_t *rel, const void *tuple, size_t len, size_t ordinal,
                  sm_datum_t *out) {
  return guard(nullptr,
               [&] { return decode(rel->schema, tuple, len, ordinal, out); });
}

/* Every attribute of a row's tuple. */
int sm_rel_deform(sm_rel_t *rel, const void *tuple, size_t len, sm_datum_t *out,
                  size_t n) {
  return guard(nullptr, [&] {
    return deform(rel->schema, rel->deformed, tuple, len, out, n);
  });
}

/* Open an index, copying its columns into the handle. */
int sm_idx_open(sm_session_t *session, uint64_t storeid, uint16_t prefix,
                const sm_key_column_t *key, size_t nkey, bool unique,
                const sm_attribute_t *include, size_t ninclude,
                sm_idx_t **out) {
  sm1::session_t *s = session_of(session);
  return guard(s, [&] {
    sm_idx_t *idx = new sm_idx_t;
    idx->session = s;
    idx->include = schema_of(include, ninclude);
    for (size_t i = 0; i < nkey; i++) {
      idx->key.push_back(
          {reinterpret_cast<const sm2::type_t *>(key[i].type), key[i].typmod,
           reinterpret_cast<const sm2::collation_t *>(key[i].collation),
           key[i].desc, key[i].nulls_first});
    }
    if (sm2::idx_open(s, storeid, prefix, idx->key, unique, idx->include,
                      idx->index) != 0) {
      delete idx;
      return -1;
    }
    *out = idx;
    return 0;
  });
}

/* Close an index and free the handle. */
void sm_idx_close(sm_idx_t *idx) {
  if (idx != nullptr) {
    sm2::idx_close(idx->index);
    delete idx;
  }
}

/* The number of attributes in an entry's value. */
size_t sm_idx_ncols(const sm_idx_t *idx) {
  return sm2::idx_value_schema(idx->index).size();
}

/* Add an entry. */
int sm_idx_insert(sm_idx_t *idx, const sm_datum_t *key, size_t nkey,
                  uint64_t rowno, const sm_datum_t *include, size_t ninclude) {
  return guard(idx->session, [&] {
    return sm2::idx_insert(idx->index, datums_of(idx->key_datums, key, nkey),
                           rowno,
                           datums_of(idx->include_datums, include, ninclude));
  });
}

/* Remove an entry. */
int sm_idx_delete(sm_idx_t *idx, const sm_datum_t *key, size_t nkey,
                  uint64_t rowno) {
  return guard(idx->session, [&] {
    return sm2::idx_delete(idx->index, datums_of(idx->key_datums, key, nkey),
                           rowno);
  });
}

/* Open a cursor over the entries between lo and hi. */
int sm_idx_scan(sm_idx_t *idx, const sm_bound_t *lo, const sm_bound_t *hi,
                bool *widened, sm_idx_scan_t **out) {
  return guard(idx->session, [&] {
    std::vector<sm2::datum_t> lo_buf, hi_buf;
    sm2::bound_t lo_b, hi_b;
    if (lo != nullptr) {
      lo_b = bound_of(lo_buf, *lo);
    }
    if (hi != nullptr) {
      hi_b = bound_of(hi_buf, *hi);
    }
    sm2::index_scan_t *scan = nullptr;
    if (sm2::idx_scan(idx->index, lo != nullptr ? &lo_b : nullptr,
                      hi != nullptr ? &hi_b : nullptr, scan, widened) != 0) {
      return -1;
    }
    *out = reinterpret_cast<sm_idx_scan_t *>(scan);
    return 0;
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
    std::vector<sm2::datum_t> buf;
    return sm2::idx_scan_seek(idx_scan_of(scan), bound_of(buf, *lo));
  });
}

/* Close an index cursor. */
void sm_idx_scanend(sm_idx_scan_t *scan) {
  sm2::idx_scanend(idx_scan_of(scan));
}

/* One attribute of an entry's value. */
int sm_idx_decode(sm_idx_t *idx, const void *value, size_t len, size_t ordinal,
                  sm_datum_t *out) {
  return guard(nullptr, [&] {
    return decode(sm2::idx_value_schema(idx->index), value, len, ordinal, out);
  });
}

/* Every attribute of an entry's value. */
int sm_idx_deform(sm_idx_t *idx, const void *value, size_t len, sm_datum_t *out,
                  size_t n) {
  return guard(nullptr, [&] {
    return deform(sm2::idx_value_schema(idx->index), idx->deformed, value, len,
                  out, n);
  });
}

} // extern "C"

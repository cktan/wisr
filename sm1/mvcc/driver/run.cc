/*
 * run.cc -- running a parsed script (DRIVER.md §2.4, §4).
 *
 * Each session gets a thread, because a step may block on a
 * row lock and the driver must carry on with the next step
 * rather than wait forever. The controller thread dispatches
 * one step at a time, waits a little for it, and reports it as
 * blocked if it has not finished; a blocked step is printed
 * again, as resumed, once a later step releases it.
 *
 * Nothing the engine generates -- timestamps, ids, paths --
 * reaches the transcript, so two runs of one script produce
 * the same bytes.
 */

#include "driver.h"

#include "sm1.h"

#include <stdlib.h>

#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <thread>

namespace {

/* The symbolic name of the current error (DRIVER.md §4). */
std::string error_name() {
  switch (sm1::errnum()) {
  case sm1::SM_ERR_CONFLICT:
    return "conflict";
  case sm1::SM_ERR_SERIALIZATION:
    return "serialization";
  case sm1::SM_ERR_DUPKEY:
    return "dupkey";
  case sm1::SM_ERR_NOTFOUND:
    return "notfound";
  case sm1::SM_ERR_NOTRANS:
    return "notrans";
  case sm1::SM_ERR_DEADLOCK:
    return "deadlock";
  case sm1::SM_ERR_PREPARED:
    return "prepared";
  case sm1::SM_ERR_INVALID:
    return "invalid";
  case sm1::SM_ERR_UPDATED:
    return "updated";
  default:
    return std::string("error:") + sm1::errmsg();
  }
}

/* One session of the script, and the thread that drives it. */
struct worker_t {
  std::string name;
  sm1::xctlevel_t level = sm1::xctlevel_t::repeatable_read;
  sm1::session_t *sess = nullptr;
  uint64_t store = 0;
  uint64_t rel2 = 0; /* the second store, for rmstore/lookup */

  std::thread thread;
  std::mutex mu;
  std::condition_variable cv;
  const item_t *req = nullptr; /* the step being run, if any */
  bool done = true;
  bool stop = false;
  std::string result;

  std::map<std::string, sm1::scan_t *> cursors; /* open, by name */
  const item_t *pending = nullptr;              /* a step reported as blocked */
  std::string last;                             /* that session's last result */
  int commit_state = 0; /* 1 committed, -1 failed, 0 neither */
};

/* The line a step prints, without its result. */
std::string step_text(const item_t &it) {
  std::ostringstream out;
  out << it.time << " " << (it.kind == ITEM_CHECK ? "check" : it.session) << " "
      << it.verb;
  for (const std::string &a : it.arg) {
    out << " " << a;
  }
  if (it.expect_block) {
    out << " blocks";
  }
  return out.str();
}

/* Run one step against w's session, returning what to print. */
std::string execute(worker_t *w, const item_t &it) {
  const std::vector<std::string> &a = it.arg;
  if (it.verb == "begin") {
    return sm1::begin(w->sess, w->level) == 0 ? "ok" : error_name();
  }
  if (it.verb == "commit" || it.verb == "commit?") {
    bool ok = sm1::commit(w->sess) == 0;
    w->commit_state = ok ? 1 : -1;
    // "commit?" hides an outcome that may differ run to run;
    // a check states what must hold instead (DRIVER.md §2.2).
    if (it.verb == "commit?") {
      return "recorded";
    }
    return ok ? "ok" : error_name();
  }
  if (it.verb == "abort") {
    return sm1::abort(w->sess) == 0 ? "ok" : error_name();
  }
  if (it.verb == "read") {
    const char *data = nullptr;
    size_t len = 0;
    int rc =
        sm1::read(w->sess, w->store, a[0].data(), a[0].size(), &data, &len);
    if (rc < 0) {
      return error_name();
    }
    return rc == 1 ? "-" : std::string(data, len);
  }
  if (it.verb == "readlatest") {
    // readlatest <row>: the newest committed version of a row
    // this transaction holds locked, past its snapshot.
    const char *data = nullptr;
    size_t len = 0;
    int rc = sm1::read_latest(w->sess, w->store, a[0].data(), a[0].size(),
                              &data, &len);
    if (rc < 0) {
      return error_name();
    }
    return rc == 1 ? "-" : std::string(data, len);
  }
  if (it.verb == "write" || it.verb == "insert") {
    int rc = it.verb == "write"
                 ? sm1::update(w->sess, w->store, a[0].data(), a[0].size(),
                               a[1].data(), a[1].size(), nullptr, nullptr)
                 : sm1::insert(w->sess, w->store, a[0].data(), a[0].size(),
                               a[1].data(), a[1].size());
    return rc == 0 ? "ok" : error_name();
  }
  if (it.verb == "delete") {
    return sm1::erase(w->sess, w->store, a[0].data(), a[0].size(), nullptr,
                      nullptr) == 0
               ? "ok"
               : error_name();
  }
  if (it.verb == "scan") {
    // scan <lo> <hi> [limit <n>]: take n rows, then close.
    long limit = -1;
    if (a.size() == 4 && a[2] == "limit") {
      limit = strtol(a[3].c_str(), nullptr, 10);
    }
    sm1::scan_t *c = nullptr;
    if (sm1::scan(w->sess, w->store, a[0].data(), a[0].size(), 1, a[1].data(),
                  a[1].size(), 1, c) != 0) {
      return error_name();
    }
    std::ostringstream out;
    out << "{";
    const char *k, *d;
    size_t kl, dl;
    long n = 0;
    while ((limit < 0 || n < limit) && sm1::scannxt(c, &k, &kl, &d, &dl) == 0) {
      out << (n++ ? ", " : "") << std::string(k, kl);
    }
    out << "}";
    sm1::scanend(c);
    return out.str();
  }
  if (it.verb == "open") {
    // open <name> <lo> <hi>: a cursor that outlives this step.
    sm1::scan_t *c = nullptr;
    if (sm1::scan(w->sess, w->store, a[1].data(), a[1].size(), 1, a[2].data(),
                  a[2].size(), 1, c) != 0) {
      return error_name();
    }
    w->cursors[a[0]] = c;
    return "ok";
  }
  if (it.verb == "next") {
    // next <name>: the cursor's next key, or "end".
    auto it2 = w->cursors.find(a[0]);
    if (it2 == w->cursors.end()) {
      return "ERROR: no cursor " + a[0];
    }
    const char *k, *d;
    size_t kl, dl;
    int rc = sm1::scannxt(it2->second, &k, &kl, &d, &dl);
    if (rc < 0) {
      return error_name();
    }
    return rc == 1 ? "end" : std::string(k, kl) + "=" + std::string(d, dl);
  }
  if (it.verb == "close") {
    // close <name>: release the cursor and its predicate.
    auto it2 = w->cursors.find(a[0]);
    if (it2 == w->cursors.end()) {
      return "ERROR: no cursor " + a[0];
    }
    sm1::scanend(it2->second);
    w->cursors.erase(it2);
    return "ok";
  }
  if (it.verb == "seek") {
    // seek <cursor> <key>: move an open cursor forward, the
    // key itself included.
    auto c = w->cursors.find(a[0]);
    if (c == w->cursors.end()) {
      return "ERROR: no such cursor " + a[0];
    }
    return sm1::scan_seek(c->second, a[1].data(), a[1].size(), 1) == 0
               ? "ok"
               : error_name();
  }
  if (it.verb == "stmt") {
    // Start a statement: read committed then reads at one
    // snapshot until the next one.
    return sm1::stmt_begin(w->sess) == 0 ? "ok" : error_name();
  }
  if (it.verb == "savepoint") {
    int depth = sm1::savepoint(w->sess);
    // The depth is the mark's name, so print it: a script
    // rolls back to the number this step reported.
    return depth < 0 ? error_name() : std::to_string(depth);
  }
  if (it.verb == "rollback") {
    // rollback <depth>: undo to that mark, keeping it.
    return sm1::rollback(w->sess, atoi(a[0].c_str())) == 0 ? "ok"
                                                           : error_name();
  }
  if (it.verb == "lock") {
    // lock <row> <shared|exclusive>: take the row lock
    // without writing the row.
    if (a[1] != "shared" && a[1] != "exclusive") {
      return "ERROR: lock mode must be shared or exclusive, not " + a[1];
    }
    sm1::lockmode_t mode =
        a[1] == "shared" ? sm1::lockmode_t::shared : sm1::lockmode_t::exclusive;
    return sm1::lock(w->sess, w->store, a[0].data(), a[0].size(), mode) == 0
               ? "ok"
               : error_name();
  }
  if (it.verb == "lookup") {
    // lookup <row>: read a row of the SECOND store. What
    // the scenario is usually after is the storeid resolution
    // this takes, which is a tracked catalog read (§11).
    const char *data = nullptr;
    size_t len = 0;
    int rc = sm1::read(w->sess, w->rel2, a[0].data(), a[0].size(), &data, &len);
    if (rc < 0) {
      return error_name();
    }
    return rc == 1 ? "-" : std::string(data, len);
  }
  if (it.verb == "rmstore") {
    // Drop the second store; the first one every other verb
    // works in is never dropped.
    return sm1::rmstore(w->sess, w->rel2) == 0 ? "ok" : error_name();
  }
  if (it.verb == "mkstore") {
    uint64_t storeid = 0;
    // The storeid itself is generated, so it is not printed.
    return sm1::mkstore(w->sess, &storeid) == 0 ? "ok" : error_name();
  }
  if (it.verb == "lsstore") {
    uint64_t *list = nullptr;
    size_t n = 0;
    if (sm1::lsstore(w->sess, &list, &n) != 0) {
      return error_name();
    }
    free(list);
    return std::to_string(n) + " stores";
  }
  return "ERROR: unknown verb " + it.verb;
}

/* The loop each session's thread runs. */
void worker_main(worker_t *w) {
  for (;;) {
    const item_t *it = nullptr;
    {
      std::unique_lock<std::mutex> lk(w->mu);
      w->cv.wait(lk, [w] { return w->req || w->stop; });
      if (w->stop) {
        return;
      }
      it = w->req;
    }
    std::string res = execute(w, *it);
    {
      std::lock_guard<std::mutex> lk(w->mu);
      w->result = res;
      w->req = nullptr;
      w->done = true;
    }
    w->cv.notify_all();
  }
}

/* One run of a script: its sessions, database, and transcript. */
class run_t {
public:
  /* Create the database, seed it, and start the sessions. */
  run_t(const script_t &script, pairing_t pairing, int block_ms,
        std::ostringstream *out)
      : m_script(script), m_pairing(pairing), m_block_ms(block_ms),
        m_out(*out) {
    // A home directory of its own, so no run sees another's state.
    std::string tmpl =
        (std::filesystem::temp_directory_path() / "wisr-driver-XXXXXX")
            .string();
    m_home = mkdtemp(tmpl.data());
    m_ok = sm1::init(m_home.c_str()) == 0;
    m_ok = m_ok && sm1::attach(m_home.c_str(), m_env) == 0;
    m_ok = m_ok && m_env && sm1::mkdb(m_env, "db", nullptr) == 0;
    if (!m_ok) {
      return;
    }
    // The driver's own session creates the stores and seeds
    // them. There are two: the one every row verb works in,
    // and a second holding the same rows, which exists to be
    // dropped and looked up (DRIVER.md §5).
    sm1::opendb(m_env, "db", m_checker);
    m_ok = m_checker &&
           sm1::begin(m_checker, sm1::xctlevel_t::repeatable_read) == 0 &&
           sm1::mkstore(m_checker, &m_store) == 0 &&
           sm1::mkstore(m_checker, &m_rel2) == 0;
    for (const auto &kv : m_script.seeds) {
      // The same rows in both, so a script can read either.
      for (uint64_t store : {m_store, m_rel2}) {
        m_ok = m_ok &&
               sm1::insert(m_checker, store, kv.first.data(), kv.first.size(),
                           kv.second.data(), kv.second.size()) == 0;
      }
    }
    m_ok = m_ok && sm1::commit(m_checker) == 0;

    // One session, and one thread, per declared session. A
    // worker holds a mutex and a thread, so it is held by
    // pointer rather than by value.
    for (size_t i = 0; i < m_script.sessions.size(); i++) {
      m_workers.push_back(std::make_unique<worker_t>());
      worker_t &w = *m_workers[i];
      const session_decl_t &d = m_script.sessions[i];
      w.name = d.name;
      w.level = level_for(d, i);
      sm1::opendb(m_env, "db", w.sess);
      w.store = m_store;
      w.rel2 = m_rel2;
      m_ok = m_ok && w.sess != nullptr;
      w.thread = std::thread(worker_main, &w);
    }
  }

  /* Stop the threads, close the sessions, remove the database. */
  ~run_t() {
    for (std::unique_ptr<worker_t> &wp : m_workers) {
      worker_t &w = *wp;
      {
        std::lock_guard<std::mutex> lk(w.mu);
        w.stop = true;
      }
      w.cv.notify_all();
      if (w.thread.joinable()) {
        w.thread.join();
      }
      sm1::closedb(w.sess);
    }
    sm1::closedb(m_checker);
    sm1::detach(m_env, 1);
    std::error_code ec;
    std::filesystem::remove_all(m_home, ec);
  }

  /* Whether the database and sessions came up. */
  bool ok() const { return m_ok; }

  /* Run every item of the script, returning the failed checks. */
  int run(size_t first) {
    m_out << "== pairing " << pairing_label(m_pairing) << " ==";
    for (const std::unique_ptr<worker_t> &w : m_workers) {
      m_out << " " << w->name << "="
            << (w->level == sm1::xctlevel_t::serializable     ? "SER"
                : w->level == sm1::xctlevel_t::read_committed ? "RC"
                                                              : "RR");
    }
    m_out << "\n";

    for (size_t n = first; n < m_script.items.size(); n++) {
      const item_t &it = m_script.items[n];
      // A comment or blank line goes through as it stands.
      if (it.kind == ITEM_TEXT) {
        m_out << it.text << "\n";
        continue;
      }
      // "when" limits a step to some pairings.
      if (!pairing_selected(m_pairing, it.when)) {
        continue;
      }
      // Print any blocked step that has since finished. The
      // wait is short: a step still blocked here is meant to
      // be, and the next step is what releases it.
      drain(m_block_ms);
      if (it.kind == ITEM_CHECK) {
        run_check(it);
      } else {
        dispatch(it);
      }
    }
    // Nothing may still be blocked when the script ends, so
    // wait properly before calling it a deadlock.
    drain(m_patience_ms);
    for (std::unique_ptr<worker_t> &w : m_workers) {
      if (w->pending) {
        m_out << step_text(*w->pending) << " -> STILL BLOCKED\n";
        w->pending = nullptr;
        m_failures++;
      }
    }
    return m_failures;
  }

private:
  /* The level a session runs at in this pairing. */
  sm1::xctlevel_t level_for(const session_decl_t &d, size_t index) const {
    // An explicit level in the script wins over the pairing.
    if (d.level == "rc") {
      return sm1::xctlevel_t::read_committed;
    }
    if (d.level == "rr") {
      return sm1::xctlevel_t::repeatable_read;
    }
    if (d.level == "ser") {
      return sm1::xctlevel_t::serializable;
    }
    // The first session takes T1's level; the rest take T2's.
    bool ser = index == 0 ? (m_pairing == PAIR_S_RR || m_pairing == PAIR_S_S)
                          : (m_pairing == PAIR_RR_S || m_pairing == PAIR_S_S);
    return ser ? sm1::xctlevel_t::serializable
               : sm1::xctlevel_t::repeatable_read;
  }

  /* The worker of a named session, or null. */
  worker_t *worker(const std::string &name) {
    for (std::unique_ptr<worker_t> &w : m_workers) {
      if (w->name == name) {
        return w.get();
      }
    }
    return nullptr;
  }

  /*
   * Send a step to its session. Only a step the script marks
   * "blocks" may be reported as blocked: every other step is
   * waited for, however slow the machine, so the transcript
   * does not depend on timing.
   */
  void dispatch(const item_t &it) {
    worker_t *w = worker(it.session);
    // A session with a blocked step cannot take another.
    if (w->pending) {
      m_out << step_text(it) << " -> ERROR: " << w->name << " is blocked\n";
      m_failures++;
      return;
    }
    std::unique_lock<std::mutex> lk(w->mu);
    w->req = &it;
    w->done = false;
    w->cv.notify_all();
    // A step claimed to block gets the window; any other gets
    // the patience limit, which only a hang can exhaust.
    int wait_ms = it.expect_block ? m_block_ms : m_patience_ms;
    bool finished = w->cv.wait_for(lk, std::chrono::milliseconds(wait_ms),
                                   [w] { return w->done; });
    if (!finished) {
      w->pending = &it;
      lk.unlock();
      // A step that was not claimed to block, and did not
      // finish, is a hang: the script or the engine is wrong.
      m_out << step_text(it)
            << (it.expect_block ? " -> blocked\n" : " -> HUNG\n");
      m_failures += it.expect_block ? 0 : 1;
      return;
    }
    w->last = w->result;
    lk.unlock();
    m_out << step_text(it) << " -> " << w->last;
    // The script said it would wait, and it did not.
    if (it.expect_block) {
      m_out << " (DID NOT BLOCK)";
      m_failures++;
    }
    m_out << "\n";
  }

  /* Print each blocked step that has finished since. */
  void drain(int wait_ms) {
    for (std::unique_ptr<worker_t> &wp : m_workers) {
      worker_t &w = *wp;
      if (!w.pending) {
        continue;
      }
      std::unique_lock<std::mutex> lk(w.mu);
      bool finished = w.cv.wait_for(lk, std::chrono::milliseconds(wait_ms),
                                    [&w] { return w.done; });
      if (!finished) {
        continue;
      }
      w.last = w.result;
      const item_t *it = w.pending;
      w.pending = nullptr;
      lk.unlock();
      m_out << step_text(*it) << " -> " << w.last << " (resumed)\n";
    }
  }

  /* The rows a fresh transaction sees, as "{r1=0, r2=t2}". */
  std::string rows() {
    std::ostringstream out;
    if (sm1::begin(m_checker, sm1::xctlevel_t::repeatable_read) != 0) {
      return error_name();
    }
    sm1::scan_t *c = nullptr;
    sm1::scan(m_checker, m_store, nullptr, 0, 0, nullptr, 0, 0, c);
    out << "{";
    const char *k, *d;
    size_t kl, dl;
    int n = 0;
    while (c && sm1::scannxt(c, &k, &kl, &d, &dl) == 0) {
      out << (n++ ? ", " : "") << std::string(k, kl) << "="
          << std::string(d, dl);
    }
    out << "}";
    sm1::scanend(c);
    sm1::commit(m_checker);
    return out.str();
  }

  /* The value of a row as a fresh transaction reads it. */
  std::string value(const std::string &row) {
    if (sm1::begin(m_checker, sm1::xctlevel_t::repeatable_read) != 0) {
      return error_name();
    }
    const char *data = nullptr;
    size_t len = 0;
    int rc = sm1::read(m_checker, m_store, row.data(), row.size(), &data, &len);
    std::string v = rc == 0 ? std::string(data, len) : "-";
    sm1::commit(m_checker);
    return v;
  }

  /* How many of the named sessions committed. */
  int committed_count(const std::vector<std::string> &names) {
    int n = 0;
    for (const std::string &name : names) {
      worker_t *w = worker(name);
      n += w && w->commit_state == 1;
    }
    return n;
  }

  /* Run a check and print it as satisfied or FAILED. */
  void run_check(const item_t &it) {
    std::string detail;
    bool ok = true;
    if (it.verb == "rows") {
      // Not an assertion: it prints the state as data.
      m_out << step_text(it) << " -> " << rows() << "\n";
      return;
    } else if (it.verb == "row") {
      std::string got = value(it.arg[0]);
      ok = got == it.arg[1];
      detail = it.arg[0] + " = " + got;
    } else if (it.verb == "committed") {
      ok = committed_count(it.arg) == static_cast<int>(it.arg.size());
      detail = "committed " + std::to_string(committed_count(it.arg)) + " of " +
               std::to_string(it.arg.size());
    } else if (it.verb == "failed") {
      int n = 0;
      for (const std::string &name : it.arg) {
        worker_t *w = worker(name);
        n += w && w->commit_state == -1;
      }
      ok = n == static_cast<int>(it.arg.size());
      detail = std::to_string(n) + " of " + std::to_string(it.arg.size()) +
               " failed";
    } else if (it.verb == "exactly-one-committed") {
      int n = committed_count(it.arg);
      ok = n == 1;
      detail = std::to_string(n) + " committed";
    } else if (it.verb == "not-all-committed") {
      int n = committed_count(it.arg);
      ok = n < static_cast<int>(it.arg.size());
      detail = std::to_string(n) + " of " + std::to_string(it.arg.size()) +
               " committed";
    } else if (it.verb == "at-most-one-committed") {
      int n = committed_count(it.arg);
      ok = n <= 1;
      detail = std::to_string(n) + " committed";
    } else if (it.verb == "last") {
      worker_t *w = worker(it.arg[0]);
      ok = w && w->last == it.arg[1];
      detail = it.arg[0] + " = " + (w ? w->last : "?");
    } else {
      ok = false;
      detail = "unknown check";
    }
    if (ok) {
      m_out << step_text(it) << " -> satisfied\n";
    } else {
      m_out << step_text(it) << " -> FAILED (" << detail << ")\n";
      m_failures++;
    }
  }

  const script_t &m_script;
  pairing_t m_pairing;
  int m_block_ms;
  int m_patience_ms = 60000; /* a step not claimed to block */
  std::ostringstream &m_out;
  std::string m_home;
  sm1::env_t *m_env = nullptr;
  sm1::session_t *m_checker = nullptr;
  uint64_t m_store = 0;
  uint64_t m_rel2 = 0;
  std::vector<std::unique_ptr<worker_t>> m_workers;
  bool m_ok = false;
  int m_failures = 0;
};

} // namespace

/* Run script once per selected pairing (DRIVER.md §2.1). */
int script_run(const script_t &script, int block_ms, std::string *out) {
  std::ostringstream text;
  int failures = 0;
  // The comments a script opens with describe the scenario, so
  // they head the transcript once rather than once per pairing.
  size_t first = 0;
  while (first < script.items.size() && script.items[first].kind == ITEM_TEXT) {
    text << script.items[first].text << "\n";
    first++;
  }
  for (pairing_t p : {PAIR_RR_RR, PAIR_RR_S, PAIR_S_RR, PAIR_S_S}) {
    // The script may ask for one group of pairings only.
    if (!pairing_selected(p, script.pairings)) {
      continue;
    }
    run_t run(script, p, block_ms, &text);
    // A database that did not come up is not a test result.
    if (!run.ok()) {
      *out = text.str();
      return -1;
    }
    failures += run.run(first);
  }
  *out = text.str();
  return failures;
}

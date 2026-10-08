/*
 * run.cc -- running a parsed script (DRIVER.md §2.4, §4).
 *
 * Each session gets a thread, because a step may block on
 * another transaction and the driver must carry on with the next step
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

#include "session.h"
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

/*
 * Every error a step can report: its code, the symbolic name
 * the transcript prints (DRIVER.md §2.2, §4), and the
 * PostgreSQL outcome class a permutation sorts it into
 * (DRIVER.md §6). One row per error, because a name without a
 * class reads as success in error_class() below; adding an
 * error here gives it both at once.
 */
struct errinfo_t {
  sm1::errnum_t num;
  const char *name;
  const char *cls;
};
const errinfo_t errtab[] = {
    {sm1::SM_ERR_CONFLICT, "conflict", "serialize"},
    {sm1::SM_ERR_SERIALIZATION, "serialization", "serialize"},
    {sm1::SM_ERR_DUPKEY, "dupkey", "unique"},
    {sm1::SM_ERR_DEADLOCK, "deadlock", "deadlock"},
    {sm1::SM_ERR_NOTFOUND, "notfound", "abort"},
    {sm1::SM_ERR_NOTRANS, "notrans", "abort"},
    {sm1::SM_ERR_PREPARED, "prepared", "abort"},
    {sm1::SM_ERR_INVALID, "invalid", "abort"},
    {sm1::SM_ERR_UPDATED, "updated", "abort"},
    {sm1::SM_ERR_READONLY, "readonly", "abort"},
};

/* The symbolic name of the current error (DRIVER.md §4). */
std::string error_name() {
  sm1::errnum_t num = sm1::errnum();
  for (const errinfo_t &e : errtab) {
    if (e.num == num) {
      return e.name;
    }
  }
  // Unnamed: the message, which a transcript diff will show as
  // the name DRIVER.md §2.2 says it should have had.
  return std::string("error:") + sm1::errmsg();
}

/* One session of the script, and the thread that drives it. */
struct worker_t {
  std::string name;
  sm1::xctlevel_t level = sm1::xctlevel_t::repeatable_read;
  bool read_only = false; /* begins its transactions read-only */
  sm1::session_t *sess = nullptr;
  uint64_t store = 0;
  uint64_t store2 = 0; /* the second store, for rmstore/lookup */

  std::thread thread;
  std::mutex mu;
  std::condition_variable cv;
  const item_t *req = nullptr; /* the step being run, if any */
  bool done = true;
  bool stop = false;
  std::string result;

  const named_step_t *req_step = nullptr; /* a permutation's step */
  bool begun = false;  /* the session's transaction is begun */
  std::string outcome; /* commit, or the class of the error that ended it */

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
    return sm1::begin(w->sess, w->level, w->read_only) == 0 ? "ok"
                                                            : error_name();
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
    std::string_view data;
    int rc = sm1::read(w->sess, w->store, a[0], data);
    if (rc < 0) {
      return error_name();
    }
    return rc == 1 ? "-" : std::string(data);
  }
  if (it.verb == "readlatest") {
    // readlatest <row>: the newest committed version of a row
    // this transaction holds locked, past its snapshot.
    std::string_view data;
    int rc = sm1::read_latest(w->sess, w->store, a[0], data);
    if (rc < 0) {
      return error_name();
    }
    return rc == 1 ? "-" : std::string(data);
  }
  if (it.verb == "write" || it.verb == "insert") {
    int rc = it.verb == "write" ? sm1::update(w->sess, w->store, a[0], a[1])
                                : sm1::insert(w->sess, w->store, a[0], a[1]);
    return rc == 0 ? "ok" : error_name();
  }
  if (it.verb == "delete") {
    return sm1::erase(w->sess, w->store, a[0]) == 0 ? "ok" : error_name();
  }
  if (it.verb == "update-where") {
    // update-where <lo> <hi> <old> <new>: SQL's UPDATE ... SET
    // v = new WHERE v = old, over the rows lo..hi: a scan, so the
    // read is a range, then a write of each row that matched.
    sm1::scan_t *c = nullptr;
    if (sm1::scan(w->sess, w->store, a[0].data(), a[0].size(), 1, a[1].data(),
                  a[1].size(), 1, c) != 0) {
      return error_name();
    }
    std::vector<std::string> hit;
    std::string_view k, d;
    while (sm1::scannxt(c, k, d) == 0) {
      if (d == a[2]) {
        hit.emplace_back(k);
      }
    }
    sm1::scanend(c);
    std::ostringstream out;
    out << "{";
    for (size_t i = 0; i < hit.size(); i++) {
      if (sm1::update(w->sess, w->store, hit[i], a[3]) != 0) {
        return error_name();
      }
      out << (i ? ", " : "") << hit[i];
    }
    out << "}";
    return out.str();
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
    std::string_view k, d;
    long n = 0;
    while ((limit < 0 || n < limit) && sm1::scannxt(c, k, d) == 0) {
      out << (n++ ? ", " : "") << std::string(k);
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
    std::string_view k, d;
    int rc = sm1::scannxt(it2->second, k, d);
    if (rc < 0) {
      return error_name();
    }
    return rc == 1 ? "end" : std::string(k) + "=" + std::string(d);
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
    return sm1::scan_seek(c->second, a[1], 1) == 0 ? "ok" : error_name();
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
    // lock <row> <is|ix|shared|six|exclusive>: take the row
    // lock in one of Gray's modes without writing the row.
    static const std::pair<const char *, sm1::lockmode_t> modes[] = {
        {"is", sm1::SM_LOCKMODE_IS},        {"ix", sm1::SM_LOCKMODE_IX},
        {"shared", sm1::SM_LOCKMODE_SH},    {"six", sm1::SM_LOCKMODE_SIX},
        {"exclusive", sm1::SM_LOCKMODE_EX},
    };
    for (const auto &[name, mode] : modes) {
      if (a[1] == name) {
        return sm1::lock(w->sess, sm1::LOCKTYPE_ROW, w->store, a[0], mode) == 0
                   ? "ok"
                   : error_name();
      }
    }
    return "ERROR: lock mode must be is, ix, shared, six or exclusive, not " +
           a[1];
  }
  if (it.verb == "lookup") {
    // lookup <row>: read a row of the SECOND store. What
    // the scenario is usually after is the storeid resolution
    // this takes, which is a tracked catalog read (§11).
    std::string_view data;
    int rc = sm1::read(w->sess, w->store2, a[0], data);
    if (rc < 0) {
      return error_name();
    }
    return rc == 1 ? "-" : std::string(data);
  }
  if (it.verb == "rmstore") {
    // Drop the second store; the first one every other verb
    // works in is never dropped.
    return sm1::rmstore(w->sess, w->store2) == 0 ? "ok" : error_name();
  }
  if (it.verb == "mkstore") {
    uint64_t storeid = 0;
    // The storeid itself is generated, so it is not printed.
    return sm1::mkstore(w->sess, storeid) == 0 ? "ok" : error_name();
  }
  if (it.verb == "lsstore") {
    std::vector<uint64_t> list;
    if (sm1::lsstore(w->sess, list) != 0) {
      return error_name();
    }
    return std::to_string(list.size()) + " stores";
  }
  return "ERROR: unknown verb " + it.verb;
}

/*
 * The outcome class an operation's result names, or "" if the
 * operation succeeded: PostgreSQL's errors, as the permutation
 * scripts compare them (DRIVER.md §6). sm1's conflict and its
 * serialization failure are both PostgreSQL's 40001, which is
 * why two rows of errtab share a class.
 */
std::string error_class(const std::string &r) {
  for (const errinfo_t &e : errtab) {
    if (r == e.name) {
      return e.cls;
    }
  }
  // An error error_name() could not name, or one the driver
  // itself raised: PostgreSQL ends the transaction either way.
  if (r.rfind("error:", 0) == 0 || r.rfind("ERROR", 0) == 0) {
    return "abort";
  }
  // Not an error: the step's value, "ok", a row set, a depth.
  return "";
}

/*
 * Run a permutation's step, as PostgreSQL would run its SQL:
 * the session's transaction begins at its first step, which is
 * when PostgreSQL takes the snapshot, and any error ends the
 * transaction, the rest of its steps doing nothing.
 */
std::string execute_step(worker_t *w, const named_step_t &st) {
  if (!w->outcome.empty()) {
    return w->outcome == "commit" ? "ERROR: already committed" : "skipped";
  }
  if (!w->begun) {
    if (sm1::begin(w->sess, w->level, w->read_only) != 0) {
      w->outcome = "abort";
      return "begin: " + error_name();
    }
    w->begun = true;
  }
  std::string out;
  for (const item_t &op : st.ops) {
    std::string r = execute(w, op);
    out += (out.empty() ? "" : " | ") + r;
    if (op.verb == "commit" && r == "ok") {
      w->outcome = "commit";
    }
    std::string cls = error_class(r);
    if (!cls.empty()) {
      // PostgreSQL aborts the transaction on any error; sm1 has
      // already, for a retryable one.
      w->outcome = cls;
      sm1::abort(w->sess);
      break;
    }
  }
  return out;
}

/* The loop each session's thread runs. */
void worker_main(worker_t *w) {
  for (;;) {
    const item_t *it = nullptr;
    const named_step_t *st = nullptr;
    {
      std::unique_lock<std::mutex> lk(w->mu);
      w->cv.wait(lk, [w] { return w->req || w->req_step || w->stop; });
      if (w->stop) {
        return;
      }
      it = w->req;
      st = w->req_step;
    }
    std::string res = st ? execute_step(w, *st) : execute(w, *it);
    {
      std::lock_guard<std::mutex> lk(w->mu);
      w->result = res;
      w->req = nullptr;
      w->req_step = nullptr;
      w->done = true;
    }
    w->cv.notify_all();
  }
}

/* Whether the session's transaction waits on a lock or a writer. */
bool waiting(sm1::session_t *s) {
  std::lock_guard<std::mutex> guard(s->db->lock_mu);
  return !s->waiting_for.empty();
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
    // A permutation script runs hundreds of short transactions,
    // and what it checks never needs a commit to survive a crash:
    // its database does not sync each commit (DRIVER.md §6).
    sm1::options_t opt;
    if (!m_script.perms.empty()) {
      opt.durability_sync = 0;
    }
    sm1::opendb(m_env, "db", m_checker, &opt);
    m_ok = m_checker &&
           sm1::begin(m_checker, sm1::xctlevel_t::repeatable_read) == 0 &&
           sm1::mkstore(m_checker, m_store) == 0 &&
           sm1::mkstore(m_checker, m_store2) == 0 &&
           sm1::commit(m_checker) == 0;
    m_ok = m_ok && make_stores();

    // One session, and one thread, per declared session. A
    // worker holds a mutex and a thread, so it is held by
    // pointer rather than by value.
    for (size_t i = 0; i < m_script.sessions.size(); i++) {
      m_workers.push_back(std::make_unique<worker_t>());
      worker_t &w = *m_workers[i];
      const session_decl_t &d = m_script.sessions[i];
      w.name = d.name;
      w.level = level_for(d, i);
      w.read_only = d.read_only;
      sm1::opendb(m_env, "db", w.sess);
      w.store = m_store;
      w.store2 = m_store2;
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

  /*
   * Set the two stores' rows to the seeds, in one transaction. A
   * permutation script calls it again before each permutation
   * after the first, so each starts from the seeded rows, with
   * every row an earlier permutation added deleted again.
   */
  bool make_stores() {
    bool ok = sm1::begin(m_checker, sm1::xctlevel_t::repeatable_read) == 0;
    for (uint64_t store : {m_store, m_store2}) {
      // Delete every row there is, then insert the seeds.
      std::vector<std::string> keys;
      sm1::scan_t *c = nullptr;
      ok = ok &&
           sm1::scan(m_checker, store, nullptr, 0, 0, nullptr, 0, 0, c) == 0;
      std::string_view k, d;
      while (ok && sm1::scannxt(c, k, d) == 0) {
        keys.emplace_back(k);
      }
      if (c) {
        sm1::scanend(c);
      }
      for (const std::string &key : keys) {
        ok = ok && sm1::erase(m_checker, store, key) == 0;
      }
      // The same rows in both, so a script can read either.
      for (const auto &kv : m_script.seeds) {
        ok = ok && sm1::insert(m_checker, store, kv.first, kv.second) == 0;
      }
    }
    ok = ok && sm1::commit(m_checker) == 0;
    // Every session works in the new stores, from a clean slate.
    for (std::unique_ptr<worker_t> &w : m_workers) {
      w->store = m_store;
      w->store2 = m_store2;
      w->begun = false;
      w->outcome.clear();
      w->last.clear();
      w->commit_state = 0;
    }
    return ok;
  }

  /*
   * Ready the run for its next permutation: end any transaction
   * the last one left open, then make fresh stores.
   */
  bool next_permutation() {
    for (std::unique_ptr<worker_t> &w : m_workers) {
      for (auto &[name, c] : w->cursors) {
        sm1::scanend(c);
      }
      w->cursors.clear();
      // A session whose permutation ended before its commit.
      if (w->begun && w->outcome.empty()) {
        sm1::abort(w->sess);
      }
    }
    return make_stores();
  }

  /* Failures other than outcomes: a hang, a step out of turn. */
  int failures() const { return m_failures; }

  /* Whether the database and sessions came up. */
  bool ok() const { return m_ok; }

  /*
   * Run one permutation (DRIVER.md §6): each step to its
   * session, a step that waits reported as such and printed
   * again when it finishes, then each session's outcome against
   * PostgreSQL's. Returns 1 if the outcomes differ, else 0.
   */
  int run_permutation(size_t index, const permutation_t &p) {
    m_out << "== permutation " << index << ":";
    for (const std::string &s : p.steps) {
      m_out << " " << s;
    }
    m_out << " ==\n";
    std::vector<const named_step_t *> pending(m_workers.size(), nullptr);
    for (const std::string &name : p.steps) {
      const named_step_t *st = nullptr;
      for (const named_step_t &s : m_script.steps) {
        if (s.name == name) {
          st = &s;
        }
      }
      size_t wi = worker_index(st->session);
      worker_t &w = *m_workers[wi];
      // A step that finished since is printed before the next.
      drain_steps(pending, m_block_ms);
      if (pending[wi]) {
        m_out << name << " " << w.name << " -> ERROR: " << w.name
              << " is waiting\n";
        m_failures++;
        continue;
      }
      std::unique_lock<std::mutex> lk(w.mu);
      w.req_step = st;
      w.done = false;
      w.cv.notify_all();
      // Done, or waiting on another transaction: that is how
      // PostgreSQL's tester tells a waiting step too.
      bool finished = false;
      for (int ms = 0; ms < m_patience_ms; ms++) {
        if (w.cv.wait_for(lk, std::chrono::milliseconds(1),
                          [&w] { return w.done; })) {
          finished = true;
          break;
        }
        lk.unlock();
        bool waits = waiting(w.sess);
        lk.lock();
        if (waits && !w.done) {
          break;
        }
      }
      finished = finished || w.done;
      if (!finished) {
        pending[wi] = st;
        lk.unlock();
        m_out << name << " " << w.name << " -> waiting\n";
        continue;
      }
      std::string r = w.result;
      lk.unlock();
      m_out << name << " " << w.name << " -> " << r << "\n";
    }
    // Every step must finish by the end: wait properly first.
    drain_steps(pending, m_patience_ms);
    for (size_t i = 0; i < pending.size(); i++) {
      if (pending[i]) {
        m_out << pending[i]->name << " " << m_workers[i]->name
              << " -> STILL WAITING\n";
        m_failures++;
      }
    }
    // Each session's outcome, against PostgreSQL's.
    std::string got, pg, ours;
    bool as_pg = true, as_noted = !p.sm1.empty();
    for (const auto &[name, o] : p.expect) {
      worker_t &w = *m_workers[worker_index(name)];
      std::string mine = w.outcome.empty() ? "open" : w.outcome;
      got += (got.empty() ? "" : " ") + name + "=" + mine;
      pg += (pg.empty() ? "" : " ") + name + "=" + o;
      as_pg = as_pg && mine == o;
      std::string noted = o;
      for (const auto &[n2, o2] : p.sm1) {
        if (n2 == name) {
          noted = o2;
        }
      }
      ours += (ours.empty() ? "" : " ") + name + "=" + noted;
      as_noted = as_noted && mine == noted;
    }
    m_out << "outcome " << got;
    if (as_pg) {
      m_out << " -> as PostgreSQL\n";
      return 0;
    }
    if (as_noted) {
      m_out << " -> differs from PostgreSQL (" << pg << ") as noted\n";
      return 0;
    }
    m_out << " -> FAILED (PostgreSQL: " << pg << ")\n";
    return 1;
  }

  /* Run every item of the script, returning the failed checks. */
  int run(size_t first) {
    m_out << "== pairing " << pairing_label(m_pairing) << " ==";
    for (const std::unique_ptr<worker_t> &w : m_workers) {
      m_out << " " << w->name << "="
            << (w->level == sm1::xctlevel_t::serializable     ? "SER"
                : w->level == sm1::xctlevel_t::read_committed ? "RC"
                                                              : "RR")
            << (w->read_only ? "-RO" : "");
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
    std::string l = pairing_level(m_pairing, index == 0 ? 0 : 1);
    return l == "rc"   ? sm1::xctlevel_t::read_committed
           : l == "rr" ? sm1::xctlevel_t::repeatable_read
                       : sm1::xctlevel_t::serializable;
  }

  /* The index of a named session's worker. */
  size_t worker_index(const std::string &name) const {
    for (size_t i = 0; i < m_workers.size(); i++) {
      if (m_workers[i]->name == name) {
        return i;
      }
    }
    return 0;
  }

  /* Print each waiting permutation step that has finished. */
  void drain_steps(std::vector<const named_step_t *> &pending, int wait_ms) {
    for (size_t i = 0; i < pending.size(); i++) {
      if (!pending[i]) {
        continue;
      }
      worker_t &w = *m_workers[i];
      std::unique_lock<std::mutex> lk(w.mu);
      if (!w.cv.wait_for(lk, std::chrono::milliseconds(wait_ms),
                         [&w] { return w.done; })) {
        continue;
      }
      std::string r = w.result;
      lk.unlock();
      m_out << pending[i]->name << " " << w.name << " -> " << r
            << " (resumed)\n";
      pending[i] = nullptr;
    }
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
    std::string_view k, d;
    int n = 0;
    while (c && sm1::scannxt(c, k, d) == 0) {
      out << (n++ ? ", " : "") << std::string(k) << "=" << std::string(d);
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
    std::string_view data;
    int rc = sm1::read(m_checker, m_store, row, data);
    std::string v = rc == 0 ? std::string(data) : "-";
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
  uint64_t m_store2 = 0;
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
  // A permutation script runs each permutation on a database of
  // its own; the sessions name their levels, so there are no
  // pairings (DRIVER.md §6).
  if (!script.perms.empty()) {
    run_t run(script, PAIR_RR_RR, block_ms, &text);
    for (size_t i = 0; i < script.perms.size(); i++) {
      // The first permutation has the stores the run made.
      if (!run.ok() || (i > 0 && !run.next_permutation())) {
        *out = text.str();
        return -1;
      }
      failures += run.run_permutation(i + 1, script.perms[i]);
    }
    failures += run.failures();
    *out = text.str();
    return failures;
  }
  // "levels" runs every pairing of the three levels, READ
  // COMMITTED first; the other groups only RR and SERIALIZABLE.
  std::vector<pairing_t> order = {PAIR_RR_RR, PAIR_RR_S, PAIR_S_RR, PAIR_S_S};
  if (script.pairings == "levels") {
    order = {PAIR_RC_RC, PAIR_RC_RR, PAIR_RC_S, PAIR_RR_RC, PAIR_RR_RR,
             PAIR_RR_S,  PAIR_S_RC,  PAIR_S_RR, PAIR_S_S};
  }
  for (pairing_t p : order) {
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

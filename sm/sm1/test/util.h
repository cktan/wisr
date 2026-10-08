/*
 * util.h -- helpers shared by the test files.
 */
#ifndef WISR_TEST_UTIL_H
#define WISR_TEST_UTIL_H

#include "doctest.h"

#include "sm1.h"

#include <stdlib.h>

#include <filesystem>
#include <set>
#include <string>
#include <string_view>
#include <vector>

/* A fresh directory under the system temp dir, removed at scope exit. */
class tmpdir_t {
public:
  /* Create the directory. */
  tmpdir_t() {
    std::string tmpl =
        (std::filesystem::temp_directory_path() / "wisr-test-XXXXXX").string();
    m_path = mkdtemp(tmpl.data());
  }

  /* Remove the directory and everything in it. */
  ~tmpdir_t() {
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
  }

  /* The directory's path. */
  const char *path() const { return m_path.c_str(); }

private:
  std::string m_path;
};

/*
 * sm1::attach, handing back the environment, or nullptr with the
 * error set: the shape tests check it in.
 */
inline sm1::env_t *attach(std::string_view homedir) {
  sm1::env_t *env = nullptr;
  attach(homedir, env);
  return env;
}

/* sm1::opendb, handing back the session, or nullptr with the error set. */
inline sm1::session_t *opendb(sm1::env_t *env, std::string_view dbname,
                              const sm1::options_t *opt = nullptr) {
  sm1::session_t *s = nullptr;
  opendb(env, dbname, s, opt);
  return s;
}

/* sm1::scan, handing back the cursor, or nullptr with the error set. */
inline sm1::scan_t *scan(sm1::session_t *s, uint64_t storeid, const char *skey,
                         size_t sklen, int skey_inclusive, const char *ekey,
                         size_t eklen, int ekey_inclusive) {
  sm1::scan_t *c = nullptr;
  scan(s, storeid, skey, sklen, skey_inclusive, ekey, eklen, ekey_inclusive, c);
  return c;
}

/* A home directory, initialized and attached for one test. */
class home_t {
public:
  /* Initialize and attach a fresh home directory. */
  home_t() {
    REQUIRE(sm1::init(m_tmp.path()) == 0);
    m_env = ::attach(m_tmp.path());
    REQUIRE(m_env);
  }

  /* Detach, then remove the directory. */
  ~home_t() { sm1::detach(m_env, 1); }

  /* The environment. */
  sm1::env_t *env() const { return m_env; }

  /* Detach and attach again, as a new process would. */
  void reattach() {
    REQUIRE(sm1::detach(m_env, 0) == 0);
    m_env = ::attach(m_tmp.path());
    REQUIRE(m_env);
  }

  /* Detach, closing any open sessions, so another process may attach. */
  void detach() {
    REQUIRE(sm1::detach(m_env, 1) == 0);
    m_env = nullptr;
  }

  /* Attach again after detach(). */
  void attach() {
    m_env = ::attach(m_tmp.path());
    REQUIRE_MESSAGE(m_env, sm1::errmsg());
  }

  /* The home directory's path. */
  const char *path() const { return m_tmp.path(); }

  /* The names of the entries under base/. */
  std::set<std::string> base_entries() const {
    std::set<std::string> out;
    for (const auto &e : std::filesystem::directory_iterator(
             std::string(m_tmp.path()) + "/base")) {
      out.insert(e.path().filename().string());
    }
    return out;
  }

private:
  tmpdir_t m_tmp;
  sm1::env_t *m_env = nullptr;
};

/*
 * The value of key in store as sess reads it, or "<none>" if the
 * key is absent. Fails the test on an error.
 */
inline std::string get(sm1::session_t *sess, uint64_t store,
                       const std::string &key) {
  std::string_view data;
  int r = sm1::read(sess, store, key, data);
  REQUIRE_MESSAGE(r >= 0, sm1::errmsg());
  return r == 1 ? "<none>" : std::string(data);
}

/* Insert key -> val into store, failing the test on an error. */
inline void put(sm1::session_t *sess, uint64_t store, const std::string &key,
                const std::string &val) {
  REQUIRE_MESSAGE(sm1::insert(sess, store, key, val) == 0, sm1::errmsg());
}

/* Create a store and return its storeid, failing the test on an error. */
inline uint64_t mkstore(sm1::session_t *sess) {
  uint64_t storeid = 0;
  REQUIRE_MESSAGE(sm1::mkstore(sess, storeid) == 0, sm1::errmsg());
  return storeid;
}

/* The storeids sess's transaction sees, failing the test on an error. */
inline std::vector<uint64_t> lsstore(sm1::session_t *sess) {
  std::vector<uint64_t> out;
  REQUIRE_MESSAGE(sm1::lsstore(sess, out) == 0, sm1::errmsg());
  return out;
}

#endif /* WISR_TEST_UTIL_H */

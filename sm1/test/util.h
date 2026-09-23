/*
 * util.h -- helpers shared by the test files.
 */
#ifndef WISR_TEST_UTIL_H
#define WISR_TEST_UTIL_H

#include "doctest.h"

#include "sm.h"

#include <stdlib.h>

#include <filesystem>
#include <set>
#include <string>
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

/* A home directory, initialized and attached for one test. */
class home_t {
public:
  /* Initialize and attach a fresh home directory. */
  home_t() {
    REQUIRE(sm_init(m_tmp.path()) == 0);
    m_env = sm_attach(m_tmp.path());
    REQUIRE(m_env);
  }

  /* Detach, then remove the directory. */
  ~home_t() { sm_detach(m_env, 1); }

  /* The environment. */
  sm_env_t *env() const { return m_env; }

  /* Detach and attach again, as a new process would. */
  void reattach() {
    REQUIRE(sm_detach(m_env, 0) == 0);
    m_env = sm_attach(m_tmp.path());
    REQUIRE(m_env);
  }

  /* Detach, closing any open sessions, so another process may attach. */
  void detach() {
    REQUIRE(sm_detach(m_env, 1) == 0);
    m_env = nullptr;
  }

  /* Attach again after detach(). */
  void attach() {
    m_env = sm_attach(m_tmp.path());
    REQUIRE_MESSAGE(m_env, sm_errmsg());
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
  sm_env_t *m_env = nullptr;
};

/*
 * The value of key in store as sess reads it, or "<none>" if the
 * key is absent. Fails the test on an error.
 */
inline std::string get(sm_session_t *sess, uint64_t store,
                       const std::string &key) {
  const char *data = nullptr;
  size_t len = 0;
  int r = sm_read(sess, store, key.data(), key.size(), &data, &len);
  REQUIRE_MESSAGE(r >= 0, sm_errmsg());
  return r == 1 ? "<none>" : std::string(data, len);
}

/* Insert key -> val into store, failing the test on an error. */
inline void put(sm_session_t *sess, uint64_t store, const std::string &key,
                const std::string &val) {
  REQUIRE_MESSAGE(sm_insert(sess, store, key.data(), key.size(), val.data(),
                            val.size()) == 0,
                  sm_errmsg());
}

/* Create a store and return its storeid, failing the test on an error. */
inline uint64_t mkstore(sm_session_t *sess) {
  uint64_t storeid = 0;
  REQUIRE_MESSAGE(sm_mkstore(sess, &storeid) == 0, sm_errmsg());
  return storeid;
}

/* The storeids sess's transaction sees, failing the test on an error. */
inline std::vector<uint64_t> lsstore(sm_session_t *sess) {
  uint64_t *list = nullptr;
  size_t n = 0;
  REQUIRE_MESSAGE(sm_lsstore(sess, &list, &n) == 0, sm_errmsg());
  std::vector<uint64_t> out(list, list + n);
  free(list);
  return out;
}

#endif /* WISR_TEST_UTIL_H */

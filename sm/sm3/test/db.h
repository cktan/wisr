/*
 * db.h -- a throwaway sm1 database for sm3's tests: a fresh home
 * and a database, removed at the end.
 */
#ifndef SM3_TEST_DB_H
#define SM3_TEST_DB_H

#include "doctest.h"

#include "sm1.h"

#include <stdlib.h>

#include <filesystem>
#include <string>

/* A fresh sm1 home with a database "db", and sessions on it. */
class db_t {
public:
  /* Initialize a home and a database "db" in it, with opt's options. */
  explicit db_t(const sm1::options_t &opt = sm1::options_t()) : m_opt(opt) {
    std::string tmpl =
        (std::filesystem::temp_directory_path() / "sm3-XXXXXX").string();
    m_path = mkdtemp(tmpl.data());
    REQUIRE(sm1::init(m_path) == 0);
    REQUIRE(sm1::attach(m_path, m_env) == 0);
    REQUIRE(sm1::mkdb(m_env, "db", &m_opt) == 0);
  }

  /* Detach, closing every session, then remove the home. */
  ~db_t() {
    sm1::detach(m_env, 1);
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
  }

  /* A new session on "db". */
  sm1::session_t *open() {
    sm1::session_t *s = nullptr;
    REQUIRE(sm1::opendb(m_env, "db", s, &m_opt) == 0);
    return s;
  }

  /*
   * A new session on "db", or nullptr: for a thread other than the
   * test's, where a failed REQUIRE has nowhere to go.
   */
  sm1::session_t *try_open() {
    sm1::session_t *s = nullptr;
    return sm1::opendb(m_env, "db", s, &m_opt) == 0 ? s : nullptr;
  }

  /* The home's environment, for a call that opens its own session. */
  sm1::env_t *env() { return m_env; }

  /* Close every session and reattach the home, as a restart does. */
  void restart() {
    REQUIRE(sm1::detach(m_env, 1) == 0);
    REQUIRE(sm1::attach(m_path, m_env) == 0);
  }

private:
  std::string m_path;
  sm1::env_t *m_env = nullptr;
  sm1::options_t m_opt; /* the options "db" is made and opened with */
};

#endif /* SM3_TEST_DB_H */

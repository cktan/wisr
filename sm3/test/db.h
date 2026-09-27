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
  /* Initialize a home and a database "db" in it. */
  db_t() {
    std::string tmpl =
        (std::filesystem::temp_directory_path() / "sm3-XXXXXX").string();
    m_path = mkdtemp(tmpl.data());
    REQUIRE(sm1::init(m_path) == 0);
    REQUIRE(sm1::attach(m_path, m_env) == 0);
    REQUIRE(sm1::mkdb(m_env, "db") == 0);
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
    REQUIRE(sm1::opendb(m_env, "db", s) == 0);
    return s;
  }

  /* Close every session and reattach the home, as a restart does. */
  void restart() {
    REQUIRE(sm1::detach(m_env, 1) == 0);
    REQUIRE(sm1::attach(m_path, m_env) == 0);
  }

private:
  std::string m_path;
  sm1::env_t *m_env = nullptr;
};

#endif /* SM3_TEST_DB_H */

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * db.h -- a throwaway sm1 database for the tests that run sm2 over
 * sm1: a fresh home, a database and a store, removed at the end.
 */
#ifndef SM2_TEST_DB_H
#define SM2_TEST_DB_H

#include "doctest.h"

#include "sm1.h"
#include "sm2.h"

#include <stdlib.h>

#include <filesystem>
#include <memory>
#include <string>

/*
 * A raw pin a test owns, freed when it goes out of scope; made
 * after the db_t, so it is emptied before the database closes.
 */
struct rawpin_ptr_t
    : std::unique_ptr<sm1::rawpin_t, void (*)(sm1::rawpin_t *)> {
  rawpin_ptr_t()
      : std::unique_ptr<sm1::rawpin_t, void (*)(sm1::rawpin_t *)>(
            sm1::rawpin_new(), sm1::rawpin_free) {
    REQUIRE(get());
  }
};

/*
 * A fresh sm1 home, database and store with a rowno sequence, one
 * for TOAST ids and one for trees' page ids, and sessions on it; all
 * of it is removed at the end.
 */
class db_t {
public:
  /*
   * Initialize a home, a database "db", a store, "rowno" and "toast",
   * attached with opt, if any.
   */
  explicit db_t(const sm1::env_options_t *opt = nullptr) {
    std::string tmpl =
        (std::filesystem::temp_directory_path() / "sm2-rel-XXXXXX").string();
    m_path = mkdtemp(tmpl.data());
    REQUIRE(sm1::init(m_path) == 0);
    REQUIRE(sm1::attach(m_path, m_env, opt) == 0);
    REQUIRE(sm1::mkdb(m_env, "db") == 0);
    sm1::session_t *s = open();
    REQUIRE(sm1::begin(s, sm1::xctlevel_t::repeatable_read) == 0);
    REQUIRE(sm1::mkstore(s, m_store) == 0);
    REQUIRE(sm1::mkseq(s, "rowno", 1, 1) == 0);
    REQUIRE(sm1::mkseq(s, "toast", 1, 1) == 0);
    REQUIRE(sm1::mkseq(s, sm2::tree_page_seq, 1, 1) == 0);
    REQUIRE(sm1::commit(s) == 0);
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

  /* The store the rels live in. */
  uint64_t store() const { return m_store; }

private:
  std::string m_path;
  sm1::env_t *m_env = nullptr;
  uint64_t m_store = 0;
};

#endif /* SM2_TEST_DB_H */

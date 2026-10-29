/*
 * home.h -- a throwaway sm home with pg_global in it, for aa's tests:
 * made by aa_create(), read by aa_load(), removed at the end.
 */
#ifndef AA_TEST_HOME_H
#define AA_TEST_HOME_H

#include "doctest.h"

#include "aa.h"
#include "sm.h"

#include <stdlib.h>

#include <filesystem>
#include <string>

/* A fresh home with pg_global, its superuser "admin", and aa's state. */
class home_t {
public:
  /* Initialize a home, make pg_global in it, and read it. */
  explicit home_t(bool create = true) {
    std::string tmpl =
        (std::filesystem::temp_directory_path() / "aa-XXXXXX").string();
    m_path = mkdtemp(tmpl.data());
    REQUIRE(0 == sm_init(m_path.c_str()));
    REQUIRE(0 == sm_attach(m_path.c_str(), &m_sm));
    if (create) {
      REQUIRE(0 == aa_create(m_sm, "admin", NULL));
      REQUIRE(0 == aa_load(m_sm, &m_aa));
    }
  }

  /* Drop aa's state, detach, closing every session, remove the home. */
  ~home_t() {
    aa_close(m_aa);
    sm_detach(m_sm, 1);
    std::error_code ec;
    std::filesystem::remove_all(m_path, ec);
  }

  /*
   * As a restart: drop aa's state and every session, reattach the home
   * and read pg_global again. Sessions made before are gone.
   */
  void restart() {
    aa_close(m_aa);
    m_aa = NULL;
    REQUIRE(0 == sm_detach(m_sm, 1));
    REQUIRE(0 == sm_attach(m_path.c_str(), &m_sm));
    REQUIRE(0 == aa_load(m_sm, &m_aa));
  }

  sm_env_t *sm() { return m_sm; }
  aa_env_t *aa() { return m_aa; }

private:
  std::string m_path;
  sm_env_t *m_sm = NULL;
  aa_env_t *m_aa = NULL;
};

/* The role named name in roles, or NULL. */
inline const aa_role_t *find_role(const aa_roles_t &roles, const char *name) {
  for (const aa_role_t &r : roles.roles)
    if (r.rolname == name)
      return &r;
  return NULL;
}

#endif /* AA_TEST_HOME_H */

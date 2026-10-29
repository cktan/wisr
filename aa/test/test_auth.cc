/*
 * test_auth.cc -- how a client gets in (ROLE.md section 5): the method it
 * must prove itself by, the secret, and the checks after the exchange.
 */
#include "doctest.h"

#include "home.h"

/* Commit a role named name, with a password secret if not NULL. */
static aa_role_t add_role(home_t &h, const char *name, const char *secret) {
  aa_session_t *s = aa_session_new(h.aa());
  aa_role_t role;

  REQUIRE(0 == aa_new_oid(s, &role.oid));
  role.rolname = name;
  role.rolcanlogin = true;
  role.has_password = NULL != secret;
  if (NULL != secret)
    role.rolpassword = secret;
  aa_put_role(s, role);
  REQUIRE(0 == aa_commit(s));
  aa_session_free(s);
  return role;
}

// Verify the method a client must prove itself by: peer on the local
// socket by its own name, or as the data directory's owner asking for
// the bootstrap superuser; the password otherwise, and always over TCP.
// 1. Ask on the local socket with the OS user the role's name.
// 2. Ask as another OS user, and with the OS user unknown.
// 3. Ask as the data directory's owner for the superuser, and for
//    another role; and as another OS user for the superuser.
// 4. Ask over TCP with the matching name.
TEST_CASE("auth: method") {
  home_t h;
  bool owner;

  // 1.
  CHECK(AA_PEER ==
        aa_auth_method(h.aa(), "alice", true, "alice", false, &owner));
  CHECK(!owner);

  // 2.
  CHECK(AA_PASSWORD ==
        aa_auth_method(h.aa(), "alice", true, "bob", false, &owner));
  CHECK(AA_PASSWORD ==
        aa_auth_method(h.aa(), "alice", true, NULL, true, &owner));

  // 3.
  CHECK(AA_PEER == aa_auth_method(h.aa(), "admin", true, "srv", true, &owner));
  CHECK(owner);
  CHECK(AA_PASSWORD ==
        aa_auth_method(h.aa(), "alice", true, "srv", true, &owner));
  CHECK(!owner);
  CHECK(AA_PASSWORD ==
        aa_auth_method(h.aa(), "admin", true, "srv", false, &owner));
  CHECK(!owner);

  // 4.
  CHECK(AA_PASSWORD ==
        aa_auth_method(h.aa(), "alice", false, "alice", true, &owner));
  CHECK(!owner);
}

// Verify the secret handed to the exchange, and each reason one is not.
// 1. Ask for a role's that has a password.
// 2. Ask for a role that does not exist, and one with no password.
// 3. Give the first a VALID UNTIL; ask before it and after it.
TEST_CASE("auth: secret") {
  home_t h;
  std::string secret, why;

  // 1.
  aa_role_t alice = add_role(h, "alice", "SCRAM-SHA-256$x");
  add_role(h, "bob", NULL);
  CHECK(aa_role_secret(h.aa(), "alice", 0, &secret, &why));
  CHECK("SCRAM-SHA-256$x" == secret);

  // 2.
  CHECK(!aa_role_secret(h.aa(), "carol", 0, &secret, &why));
  CHECK("Role \"carol\" does not exist." == why);
  CHECK(!aa_role_secret(h.aa(), "bob", 0, &secret, &why));
  CHECK("User \"bob\" has no password assigned." == why);

  // 3.
  aa_session_t *s = aa_session_new(h.aa());
  alice.has_validuntil = true;
  alice.rolvaliduntil = 1000;
  aa_put_role(s, alice);
  REQUIRE(0 == aa_commit(s));
  aa_session_free(s);
  CHECK(aa_role_secret(h.aa(), "alice", 1000, &secret, &why));
  CHECK(!aa_role_secret(h.aa(), "alice", 1001, &secret, &why));
  CHECK("User \"alice\" has an expired password." == why);
}

// Verify the checks after the exchange: LOGIN, VALID UNTIL and
// CONNECTION LIMIT, which a superuser has none of, and the data
// directory's owner passes whatever the role says.
// 1. Check a LOGIN role with no limit.
// 2. Take LOGIN away; then give it back with a VALID UNTIL passed.
// 3. Give it a limit of 2; check with 2 sessions and with 3.
// 4. Make it a superuser; check with 3.
// 5. Take LOGIN away again; check as the data directory's owner.
TEST_CASE("auth: login okay") {
  const char *why;
  aa_role_t r;

  // 1.
  r.rolcanlogin = true;
  CHECK(aa_login_okay(r, 0, 100, false, &why));

  // 2.
  r.rolcanlogin = false;
  CHECK(!aa_login_okay(r, 0, 1, false, &why));
  CHECK(std::string("is not permitted to log in") == why);
  r.rolcanlogin = true;
  r.has_validuntil = true;
  r.rolvaliduntil = 10;
  CHECK(aa_login_okay(r, 10, 1, false, &why));
  CHECK(!aa_login_okay(r, 11, 1, false, &why));
  CHECK(std::string("is past its VALID UNTIL") == why);
  r.has_validuntil = false;

  // 3.
  r.rolconnlimit = 2;
  CHECK(aa_login_okay(r, 0, 2, false, &why));
  CHECK(!aa_login_okay(r, 0, 3, false, &why));
  CHECK(std::string("has reached its CONNECTION LIMIT") == why);

  // 4.
  r.rolsuper = true;
  CHECK(aa_login_okay(r, 0, 3, false, &why));

  // 5.
  r.rolcanlogin = false;
  CHECK(aa_login_okay(r, 0, 1, true, &why));
}

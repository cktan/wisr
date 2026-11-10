/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * test_roles.cc -- pg_global's roles: as initdb makes them, as role DDL
 * changes them, and as two sessions changing one conflict.
 */
#include "doctest.h"

#include <atomic>
#include <thread>

#include "home.h"

// Verify that aa_create() makes the superuser and PostgreSQL's
// predefined roles, and pg_monitor's memberships.
// 1. Make and read pg_global.
// 2. Confirm the superuser, oid 10, is SUPERUSER LOGIN, not REPLICATION.
// 3. Confirm the 16 predefined roles, and pg_monitor's 3 memberships.
// 4. Confirm the next oid is the first normal one past the memberships.
TEST_CASE("roles: as initdb makes them") {
  // 1.
  home_t h;
  auto roles = aa_roles(h.aa());

  // 2.
  const aa_role_t *admin = find_role(*roles, "admin");
  REQUIRE(nullptr != admin);
  CHECK(AA_BOOTSTRAP_SUPERUSER == admin->oid);
  CHECK(admin->rolsuper);
  CHECK(admin->rolcanlogin);
  CHECK(!admin->rolreplication);
  CHECK(!admin->has_password);

  // 3.
  CHECK(17 == roles->roles.size());
  CHECK(nullptr != find_role(*roles, "pg_database_owner"));
  REQUIRE(3 == roles->members.size());
  for (const aa_member_t &m : roles->members)
    CHECK(3373 == m.member);

  // 4.
  aa_session_t *s = aa_session_new(h.aa());
  aa_oid_t oid;
  REQUIRE(0 == aa_new_oid(s, &oid));
  CHECK(AA_FIRST_NORMAL_OID + 3 == oid);
  aa_session_free(s);
}

// Verify that a home with no pg_global is refused, by its own error.
// 1. Make a home without pg_global.
// 2. Read it with aa_load().
// 3. Confirm AA_ERR_NODB.
TEST_CASE("roles: no pg_global") {
  // 1.
  home_t h(false);
  aa_env_t *env = NULL;

  // 2.
  CHECK(-1 == aa_load(h.sm(), &env));

  // 3.
  CHECK(AA_ERR_NODB == aa_errnum());
}

// Verify that a role made in a session is its own until the commit,
// then every session's, and survives a restart.
// 1. In one session, make a role with a password and a VALID UNTIL.
// 2. Confirm that session sees it, another does not.
// 3. Commit; confirm the other sees it, told once its roles moved.
// 4. Restart; confirm the role reads back the same.
TEST_CASE("roles: create, commit, restart") {
  home_t h;
  aa_session_t *s1 = aa_session_new(h.aa());
  aa_session_t *s2 = aa_session_new(h.aa());
  aa_role_t role;

  // 1.
  REQUIRE(0 == aa_new_oid(s1, &role.oid));
  role.rolname = "alice";
  role.rolcanlogin = true;
  role.has_password = true;
  role.rolpassword = "SCRAM-SHA-256$4096:salt$key:server";
  role.has_validuntil = true;
  role.rolvaliduntil = 123456789;
  aa_put_role(s1, role);

  // 2.
  CHECK(nullptr != find_role(*aa_session_roles(s1), "alice"));
  CHECK(nullptr == find_role(*aa_session_roles(s2), "alice"));
  (void)aa_roles_moved(s2);

  // 3.
  REQUIRE(0 == aa_commit(s1));
  REQUIRE(0 == aa_post_commit(s1));
  CHECK(nullptr != find_role(*aa_session_roles(s2), "alice"));
  CHECK(aa_roles_moved(s2));
  CHECK(!aa_roles_moved(s2));
  CHECK(!aa_roles_moved(s1));
  aa_session_free(s1);
  aa_session_free(s2);

  // 4.
  h.restart();
  const aa_role_t *back = find_role(*aa_roles(h.aa()), "alice");
  REQUIRE(nullptr != back);
  CHECK(role.oid == back->oid);
  CHECK(back->rolcanlogin);
  CHECK(role.rolpassword == back->rolpassword);
  CHECK(back->has_validuntil);
  CHECK(123456789 == back->rolvaliduntil);
}

// Verify that a session's own role commit does not hide another's before
// it: a session that had not seen the other's is told its roles moved.
// 1. Two sessions, each up to date.
// 2. The second makes a role and commits; the first does not ask.
// 3. The first makes a role and commits.
// 4. Confirm the first is told its roles moved, once.
TEST_CASE("roles: an own commit does not hide another's") {
  home_t h;
  aa_session_t *s1 = aa_session_new(h.aa());
  aa_session_t *s2 = aa_session_new(h.aa());
  aa_role_t bob, carol;

  // 1.
  (void)aa_roles_moved(s1);
  (void)aa_roles_moved(s2);

  // 2.
  REQUIRE(0 == aa_new_oid(s2, &bob.oid));
  bob.rolname = "bob";
  aa_put_role(s2, bob);
  REQUIRE(0 == aa_commit(s2));
  REQUIRE(0 == aa_post_commit(s2));

  // 3.
  REQUIRE(0 == aa_new_oid(s1, &carol.oid));
  carol.rolname = "carol";
  aa_put_role(s1, carol);
  REQUIRE(0 == aa_commit(s1));
  REQUIRE(0 == aa_post_commit(s1));

  // 4.
  CHECK(aa_roles_moved(s1));
  CHECK(!aa_roles_moved(s1));
  aa_session_free(s1);
  aa_session_free(s2);
}

// Verify that two sessions changing one role conflict, the second to
// commit failing and writing nothing.
// 1. Two sessions each rename the superuser.
// 2. Commit the first.
// 3. Commit the second: AA_ERR_CONFLICT.
// 4. Confirm the first's name stands, after a restart too.
TEST_CASE("roles: conflict") {
  home_t h;
  aa_session_t *s1 = aa_session_new(h.aa());
  aa_session_t *s2 = aa_session_new(h.aa());

  // 1.
  aa_role_t r1 = *find_role(*aa_roles(h.aa()), "admin");
  aa_role_t r2 = r1;
  r1.rolname = "one";
  r2.rolname = "two";
  aa_put_role(s1, r1);
  aa_put_role(s2, r2);

  // 2.
  REQUIRE(0 == aa_commit(s1));

  // 3.
  CHECK(-1 == aa_commit(s2));
  CHECK(AA_ERR_CONFLICT == aa_errnum());
  CHECK(0 == aa_abort(s2));

  // 4.
  CHECK(nullptr != find_role(*aa_roles(h.aa()), "one"));
  CHECK(nullptr == find_role(*aa_roles(h.aa()), "two"));
  aa_session_free(s1);
  aa_session_free(s2);
  h.restart();
  CHECK(nullptr != find_role(*aa_roles(h.aa()), "one"));
}

// Verify that dropping a role and its membership is written.
// 1. Delete pg_read_all_settings and its membership in pg_monitor.
// 2. Commit.
// 3. Restart; confirm both are gone.
TEST_CASE("roles: delete") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());

  // 1.
  auto roles = aa_roles(h.aa());
  for (const aa_member_t &m : roles->members)
    if (3374 == m.roleid)
      aa_delete_member(s, m.oid);
  aa_delete_role(s, 3374);

  // 2.
  REQUIRE(0 == aa_commit(s));
  aa_session_free(s);

  // 3.
  h.restart();
  roles = aa_roles(h.aa());
  CHECK(nullptr == find_role(*roles, "pg_read_all_settings"));
  CHECK(2 == roles->members.size());
}

// Verify that threads reading the roles and objects snapshots without a
// lock see each snapshot whole while another session replaces both.
// 1. Start four reader threads that read aa_roles() and aa_objects().
// 2. Replace both snapshots twenty times in a writer session.
// 3. Join the readers; confirm every snapshot read was complete.
TEST_CASE("roles: concurrent readers while snapshots are replaced") {
  home_t h;
  std::atomic<bool> stop{false};
  std::atomic<int> bad{0};
  std::thread readers[4];

  // 1.
  for (std::thread &t : readers)
    t = std::thread([&] {
      while (!stop.load(std::memory_order_relaxed)) {
        auto r = aa_roles(h.aa());
        auto o = aa_objects(h.aa());
        if (nullptr == find_role(*r, "admin") || o->size() > 20)
          bad.fetch_add(1, std::memory_order_relaxed);
      }
    });

  // 2.
  aa_session_t *s = aa_session_new(h.aa());
  for (int i = 0; i < 20; i++) {
    aa_role_t role;
    aa_object_t obj;
    REQUIRE(0 == aa_new_oid(s, &role.oid));
    role.rolname = "r" + std::to_string(i);
    aa_put_role(s, role);
    obj.dbid = 1;
    obj.objid = 20000 + i;
    obj.classid = AA_CLASS_RELATION;
    obj.owner = role.oid;
    aa_set_object(s, 1, obj);
    REQUIRE(0 == aa_commit(s));
    REQUIRE(0 == aa_post_commit(s));
  }
  aa_session_free(s);

  // 3.
  stop.store(true, std::memory_order_relaxed);
  for (std::thread &t : readers)
    t.join();
  CHECK(0 == bad.load());
}

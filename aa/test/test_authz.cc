/*
 * test_authz.cc -- who holds which privilege: membership closures by
 * INHERIT, SET and ADMIN, an ACL's mask, the grantor of a GRANT, CONNECT,
 * and the session's caches of them going stale when they should.
 */
#include "doctest.h"

#include "home.h"

/* Roles made for these tests: A in B, B in C, E in A. */
#define A 16400
#define B 16401
#define C 16402
#define E 16403
#define S 16404 /* a superuser */

/* The database these tests check in, and another. */
#define DB 1
#define OTHER_DB 2

/* A membership of member in role, by oid, with its options. */
static aa_member_t grant(aa_oid_t oid, aa_oid_t role, aa_oid_t member,
                         bool inherit, bool set, bool admin) {
  aa_member_t m;

  m.oid = oid;
  m.roleid = role;
  m.member = member;
  m.grantor = AA_BOOTSTRAP_SUPERUSER;
  m.inherit_option = inherit;
  m.set_option = set;
  m.admin_option = admin;
  return m;
}

/*
 * Commit the roles A, B, C, E and S, with A in B (INHERIT, SET, ADMIN),
 * B in C (SET only), and E in A (INHERIT).
 */
static void make_roles(home_t &h) {
  aa_session_t *s = aa_session_new(h.aa());
  const aa_oid_t oids[] = {A, B, C, E, S};
  const char *names[] = {"a", "b", "c", "e", "s"};

  for (int i = 0; i < 5; i++) {
    aa_role_t r;

    r.oid = oids[i];
    r.rolname = names[i];
    r.rolsuper = S == oids[i];
    aa_put_role(s, r);
  }
  aa_put_member(s, grant(16500, B, A, true, true, true));
  aa_put_member(s, grant(16501, C, B, false, true, false));
  aa_put_member(s, grant(16502, A, E, true, false, false));
  REQUIRE(0 == aa_commit(s));
  aa_session_free(s);
}

// Verify the closures: privileges through INHERIT grants alone, SET ROLE
// through SET grants alone, membership through any, and a superuser in
// every role but by nosuper.
// 1. Make A in B in C, B in C without INHERIT, E in A without SET.
// 2. Confirm A has B's privileges, not C's; E has A's and B's.
// 3. Confirm A may SET ROLE to B and C; E to neither A nor B.
// 4. Confirm A is a member of C; S of every role, but not by nosuper.
TEST_CASE("authz: closures") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());

  // 1.
  make_roles(h);

  // 2.
  CHECK(aa_has_privs_of_role(h.aa(), s, DB, A, B));
  CHECK(!aa_has_privs_of_role(h.aa(), s, DB, A, C));
  CHECK(aa_has_privs_of_role(h.aa(), s, DB, E, B));

  // 3.
  CHECK(aa_can_set_role(h.aa(), s, DB, A, C));
  CHECK(!aa_can_set_role(h.aa(), s, DB, E, A));
  CHECK(!aa_can_set_role(h.aa(), s, DB, E, B));

  // 4.
  CHECK(aa_is_member_of_role(h.aa(), s, DB, A, C, false));
  CHECK(!aa_is_member_of_role(h.aa(), s, DB, C, A, false));
  CHECK(aa_is_member_of_role(h.aa(), s, DB, S, A, false));
  CHECK(!aa_is_member_of_role(h.aa(), s, DB, S, A, true));
  CHECK(aa_is_superuser(h.aa(), s, S));
  CHECK(!aa_is_superuser(h.aa(), s, A));
  aa_session_free(s);
}

// Verify that a database's owner is in pg_database_owner there alone.
// 1. Make A the owner of DB.
// 2. Confirm A has pg_database_owner's privileges in DB, not in OTHER_DB,
//    nor with no database; E, in A, has them in DB too.
// 3. Confirm the bootstrap superuser, owning OTHER_DB by having no
//    entry, is a member of pg_database_owner there, with no database not.
TEST_CASE("authz: pg_database_owner") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_object_t db;

  // 1.
  make_roles(h);
  db.dbid = DB;
  db.classid = AA_CLASS_DATABASE;
  db.owner = A;
  aa_set_object(s, 1, db);

  // 2.
  CHECK(aa_has_privs_of_role(h.aa(), s, DB, A, AA_PG_DATABASE_OWNER));
  CHECK(!aa_has_privs_of_role(h.aa(), s, OTHER_DB, A, AA_PG_DATABASE_OWNER));
  CHECK(!aa_has_privs_of_role(h.aa(), s, AA_NO_DATABASE, A,
                              AA_PG_DATABASE_OWNER));
  CHECK(aa_has_privs_of_role(h.aa(), s, DB, E, AA_PG_DATABASE_OWNER));

  // 3.
  CHECK(aa_is_member_of_role(h.aa(), s, OTHER_DB, AA_BOOTSTRAP_SUPERUSER,
                             AA_PG_DATABASE_OWNER, true));
  CHECK(!aa_is_member_of_role(h.aa(), s, AA_NO_DATABASE, AA_BOOTSTRAP_SUPERUSER,
                              AA_PG_DATABASE_OWNER, true));
  aa_session_free(s);
}

// Verify ADMIN: a role granted another WITH ADMIN OPTION administers it,
// as does a member of it; no role administers itself.
// 1. Make A in B WITH ADMIN OPTION, E in A.
// 2. Confirm A and E administer B, C does not, A does not administer A.
// 3. Confirm the best admin of B for E is A; for A, A; for C, none.
// 4. Grant E B WITH ADMIN OPTION itself; confirm E is now its own best
//    admin, the nearest.
// 5. Grant C, which B is in, B WITH ADMIN OPTION, a loop: confirm B still
//    does not administer itself.
TEST_CASE("authz: admin") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());

  // 1.
  make_roles(h);

  // 2.
  CHECK(aa_is_admin_of_role(h.aa(), s, DB, A, B));
  CHECK(aa_is_admin_of_role(h.aa(), s, DB, E, B));
  CHECK(!aa_is_admin_of_role(h.aa(), s, DB, C, B));
  CHECK(!aa_is_admin_of_role(h.aa(), s, DB, A, A));
  CHECK(aa_is_admin_of_role(h.aa(), s, DB, S, A));

  // 3.
  CHECK(A == aa_select_best_admin(h.aa(), s, DB, E, B));
  CHECK(A == aa_select_best_admin(h.aa(), s, DB, A, B));
  CHECK(0 == aa_select_best_admin(h.aa(), s, DB, C, B));

  // 4.
  aa_put_member(s, grant(16503, B, E, false, false, true));
  CHECK(E == aa_select_best_admin(h.aa(), s, DB, E, B));

  // 5.
  aa_put_member(s, grant(16504, B, C, false, false, true));
  CHECK(aa_is_admin_of_role(h.aa(), s, DB, C, B));
  CHECK(!aa_is_admin_of_role(h.aa(), s, DB, B, B));
  aa_session_free(s);
}

// Verify an ACL's mask: what PUBLIC, the role and the roles whose
// privileges it has are granted, and the owner's grant options.
// 1. Make an ACL: C owns it; PUBLIC has SELECT, B UPDATE with its grant
//    option, C everything.
// 2. Confirm A holds SELECT and UPDATE, not DELETE; C holds DELETE.
// 3. Confirm A holds UPDATE's grant option by B's, and B, holding C's
//    privileges by no grant, not DELETE's.
// 4. Confirm a member of the owner, E made one, has every grant option;
//    the direct mask gives A none of B's.
TEST_CASE("authz: acl mask") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  uint64_t upd_go = AA_UPDATE << 32, del_go = AA_DELETE << 32;

  // 1.
  make_roles(h);
  aa_acl_item_t acl[] = {{AA_PUBLIC, C, AA_SELECT},
                         {B, C, AA_UPDATE | upd_go},
                         {C, C, AA_ALL_RIGHTS_RELATION}};

  // 2.
  CHECK((AA_SELECT | AA_UPDATE) ==
        aa_acl_mask(h.aa(), s, DB, acl, 3, A, C,
                    AA_SELECT | AA_UPDATE | AA_DELETE, false));
  CHECK(AA_DELETE ==
        aa_acl_mask(h.aa(), s, DB, acl, 3, C, C, AA_DELETE, false));
  CHECK(0 !=
        aa_acl_mask(h.aa(), s, DB, acl, 3, A, C, AA_SELECT | AA_DELETE, true));

  // 3.
  CHECK(upd_go == aa_acl_mask(h.aa(), s, DB, acl, 3, A, C, upd_go, false));
  CHECK(0 == aa_acl_mask(h.aa(), s, DB, acl, 3, B, C, del_go, false));

  // 4.
  CHECK(del_go == aa_acl_mask(h.aa(), s, DB, acl, 3, E, A, del_go, false));
  CHECK(0 == aa_acl_mask_direct(acl, 3, A, C, AA_UPDATE, false));
  CHECK(AA_UPDATE == aa_acl_mask_direct(acl, 3, B, C, AA_UPDATE, false));
  CHECK(del_go == aa_acl_mask_direct(acl, 3, C, C, del_go, false));
  aa_session_free(s);
}

// Verify who a GRANT is made as: the owner for itself and a superuser;
// else the role whose privileges it has with the grant options.
// 1. Make an ACL of C's granting B UPDATE and SELECT with their options.
// 2. Confirm C and S grant as C, with every option asked.
// 3. Confirm A grants UPDATE as B; DELETE as itself, with none; UPDATE
//    and DELETE as B, with UPDATE's option alone.
TEST_CASE("authz: best grantor") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_oid_t grantor;
  uint64_t go;

  // 1.
  make_roles(h);
  aa_acl_item_t acl[] = {
      {B, C, AA_UPDATE | AA_SELECT | ((AA_UPDATE | AA_SELECT) << 32)}};

  // 2.
  aa_select_best_grantor(h.aa(), s, DB, C, AA_DELETE, acl, 1, C, &grantor, &go);
  CHECK(C == grantor);
  CHECK((AA_DELETE << 32) == go);
  aa_select_best_grantor(h.aa(), s, DB, S, AA_DELETE, acl, 1, C, &grantor, &go);
  CHECK(C == grantor);

  // 3.
  aa_select_best_grantor(h.aa(), s, DB, A, AA_UPDATE, acl, 1, C, &grantor, &go);
  CHECK(B == grantor);
  CHECK((AA_UPDATE << 32) == go);
  aa_select_best_grantor(h.aa(), s, DB, A, AA_DELETE, acl, 1, C, &grantor, &go);
  CHECK(A == grantor);
  CHECK(0 == go);
  aa_select_best_grantor(h.aa(), s, DB, A, AA_UPDATE | AA_DELETE, acl, 1, C,
                         &grantor, &go);
  CHECK(B == grantor);
  CHECK((AA_UPDATE << 32) == go);
  aa_session_free(s);
}

// Verify CONNECT at a connect: PUBLIC's by default, a grant to a role
// a member of it uses, and a superuser needs none.
// 1. Confirm A may connect to a database with no entry.
// 2. Give DB an ACL granting B alone CONNECT.
// 3. Confirm A, in B, may; C may not; S may.
TEST_CASE("authz: connect") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_object_t db;

  // 1.
  make_roles(h);
  CHECK(nullptr == aa_connect_refused(h.aa(), s, DB, A));

  // 2.
  db.dbid = DB;
  db.classid = AA_CLASS_DATABASE;
  db.owner = AA_BOOTSTRAP_SUPERUSER;
  db.has_acl = true;
  db.acl = aa_acl_text({{B, AA_BOOTSTRAP_SUPERUSER, AA_CONNECT}});
  aa_set_object(s, 1, db);

  // 3.
  CHECK(nullptr == aa_connect_refused(h.aa(), s, DB, A));
  CHECK(std::string("has no CONNECT privilege on database") ==
        aa_connect_refused(h.aa(), s, DB, C));
  CHECK(nullptr == aa_connect_refused(h.aa(), s, DB, S));
  aa_session_free(s);
}

// Verify that a session's caches follow the roles: its own change at
// once, another session's once committed, and a database's new owner.
// 1. Confirm A has B's privileges, cached.
// 2. Drop A's membership in B in the session; confirm A has not.
// 3. Abort; confirm A has them again.
// 4. In another session drop it and commit; confirm the first sees it,
//    and caches E not in pg_database_owner.
// 5. Make E owner of DB in another session and commit, no role changed;
//    confirm the first sees E in pg_database_owner.
TEST_CASE("authz: caches go stale") {
  home_t h;
  make_roles(h);
  aa_session_t *s1 = aa_session_new(h.aa());
  aa_session_t *s2 = aa_session_new(h.aa());
  aa_object_t db;

  // 1.
  CHECK(aa_has_privs_of_role(h.aa(), s1, DB, A, B));
  CHECK(!aa_has_privs_of_role(h.aa(), s1, DB, E, AA_PG_DATABASE_OWNER));

  // 2.
  aa_delete_member(s1, 16500);
  CHECK(!aa_has_privs_of_role(h.aa(), s1, DB, A, B));

  // 3.
  REQUIRE(0 == aa_abort(s1));
  CHECK(aa_has_privs_of_role(h.aa(), s1, DB, A, B));

  // 4.
  aa_delete_member(s2, 16500);
  REQUIRE(0 == aa_commit(s2));
  CHECK(!aa_has_privs_of_role(h.aa(), s1, DB, A, B));
  CHECK(!aa_has_privs_of_role(h.aa(), s1, DB, E, AA_PG_DATABASE_OWNER));

  // 5.
  db.dbid = DB;
  db.classid = AA_CLASS_DATABASE;
  db.owner = E;
  aa_set_object(s2, 1, db);
  REQUIRE(0 == aa_commit(s2));
  REQUIRE(0 == aa_post_commit(s2));
  CHECK(aa_has_privs_of_role(h.aa(), s1, DB, E, AA_PG_DATABASE_OWNER));
  aa_session_free(s1);
  aa_session_free(s2);
}

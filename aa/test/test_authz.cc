/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

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
  const char *why;
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_object_t db;

  // 1.
  make_roles(h);
  CHECK(aa_connect_okay(h.aa(), s, DB, A, &why));

  // 2.
  db.dbid = DB;
  db.classid = AA_CLASS_DATABASE;
  db.owner = AA_BOOTSTRAP_SUPERUSER;
  db.has_acl = true;
  db.acl = aa_acl_text({{B, AA_BOOTSTRAP_SUPERUSER, AA_CONNECT}});
  aa_set_object(s, 1, db);

  // 3.
  CHECK(aa_connect_okay(h.aa(), s, DB, A, &why));
  CHECK(!aa_connect_okay(h.aa(), s, DB, C, &why));
  CHECK(std::string("has no CONNECT privilege on database") == why);
  CHECK(aa_connect_okay(h.aa(), s, DB, S, &why));
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

// Verify an object's mask, cached: privileges by the object's ACL, its
// default, its owner (its entry's, a temporary object's, its
// database's), through memberships, and a cached answer made stale by
// the session's own change and by another session's commit.
// 1. Give table 20000, owned by C, an ACL granting B SELECT.
// 2. Confirm A, in B, holds SELECT and not UPDATE, asked twice; E, in A,
//    holds SELECT; C holds every privilege of its own table, asked for
//    SELECT, then for UPDATE.
// 3. Make A owner of DB: confirm A holds every privilege of table 20001,
//    which has no entry, and C, not its owner, none.
// 4. Confirm C holds nothing of temporary table 30000, its owner not
//    set; make C its owner: confirm C holds UPDATE.
// 5. Confirm PUBLIC's CONNECT on DB itself, which has no ACL.
// 6. Take B's grant away in the session: confirm A's cached SELECT goes;
//    abort, and confirm it is back.
// 7. Take it away in another session and commit: confirm A loses it.
TEST_CASE("authz: object mask") {
  home_t h;
  make_roles(h);
  aa_session_t *s = aa_session_new(h.aa());
  aa_session_t *s2 = aa_session_new(h.aa());
  aa_object_t t, db;
  uint64_t both = AA_SELECT | AA_UPDATE;

  // 1.
  t.dbid = DB;
  t.objid = 20000;
  t.classid = AA_CLASS_RELATION;
  t.owner = C;
  t.has_acl = true;
  t.acl = aa_acl_text({{C, C, AA_ALL_RIGHTS_RELATION}, {B, C, AA_SELECT}});
  aa_set_object(s2, 1, t);
  REQUIRE(0 == aa_commit(s2));
  REQUIRE(0 == aa_post_commit(s2));

  // 2.
  CHECK(AA_SELECT ==
        aa_object_mask(h.aa(), s, DB, 20000, AA_CLASS_RELATION, A, both));
  CHECK(AA_SELECT ==
        aa_object_mask(h.aa(), s, DB, 20000, AA_CLASS_RELATION, A, both));
  CHECK(AA_SELECT ==
        aa_object_mask(h.aa(), s, DB, 20000, AA_CLASS_RELATION, E, both));
  CHECK(AA_SELECT ==
        aa_object_mask(h.aa(), s, DB, 20000, AA_CLASS_RELATION, C, AA_SELECT));
  CHECK(AA_UPDATE ==
        aa_object_mask(h.aa(), s, DB, 20000, AA_CLASS_RELATION, C, AA_UPDATE));
  CHECK(AA_ALL_RIGHTS_RELATION == aa_object_mask(h.aa(), s, DB, 20000,
                                                 AA_CLASS_RELATION, C,
                                                 AA_ALL_RIGHTS_RELATION));

  // 3.
  db.dbid = DB;
  db.classid = AA_CLASS_DATABASE;
  db.owner = A;
  aa_set_object(s, 1, db);
  CHECK(both ==
        aa_object_mask(h.aa(), s, DB, 20001, AA_CLASS_RELATION, A, both));
  CHECK(0 == aa_object_mask(h.aa(), s, DB, 20001, AA_CLASS_RELATION, C, both));

  // 4.
  CHECK(0 ==
        aa_object_mask(h.aa(), s, DB, 30000, AA_CLASS_RELATION, C, AA_UPDATE));
  aa_set_temp_owner(s, 1, 30000, C);
  CHECK(AA_UPDATE ==
        aa_object_mask(h.aa(), s, DB, 30000, AA_CLASS_RELATION, C, AA_UPDATE));

  // 5.
  CHECK(AA_CONNECT == aa_object_mask(h.aa(), s, OTHER_DB, 0, AA_CLASS_DATABASE,
                                     C, AA_CONNECT));

  // 6.
  t.acl = aa_acl_text({{C, C, AA_ALL_RIGHTS_RELATION}});
  aa_set_object(s, 1, t);
  CHECK(0 == aa_object_mask(h.aa(), s, DB, 20000, AA_CLASS_RELATION, A, both));
  REQUIRE(0 == aa_abort(s));
  CHECK(AA_SELECT ==
        aa_object_mask(h.aa(), s, DB, 20000, AA_CLASS_RELATION, A, both));

  // 7.
  t.acl = aa_acl_text({{C, C, AA_ALL_RIGHTS_RELATION}});
  aa_set_object(s2, 1, t);
  REQUIRE(0 == aa_commit(s2));
  REQUIRE(0 == aa_post_commit(s2));
  CHECK(0 == aa_object_mask(h.aa(), s, DB, 20000, AA_CLASS_RELATION, A, both));
  aa_session_free(s);
  aa_session_free(s2);
}

// Verify a host's object's mask, kept: found as kept, for that class,
// object and role alone, and gone at a change of roles by the session or
// by another's commit, at a forget, and at a change between the miss and
// the keep.
// 1. Confirm a mask never kept is not found.
// 2. Keep A's mask of function 1000: confirm it is found; E's of it, and
//    A's of the table 1000, are not.
// 3. Take A's membership in B away in the session: confirm the mask
//    goes; abort, keep it again, and confirm it is found.
// 4. Take the membership away in another session and commit: confirm
//    the mask goes.
// 5. Keep it again, forget the kept masks: confirm it goes.
// 6. Miss it, give A the membership back in another session and commit,
//    then keep it: confirm the next ask misses it.
TEST_CASE("authz: a host's object's mask, kept") {
  home_t h;
  make_roles(h);
  aa_session_t *s = aa_session_new(h.aa());
  aa_session_t *s2 = aa_session_new(h.aa());
  const aa_oid_t proc = 1255;
  uint64_t all = 0;

  // 1.
  CHECK(!aa_kept_mask(s, proc, 1000, A, &all));

  // 2.
  aa_keep_mask(s, proc, 1000, A, AA_EXECUTE);
  CHECK(aa_kept_mask(s, proc, 1000, A, &all));
  CHECK(AA_EXECUTE == all);
  CHECK(!aa_kept_mask(s, proc, 1000, E, &all));
  CHECK(!aa_kept_mask(s, AA_CLASS_RELATION, 1000, A, &all));

  // 3.
  aa_delete_member(s, 16500);
  CHECK(!aa_kept_mask(s, proc, 1000, A, &all));
  REQUIRE(0 == aa_abort(s));
  CHECK(!aa_kept_mask(s, proc, 1000, A, &all));
  aa_keep_mask(s, proc, 1000, A, AA_EXECUTE);
  CHECK(aa_kept_mask(s, proc, 1000, A, &all));

  // 4.
  aa_delete_member(s2, 16500);
  REQUIRE(0 == aa_commit(s2));
  CHECK(!aa_kept_mask(s, proc, 1000, A, &all));

  // 5.
  aa_keep_mask(s, proc, 1000, A, AA_EXECUTE);
  CHECK(aa_kept_mask(s, proc, 1000, A, &all));
  aa_forget_kept_masks(s);
  CHECK(!aa_kept_mask(s, proc, 1000, A, &all));

  // 6.
  CHECK(!aa_kept_mask(s, proc, 1000, A, &all));
  aa_put_member(s2, grant(16500, B, A, true, true, true));
  REQUIRE(0 == aa_commit(s2));
  aa_keep_mask(s, proc, 1000, A, AA_EXECUTE);
  CHECK(!aa_kept_mask(s, proc, 1000, A, &all));
  aa_session_free(s);
  aa_session_free(s2);
}

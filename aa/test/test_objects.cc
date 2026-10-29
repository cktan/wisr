/*
 * test_objects.cc -- objects' owners and ACLs in pg_global: written after
 * the database's commit, undone by a subtransaction's abort, and, when
 * the commit is to grant less, granting less in between.
 */
#include "doctest.h"

#include "home.h"

/* A table's entry: of the database 1, its oid objid, owned by owner. */
static aa_object_t table(aa_oid_t objid, aa_oid_t owner) {
  aa_object_t o;

  o.dbid = 1;
  o.objid = objid;
  o.classid = AA_CLASS_RELATION;
  o.owner = owner;
  return o;
}

/* Commit s's transaction as digres does: before and after the database's. */
static void commit(aa_session_t *s) {
  REQUIRE(0 == aa_commit(s));
  REQUIRE(0 == aa_post_commit(s));
}

// Verify that an object's entry is the session's until it commits, then
// every session's, which are told which object moved, and survives a
// restart.
// 1. In one session, set a table's owner.
// 2. Confirm that session finds it, and none other does.
// 3. Commit; confirm it is found as committed, and the other session
//    is told the table's oid moved, once.
// 4. Restart; confirm the entry reads back.
TEST_CASE("objects: set, commit, restart") {
  home_t h;
  aa_session_t *s1 = aa_session_new(h.aa());
  aa_session_t *s2 = aa_session_new(h.aa());
  std::vector<aa_oid_t> oids;
  aa_object_t o;
  bool all;

  // 1.
  aa_set_object(s1, 1, table(20000, 16500));

  // 2.
  CHECK(aa_find_object(h.aa(), s1, aa_key_t(1, 20000), &o));
  CHECK(!aa_find_object(h.aa(), s2, aa_key_t(1, 20000), &o));
  CHECK(!aa_find_object(h.aa(), NULL, aa_key_t(1, 20000), &o));

  // 3.
  commit(s1);
  REQUIRE(aa_find_object(h.aa(), NULL, aa_key_t(1, 20000), &o));
  CHECK(16500 == o.owner);
  CHECK(aa_objects_moved(s2, 1, &oids, &all));
  CHECK(!all);
  REQUIRE(1 == oids.size());
  CHECK(20000 == oids[0]);
  oids.clear();
  CHECK(!aa_objects_moved(s2, 1, &oids, &all));
  aa_session_free(s1);
  aa_session_free(s2);

  // 4.
  h.restart();
  REQUIRE(aa_find_object(h.aa(), NULL, aa_key_t(1, 20000), &o));
  CHECK(16500 == o.owner);
  CHECK(AA_CLASS_RELATION == o.classid);
}

// Verify that a subtransaction's abort undoes its changes alone, and its
// commit hands them to its parent.
// 1. At level 1 set a table's owner; at level 2 set another's.
// 2. Abort level 2; confirm only the first is left.
// 3. At level 2 set the second's again, commit level 2, abort level 2
//    of a new subtransaction; confirm both are left.
TEST_CASE("objects: subtransactions") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_object_t o;

  // 1.
  aa_set_object(s, 1, table(20000, 16500));
  aa_set_object(s, 2, table(20001, 16500));

  // 2.
  aa_subabort(s, 2);
  CHECK(aa_find_object(h.aa(), s, aa_key_t(1, 20000), &o));
  CHECK(!aa_find_object(h.aa(), s, aa_key_t(1, 20001), &o));

  // 3.
  aa_set_object(s, 2, table(20001, 16500));
  aa_subcommit(s, 2);
  aa_subabort(s, 2);
  CHECK(aa_find_object(h.aa(), s, aa_key_t(1, 20000), &o));
  CHECK(aa_find_object(h.aa(), s, aa_key_t(1, 20001), &o));
  aa_session_free(s);
}

// Verify that a crash between pg_global's commit and the database's
// leaves an object granting less, never more: its owner the superuser,
// its ACL what both the old and the new grant.
// 1. Commit a table owned by role A, granting role B SELECT and UPDATE.
// 2. Change its owner to C and its ACL to B SELECT and INSERT; run the
//    commit before the database's, then crash.
// 3. Restart; confirm the owner is the superuser, and B has SELECT alone.
TEST_CASE("objects: a crash between the commits grants less") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_object_t o = table(20000, 16500);
  aa_acl_item_t mine = {16500, 16500, AA_ALL_RIGHTS_RELATION};

  // 1.
  o.has_acl = true;
  o.acl = aa_acl_text({mine, {16501, 16500, AA_SELECT | AA_UPDATE}});
  aa_set_object(s, 1, o);
  commit(s);

  // 2.
  o.owner = 16502;
  o.acl = aa_acl_text({mine, {16501, 16500, AA_SELECT | AA_INSERT}});
  aa_set_object(s, 1, o);
  REQUIRE(0 == aa_commit(s));
  aa_session_free(s);

  // 3.
  h.restart();
  REQUIRE(aa_find_object(h.aa(), NULL, aa_key_t(1, 20000), &o));
  CHECK(AA_BOOTSTRAP_SUPERUSER == o.owner);
  auto items = aa_acl_items(o.acl);
  REQUIRE(2 == items.size());
  CHECK(16500 == items[0].grantee);
  CHECK(16501 == items[1].grantee);
  CHECK(AA_SELECT == items[1].privs);
}

// Verify that the database's commit failing puts back what aa_commit()
// wrote before it.
// 1. Commit a table owned by role A.
// 2. Change its owner to B; run the commit before the database's.
// 3. Abort, as a failed database commit does.
// 4. Restart; confirm A owns it.
TEST_CASE("objects: abort puts back") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_object_t o;

  // 1.
  aa_set_object(s, 1, table(20000, 16500));
  commit(s);

  // 2.
  aa_set_object(s, 1, table(20000, 16501));
  REQUIRE(0 == aa_commit(s));

  // 3.
  REQUIRE(0 == aa_abort(s));
  aa_session_free(s);

  // 4.
  h.restart();
  REQUIRE(aa_find_object(h.aa(), NULL, aa_key_t(1, 20000), &o));
  CHECK(16500 == o.owner);
}

// Verify that an object dropped, and a database dropped, lose their
// entries after the commit, and other databases' stay.
// 1. Commit three tables: two of database 1, one of database 2.
// 2. Forget one of database 1's, and database 2.
// 3. Commit; confirm only database 1's other table is left.
// 4. Restart; confirm the same.
TEST_CASE("objects: forget") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_object_t o = table(20002, 16500);

  // 1.
  o.dbid = 2;
  aa_set_object(s, 1, table(20000, 16500));
  aa_set_object(s, 1, table(20001, 16500));
  aa_set_object(s, 1, o);
  commit(s);

  // 2.
  aa_forget_object(s, 1, aa_key_t(1, 20000));
  aa_forget_database(s, 2);

  // 3.
  commit(s);
  aa_session_free(s);
  CHECK(1 == aa_objects(h.aa())->size());

  // 4.
  h.restart();
  CHECK(1 == aa_objects(h.aa())->size());
  CHECK(aa_find_object(h.aa(), NULL, aa_key_t(1, 20001), &o));
}

// Verify that the objects of a role are those it owns, and those whose
// ACL names it other than as their owner.
// 1. Set a table owned by A, granting B SELECT; another owned by B.
// 2. Ask for A's and B's objects.
// 3. Confirm A owns one and is granted none; B owns one, granted one.
TEST_CASE("objects: of a role") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_object_t o = table(20000, 16500);
  std::vector<aa_object_t> owned, granted;

  // 1.
  o.has_acl = true;
  o.acl = aa_acl_text(
      {{16500, 16500, AA_ALL_RIGHTS_RELATION}, {16501, 16500, AA_SELECT}});
  aa_set_object(s, 1, o);
  aa_set_object(s, 1, table(20001, 16501));

  // 2. 3.
  aa_objects_of(h.aa(), s, 16500, &owned, &granted);
  CHECK(1 == owned.size());
  CHECK(granted.empty());
  owned.clear();
  aa_objects_of(h.aa(), s, 16501, &owned, &granted);
  REQUIRE(1 == owned.size());
  CHECK(20001 == owned[0].objid);
  REQUIRE(1 == granted.size());
  CHECK(20000 == granted[0].objid);
  aa_session_free(s);
}

// Verify that a temporary object's owner and ACL are the session's alone
// and are never written.
// 1. Set a temporary object's owner and ACL in one session.
// 2. Confirm it finds them, another does not, and nothing is written.
// 3. Forget it; confirm it is gone.
TEST_CASE("objects: temporary") {
  home_t h;
  aa_session_t *s1 = aa_session_new(h.aa());
  aa_session_t *s2 = aa_session_new(h.aa());
  aa_oid_t owner;
  std::string acl;

  // 1.
  aa_set_temp_owner(s1, 30000, 16500);
  aa_set_temp_acl(s1, 30000, "16500,16500,3");

  // 2.
  CHECK(aa_temp_owner(s1, 30000, &owner));
  CHECK(16500 == owner);
  CHECK(aa_temp_acl(s1, 30000, &acl));
  CHECK("16500,16500,3" == acl);
  CHECK(!aa_temp_owner(s2, 30000, &owner));
  commit(s1);
  CHECK(aa_objects(h.aa())->empty());

  // 3.
  aa_forget_temp(s1, 30000);
  CHECK(!aa_temp_owner(s1, 30000, &owner));
  CHECK(!aa_temp_acl(s1, 30000, &acl));
  aa_session_free(s1);
  aa_session_free(s2);
}

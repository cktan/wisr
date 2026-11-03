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

// Verify that a session told which objects moved is never told too few:
// past the changes aa keeps of, it is told all of them moved.
// 1. One transaction sets the owner of 4,097 tables, one more than aa
//    keeps; confirm another session is told all moved, or every oid.
// 2. One transaction sets 2 tables' owners, and the next 4,095 more:
//    the first of the 2 is no longer kept; confirm the other session is
//    told all moved, or every oid of the 4,097.
TEST_CASE("objects: more changes than are kept say all") {
  home_t h;
  aa_session_t *s1 = aa_session_new(h.aa());
  aa_session_t *s2 = aa_session_new(h.aa());
  std::vector<aa_oid_t> oids;
  bool all;

  // 1.
  for (aa_oid_t i = 0; i < 4097; i++)
    aa_set_object(s1, 1, table(20000 + i, 16500));
  commit(s1);
  REQUIRE(aa_objects_moved(s2, 1, &oids, &all));
  CHECK((all || 4097 == oids.size()));

  // 2.
  oids.clear();
  for (aa_oid_t i = 0; i < 2; i++)
    aa_set_object(s1, 1, table(30000 + i, 16500));
  commit(s1);
  for (aa_oid_t i = 2; i < 4097; i++)
    aa_set_object(s1, 1, table(30000 + i, 16500));
  commit(s1);
  REQUIRE(aa_objects_moved(s2, 1, &oids, &all));
  CHECK((all || 4097 == oids.size()));
  aa_session_free(s1);
  aa_session_free(s2);
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

/* Database 1's entry with no PUBLIC privilege: the superuser's alone. */
static aa_object_t database_revoked(void) {
  aa_object_t db;

  db.dbid = 1;
  db.objid = 0;
  db.classid = AA_CLASS_DATABASE;
  db.owner = AA_BOOTSTRAP_SUPERUSER;
  db.has_acl = true;
  db.acl = aa_acl_text({{AA_BOOTSTRAP_SUPERUSER, AA_BOOTSTRAP_SUPERUSER,
                         AA_ALL_RIGHTS_DATABASE}});
  return db;
}

// Verify that a crash between the commits grants less of an object that
// has no entry, as of one that has: a database's default, which lets
// PUBLIC connect, is no entry at all.
// 1. Revoke CONNECT from PUBLIC on database 1, which has no entry, and
//    commit aa's part only.
// 2. Restart, as a crash before aa_post_commit() leaves it.
// 3. Confirm a role that is no superuser may not connect.
TEST_CASE("objects: a crash between the commits grants less of no entry") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_object_t o;
  const char *why;

  // 1.
  REQUIRE(!aa_find_object(h.aa(), NULL, aa_key_t(1, 0), &o));
  REQUIRE(aa_connect_okay(h.aa(), NULL, 1, 16501, &why));
  aa_set_object(s, 1, database_revoked());
  REQUIRE(0 == aa_commit(s));
  aa_session_free(s);

  // 2. 3.
  h.restart();
  CHECK(!aa_connect_okay(h.aa(), NULL, 1, 16501, &why));
}

// Verify that an abort after aa's part puts an object that had no entry
// back as having none.
// 1. Revoke CONNECT from PUBLIC on database 1, which has no entry, and
//    commit aa's part only.
// 2. Abort, as a failed database commit does.
// 3. Confirm the database has no entry, and a role may connect.
TEST_CASE("objects: an abort puts back no entry") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_object_t o;
  const char *why;

  // 1.
  aa_set_object(s, 1, database_revoked());
  REQUIRE(0 == aa_commit(s));

  // 2.
  REQUIRE(0 == aa_abort(s));
  aa_session_free(s);

  // 3.
  CHECK(!aa_find_object(h.aa(), NULL, aa_key_t(1, 0), &o));
  CHECK(aa_connect_okay(h.aa(), NULL, 1, 16501, &why));
  h.restart();
  CHECK(!aa_find_object(h.aa(), NULL, aa_key_t(1, 0), &o));
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
  aa_set_temp_owner(s1, 1, 30000, 16500);
  aa_set_temp_acl(s1, 1, 30000, "16500,16500,3");

  // 2.
  CHECK(aa_temp_owner(s1, 30000, &owner));
  CHECK(16500 == owner);
  CHECK(aa_temp_acl(s1, 30000, &acl));
  CHECK("16500,16500,3" == acl);
  CHECK(!aa_temp_owner(s2, 30000, &owner));
  commit(s1);
  CHECK(aa_objects(h.aa())->empty());

  // 3.
  aa_forget_temp(s1, 1, 30000);
  CHECK(!aa_temp_owner(s1, 30000, &owner));
  CHECK(!aa_temp_acl(s1, 30000, &acl));
  aa_session_free(s1);
  aa_session_free(s2);
}

// Verify that a rollback, whole or to a savepoint, undoes a temporary
// object's owner and ACL as it undoes an object's entry.
// 1. Set a temporary object's owner and ACL, and commit.
// 2. Change its ACL, and abort: confirm the ACL is the committed one.
// 3. At level 2 change its owner, and abort level 2: confirm the owner
//    is the committed one; at level 2 change its ACL, commit level 2,
//    abort: confirm the ACL is the committed one.
// 4. Forget it, and abort: confirm the owner and ACL are back.
TEST_CASE("objects: temporary rolled back") {
  home_t h;
  aa_session_t *s = aa_session_new(h.aa());
  aa_oid_t owner;
  std::string acl;

  // 1.
  aa_set_temp_owner(s, 1, 30000, 16500);
  aa_set_temp_acl(s, 1, 30000, "16500,16500,3");
  commit(s);

  // 2.
  aa_set_temp_acl(s, 1, 30000, "16500,16500,7");
  REQUIRE(0 == aa_abort(s));
  REQUIRE(aa_temp_acl(s, 30000, &acl));
  CHECK("16500,16500,3" == acl);

  // 3.
  aa_set_temp_owner(s, 2, 30000, 16501);
  aa_subabort(s, 2);
  REQUIRE(aa_temp_owner(s, 30000, &owner));
  CHECK(16500 == owner);
  aa_set_temp_acl(s, 2, 30000, "16500,16500,7");
  aa_subcommit(s, 2);
  REQUIRE(0 == aa_abort(s));
  REQUIRE(aa_temp_acl(s, 30000, &acl));
  CHECK("16500,16500,3" == acl);

  // 4.
  aa_forget_temp(s, 1, 30000);
  REQUIRE(0 == aa_abort(s));
  REQUIRE(aa_temp_owner(s, 30000, &owner));
  CHECK(16500 == owner);
  REQUIRE(aa_temp_acl(s, 30000, &acl));
  CHECK("16500,16500,3" == acl);
  aa_session_free(s);
}

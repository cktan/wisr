/*
 * test_acl.cc -- ACLs as pg_global keeps them: their text, the
 * default one, and what two grant alike.
 */
#include "doctest.h"

#include "aa.h"

// Verify that an ACL's text reads back as the items it was made of.
// 1. Make the text of two items, one with a grant option bit.
// 2. Read the items back from it, and from the empty text.
// 3. Confirm each item is as made, and the empty text has none.
TEST_CASE("acl: text round trip") {
  // 1.
  std::vector<aa_acl_item_t> items = {
      {16384, 10, AA_SELECT | AA_INSERT},
      {AA_PUBLIC, 10, AA_SELECT | (AA_SELECT << 32)}};
  std::string text = aa_acl_text(items);
  CHECK("16384,10,3 0,10,8589934594" == text);

  // 2.
  std::vector<aa_acl_item_t> back = aa_acl_items(text);

  // 3.
  REQUIRE(2 == back.size());
  for (size_t i = 0; i < 2; i++) {
    CHECK(items[i].grantee == back[i].grantee);
    CHECK(items[i].grantor == back[i].grantor);
    CHECK(items[i].privs == back[i].privs);
  }
  CHECK(aa_acl_items("").empty());
}

// Verify that reading an ACL's text stops at the first item that is not
// "grantee,grantor,privs", keeping those before it, and takes items
// apart by any number of spaces.
// 1. Read text whose second item has an empty field, one whose second has
//    a sign, and one cut short.
// 2. Confirm the first two keep one item each, the third none.
// 3. Read two items two spaces apart; confirm both.
TEST_CASE("acl: text malformed") {
  // 1.
  std::vector<aa_acl_item_t> empty = aa_acl_items("16384,10,2 ,10,2");
  std::vector<aa_acl_item_t> sign = aa_acl_items("16384,10,2 -1,10,2");
  std::vector<aa_acl_item_t> cut = aa_acl_items("16384,10");

  // 2.
  REQUIRE(1 == empty.size());
  CHECK(16384 == empty[0].grantee);
  CHECK(2 == empty[0].privs);
  CHECK(1 == sign.size());
  CHECK(cut.empty());

  // 3.
  CHECK(2 == aa_acl_items("16384,10,2  0,10,2").size());
}

// Verify that an object with no ACL gets PostgreSQL's acldefault().
// 1. Ask for a table's, a schema's and a database's default.
// 2. Confirm the owner has all rights of the class, and a database
//    grants PUBLIC CONNECT and TEMPORARY.
TEST_CASE("acl: default") {
  // 1.
  auto rel = aa_acl_default(AA_CLASS_RELATION, 20000);
  auto nsp = aa_acl_default(AA_CLASS_NAMESPACE, 20000);
  auto db = aa_acl_default(AA_CLASS_DATABASE, 20000);

  // 2.
  REQUIRE(1 == rel.size());
  CHECK(AA_ALL_RIGHTS_RELATION == rel[0].privs);
  CHECK(20000 == rel[0].grantee);
  REQUIRE(1 == nsp.size());
  CHECK(AA_ALL_RIGHTS_SCHEMA == nsp[0].privs);
  REQUIRE(2 == db.size());
  CHECK(AA_PUBLIC == db[0].grantee);
  CHECK((AA_CONNECT | AA_CREATE_TEMP) == db[0].privs);
  CHECK(AA_ALL_RIGHTS_DATABASE == db[1].privs);
}

// Verify that intersecting two ACLs keeps only what both grant.
// 1. Make one ACL granting a role SELECT and UPDATE, another PUBLIC
//    SELECT; another granting the role SELECT and INSERT.
// 2. Intersect them.
// 3. Confirm only the role's SELECT is left.
TEST_CASE("acl: intersect") {
  // 1.
  std::vector<aa_acl_item_t> a = {{16384, 10, AA_SELECT | AA_UPDATE},
                                  {AA_PUBLIC, 10, AA_SELECT}};
  std::vector<aa_acl_item_t> b = {{16384, 10, AA_SELECT | AA_INSERT}};

  // 2.
  auto both = aa_acl_intersect(a, b);

  // 3.
  REQUIRE(1 == both.size());
  CHECK(16384 == both[0].grantee);
  CHECK(AA_SELECT == both[0].privs);
}

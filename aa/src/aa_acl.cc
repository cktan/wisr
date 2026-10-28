/*
 * aa_acl.cc -- ACLs as pg_global keeps them: their text, their items,
 * what two of them grant alike, and the one an object keeps none of.
 */
#include <cstdio>

#include "aa.h"

/*
 * An ACL as pg_global keeps it, from its items: "grantee,grantor,privs"
 * each, by role oid, space-separated, so a role's rename leaves it be.
 */
std::string aa_acl_text(const std::vector<aa_acl_item_t> &items) {
  std::string out;

  for (const aa_acl_item_t &i : items) {
    if (!out.empty())
      out += ' ';
    out += std::to_string(i.grantee) + ',' + std::to_string(i.grantor) + ',' +
           std::to_string(i.privs);
  }
  return out;
}

/* The items of an ACL as pg_global keeps it (aa_acl_text()). */
std::vector<aa_acl_item_t> aa_acl_items(const std::string &text) {
  std::vector<aa_acl_item_t> out;
  const char *p = text.c_str();

  while ('\0' != *p) {
    unsigned long grantee, grantor;
    unsigned long long privs;
    int n = 0;

    if (3 != sscanf(p, "%lu,%lu,%llu%n", &grantee, &grantor, &privs, &n))
      break;
    out.push_back({(aa_oid_t)grantee, (aa_oid_t)grantor, (uint64_t)privs});
    p += n;
    while (' ' == *p)
      p++;
  }
  return out;
}

/*
 * The items of a granted in b too, each with the privileges both grant:
 * what the two ACLs grant alike.
 */
std::vector<aa_acl_item_t>
aa_acl_intersect(const std::vector<aa_acl_item_t> &a,
                 const std::vector<aa_acl_item_t> &b) {
  std::vector<aa_acl_item_t> out;

  for (const aa_acl_item_t &x : a)
    for (const aa_acl_item_t &y : b)
      if (x.grantee == y.grantee && x.grantor == y.grantor &&
          0 != (x.privs & y.privs))
        out.push_back({x.grantee, x.grantor, x.privs & y.privs});
  return out;
}

/*
 * The ACL an object of class classid owned by owner has when it keeps
 * none, as PostgreSQL's acldefault() makes it, with no grant option.
 */
std::vector<aa_acl_item_t> aa_acl_default(aa_oid_t classid, aa_oid_t owner) {
  switch (classid) {
  case AA_CLASS_NAMESPACE:
    return {{owner, owner, AA_ALL_RIGHTS_SCHEMA}};
  case AA_CLASS_DATABASE:
    return {{AA_PUBLIC, owner, AA_CREATE_TEMP | AA_CONNECT},
            {owner, owner, AA_ALL_RIGHTS_DATABASE}};
  default:
    return {{owner, owner, AA_ALL_RIGHTS_RELATION}};
  }
}

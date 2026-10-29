/*
 * aa_auth.cc -- how a client gets in (ROLE.md section 5): what it must
 * prove, the secret it proves it by, and whether its role may log in.
 * The socket, the exchange and the messages to the client are digres's;
 * these decide.
 */
#include <cstring>

#include "aa_internal.h"

/* The role named rolname in roles, or NULL. */
static const aa_role_t *role_named(const aa_roles_t &roles,
                                   const char *rolname) {
  for (const aa_role_t &r : roles.roles)
    if (r.rolname == rolname)
      return &r;
  return NULL;
}

/* The name of the bootstrap superuser in roles, or "" if none. */
static std::string bootstrap_name(const aa_roles_t &roles) {
  for (const aa_role_t &r : roles.roles)
    if (AA_BOOTSTRAP_SUPERUSER == r.oid)
      return r.rolname;
  return "";
}

/*
 * What a client connecting as rolname must prove: peer on the local
 * socket if its OS user is rolname, or owns the data directory and asks
 * for the bootstrap superuser; else the password.
 */
aa_method_t aa_auth_method(aa_env_t *env, const char *rolname, bool local,
                           const char *os_user, bool os_owner, bool *as_owner) {
  *as_owner = false;
  if (!local || NULL == os_user)
    return AA_PASSWORD;
  if (0 == strcmp(os_user, rolname))
    return AA_PEER;
  /* Locked out (ROLE.md section 8, question 3): the owner is oid 10. */
  if (os_owner && bootstrap_name(*aa_roles(env)) == rolname) {
    *as_owner = true;
    return AA_PEER;
  }
  return AA_PASSWORD;
}

/*
 * The SCRAM secret of rolname's password, into *secret: false, with why,
 * if the role does not exist, has no password, or is past VALID UNTIL.
 */
bool aa_role_secret(aa_env_t *env, const char *rolname, int64_t now,
                    std::string *secret, std::string *why) {
  std::shared_ptr<const aa_roles_t> roles = aa_roles(env);
  const aa_role_t *role = role_named(*roles, rolname);

  if (NULL == role) {
    *why = std::string("Role \"") + rolname + "\" does not exist.";
    return false;
  }
  if (!role->has_password) {
    *why = std::string("User \"") + rolname + "\" has no password assigned.";
    return false;
  }
  if (role->has_validuntil && role->rolvaliduntil < now) {
    *why = std::string("User \"") + rolname + "\" has an expired password.";
    return false;
  }
  *secret = role->rolpassword;
  return true;
}

/*
 * Why role may not log in, or NULL if it may: LOGIN, VALID UNTIL, and
 * CONNECTION LIMIT, by sessions open, this one among them; the data
 * directory's owner always may.
 */
const char *aa_login_refused(const aa_role_t &role, int64_t now, int sessions,
                             bool as_owner) {
  if (as_owner)
    return NULL;
  if (!role.rolcanlogin)
    return "is not permitted to log in";
  if (role.has_validuntil && role.rolvaliduntil < now)
    return "is past its VALID UNTIL";
  if (0 <= role.rolconnlimit && !role.rolsuper && sessions > role.rolconnlimit)
    return "has reached its CONNECTION LIMIT";
  return NULL;
}

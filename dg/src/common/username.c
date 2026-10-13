/*-------------------------------------------------------------------------
 *
 * username.c
 *	  get user name
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/common/username.c
 *
 *-------------------------------------------------------------------------
 */

#ifndef FRONTEND
#include "postgres.h"
#else
#include "postgres_fe.h"
#endif

#include <pwd.h>
#include <unistd.h>

#include "common/username.h"

/*
 * Returns the current user name in a buffer of the calling thread's: a
 * backend is a thread, and getpwuid()'s static buffer would be shared.
 * On error, returns NULL and sets *errstr to point to a palloc'd message
 */
const char *get_user_name(char **errstr) {
#ifndef WIN32
  static thread_local /*LOCAL*/ struct passwd pwbuf;
  static thread_local /*LOCAL*/ char buf[1024];
  struct passwd *pw;
  uid_t user_id = geteuid();
  int rc;

  *errstr = NULL;

  rc = getpwuid_r(user_id, &pwbuf, buf, sizeof buf, &pw);
  if (!pw) {
    *errstr =
        psprintf(_("could not look up effective user ID %ld: %s"),
                 (long)user_id, rc ? strerror(rc) : _("user does not exist"));
    return NULL;
  }

  return pw->pw_name;
#else
  /* Microsoft recommends buffer size of UNLEN+1, where UNLEN = 256 */
  /* "static" variable remains after function exit */
  static char username[256 + 1];
  DWORD len = sizeof(username);

  *errstr = NULL;

  if (!GetUserName(username, &len)) {
    *errstr =
        psprintf(_("user name lookup failure: error code %lu"), GetLastError());
    return NULL;
  }

  return username;
#endif
}

/*
 * Returns the current user name in a static buffer or exits
 */
const char *get_user_name_or_exit(const char *progname) {
  const char *user_name;
  char *errstr;

  user_name = get_user_name(&errstr);

  if (!user_name) {
    fprintf(stderr, "%s: %s\n", progname, errstr);
    exit(1);
  }
  return user_name;
}

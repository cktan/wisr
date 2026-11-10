/*-------------------------------------------------------------------------
 *
 * getpeereid.c
 *		get peer userid for UNIX-domain socket connection
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/port/getpeereid.c
 *
 *-------------------------------------------------------------------------
 */

#include "c.h"

#include <sys/param.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef HAVE_SYS_UCRED_H
#include <sys/ucred.h>
#endif

/*
 * BSD-style getpeereid() for platforms that lack it.
 */
int getpeereid(int sock, uid_t *uid, gid_t *gid) {
#if defined(SO_PEERCRED)
  /* Linux: use getsockopt(SO_PEERCRED) */
  struct ucred peercred;
  socklen_t so_len = sizeof(peercred);

  if (getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &peercred, &so_len) != 0 ||
      so_len != sizeof(peercred))
    return -1;
  *uid = peercred.uid;
  *gid = peercred.gid;
  return 0;
#elif defined(LOCAL_PEERCRED)
  /* Debian with FreeBSD kernel: use getsockopt(LOCAL_PEERCRED) */
  struct xucred peercred;
  socklen_t so_len = sizeof(peercred);

  if (getsockopt(sock, 0, LOCAL_PEERCRED, &peercred, &so_len) != 0 ||
      so_len != sizeof(peercred) || peercred.cr_version != XUCRED_VERSION)
    return -1;
  *uid = peercred.cr_uid;
  *gid = peercred.cr_gid;
  return 0;
#else
  /* No implementation available on this platform */
  errno = ENOSYS;
  return -1;
#endif
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*
 * aa_error.cc -- the thread's error state: the last failure's code and
 * message (aa.h).
 */
#include <cstdarg>
#include <cstdio>

#include "aa_internal.h"

/* The thread's last error: its code, and its message. */
static thread_local aa_errnum_t last_errnum = AA_OK;
static thread_local std::string last_errmsg;

/* The thread's last error. */
aa_errnum_t aa_errnum(void) { return last_errnum; }

/* The thread's last error's message, valid until the next call fails. */
const char *aa_errmsg(void) { return last_errmsg.c_str(); }

/* Record the error code, with a message made of fmt; returns -1. */
int aa_seterr(aa_errnum_t code, const char *fmt, ...) {
  char buf[1024];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  last_errnum = code;
  last_errmsg = buf;
  return -1;
}

/* Record sm's last error, as what failed; returns -1. */
int aa_sm_failed(const char *what) {
  return aa_seterr(AA_ERR_SM, "could not %s in database \"%s\": %s", what,
                   AA_DBNAME, sm_errmsg());
}

/*
 * Copyright (c) 2026 CK Tan
 * SPDX-License-Identifier: MIT
 */

/*-------------------------------------------------------------------------
 *
 * backend_thread.h
 *	  A backend that is a thread of the postmaster's process.
 *
 * src/include/postmaster/backend_thread.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef BACKEND_THREAD_H
#define BACKEND_THREAD_H

#include "libpq/libpq-be.h"
#include "miscadmin.h"

typedef struct backend_thread_t backend_thread_t;

extern pid_t backend_thread_launch(BackendType child_type, int child_slot,
                                   void *startup_data, size_t startup_data_len,
                                   ClientSocket *client_sock,
                                   backend_thread_t **thread);
extern void backend_thread_signal(backend_thread_t *thread, int sig);
extern bool backend_thread_ended(void);
extern backend_thread_t *backend_thread_exited(int *exitstatus);
extern void backend_thread_free(backend_thread_t *thread);
extern pid_t backend_thread_pid(backend_thread_t *thread);

#endif /* BACKEND_THREAD_H */

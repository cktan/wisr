/*-------------------------------------------------------------------------
 *
 * session_thread.h
 *	  Start a session in a thread of its own.
 *
 * src/include/utils/session_thread.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SESSION_THREAD_H
#define SESSION_THREAD_H

#include <thread>

extern std::thread session_thread_start(void (*fn)(void *), void *arg);

/*
 * A SHARED variable's place in the thread-local block, and its size: the
 * table src/tools/shared_tls.py writes for the program, by which a new
 * thread is given its parent's SHARED variables.  shared_tls_anchor is the
 * offset of progname, by which the block is found.
 */
typedef struct shared_tls_entry_t {
  size_t offset;
  size_t size;
} shared_tls_entry_t;

extern const shared_tls_entry_t shared_tls_table[];
extern const int shared_tls_count;
extern const size_t shared_tls_anchor;

#endif /* SESSION_THREAD_H */

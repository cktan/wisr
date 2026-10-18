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

#endif /* SESSION_THREAD_H */

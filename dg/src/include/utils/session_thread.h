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
extern void session_thread_adopt(void);

#endif /* SESSION_THREAD_H */

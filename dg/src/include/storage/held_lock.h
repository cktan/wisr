/*-------------------------------------------------------------------------
 *
 * held_lock.h
 *	  A C++ mutex held as PostgreSQL's LWLock was (SHMEM.md section 3).
 *
 * An LWLock held interrupts off while it was held, so the section it guarded
 * ended at its end or by an error the section raised, never by a cancel
 * taken at a CHECK_FOR_INTERRUPTS() inside it (errfinish() runs one after a
 * WARNING or a LOG).  held_lock_t does the same for a std::mutex or a
 * std::shared_mutex: it takes the lock and holds interrupts off, and lets
 * both go as its scope ends, or at release().
 *
 * An error unwinds it as LWLockReleaseAll() released an LWLock: errfinish()
 * has already set InterruptHoldoffCount to 0 before it throws, so the guard
 * then lets the lock go and leaves the count alone.
 *
 * A FATAL is the exception: proc_exit() runs the exit callbacks before it
 * throws, while a guard still holds its lock, where LWLockReleaseAll() would
 * have let an LWLock go first.  So let go of a lock an exit callback takes
 * before raising a FATAL.
 *
 * src/include/storage/held_lock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HELD_LOCK_H
#define HELD_LOCK_H

#include <exception>
#include <mutex>
#include <shared_mutex>

#include "miscadmin.h"

/* L is std::unique_lock or std::shared_lock over the mutex taken. */
template <class L> class held_lock_t {
public:
  /* Take mutex, waiting as an LWLock did, and hold interrupts off. */
  explicit held_lock_t(typename L::mutex_type &mutex)
      : m_lock(mutex), m_unwinding(std::uncaught_exceptions()) {
    HOLD_INTERRUPTS();
  }

  /* Let the lock go, if it is still held: see the head of this file. */
  ~held_lock_t() {
    if (!m_lock.owns_lock())
      return;
    if (std::uncaught_exceptions() > m_unwinding)
      m_lock.unlock(); /* an error's unwinding: the count is reset */
    else
      release();
  }

  held_lock_t(const held_lock_t &) = delete;
  held_lock_t &operator=(const held_lock_t &) = delete;

  /* Let the lock go before the scope ends, and resume interrupts. */
  void release() {
    m_lock.unlock();
    RESUME_INTERRUPTS();
  }

private:
  L m_lock;
  int m_unwinding; /* exceptions in flight when the lock was taken */
};

/* As LW_EXCLUSIVE on a plain mutex, and LW_EXCLUSIVE and LW_SHARED. */
using held_mutex_t = held_lock_t<std::unique_lock<std::mutex>>;
using held_exclusive_t = held_lock_t<std::unique_lock<std::shared_mutex>>;
using held_shared_t = held_lock_t<std::shared_lock<std::shared_mutex>>;

#endif /* HELD_LOCK_H */

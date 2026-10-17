/*-------------------------------------------------------------------------
 *
 * session_registry.c
 *	  The sessions of the process, as other sessions find them.
 *
 * In place of PostgreSQL's proc array (dg/SHMEM.md section 5.2): a mutex,
 * and the record of each session registered, by pid.  A lookup copies a
 * record's shared_ptr under the mutex and lets the mutex go; nothing is read
 * of another session but its record.
 *
 * IDENTIFICATION
 *	  src/backend/storage/ipc/session_registry.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <mutex>
#include <unordered_map>

#include "miscadmin.h"
#include "storage/session_registry.h"
#include "utils/control_block.h"

/* The registry. */
struct session_registry_t {
  std::mutex mu;                                   /* guards sessions */
  std::unordered_map<int, session_ref_t> sessions; /* by pid */
  std::atomic<uint64> next_id{1};                  /* the next session's id */
};

/* Free the registry: never, as the process ends first; for the deleter. */
void session_registry_free_t::operator()(session_registry_t *registry) const {
  delete registry;
}

/*
 * Make the registry.  The postmaster, or a standalone backend, calls it
 * before any session thread starts; a session thread's call finds it made.
 */
void session_registry_init(void) {
  if (shmem->session_registry == nullptr)
    shmem->session_registry.reset(new session_registry_t());
}

/*
 * Register this session: its record, which other sessions find it by, is
 * made here and kept in its tcb.  As many sessions as MaxBackends may be
 * registered at once.
 */
void session_register(void) {
  session_registry_t *registry = shmem->session_registry.get();
  session_ref_t record = std::make_shared<session_record_t>();

  record->id = registry->next_id.fetch_add(1, std::memory_order_relaxed);
  record->pid = MyProcPid;
  record->thread = sm_thread_self();
  {
    std::lock_guard<std::mutex> lock(registry->mu);

    if ((int)registry->sessions.size() >= MaxBackends)
      ereport(FATAL, (errcode(ERRCODE_TOO_MANY_CONNECTIONS),
                      errmsg("sorry, too many clients already")));
    registry->sessions[record->pid] = record;
  }
  tcb->session = record;
}

/* Record the database this session is connected to, once it has one. */
void session_set_database(Oid database) {
  if (tcb->session != nullptr)
    tcb->session->database.store(database, std::memory_order_release);
}

/* Record the role this session runs as, once it has one. */
void session_set_role(Oid role) {
  if (tcb->session != nullptr)
    tcb->session->role.store(role, std::memory_order_release);
}

/* Record this session's cancel key, which a cancel request must match. */
void session_set_cancel_key(const uint8 *key, int len) {
  Assert(len >= 0 && len <= MAX_CANCEL_KEY_LENGTH);
  /* A cancel request reads the key under this mutex. */
  std::lock_guard<std::mutex> lock(shmem->cancel_key_mutex);

  if (len > 0)
    memcpy(tcb->session->cancel_key, key, len);
  tcb->session->cancel_key_length = len;
}

/*
 * Raise interrupt bits in the session's own thread, which is its record's
 * alone, so a session that has ended is interrupted harmlessly, and no
 * other is.
 */
void session_interrupt(const session_ref_t &record, uint32 bits) {
  sm_interrupt(record->thread, bits);
}

/*
 * Leave the registry: the record is ended, and found by no lookup after.
 * A caller keeping it reads ended, and no more of the session.
 */
void session_unregister(void) {
  session_registry_t *registry = shmem->session_registry.get();
  session_ref_t record = tcb->session;

  if (record == nullptr)
    return;
  record->ended.store(true, std::memory_order_release);
  {
    std::lock_guard<std::mutex> lock(registry->mu);

    registry->sessions.erase(record->pid);
  }
  tcb->session.reset();
}

/* The session registered with pid, or none. */
session_ref_t session_by_pid(int pid) {
  session_registry_t *registry = shmem->session_registry.get();
  std::lock_guard<std::mutex> lock(registry->mu);
  auto it = registry->sessions.find(pid);

  return it == registry->sessions.end() ? nullptr : it->second;
}

/* Every session registered now. */
std::vector<session_ref_t> sessions_all(void) {
  session_registry_t *registry = shmem->session_registry.get();
  std::vector<session_ref_t> out;
  std::lock_guard<std::mutex> lock(registry->mu);

  out.reserve(registry->sessions.size());
  for (const auto &[pid, record] : registry->sessions)
    out.push_back(record);
  return out;
}

/*
 * Whether n more sessions may register: if not, *nfree is how many may.
 * Else *nfree is n.  The caller is registered already.
 */
bool session_have_free(int n, int *nfree) {
  session_registry_t *registry = shmem->session_registry.get();
  std::lock_guard<std::mutex> lock(registry->mu);
  int free = MaxBackends - (int)registry->sessions.size();

  *nfree = Min(n, free);
  return free >= n;
}

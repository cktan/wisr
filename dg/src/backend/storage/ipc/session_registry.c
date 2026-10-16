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
#include "storage/proc.h"
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
  if (pcb->session_registry == nullptr)
    pcb->session_registry.reset(new session_registry_t());
}

/*
 * Register this session, whose PGPROC is proc: its record, which other
 * sessions find it by, is made here and kept in its tcb.  As many sessions
 * as MaxBackends may be registered at once.
 */
void session_register(PGPROC *proc) {
  session_registry_t *registry = pcb->session_registry.get();
  session_ref_t record = std::make_shared<session_record_t>();

  record->id = registry->next_id.fetch_add(1, std::memory_order_relaxed);
  record->pid = MyProcPid;
  record->proc = proc;
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

/*
 * Raise interrupt bits in the session's own thread: not its PGPROC's latch
 * owner, which is another session's once its slot is reused, so a session
 * that has ended is interrupted harmlessly, and no other is.
 */
void session_interrupt(const session_ref_t &record, uint32 bits) {
  sm_interrupt(record->thread, bits);
}

/*
 * Leave the registry: the record is ended, and found by no lookup after.
 * A caller keeping it reads ended, and no more of the session.
 */
void session_unregister(void) {
  session_registry_t *registry = pcb->session_registry.get();
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
  session_registry_t *registry = pcb->session_registry.get();
  std::lock_guard<std::mutex> lock(registry->mu);
  auto it = registry->sessions.find(pid);

  return it == registry->sessions.end() ? nullptr : it->second;
}

/* Every session registered now. */
std::vector<session_ref_t> sessions_all(void) {
  session_registry_t *registry = pcb->session_registry.get();
  std::vector<session_ref_t> out;
  std::lock_guard<std::mutex> lock(registry->mu);

  out.reserve(registry->sessions.size());
  for (const auto &[pid, record] : registry->sessions)
    out.push_back(record);
  return out;
}

/*-------------------------------------------------------------------------
 *
 * session_registry.h
 *	  The sessions of the process, as other sessions find them.
 *
 * A session has a record, made as it registers and owned by shared_ptr: a
 * caller that looks one up keeps it for as long as it holds the pointer, and
 * it is never another session's, as a PGPROC slot or a pid may be.  What
 * one session reads of another is in the record, fixed before it registers
 * or atomic (dg/SHMEM.md section 5.2).
 *
 * src/include/storage/session_registry.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SESSION_REGISTRY_H
#define SESSION_REGISTRY_H

#include <atomic>
#include <memory>
#include <vector>

#include "sm_sys.h"

struct PGPROC;

/* A registered session. */
typedef struct session_record_t {
  uint64 id;              /* from a counter for the process; never reused */
  int pid;                /* its pid, fixed */
  struct PGPROC *proc;    /* its PGPROC, its own until ended is set */
  sm_thread_ref_t thread; /* its thread, fixed: what an interrupt reaches */
  std::atomic<Oid> database{InvalidOid}; /* its database, once it has one */
  std::atomic<Oid> role{InvalidOid};     /* its role, once it has one */
  std::atomic<bool> ended{false};        /* set as it leaves the registry */
} session_record_t;

typedef std::shared_ptr<session_record_t> session_ref_t;

/* The registry is the pcb's (utils/control_block.h), and declared there. */

extern void session_registry_init(void);
extern void session_register(struct PGPROC *proc);
extern void session_set_database(Oid database);
extern void session_set_role(Oid role);
extern void session_interrupt(const session_ref_t &record, uint32 bits);
extern void session_unregister(void);
extern session_ref_t session_by_pid(int pid);
extern std::vector<session_ref_t> sessions_all(void);

#endif /* SESSION_REGISTRY_H */

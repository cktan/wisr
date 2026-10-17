/*-------------------------------------------------------------------------
 *
 * postmaster.h
 *	  Exports from postmaster/postmaster.c.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/postmaster/postmaster.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef _POSTMASTER_H
#define _POSTMASTER_H

#include "lib/ilist.h"
#include "miscadmin.h"

/*
 * A struct representing an active postmaster child process.  This is used
 * mainly to keep track of how many children we have and send them appropriate
 * signals when necessary.  All postmaster child processes are assigned a
 * PMChild entry.  That includes "normal" client sessions, but also autovacuum
 * workers, background workers, and aux processes.
 *
 * "dead-end" children are also allocated a PMChild entry: these are children
 * launched just for the purpose of sending a friendly rejection message to a
 * would-be client.  We must track them because they are attached to shared
 * memory, but we know they will never become live backends.
 *
 * child_slot is an identifier that is unique across all running child
 * processes, by which pmchild.c counts them against their limits.  dead-end
 * children are not assigned a child_slot and have child_slot == 0 (valid
 * child_slot ids start from 1).
 */
typedef struct {
  pid_t pid;                       /* process id of backend */
  int child_slot;                  /* PMChildSlot for this backend, if any */
  BackendType bkend_type;          /* child process flavor, see above */
  struct backend_thread_t *thread; /* a backend that is a thread, or NULL */
  dlist_node elem;                 /* list link in ActiveChildList */
} PMChild;

/* GUC options */
extern thread_local /*LOCAL*/ PGDLLIMPORT bool EnableSSL;
extern thread_local /*LOCAL*/ PGDLLIMPORT int SuperuserReservedConnections;
extern thread_local /*LOCAL*/ PGDLLIMPORT int ReservedConnections;
extern thread_local /*LOCAL*/ PGDLLIMPORT int PostPortNumber;
extern thread_local /*LOCAL*/ PGDLLIMPORT int Unix_socket_permissions;
extern thread_local /*LOCAL*/ PGDLLIMPORT char *Unix_socket_group;
extern thread_local /*LOCAL*/ PGDLLIMPORT char *Unix_socket_directories;
extern thread_local /*LOCAL*/ PGDLLIMPORT char *ListenAddresses;
extern thread_local /*LOCAL*/ PGDLLIMPORT bool ClientAuthInProgress;
extern thread_local /*LOCAL*/ PGDLLIMPORT int PreAuthDelay;
extern thread_local /*LOCAL*/ PGDLLIMPORT int AuthenticationTimeout;
extern thread_local /*LOCAL*/ PGDLLIMPORT bool log_hostname;
extern thread_local /*LOCAL*/ PGDLLIMPORT bool enable_bonjour;
extern thread_local /*LOCAL*/ PGDLLIMPORT char *bonjour_name;
extern thread_local /*LOCAL*/ PGDLLIMPORT bool send_abort_for_crash;
extern thread_local /*LOCAL*/ PGDLLIMPORT bool send_abort_for_kill;

extern thread_local /*SHARED*/ PGDLLIMPORT const char *progname;

extern PGDLLIMPORT bool LoadedSSL;

pg_noreturn extern void PostmasterMain(int argc, char *argv[]);
extern void InitProcessGlobals(void);

extern int MaxLivePostmasterChildren(void);

/* defined in globals.c */
extern thread_local /*LOCAL*/ PGDLLIMPORT struct ClientSocket *MyClientSocket;

/* prototypes for functions in launch_backend.c */
const char *PostmasterChildName(BackendType child_type);

/* defined in pmchild.c */
extern thread_local /*SHARED*/ PGDLLIMPORT dlist_head ActiveChildList;

extern void InitPostmasterChildSlots(void);
extern PMChild *AssignPostmasterChildSlot(BackendType btype);
extern PMChild *AllocDeadEndChild(void);
extern void ReleasePostmasterChildSlot(PMChild *pmchild);
extern PMChild *FindPostmasterChildByPid(int pid);

/*
 * These values correspond to the special must-be-first options for dispatching
 * to various subprograms.  parse_dispatch_option() can be used to convert an
 * option name to one of these values.
 */
typedef enum DispatchOption {
  DISPATCH_CHECK,
  DISPATCH_BOOT,
  DISPATCH_DESCRIBE_CONFIG,
  DISPATCH_SINGLE,
  DISPATCH_POSTMASTER, /* must be last */
} DispatchOption;

extern DispatchOption parse_dispatch_option(const char *name);

#endif /* _POSTMASTER_H */

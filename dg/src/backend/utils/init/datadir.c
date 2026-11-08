/*-------------------------------------------------------------------------
 *
 * datadir.c
 *	  What the server reads from its data directory at startup.
 *
 * initdb leaves what the server needs before it takes a connection
 * (src/backend/main/initdb.c): sm's home directory, which knows the
 * databases, among them pg_global, which holds the roles
 * (utils/init/pg_global.c).
 *
 * The postmaster attaches sm's home directory once, and reads the roles
 * once: sm keeps its state in the memory of the process that attached it
 * (sm/PART4.md section 2), and every session is a thread of that process,
 * given both (SHARED). A session asks sm for its database, and opens an sm
 * session on it for as long as it lasts.  pg_global is no database a
 * client sees: it is not listed, connected to or dropped.
 *
 * src/backend/utils/init/datadir.c
 *
 *-------------------------------------------------------------------------
 */
/* sm's header first: it includes the C++ library, which must be read
 * before PostgreSQL's macros. */
#include "sm.h"

#include "postgres.h"

#include <pthread.h>
#include <signal.h>

#include "access/smstore.h"
#include "access/transam.h"
#include "miscadmin.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "utils/control_block.h"
#include "utils/datadir.h"
#include "utils/guc_hooks.h"
#include "utils/memutils.h"
#include "utils/pg_global.h"

/*
 * The process's, set by the main thread as it loads the data directory, and
 * read by every session: pcb->datadir (utils/control_block.h).
 */
struct datadir_state_t {
  /* sm's home directory, attached by the postmaster for every session. */
  sm_env_t *sm_env = NULL;
};

/* Make the state, empty: pcb_init() calls it. */
datadir_state_t *datadir_state_new(void) { return new datadir_state_t(); }

/* The state, to read. */
static inline const datadir_state_t &dds(void) { return *pcb->datadir; }

/* The state, to write: the main thread's alone. */
static inline datadir_state_t &dds_main(void) { return *pcb_main()->datadir; }

/* This session's sm session on its database, or NULL. */
static thread_local /*LOCAL*/ sm_session_t *sm_session = NULL;

/* Detach sm's home directory as the postmaster exits. */
static void detach_sm(int code, Datum arg) {
  sm_detach(dds().sm_env, 1);
  dds_main().sm_env = NULL;
}

/* Close the session's sm session as it ends. */
static void close_sm_session(int code, Datum arg) {
  sm_closedb(sm_session);
  sm_session = NULL;
}

/*
 * Attach sm's home directory and read the roles. The postmaster calls this
 * once it is in the data directory.
 */
void datadir_load(void) {
  int rc;

  /*
   * Once for the process: every session opens its database on it, and reads
   * through one block cache of shared_buffers.
   */
  sm_env_options_t opt;
  sm_env_options_init(&opt);
  opt.block_cache_bytes = (long long)NBuffers * BLCKSZ;
  opt.max_sessions = MaxConnections;
  rc = sm_attach(".", &dds_main().sm_env, &opt);
  if (rc != 0)
    ereport(FATAL, (errmsg("could not attach storage: %s", sm_errmsg())));
  on_proc_exit(detach_sm, 0);
  /*
   * Before any database is opened: digres reads sm's TOAST pointers
   * (smstore_fetch_toast()), so sm may store a row's long values out of line.
   */
  if (sm_set_toast(dds().sm_env, 1) != 0)
    ereport(FATAL, (errmsg("could not turn on TOAST: %s", sm_errmsg())));
  /*
   * Before any database is opened, which may resume the build of a partial
   * or an expression index at once: digres evaluates them (smstore.c).
   */
  if (smstore_set_index_evaluator(dds().sm_env) != 0)
    ereport(FATAL,
            (errmsg("could not set the index evaluator: %s", sm_errmsg())));
  /* Once for the process, after the two above, as it opens a database. */
  pg_global_load(dds().sm_env);
}

/* Whether dbname is pg_global, which no client sees. */
static bool is_pg_global(const char *dbname) {
  return 0 == strcmp(dbname, PG_GLOBAL_DBNAME);
}

/*
 * The oid of the database dbname, or InvalidOid if there is none: the id sm
 * gave it, which no other database has had (sm_dbid), above the oids that
 * are built in.
 */
Oid datadir_database_oid(const char *dbname) {
  uint64_t dbid;

  if (is_pg_global(dbname))
    return InvalidOid;
  if (sm_dbid(dds().sm_env, dbname, &dbid) != 0) {
    if (sm_errnum() == SM_ERR_NOTFOUND)
      return InvalidOid;
    ereport(FATAL, (errmsg("could not look up database \"%s\": %s", dbname,
                           sm_errmsg())));
  }
  if (dbid > PG_UINT32_MAX - FirstNormalObjectId)
    ereport(FATAL, (errmsg("database \"%s\" has an id out of range: %llu",
                           dbname, (unsigned long long)dbid)));
  return FirstNormalObjectId + (Oid)dbid;
}

/*
 * The name of the database whose oid is dboid, palloc'd, or NULL if there is
 * none: the one of sm's databases with that id.
 */
char *datadir_database_name(Oid dboid) {
  std::vector<std::string> names;

  if (sm_lsdb(dds().sm_env, names) != 0)
    ereport(ERROR, (errmsg("could not list databases: %s", sm_errmsg())));
  for (const std::string &name : names)
    if (datadir_database_oid(name.c_str()) == dboid)
      return pstrdup(name.c_str());
  return NULL;
}

/* The names of sm's databases, palloc'd, as sm lists them, but pg_global. */
List *datadir_database_names(void) {
  std::vector<std::string> names;
  List *out = NIL;

  if (sm_lsdb(dds().sm_env, names) != 0)
    ereport(ERROR, (errmsg("could not list databases: %s", sm_errmsg())));
  for (const std::string &name : names)
    if (!is_pg_global(name.c_str()))
      out = lappend(out, pstrdup(name.c_str()));
  return out;
}

/*
 * Create the database dbname in sm, at once and for good, as CREATE
 * DATABASE does outside a transaction: false if it exists already.
 */
bool datadir_create_database(const char *dbname) {
  if (sm_mkdb(dds().sm_env, dbname, NULL) == 0)
    return true;
  if (sm_errnum() == SM_ERR_EXISTS)
    return false;
  ereport(ERROR, (errmsg("could not create database \"%s\": %s", dbname,
                         sm_errmsg())));
  return false; /* not reached */
}

/*
 * Drop the database dbname from sm, at once and for good: DATADIR_DROPPED,
 * or DATADIR_NO_DATABASE if there is none, or DATADIR_IN_USE while a
 * session has it open.
 */
datadir_drop_t datadir_drop_database(const char *dbname) {
  if (is_pg_global(dbname))
    return DATADIR_NO_DATABASE;
  if (sm_rmdb(dds().sm_env, dbname) == 0)
    return DATADIR_DROPPED;
  switch (sm_errnum()) {
  case SM_ERR_NOTFOUND:
    return DATADIR_NO_DATABASE;
  case SM_ERR_INUSE:
    return DATADIR_IN_USE;
  default:
    ereport(ERROR, (errmsg("could not drop database \"%s\": %s", dbname,
                           sm_errmsg())));
  }
  return DATADIR_DROPPED; /* not reached */
}

/*
 * Open this session's sm session on the database dbname, for as long as
 * the session lasts.  The first on a database loads its catalog.
 */
void datadir_open_session(const char *dbname) {
  if (sm_opendb(dds().sm_env, dbname, NULL, &sm_session) != 0)
    ereport(FATAL, (errmsg("could not open database \"%s\": %s", dbname,
                           sm_errmsg())));
  on_proc_exit(close_sm_session, 0);
  /*
   * sm names the session by its thread's id, its pid, to one who asks whom
   * it waits for (datadir_sm_blockers()).
   */
  (void)sm_session_tag(sm_session, (uint64_t)MyProcPid);
  /* sm waits as long as deadlock_timeout says before it looks for a cycle. */
  (void)sm_set_deadlock_ms(sm_session, DeadlockTimeout);
  /* Time sm's lock waits even when a bulk load does other work first. */
  (void)sm_set_lock_wait_hook(sm_session, smstore_lock_wait, NULL);
}

/*
 * deadlock_timeout's assign hook: how long this session's waits in sm last
 * before they look for a cycle.  A session with no sm session yet is given
 * it as it opens one (datadir_open_session()).
 */
void assign_deadlock_timeout(int newval, void *extra) {
  if (sm_session != NULL)
    (void)sm_set_deadlock_ms(sm_session, newval);
}

/*
 * The pids of the sessions the session with pid waits for in sm: the
 * holders of the lock it waits on, or the writer whose row it waits behind.
 * sm knows each by its pid, its thread's id (datadir_open_session()).
 */
std::vector<int> datadir_sm_blockers(int pid) {
  std::vector<int> pids;
  std::vector<uint64_t> tags;

  /* A pid is never 0, which sm takes for no session. */
  if (pid > 0 && sm_waits_for(dds().sm_env, (uint64_t)pid, &tags) == 0)
    for (uint64_t tag : tags)
      pids.push_back((int)tag);
  return pids;
}

/* sm's name for a lock mode, as pg_locks shows it: Gray's, not PostgreSQL's. */
static const char *datadir_mode_name(sm_lockmode_t mode) {
  switch (mode) {
  case SM_LOCKMODE_IS:
    return "IS";
  case SM_LOCKMODE_IX:
    return "IX";
  case SM_LOCKMODE_SH:
    return "S";
  case SM_LOCKMODE_SIX:
    return "SIX";
  case SM_LOCKMODE_EX:
    return "X";
  }
  return "?";
}

/*
 * Every lock of sm's open databases, held or waited for: a table's, a
 * row's or a catalog key's (sm_lock_status()), for pg_locks.
 */
std::vector<datadir_lock_t> datadir_sm_locks(void) {
  std::vector<sm_lock_status_t> locks;
  std::vector<datadir_lock_t> out;

  if (sm_lock_status(dds().sm_env, locks) != 0)
    ereport(ERROR, (errmsg("could not list sm's locks: %s", sm_errmsg())));
  for (const sm_lock_status_t &l : locks) {
    datadir_lockname_t name = l.name == SM_LOCKNAME_OBJECT ? DATADIR_LOCK_OBJECT
                              : l.name == SM_LOCKNAME_ROW  ? DATADIR_LOCK_ROW
                                                           : DATADIR_LOCK_KEY;

    /*
     * A database's oid is its id above the built-in oids
     * (datadir_database_oid()); sm knows a session by its pid
     * (datadir_open_session()).
     */
    out.push_back({FirstNormalObjectId + (Oid)l.dbid, name, (Oid)l.oid,
                   datadir_mode_name(l.mode), (int)l.tag, l.granted});
  }
  return out;
}

/* This session's sm session, or NULL if it has opened none. */
sm_session_t *datadir_sm_session(void) { return sm_session; }

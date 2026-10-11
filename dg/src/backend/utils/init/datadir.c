/*-------------------------------------------------------------------------
 *
 * datadir.c
 *	  What the server reads from its data directory at startup.
 *
 * initdb leaves two things the server needs before it takes a connection
 * (src/backend/main/initdb.c): the superuser file, which names the one
 * role there is and holds the SCRAM secret of its password; and sm's home
 * directory, which knows the databases.
 *
 * The postmaster reads the superuser file once, and attaches sm's home
 * directory once: sm keeps its state in the memory of the process that
 * attached it (sm/PART4.md section 2), and every session is a thread of
 * that process, given both (SHARED). A session asks sm for its database,
 * and opens an sm session on it for as long as it lasts.
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

#include "access/transam.h"
#include "common/string.h"
#include "postmaster/initdb.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "utils/datadir.h"
#include "utils/memutils.h"

/* The superuser's name, and its SCRAM secret or NULL. */
static thread_local /*SHARED*/ char *superuser_name = NULL;
static thread_local /*SHARED*/ char *superuser_secret = NULL;

/* sm's home directory, attached by the postmaster for every session. */
static thread_local /*SHARED*/ sm_env_t *sm_env = NULL;

/* This session's sm session on its database, or NULL. */
static thread_local /*LOCAL*/ sm_session_t *sm_session = NULL;

/* Read the superuser file of the data directory, which is the current one. */
static void load_superuser(void) {
  FILE *file = AllocateFile(SUPERUSER_FILE, "r");
  char *line;

  if (file == NULL)
    ereport(FATAL, (errcode_for_file_access(),
                    errmsg("could not open file \"%s\": %m", SUPERUSER_FILE),
                    errhint("Run initdb to make a digres data directory.")));

  line = pg_get_line(file, NULL);
  if (line == NULL || pg_strip_crlf(line) == 0)
    ereport(FATAL, (errmsg("file \"%s\" names no superuser", SUPERUSER_FILE)));
  superuser_name = MemoryContextStrdup(TopMemoryContext, line);

  line = pg_get_line(file, NULL);
  if (line != NULL && pg_strip_crlf(line) > 0)
    superuser_secret = MemoryContextStrdup(TopMemoryContext, line);
  FreeFile(file);
}

/* Detach sm's home directory as the postmaster exits. */
static void detach_sm(int code, Datum arg) {
  sm_detach(sm_env, 1);
  sm_env = NULL;
}

/* Close the session's sm session as it ends. */
static void close_sm_session(int code, Datum arg) {
  sm_closedb(sm_session);
  sm_session = NULL;
}

/*
 * Read the superuser file and attach sm's home directory. The postmaster
 * calls this once it is in the data directory.
 */
void datadir_load(void) {
  sigset_t all, old;
  int rc;

  load_superuser();
  /*
   * Every signal blocked while sm attaches: the threads it and RocksDB start
   * inherit the mask, so a signal to the process lands only in the
   * postmaster's thread, whose handlers set its latch.
   */
  sigfillset(&all);
  pthread_sigmask(SIG_SETMASK, &all, &old);
  /* Once for the process: every session opens its database on it. */
  rc = sm_attach(".", &sm_env);
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  if (rc != 0)
    ereport(FATAL, (errmsg("could not attach storage: %s", sm_errmsg())));
  on_proc_exit(detach_sm, 0);
}

/* The name of the superuser, the one role there is. */
const char *datadir_superuser(void) { return superuser_name; }

/* The SCRAM secret of the superuser's password, or NULL if it has none. */
const char *datadir_superuser_secret(void) { return superuser_secret; }

/*
 * The oid of the database dbname, or InvalidOid if there is none: the id sm
 * gave it, which no other database has had (sm_dbid), above the oids that
 * are built in.
 */
Oid datadir_database_oid(const char *dbname) {
  uint64_t dbid;

  if (sm_dbid(sm_env, dbname, &dbid) != 0) {
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

  if (sm_lsdb(sm_env, names) != 0)
    ereport(ERROR, (errmsg("could not list databases: %s", sm_errmsg())));
  for (const std::string &name : names)
    if (datadir_database_oid(name.c_str()) == dboid)
      return pstrdup(name.c_str());
  return NULL;
}

/*
 * Create the database dbname in sm, at once and for good, as CREATE
 * DATABASE does outside a transaction: false if it exists already.
 */
bool datadir_create_database(const char *dbname) {
  if (sm_mkdb(sm_env, dbname, NULL) == 0)
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
  if (sm_rmdb(sm_env, dbname) == 0)
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
  if (sm_opendb(sm_env, dbname, NULL, &sm_session) != 0)
    ereport(FATAL, (errmsg("could not open database \"%s\": %s", dbname,
                           sm_errmsg())));
  on_proc_exit(close_sm_session, 0);
}

/* This session's sm session, or NULL if it has opened none. */
sm_session_t *datadir_sm_session(void) { return sm_session; }

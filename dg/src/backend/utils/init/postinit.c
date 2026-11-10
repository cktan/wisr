/*-------------------------------------------------------------------------
 *
 * postinit.c
 *	  postgres initialization utilities
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/utils/init/postinit.c
 *
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/htup_details.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_tablespace.h"
#include "libpq/auth.h"
#include "libpq/libpq-be.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "postmaster/postmaster.h"
#include "storage/advisory_lock.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/proc.h"
#include "storage/session_registry.h"
#include "storage/procnumber.h"
#include "storage/procsignal.h"
#include "tcop/backend_startup.h"
#include "tcop/tcopprot.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/control_block.h"
#include "utils/datadir.h"
#include "utils/fmgroids.h"
#include "utils/guc_hooks.h"
#include "utils/injection_point.h"
#include "utils/memutils.h"
#include "utils/pg_locale.h"
#include "utils/pg_global.h"
#include "utils/portal.h"
#include "utils/ps_status.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timeout.h"

static void PerformAuthentication(Port *port);
static void CheckMyDatabase(const char *name, bool am_superuser,
                            bool override_allow_connections);
static void ShutdownPostgres(int code, Datum arg);
static void log_shutdown(int code, Datum arg);
static void StatementTimeoutHandler(void);
static void LockTimeoutHandler(void);
static void IdleInTransactionSessionTimeoutHandler(void);
static void TransactionTimeoutHandler(void);
static void IdleSessionTimeoutHandler(void);
static void ClientCheckTimeoutHandler(void);
static bool ThereIsAtLeastOneRole(void);
static void process_startup_options(Port *port, bool am_superuser);

/*** InitPostgres support ***/

/*
 * PerformAuthentication -- authenticate a remote client
 *
 * returns: nothing.  Will not return at all if there's any failure.
 */
static void PerformAuthentication(Port *port) {
  /* This should be set already, but let's make sure */
  ClientAuthInProgress = true; /* limit visibility of log messages */

  /*
   * In EXEC_BACKEND case, we didn't inherit the contents of pg_hba.conf
   * etcetera from the postmaster, and have to load them ourselves.
   *
   * FIXME: [fork/exec] Ugh.  Is there a way around this overhead?
   */

  /* Capture authentication start time for logging */
  conn_timing.auth_start = GetCurrentTimestamp();

  /*
   * Set up a timeout in case a buggy or malicious client fails to respond
   * during authentication.  Since we're inside a transaction and might do
   * database access, we have to use the statement_timeout infrastructure.
   */
  enable_timeout_after(
      STATEMENT_TIMEOUT,
      shmem->authentication_timeout.load(std::memory_order_relaxed) * 1000);

  /*
   * Now perform authentication exchange.
   */
  set_ps_display("authentication");
  ClientAuthentication(port); /* might not return, if failure */

  /*
   * Done with authentication.  Disable the timeout, and log if needed.
   */
  disable_timeout(STATEMENT_TIMEOUT, false);

  /* Capture authentication end time for logging */
  conn_timing.auth_end = GetCurrentTimestamp();

  if (log_connections & LOG_CONNECTION_AUTHORIZATION) {
    StringInfoData logmsg;

    initStringInfo(&logmsg);
    appendStringInfo(&logmsg, _("connection authorized: user=%s"),
                     port->user_name);
    appendStringInfo(&logmsg, _(" database=%s"), port->database_name);

    if (port->application_name != NULL)
      appendStringInfo(&logmsg, _(" application_name=%s"),
                       port->application_name);

#ifdef USE_SSL
    if (port->ssl_in_use)
      appendStringInfo(&logmsg,
                       _(" SSL enabled (protocol=%s, cipher=%s, bits=%d)"),
                       be_tls_get_version(port), be_tls_get_cipher(port),
                       be_tls_get_cipher_bits(port));
#endif
#ifdef ENABLE_GSS
    if (port->gss) {
      const char *princ = be_gssapi_get_princ(port);

      if (princ)
        appendStringInfo(&logmsg,
                         _(" GSS (authenticated=%s, encrypted=%s, "
                           "delegated_credentials=%s, principal=%s)"),
                         be_gssapi_get_auth(port) ? _("yes") : _("no"),
                         be_gssapi_get_enc(port) ? _("yes") : _("no"),
                         be_gssapi_get_delegation(port) ? _("yes") : _("no"),
                         princ);
      else
        appendStringInfo(&logmsg,
                         _(" GSS (authenticated=%s, encrypted=%s, "
                           "delegated_credentials=%s)"),
                         be_gssapi_get_auth(port) ? _("yes") : _("no"),
                         be_gssapi_get_enc(port) ? _("yes") : _("no"),
                         be_gssapi_get_delegation(port) ? _("yes") : _("no"));
    }
#endif

    ereport(LOG, errmsg_internal("%s", logmsg.data));
    pfree(logmsg.data);
  }

  set_ps_display("startup");

  ClientAuthInProgress = false; /* client_min_messages is active now */
}

/*
 * CheckMyDatabase -- fetch information from the pg_database entry for our DB
 */
static void CheckMyDatabase(const char *name, bool am_superuser,
                            bool override_allow_connections) {
  /*
   * The second of aa/ROLE.md section 5's checks: CONNECT on the database,
   * which a superuser need not have, decided by aa (aa_connect_okay()).
   * The client is told what a wrong password tells it, and the log which
   * check failed.
   */
  if (IsUnderPostmaster && !am_superuser) {
    const char *why;

    if (!aa_connect_okay(pg_global_env(), pg_global_session(),
                         pg_global_dbid(MyDatabaseId), GetUserId(), &why))
      ereport(FATAL, (errcode(ERRCODE_INVALID_PASSWORD),
                      errmsg("password authentication failed for user \"%s\"",
                             GetUserNameFromId(GetUserId(), false)),
                      errdetail_log("Role %s \"%s\".", why, name)));
  }

  /*
   * digres keeps no pg_database row to check otherwise.  Its encoding is
   * UTF8, and its collation and character classification are C
   * (WORKNOTE.md step 8).
   */
  SetDatabaseEncoding(PG_UTF8);

  /* Record it as a GUC internal option, too */
  SetConfigOption("server_encoding", GetDatabaseEncodingName(), PGC_INTERNAL,
                  PGC_S_DYNAMIC_DEFAULT);
  /* If we have no other source of client_encoding, use server encoding */
  SetConfigOption("client_encoding", GetDatabaseEncodingName(), PGC_BACKEND,
                  PGC_S_DYNAMIC_DEFAULT);

  /*
   * The process's LC_COLLATE and LC_CTYPE are C, set by main() before any
   * backend: a backend is a thread, and setlocale() and setenv() would
   * change every thread's.  Only the message encoding is the backend's,
   * as pg_perm_setlocale() would set it.
   */
#ifdef ENABLE_NLS
  SetMessageEncoding(pg_bind_textdomain_codeset(textdomain(NULL)));
#else
  SetMessageEncoding(GetDatabaseEncoding());
#endif
  database_ctype_is_c = true;

  /* The default collation, C as the locale is, for comparing text. */
  init_database_collation();
}

/*
 * pg_split_opts -- split a string of options and append it to an argv array
 *
 * The caller is responsible for ensuring the argv array is large enough.  The
 * maximum possible number of arguments added by this routine is
 * (strlen(optstr) + 1) / 2.
 *
 * Because some option values can contain spaces we allow escaping using
 * backslashes, with \\ representing a literal backslash.
 */
void pg_split_opts(char **argv, int *argcp, const char *optstr) {
  StringInfoData s;

  initStringInfo(&s);

  while (*optstr) {
    bool last_was_escape = false;

    resetStringInfo(&s);

    /* skip over leading space */
    while (isspace((unsigned char)*optstr))
      optstr++;

    if (*optstr == '\0')
      break;

    /*
     * Parse a single option, stopping at the first space, unless it's
     * escaped.
     */
    while (*optstr) {
      if (isspace((unsigned char)*optstr) && !last_was_escape)
        break;

      if (!last_was_escape && *optstr == '\\')
        last_was_escape = true;
      else {
        last_was_escape = false;
        appendStringInfoChar(&s, *optstr);
      }

      optstr++;
    }

    /* now store the option in the next argv[] position */
    argv[(*argcp)++] = pstrdup(s.data);
  }

  pfree(s.data);
}

/*
 * Initialize MaxBackends value from config options.
 *
 * This must be called after modules have had the chance to alter GUCs in
 * shared_preload_libraries and before shared memory size is determined.
 *
 * Note that in EXEC_BACKEND environment, the value is passed down from
 * postmaster to subprocesses via BackendParameters in SubPostmasterMain; only
 * postmaster itself and processes not under postmaster control should call
 * this.
 */
void InitializeMaxBackends(void) {
  Assert(pcb->MaxBackends == 0);

  /* Note that this does not include "auxiliary" processes */
  pcb_main()->MaxBackends = pcb->MaxConnections;

  if (pcb->MaxBackends > MAX_BACKENDS)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("too many server processes configured"),
                    errdetail("\"max_connections\" (%d) must be less than %d.",
                              pcb->MaxConnections, MAX_BACKENDS + 1)));
}

/*
 * Early initialization of a backend (either standalone or under postmaster).
 * This happens even before InitPostgres.
 *
 * This is separate from InitPostgres because it is also called by auxiliary
 * processes, such as the background writer process, which may not call
 * InitPostgres at all.
 */
void BaseInit(void) {
  Assert(MyProcNumber != INVALID_PROC_NUMBER);

  /*
   * Initialize file access. Done early so other subsystems can access
   * files.
   */
  InitFileAccess();

  /*
   * Initialize temporary file access after pgstat, so that the temporary
   * file shutdown hook can report temporary file statistics.
   */
  InitTemporaryFileAccess();
}

/* --------------------------------
 * InitPostgres
 *		Initialize POSTGRES.
 *
 * Parameters:
 *	in_dbname, dboid: specify database to connect to, as described below
 *	username, useroid: specify role to connect as, as described below
 *	flags:
 *	  - INIT_PG_LOAD_SESSION_LIBS to honor
 *[session|local]_preload_libraries.
 *	  - INIT_PG_OVERRIDE_ALLOW_CONNS to connect despite !datallowconn.
 *	  - INIT_PG_OVERRIDE_ROLE_LOGIN to connect despite !rolcanlogin.
 *	out_dbname: optional output parameter, see below; pass NULL if not used
 *
 * The database can be specified by name, using the in_dbname parameter, or by
 * OID, using the dboid parameter.  Specify NULL or InvalidOid respectively
 * for the unused parameter.  If dboid is provided, the actual database
 * name can be returned to the caller in out_dbname.  If out_dbname isn't
 * NULL, it must point to a buffer of size NAMEDATALEN.
 *
 * Similarly, the role can be passed by name, using the username parameter,
 * or by OID using the useroid parameter.
 *
 * In bootstrap mode the database and username parameters are NULL/InvalidOid.
 *
 * By convention, INIT_PG_LOAD_SESSION_LIBS should be passed in "flags" in
 * "interactive" sessions (including standalone backends), but not in
 * background processes.
 *
 * We expect that InitProcess() was already called, so we already have a
 * session number ... but no record in the session registry yet.
 *
 * Note:
 *		Be very careful with the order of calls in the InitPostgres
 *function.
 * --------------------------------
 */
void InitPostgres(const char *in_dbname, Oid dboid, const char *username,
                  Oid useroid, bits32 flags, char *out_dbname) {
  bool bootstrap = IsBootstrapProcessingMode();
  bool am_superuser;
  char *fullpath;
  char dbname[NAMEDATALEN];
  int nfree = 0;

  elog(DEBUG3, "InitPostgres");

  /*
   * Add my record to the session registry.
   *
   * Once I have done this, I am visible to other backends!
   */
  InitProcessPhase2();

  /*
   * And initialize an entry in the PgBackendStatus array.  That way, if
   * LWLocks or third-party authentication should happen to hang, it is
   * possible to retrieve some information about what is going on.
   */
  if (!bootstrap) {
    INJECTION_POINT("init-pre-auth", NULL);
  }

  /* A cancel request for this session must give this key. */
  session_set_cancel_key(MyCancelKey, MyCancelKeyLength);

  /*
   * Also set up timeout handlers needed for backend operation.  We need
   * these in every case except bootstrap.
   */
  if (!bootstrap) {
    RegisterTimeout(STATEMENT_TIMEOUT, StatementTimeoutHandler);
    /* A wait in sm for a lock is cancelled after lock_timeout (smstore.c). */
    RegisterTimeout(LOCK_TIMEOUT, LockTimeoutHandler);
    RegisterTimeout(IDLE_IN_TRANSACTION_SESSION_TIMEOUT,
                    IdleInTransactionSessionTimeoutHandler);
    RegisterTimeout(TRANSACTION_TIMEOUT, TransactionTimeoutHandler);
    RegisterTimeout(IDLE_SESSION_TIMEOUT, IdleSessionTimeoutHandler);
    RegisterTimeout(CLIENT_CONNECTION_CHECK_TIMEOUT, ClientCheckTimeoutHandler);
  }

  /*
   * If this is either a bootstrap process or a standalone backend, register
   * the shutdown of the server at exit.
   */
  if (!IsUnderPostmaster) {
    /*
     * Use before_shmem_exit() so that pgstats can rely on DSM segments etc
     * to work.
     */
    before_shmem_exit(log_shutdown, 0);
  }

  /*
   * Initialize the relation cache and the system catalog caches.  Note that
   * no catalog access happens here; we only set up the hashtable structure.
   * We must do this before starting a transaction because transaction abort
   * would try to touch these hashtables.
   */
  RelationCacheInitialize();
  InitCatalogCache();
  InitPlanCache();

  /* Initialize portal manager */
  EnablePortalManager();

  /*
   * No relcache entries are loaded for the shared system catalogs: digres
   * has no catalog tables, and what was read from them here comes from
   * the data directory (utils/init/datadir.c).
   */

  /*
   * Set up process-exit callback to do pre-shutdown cleanup.  This is the
   * one of the first before_shmem_exit callbacks we register; thus, this
   * will be one the last things we do before low-level modules like the
   * buffer manager begin to close down.  We need to have this in place
   * before we begin our first transaction --- if we fail during the
   * initialization transaction, as is entirely possible, we need the
   * AbortTransaction call to clean up.
   */
  before_shmem_exit(ShutdownPostgres, 0);

  /*
   * Start a new transaction here before first access to db.
   */
  if (!bootstrap) {
    /* statement_timestamp must be set for timeouts to work correctly */
    SetCurrentStatementStartTimestamp();
    StartTransactionCommand();

    /*
     * transaction_isolation will have been set to the default by the
     * above.  If the default is "serializable", and we are in hot
     * standby, we will fail if we don't change it to something lower.
     * Fortunately, "read committed" is plenty good enough.
     */
    XactIsoLevel = XACT_READ_COMMITTED;
  }

  /*
   * Perform client authentication if necessary, then figure out our
   * postgres user ID, and see if we are a superuser.
   *
   * In standalone mode we use a fixed ID, otherwise we figure it out from
   * the authenticated user name.
   */
  if (bootstrap) {
    InitializeSessionUserIdStandalone();
    am_superuser = true;
  } else if (!IsUnderPostmaster) {
    InitializeSessionUserIdStandalone();
    am_superuser = true;
    if (!ThereIsAtLeastOneRole())
      ereport(
          WARNING,
          (errcode(ERRCODE_UNDEFINED_OBJECT),
           errmsg("no roles are defined in this database system"),
           errhint("You should immediately run CREATE USER \"%s\" SUPERUSER;.",
                   username != NULL ? username : "postgres")));
  } else {
    /* normal multiuser case */
    Assert(MyProcPort != NULL);
    PerformAuthentication(MyProcPort);
    InitializeSessionUserId(username, useroid, false);
    /* ensure that auth_method is actually valid, aka authn_id is not NULL */
    if (MyClientConnectionInfo.authn_id)
      InitializeSystemUser(MyClientConnectionInfo.authn_id,
                           hba_authname(MyClientConnectionInfo.auth_method));
    am_superuser = superuser();
  }

  /* Report any SSL/GSS details for the session. */
  if (MyProcPort != NULL) {
    Assert(!bootstrap);
  }

  /*
   * Binary upgrades only allowed super-user connections
   */
  if (pcb->IsBinaryUpgrade && !am_superuser) {
    ereport(FATAL,
            (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
             errmsg("must be superuser to connect in binary upgrade mode")));
  }

  /*
   * The last few regular connection slots are reserved for superusers and
   * roles with privileges of pg_use_reserved_connections.  We do not apply
   * these limits to background processes.
   *
   * Note: At this point, the new backend is registered already, so we must
   * check whether the number of free slots is strictly less than the
   * reserved connection limits.
   */
  if (AmRegularBackendProcess() && !am_superuser &&
      (pcb->SuperuserReservedConnections + pcb->ReservedConnections) > 0 &&
      !session_have_free(pcb->SuperuserReservedConnections +
                             pcb->ReservedConnections,
                         &nfree)) {
    if (nfree < pcb->SuperuserReservedConnections)
      ereport(FATAL, (errcode(ERRCODE_TOO_MANY_CONNECTIONS),
                      errmsg("remaining connection slots are reserved for "
                             "roles with the %s attribute",
                             "SUPERUSER")));

    if (!has_privs_of_role(GetUserId(), ROLE_PG_USE_RESERVED_CONNECTIONS))
      ereport(FATAL, (errcode(ERRCODE_TOO_MANY_CONNECTIONS),
                      errmsg("remaining connection slots are reserved for "
                             "roles with privileges of the \"%s\" role",
                             "pg_use_reserved_connections")));
  }

  /*
   * Set up the global variables holding database id and default tablespace.
   * But note we won't actually try to touch the database just yet.
   *
   * We take a shortcut in the bootstrap case, otherwise we have to look up
   * the db's entry in pg_database.
   */
  if (bootstrap) {
    dboid = Template1DbOid;
    MyDatabaseTableSpace = DEFAULTTABLESPACE_OID;
  } else if (in_dbname != NULL) {
    /* The databases are sm's: its id is the oid, and a session opens it. */
    dboid = datadir_database_oid(in_dbname);
    if (!OidIsValid(dboid))
      ereport(FATAL, (errcode(ERRCODE_UNDEFINED_DATABASE),
                      errmsg("database \"%s\" does not exist", in_dbname)));
    datadir_open_session(in_dbname);
  } else if (!OidIsValid(dboid)) {
    /*
     * If this is a background worker not bound to any particular
     * database, we're done now.  Everything that follows only makes sense
     * if we are bound to a specific database.  We do need to close the
     * transaction we started before returning.
     */
    if (!bootstrap) {
      CommitTransactionCommand();
    }
    return;
  }

  /*
   * Now, take a writer's lock on the database we are trying to connect to.
   * If there is a concurrently running DROP DATABASE on that database, this
   * will block us until it finishes (and has committed its update of
   * pg_database).
   *
   * Note that the lock is not held long, only until the end of this startup
   * transaction.  This is OK since we will advertise our use of the
   * database in the ProcArray before dropping the lock (in fact, that's the
   * next thing to do).  Anyone trying a DROP DATABASE after this point will
   * see us in the array once they have the lock.  Ordering is important for
   * this because we don't want to advertise ourselves as being in this
   * database until we have the lock; otherwise we create what amounts to a
   * deadlock with CountOtherDBBackends().
   *
   * Note: use of RowExclusiveLock here is reasonable because we envision
   * our session as being a concurrent writer of the database.  If we had a
   * way of declaring a session as being guaranteed-read-only, we could use
   * AccessShareLock for such sessions and thereby not conflict against
   * CREATE DATABASE.
   */
  if (!bootstrap)
    LockSharedObject(DatabaseRelationId, dboid, 0, RowExclusiveLock);

  /*
   * There is no pg_database row to recheck, and no DROP DATABASE to race
   * with. A database is named by whoever connects to it; every one is in
   * the default tablespace.
   */
  if (!bootstrap) {
    if (in_dbname == NULL)
      ereport(FATAL, (errcode(ERRCODE_UNDEFINED_DATABASE),
                      errmsg("database %u does not exist", dboid)));
    strlcpy(dbname, in_dbname, sizeof(dbname));
    MyDatabaseTableSpace = DEFAULTTABLESPACE_OID;

    /* pass the database name back to the caller */
    if (out_dbname)
      strcpy(out_dbname, dbname);
  }

  /*
   * Now that we rechecked, we are certain to be connected to a database and
   * thus can set MyDatabaseId.
   *
   * It is important that MyDatabaseId only be set once we are sure that the
   * target database can no longer be concurrently dropped or renamed.  For
   * example, without this guarantee, pgstat_update_dbstats() could create
   * entries for databases that were just dropped in the pgstat shutdown
   * callback, which could confuse other code paths.
   */
  MyDatabaseId = dboid;

  /* What another session reads of the database, it reads in our record. */
  session_set_database(MyDatabaseId);

  /*
   * We established a catalog snapshot while reading pg_authid and/or
   * pg_database; but until we have set up MyDatabaseId, we won't react to
   * incoming sinval messages for unshared catalogs, so we won't realize it
   * if the snapshot has been invalidated.  Assume it's no good anymore.
   */
  InvalidateCatalogSnapshot();

  /*
   * The database's directory is sm's, under a name of sm's choosing, so
   * the one PostgreSQL would have made is not looked for.
   */
  fullpath = GetDatabasePath(MyDatabaseId, MyDatabaseTableSpace);
  SetDatabasePath(fullpath);
  pfree(fullpath);

  /* No relcache entries are loaded for the system catalogs: see above. */

  /*
   * The ACL framework is not set up: it caches role memberships read from
   * catalog tables, and digres has one role, a superuser.
   */

  /*
   * Re-read the pg_database row for our database, check permissions and set
   * up database-specific GUC settings.  We can't do this until all the
   * database-access infrastructure is up.  (Also, it wants to know if the
   * user is a superuser, so the above stuff has to happen first.)
   */
  if (!bootstrap)
    CheckMyDatabase(dbname, am_superuser,
                    (flags & INIT_PG_OVERRIDE_ALLOW_CONNS) != 0);

  /*
   * Now process any command-line switches and any additional GUC variable
   * settings passed in the startup packet.   We couldn't do this before
   * because we didn't know if client is a superuser.
   */
  if (MyProcPort != NULL)
    process_startup_options(MyProcPort, am_superuser);

  /* No pg_db_role_setting options: there is no such catalog table. */

  /* Apply PostAuthDelay as soon as we've read all options */
  if (PostAuthDelay > 0)
    pg_usleep(PostAuthDelay * 1000000L);

  /*
   * Initialize various default states that can't be set up until we've
   * selected the active user and gotten the right GUC settings.
   */

  /* set default namespace search path */
  InitializeSearchPath();

  /* initialize client encoding */
  InitializeClientEncoding();

  /*
   * If this is an interactive session, load any libraries that should be
   * preloaded at backend start.  Since those are determined by GUCs, this
   * can't happen until GUC settings are complete, but we want it to happen
   * during the initial transaction in case anything that requires database
   * access needs to be done.
   */
  if ((flags & INIT_PG_LOAD_SESSION_LIBS) != 0)
    process_session_preload_libraries();

  /* close the transaction we started above */
  if (!bootstrap)
    CommitTransactionCommand();
}

/*
 * Process any command-line switches and any additional GUC variable
 * settings passed in the startup packet.
 */
static void process_startup_options(Port *port, bool am_superuser) {
  GucContext gucctx;
  ListCell *gucopts;

  gucctx = am_superuser ? PGC_SU_BACKEND : PGC_BACKEND;

  /*
   * First process any command-line switches that were included in the
   * startup packet, if we are in a regular backend.
   */
  if (port->cmdline_options != NULL) {
    /*
     * The maximum possible number of commandline arguments that could
     * come from port->cmdline_options is (strlen + 1) / 2; see
     * pg_split_opts().
     */
    char **av;
    int maxac;
    int ac;

    maxac = 2 + (strlen(port->cmdline_options) + 1) / 2;

    av = (char **)palloc(maxac * sizeof(char *));
    ac = 0;

    av[ac++] = "postgres";

    pg_split_opts(av, &ac, port->cmdline_options);

    av[ac] = NULL;

    Assert(ac < maxac);

    process_postgres_switches(ac, av, gucctx);
  }

  /*
   * Process any additional GUC variable settings passed in startup packet.
   * These are handled exactly like command-line variables.
   */
  gucopts = list_head(port->guc_options);
  while (gucopts) {
    char *name;
    char *value;

    name = (char *)lfirst(gucopts);
    gucopts = lnext(port->guc_options, gucopts);

    value = (char *)lfirst(gucopts);
    gucopts = lnext(port->guc_options, gucopts);

    SetConfigOption(name, value, gucctx, PGC_S_CLIENT);
  }
}

/*
 * Say that a standalone backend or bootstrap is shutting down.
 */
static void log_shutdown(int code, Datum arg) {
  /* Don't be chatty in standalone mode */
  ereport(NOTICE, (errmsg("shutting down")));
}

/*
 * Backend-shutdown callback.  Do cleanup that we want to be sure happens
 * before all the supporting modules begin to nail their doors shut via
 * their own callbacks.
 *
 * User-level cleanup, such as temp-relation removal and UNLISTEN, happens
 * via separate callbacks that execute before this one.  We don't combine the
 * callbacks because we still want this one to happen if the user-level
 * cleanup fails.
 */
static void ShutdownPostgres(int code, Datum arg) {
  /* Make sure we've killed any active transaction */
  AbortOutOfAnyTransaction();

  /*
   * User locks are not released by transaction end, so be sure to release
   * them explicitly.
   */
  advisory_release_all();
}

/*
 * STATEMENT_TIMEOUT handler: trigger a query-cancel interrupt.
 */
static void StatementTimeoutHandler(void) {
  uint32 bit = INTERRUPT_CANCEL;

  /*
   * During authentication the timeout is used to deal with
   * authentication_timeout - we want to quit in response to such timeouts.
   */
  if (ClientAuthInProgress)
    bit = INTERRUPT_DIE;

  InterruptSelf(bit);
}

/*
 * LOCK_TIMEOUT handler: trigger a query-cancel interrupt.
 */
static void LockTimeoutHandler(void) { InterruptSelf(INTERRUPT_CANCEL); }

static void TransactionTimeoutHandler(void) {
  TransactionTimeoutPending = true;
  InterruptPending = true;
  SetLatch(MyLatch);
}

static void IdleInTransactionSessionTimeoutHandler(void) {
  IdleInTransactionSessionTimeoutPending = true;
  InterruptPending = true;
  SetLatch(MyLatch);
}

static void IdleSessionTimeoutHandler(void) {
  IdleSessionTimeoutPending = true;
  InterruptPending = true;
  SetLatch(MyLatch);
}

static void ClientCheckTimeoutHandler(void) {
  CheckClientConnectionPending = true;
  InterruptPending = true;
  SetLatch(MyLatch);
}

/*
 * Returns true if at least one role is defined in this database cluster.
 */
static bool ThereIsAtLeastOneRole(void) {
  Relation pg_authid_rel;
  TableScanDesc scan;
  bool result;

  pg_authid_rel = table_open(AuthIdRelationId, AccessShareLock);

  scan = table_beginscan_catalog(pg_authid_rel, 0, NULL);
  result = (catalog_scan_next(scan) != NULL);

  table_endscan(scan);
  table_close(pg_authid_rel, AccessShareLock);

  return result;
}

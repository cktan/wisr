/*-------------------------------------------------------------------------
 *
 * dbcommands.c
 *		Database management commands (create/drop database).
 *
 * A database is sm's (utils/datadir.h): CREATE DATABASE makes one in sm and
 * DROP DATABASE drops it, each at once and for good, outside a transaction
 * block.  A database's oid is the id sm gave it.  sm keeps no owner,
 * settings or other attributes of a database yet, nor renames one, so
 * every ALTER DATABASE is refused.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/dbcommands.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_database.h"
#include "commands/dbcommands.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/procsignal.h"
#include "storage/session_registry.h"
#include "utils/acl.h"
#include "utils/control_block.h"
#include "utils/datadir.h"
#include "utils/pg_global.h"
#include "utils/pg_locale.h"
#include "utils/syscache.h"

/*
 * Terminate existing connections to the specified database. This routine
 * is used by the DROP DATABASE command when user has asked to forcefully
 * drop the database.
 *
 * The current backend is always ignored; it is caller's responsibility to
 * check whether the current backend uses the given DB, if it's important.
 *
 * If the target database has a prepared transaction or permissions checks
 * fail for a connection, this fails without terminating anything.
 */
static void TerminateOtherDBBackends(Oid databaseId) {
  std::vector<session_ref_t> others;

  /* The records of the database's other sessions (SHMEM.md section 5.2). */
  for (session_ref_t &record : sessions_all())
    if (record->database.load(std::memory_order_acquire) == databaseId &&
        record != tcb->session)
      others.push_back(std::move(record));

  /*
   * Permissions checks relax the pg_terminate_backend checks in two ways,
   * both by omitting the !OidIsValid(role) check:
   *
   * - Accept terminating autovacuum workers, since DROP DATABASE without
   * FORCE terminates them.
   *
   * Unlike pg_terminate_backend, we don't raise some warnings - like "PID
   * %d is not a PostgreSQL server process", because for us already finished
   * session is not a problem.
   */
  for (const session_ref_t &record : others) {
    Oid role = record->role.load(std::memory_order_acquire);

    if (superuser_arg(role) && !superuser())
      ereport(ERROR,
              (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
               errmsg("permission denied to terminate process"),
               errdetail("Only roles with the %s attribute may terminate "
                         "processes of roles with the %s attribute.",
                         "SUPERUSER", "SUPERUSER")));

    if (!has_privs_of_role(GetUserId(), role) &&
        !has_privs_of_role(GetUserId(), ROLE_PG_SIGNAL_BACKEND))
      ereport(ERROR,
              (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
               errmsg("permission denied to terminate process"),
               errdetail("Only roles with privileges of the role whose "
                         "process is being terminated or with privileges "
                         "of the \"%s\" role may terminate this process.",
                         "pg_signal_backend")));
  }

  /* Each in its own thread: one that has ended since is not another's. */
  for (const session_ref_t &record : others)
    session_interrupt(record, INTERRUPT_DIE);
}

/* Refuse an ALTER DATABASE: sm has nothing to alter yet. */
static void refuse_alter(void) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("ALTER DATABASE is not supported")));
}

/*
 * CREATE DATABASE name, in sm.  Its options, which sm has nothing to keep
 * for, are refused.  Returns the new database's oid.
 */
Oid createdb(ParseState *pstate, const CreatedbStmt *stmt) {
  Oid db_id;

  if (stmt->options != NIL)
    ereport(ERROR,
            (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
             errmsg("CREATE DATABASE takes no options yet"),
             parser_errposition(
                 pstate, linitial_node(DefElem, stmt->options)->location)));
  if (!have_createdb_privilege())
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("permission denied to create database")));
  if (!datadir_create_database(stmt->dbname))
    ereport(ERROR, (errcode(ERRCODE_DUPLICATE_DATABASE),
                    errmsg("database \"%s\" already exists", stmt->dbname)));
  db_id = datadir_database_oid(stmt->dbname);
  /* Its owner, the current role, is pg_global's (aa/ROLE.md). */
  pg_global_set_owner(pg_global_dbid(db_id), DatabaseRelationId, InvalidOid,
                      GetUserId(), false);
  return db_id;
}

/*
 * DROP DATABASE dbname, from sm.  Not the session's own; with force, the
 * other sessions on it are told to end first.  A session leaves sm a moment
 * after it ends, so a database still in use is tried again, for 5 seconds,
 * as PostgreSQL waits for its sessions.
 */
void dropdb(const char *dbname, bool missing_ok, bool force) {
  Oid db_id = datadir_database_oid(dbname);

  if (!OidIsValid(db_id)) {
    if (!missing_ok)
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                      errmsg("database \"%s\" does not exist", dbname)));
    ereport(NOTICE,
            (errmsg("database \"%s\" does not exist, skipping", dbname)));
    return;
  }
  if (!have_createdb_privilege())
    aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE, dbname);
  if (db_id == MyDatabaseId)
    ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE),
                    errmsg("cannot drop the currently open database")));
  /* Tell the database's other sessions to end. */
  if (force)
    TerminateOtherDBBackends(db_id);

  for (int tries = 0;; tries++) {
    switch (datadir_drop_database(dbname)) {
    case DATADIR_DROPPED:
      /* Its entries in pg_global go, after the commit (aa/ROLE.md). */
      pg_global_forget_database(pg_global_dbid(db_id));
      return;
    case DATADIR_NO_DATABASE:
      /* Dropped by another session meanwhile. */
      if (!missing_ok)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                        errmsg("database \"%s\" does not exist", dbname)));
      return;
    case DATADIR_IN_USE:
      if (tries >= 50)
        ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE),
                        errmsg("database \"%s\" is being accessed by other "
                               "users",
                               dbname)));
      break;
    }
    CHECK_FOR_INTERRUPTS();
    /* Wait for its sessions to leave sm: 50 tries, 100ms apart. */
    pg_usleep(100 * 1000L);
  }
}

/* DROP DATABASE: its options, then dropdb(). */
void DropDatabase(ParseState *pstate, DropdbStmt *stmt) {
  bool force = false;
  ListCell *lc;

  foreach (lc, stmt->options) {
    DefElem *opt = (DefElem *)lfirst(lc);

    if (strcmp(opt->defname, "force") == 0)
      force = true;
    else
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("unrecognized %s option \"%s\"", "DROP DATABASE",
                             opt->defname),
                      parser_errposition(pstate, opt->location)));
  }

  dropdb(stmt->dbname, stmt->missing_ok, force);
}

/* ALTER DATABASE ... RENAME TO: refused. */
ObjectAddress RenameDatabase(const char *oldname, const char *newname) {
  refuse_alter();
  return InvalidObjectAddress; /* not reached */
}

/* ALTER DATABASE name WITH options: refused. */
Oid AlterDatabase(ParseState *pstate, AlterDatabaseStmt *stmt,
                  bool isTopLevel) {
  refuse_alter();
  return InvalidOid; /* not reached */
}

/* ALTER DATABASE name REFRESH COLLATION VERSION: refused. */
ObjectAddress AlterDatabaseRefreshColl(AlterDatabaseRefreshCollStmt *stmt) {
  refuse_alter();
  return InvalidObjectAddress; /* not reached */
}

/* ALTER DATABASE name SET or RESET a setting: refused. */
Oid AlterDatabaseSet(AlterDatabaseSetStmt *stmt) {
  refuse_alter();
  return InvalidOid; /* not reached */
}

/*
 * ALTER DATABASE name OWNER TO: the owner is pg_global's, written after
 * the commit (aa/ROLE.md section 3), with PostgreSQL's checks: a role that
 * is not a superuser must own the database, be able to become the new
 * owner, and have CREATEDB.
 */
ObjectAddress AlterDatabaseOwner(const char *dbname, Oid newOwnerId) {
  Oid db_id = datadir_database_oid(dbname);
  ObjectAddress address;

  if (!OidIsValid(db_id))
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database \"%s\" does not exist", dbname)));
  if (pg_global_database_owner(pg_global_dbid(db_id)) != newOwnerId) {
    if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
      aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE, dbname);
    check_can_set_role(GetUserId(), newOwnerId);
    if (!have_createdb_privilege())
      ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                      errmsg("permission denied to change owner of database")));
    pg_global_change_owner(pg_global_dbid(db_id), DatabaseRelationId,
                           InvalidOid, newOwnerId, false);
  }
  ObjectAddressSet(address, DatabaseRelationId, db_id);
  return address;
}

/*
 * Whether the current user may create a database: a superuser, or a role
 * with CREATEDB.
 */
bool have_createdb_privilege(void) {
  bool result = false;
  HeapTuple utup;

  /* Superusers can always do everything */
  if (superuser())
    return true;

  utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(GetUserId()));
  if (HeapTupleIsValid(utup)) {
    result = ((Form_pg_authid)GETSTRUCT(utup))->rolcreatedb;
    ReleaseSysCache(utup);
  }
  return result;
}

/*
 * Check that encoding is the one locales collate and ctype need, or one
 * that suits any.
 */
void check_encoding_locale_matches(int encoding, const char *collate,
                                   const char *ctype) {
  int ctype_encoding = pg_get_encoding_from_locale(ctype, true);
  int collate_encoding = pg_get_encoding_from_locale(collate, true);

  if (!(ctype_encoding == encoding || ctype_encoding == PG_SQL_ASCII ||
        ctype_encoding == -1 ||
#ifdef WIN32
        encoding == PG_UTF8 ||
#endif
        (encoding == PG_SQL_ASCII && superuser())))
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("encoding \"%s\" does not match locale \"%s\"",
                    pg_encoding_to_char(encoding), ctype),
             errdetail("The chosen LC_CTYPE setting requires encoding \"%s\".",
                       pg_encoding_to_char(ctype_encoding))));

  if (!(collate_encoding == encoding || collate_encoding == PG_SQL_ASCII ||
        collate_encoding == -1 ||
#ifdef WIN32
        encoding == PG_UTF8 ||
#endif
        (encoding == PG_SQL_ASCII && superuser())))
    ereport(
        ERROR,
        (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
         errmsg("encoding \"%s\" does not match locale \"%s\"",
                pg_encoding_to_char(encoding), collate),
         errdetail("The chosen LC_COLLATE setting requires encoding \"%s\".",
                   pg_encoding_to_char(collate_encoding))));
}

/*
 * The oid of the database dbname, or InvalidOid if missing_ok and there is
 * none.
 */
Oid get_database_oid(const char *dbname, bool missing_ok) {
  Oid oid = datadir_database_oid(dbname);

  if (!OidIsValid(oid) && !missing_ok)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database \"%s\" does not exist", dbname)));
  return oid;
}

/* The name of the database dbid, palloc'd, or NULL if there is none. */
char *get_database_name(Oid dbid) { return datadir_database_name(dbid); }

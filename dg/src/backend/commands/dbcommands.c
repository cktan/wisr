/*-------------------------------------------------------------------------
 *
 * dbcommands.c
 *		Database management commands (create/drop database).
 *
 * Note: database creation/destruction commands use exclusive locks on
 * the database objects (as expressed by LockSharedObject()) to avoid
 * stepping on each others' toes.  Formerly we used table-level locks
 * on pg_database, but that's too coarse-grained.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/dbcommands.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/multixact.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_database.h"
#include "catalog/pg_db_role_setting.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "commands/comment.h"
#include "commands/dbcommands.h"
#include "commands/defrem.h"
#include "commands/seclabel.h"
#include "commands/tablespace.h"
#include "common/file_perm.h"
#include "mb/pg_wchar.h"
#include "miscadmin.h"
#include "storage/copydir.h"
#include "storage/fd.h"
#include "storage/ipc.h"
#include "storage/lmgr.h"
#include "storage/md.h"
#include "storage/procarray.h"
#include "storage/procsignal.h"
#include "storage/smgr.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/pg_locale.h"
#include "utils/relmapper.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"

typedef struct {
  Oid src_dboid;  /* source (template) DB */
  Oid dest_dboid; /* DB we are trying to create */
} createdb_failure_params;

typedef struct {
  Oid dest_dboid; /* DB we are trying to move */
  Oid dest_tsoid; /* tablespace we are trying to move to */
} movedb_failure_params;

/* non-export function prototypes */
static void createdb_failure_callback(int code, Datum arg);
static void movedb(const char *dbname, const char *tblspcname);
static void movedb_failure_callback(int code, Datum arg);
static bool get_db_info(const char *name, LOCKMODE lockmode, Oid *dbIdP,
                        Oid *ownerIdP, int *encodingP, bool *dbIsTemplateP,
                        bool *dbAllowConnP, bool *dbHasLoginEvtP,
                        TransactionId *dbFrozenXidP, MultiXactId *dbMinMultiP,
                        Oid *dbTablespace, char **dbCollate, char **dbCtype,
                        char **dbLocale, char **dbIcurules, char *dbLocProvider,
                        char **dbCollversion);
static void remove_dbtablespaces(Oid db_id);
static bool check_db_file_conflict(Oid db_id);
static int errdetail_busy_db(int notherbackends, int npreparedxacts);
static void CreateDatabaseUsingFileCopy(Oid src_dboid, Oid dst_dboid,
                                        Oid src_tsid, Oid dst_tsid);

/*
 * Create a new database using the FILE_COPY strategy.
 *
 * Copy each tablespace at the filesystem level, and log a single WAL record
 * for each tablespace copied.  This requires a checkpoint before and after the
 * copy, which may be expensive, but it does greatly reduce WAL generation
 * if the copied database is large.
 */
static void CreateDatabaseUsingFileCopy(Oid src_dboid, Oid dst_dboid,
                                        Oid src_tsid, Oid dst_tsid) {
  TableScanDesc scan;
  Relation rel;
  HeapTuple tuple;

  /*
   * Write the source database's dirty buffers out before copying its files.
   * There is no checkpoint to do it.
   */
  FlushDatabaseBuffers(src_dboid);

  /*
   * Iterate through all tablespaces of the template database, and copy each
   * one to the new database.
   */
  rel = table_open(TableSpaceRelationId, AccessShareLock);
  scan = table_beginscan_catalog(rel, 0, NULL);
  while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
    Form_pg_tablespace spaceform = (Form_pg_tablespace)GETSTRUCT(tuple);
    Oid srctablespace = spaceform->oid;
    Oid dsttablespace;
    char *srcpath;
    char *dstpath;
    struct stat st;

    /* No need to copy global tablespace */
    if (srctablespace == GLOBALTABLESPACE_OID)
      continue;

    srcpath = GetDatabasePath(src_dboid, srctablespace);

    if (stat(srcpath, &st) < 0 || !S_ISDIR(st.st_mode) ||
        directory_is_empty(srcpath)) {
      /* Assume we can ignore it */
      pfree(srcpath);
      continue;
    }

    if (srctablespace == src_tsid)
      dsttablespace = dst_tsid;
    else
      dsttablespace = srctablespace;

    dstpath = GetDatabasePath(dst_dboid, dsttablespace);

    /*
     * Copy this subdirectory to the new location
     *
     * We don't need to copy subdirectories
     */
    copydir(srcpath, dstpath, false);

    pfree(srcpath);
    pfree(dstpath);
  }
  table_endscan(scan);
  table_close(rel, AccessShareLock);
}

/*
 * CREATE DATABASE
 */
Oid createdb(ParseState *pstate, const CreatedbStmt *stmt) {
  Oid src_dboid;
  Oid src_owner;
  int src_encoding = -1;
  char *src_collate = NULL;
  char *src_ctype = NULL;
  char *src_locale = NULL;
  char *src_icurules = NULL;
  char src_locprovider = '\0';
  char *src_collversion = NULL;
  bool src_istemplate;
  bool src_hasloginevt = false;
  bool src_allowconn;
  TransactionId src_frozenxid = InvalidTransactionId;
  MultiXactId src_minmxid = InvalidMultiXactId;
  Oid src_deftablespace;
  volatile Oid dst_deftablespace;
  Relation pg_database_rel;
  HeapTuple tuple;
  Datum new_record[Natts_pg_database] = {0};
  bool new_record_nulls[Natts_pg_database] = {0};
  Oid dboid = InvalidOid;
  Oid datdba;
  ListCell *option;
  DefElem *tablespacenameEl = NULL;
  DefElem *ownerEl = NULL;
  DefElem *templateEl = NULL;
  DefElem *encodingEl = NULL;
  DefElem *localeEl = NULL;
  DefElem *builtinlocaleEl = NULL;
  DefElem *collateEl = NULL;
  DefElem *ctypeEl = NULL;
  DefElem *iculocaleEl = NULL;
  DefElem *icurulesEl = NULL;
  DefElem *locproviderEl = NULL;
  DefElem *istemplateEl = NULL;
  DefElem *allowconnectionsEl = NULL;
  DefElem *connlimitEl = NULL;
  DefElem *collversionEl = NULL;
  DefElem *strategyEl = NULL;
  char *dbname = stmt->dbname;
  char *dbowner = NULL;
  const char *dbtemplate = NULL;
  char *dbcollate = NULL;
  char *dbctype = NULL;
  const char *dblocale = NULL;
  char *dbicurules = NULL;
  char dblocprovider = '\0';
  char *canonname;
  int encoding = -1;
  bool dbistemplate = false;
  bool dballowconnections = true;
  int dbconnlimit = DATCONNLIMIT_UNLIMITED;
  char *dbcollversion = NULL;
  int notherbackends;
  int npreparedxacts;
  createdb_failure_params fparms;

  /* Extract options from the statement node tree */
  foreach (option, stmt->options) {
    DefElem *defel = (DefElem *)lfirst(option);

    if (strcmp(defel->defname, "tablespace") == 0) {
      if (tablespacenameEl)
        errorConflictingDefElem(defel, pstate);
      tablespacenameEl = defel;
    } else if (strcmp(defel->defname, "owner") == 0) {
      if (ownerEl)
        errorConflictingDefElem(defel, pstate);
      ownerEl = defel;
    } else if (strcmp(defel->defname, "template") == 0) {
      if (templateEl)
        errorConflictingDefElem(defel, pstate);
      templateEl = defel;
    } else if (strcmp(defel->defname, "encoding") == 0) {
      if (encodingEl)
        errorConflictingDefElem(defel, pstate);
      encodingEl = defel;
    } else if (strcmp(defel->defname, "locale") == 0) {
      if (localeEl)
        errorConflictingDefElem(defel, pstate);
      localeEl = defel;
    } else if (strcmp(defel->defname, "builtin_locale") == 0) {
      if (builtinlocaleEl)
        errorConflictingDefElem(defel, pstate);
      builtinlocaleEl = defel;
    } else if (strcmp(defel->defname, "lc_collate") == 0) {
      if (collateEl)
        errorConflictingDefElem(defel, pstate);
      collateEl = defel;
    } else if (strcmp(defel->defname, "lc_ctype") == 0) {
      if (ctypeEl)
        errorConflictingDefElem(defel, pstate);
      ctypeEl = defel;
    } else if (strcmp(defel->defname, "icu_locale") == 0) {
      if (iculocaleEl)
        errorConflictingDefElem(defel, pstate);
      iculocaleEl = defel;
    } else if (strcmp(defel->defname, "icu_rules") == 0) {
      if (icurulesEl)
        errorConflictingDefElem(defel, pstate);
      icurulesEl = defel;
    } else if (strcmp(defel->defname, "locale_provider") == 0) {
      if (locproviderEl)
        errorConflictingDefElem(defel, pstate);
      locproviderEl = defel;
    } else if (strcmp(defel->defname, "is_template") == 0) {
      if (istemplateEl)
        errorConflictingDefElem(defel, pstate);
      istemplateEl = defel;
    } else if (strcmp(defel->defname, "allow_connections") == 0) {
      if (allowconnectionsEl)
        errorConflictingDefElem(defel, pstate);
      allowconnectionsEl = defel;
    } else if (strcmp(defel->defname, "connection_limit") == 0) {
      if (connlimitEl)
        errorConflictingDefElem(defel, pstate);
      connlimitEl = defel;
    } else if (strcmp(defel->defname, "collation_version") == 0) {
      if (collversionEl)
        errorConflictingDefElem(defel, pstate);
      collversionEl = defel;
    } else if (strcmp(defel->defname, "location") == 0) {
      ereport(WARNING, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("LOCATION is not supported anymore"),
                        errhint("Consider using tablespaces instead."),
                        parser_errposition(pstate, defel->location)));
    } else if (strcmp(defel->defname, "oid") == 0) {
      dboid = defGetObjectId(defel);

      /*
       * We don't normally permit new databases to be created with
       * system-assigned OIDs. pg_upgrade tries to preserve database
       * OIDs, so we can't allow any database to be created with an OID
       * that might be in use in a freshly-initialized cluster created
       * by some future version. We assume all such OIDs will be from
       * the system-managed OID range.
       *
       * As an exception, however, we permit any OID to be assigned when
       * allow_system_table_mods=on (so that initdb can assign system
       * OIDs to template0 and postgres) or when performing a binary
       * upgrade (so that pg_upgrade can preserve whatever OIDs it finds
       * in the source cluster).
       */
      if (dboid < FirstNormalObjectId && !allowSystemTableMods &&
          !IsBinaryUpgrade)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE)),
                errmsg("OIDs less than %u are reserved for system objects",
                       FirstNormalObjectId));
    } else if (strcmp(defel->defname, "strategy") == 0) {
      if (strategyEl)
        errorConflictingDefElem(defel, pstate);
      strategyEl = defel;
    } else
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("option \"%s\" not recognized", defel->defname),
                      parser_errposition(pstate, defel->location)));
  }

  if (ownerEl && ownerEl->arg)
    dbowner = defGetString(ownerEl);
  if (templateEl && templateEl->arg)
    dbtemplate = defGetString(templateEl);
  if (encodingEl && encodingEl->arg) {
    const char *encoding_name;

    if (IsA(encodingEl->arg, Integer)) {
      encoding = defGetInt32(encodingEl);
      encoding_name = pg_encoding_to_char(encoding);
      if (strcmp(encoding_name, "") == 0 ||
          pg_valid_server_encoding(encoding_name) < 0)
        ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                        errmsg("%d is not a valid encoding code", encoding),
                        parser_errposition(pstate, encodingEl->location)));
    } else {
      encoding_name = defGetString(encodingEl);
      encoding = pg_valid_server_encoding(encoding_name);
      if (encoding < 0)
        ereport(ERROR,
                (errcode(ERRCODE_UNDEFINED_OBJECT),
                 errmsg("%s is not a valid encoding name", encoding_name),
                 parser_errposition(pstate, encodingEl->location)));
    }
  }
  if (localeEl && localeEl->arg) {
    dbcollate = defGetString(localeEl);
    dbctype = defGetString(localeEl);
    dblocale = defGetString(localeEl);
  }
  if (builtinlocaleEl && builtinlocaleEl->arg)
    dblocale = defGetString(builtinlocaleEl);
  if (collateEl && collateEl->arg)
    dbcollate = defGetString(collateEl);
  if (ctypeEl && ctypeEl->arg)
    dbctype = defGetString(ctypeEl);
  if (iculocaleEl && iculocaleEl->arg)
    dblocale = defGetString(iculocaleEl);
  if (icurulesEl && icurulesEl->arg)
    dbicurules = defGetString(icurulesEl);
  if (locproviderEl && locproviderEl->arg) {
    char *locproviderstr = defGetString(locproviderEl);

    if (pg_strcasecmp(locproviderstr, "builtin") == 0)
      dblocprovider = COLLPROVIDER_BUILTIN;
    else if (pg_strcasecmp(locproviderstr, "icu") == 0)
      dblocprovider = COLLPROVIDER_ICU;
    else if (pg_strcasecmp(locproviderstr, "libc") == 0)
      dblocprovider = COLLPROVIDER_LIBC;
    else
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
               errmsg("unrecognized locale provider: %s", locproviderstr)));
  }
  if (istemplateEl && istemplateEl->arg)
    dbistemplate = defGetBoolean(istemplateEl);
  if (allowconnectionsEl && allowconnectionsEl->arg)
    dballowconnections = defGetBoolean(allowconnectionsEl);
  if (connlimitEl && connlimitEl->arg) {
    dbconnlimit = defGetInt32(connlimitEl);
    if (dbconnlimit < DATCONNLIMIT_UNLIMITED)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("invalid connection limit: %d", dbconnlimit)));
  }
  if (collversionEl)
    dbcollversion = defGetString(collversionEl);

  /* obtain OID of proposed owner */
  if (dbowner)
    datdba = get_role_oid(dbowner, false);
  else
    datdba = GetUserId();

  /*
   * To create a database, must have createdb privilege and must be able to
   * become the target role (this does not imply that the target role itself
   * must have createdb privilege).  The latter provision guards against
   * "giveaway" attacks.  Note that a superuser will always have both of
   * these privileges a fortiori.
   */
  if (!have_createdb_privilege())
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("permission denied to create database")));

  check_can_set_role(GetUserId(), datdba);

  /*
   * Lookup database (template) to be cloned, and obtain share lock on it.
   * ShareLock allows two CREATE DATABASEs to work from the same template
   * concurrently, while ensuring no one is busy dropping it in parallel
   * (which would be Very Bad since we'd likely get an incomplete copy
   * without knowing it).  This also prevents any new connections from being
   * made to the source until we finish copying it, so we can be sure it
   * won't change underneath us.
   */
  if (!dbtemplate)
    dbtemplate = "template1"; /* Default template database name */

  if (!get_db_info(dbtemplate, ShareLock, &src_dboid, &src_owner, &src_encoding,
                   &src_istemplate, &src_allowconn, &src_hasloginevt,
                   &src_frozenxid, &src_minmxid, &src_deftablespace,
                   &src_collate, &src_ctype, &src_locale, &src_icurules,
                   &src_locprovider, &src_collversion))
    ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_DATABASE),
             errmsg("template database \"%s\" does not exist", dbtemplate)));

  /*
   * If the source database was in the process of being dropped, we can't
   * use it as a template.
   */
  if (database_is_invalid_oid(src_dboid))
    ereport(
        ERROR, errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
        errmsg("cannot use invalid database \"%s\" as template", dbtemplate),
        errhint("Use DROP DATABASE to drop invalid databases."));

  /*
   * Permission check: to copy a DB that's not marked datistemplate, you
   * must be superuser or the owner thereof.
   */
  if (!src_istemplate) {
    if (!object_ownercheck(DatabaseRelationId, src_dboid, GetUserId()))
      ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                      errmsg("permission denied to copy database \"%s\"",
                             dbtemplate)));
  }

  /*
   * Validate the database creation strategy.  The database is copied file
   * by file: wal_log, which copies it block by block through WAL, needs
   * WAL, and digres has none.
   */
  if (strategyEl && strategyEl->arg) {
    char *strategy;

    strategy = defGetString(strategyEl);
    if (pg_strcasecmp(strategy, "file_copy") != 0)
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("invalid create database strategy \"%s\"", strategy),
               errhint("The only valid strategy is \"file_copy\".")));
  }

  /* If encoding or locales are defaulted, use source's setting */
  if (encoding < 0)
    encoding = src_encoding;
  if (dbcollate == NULL)
    dbcollate = src_collate;
  if (dbctype == NULL)
    dbctype = src_ctype;
  if (dblocprovider == '\0')
    dblocprovider = src_locprovider;
  if (dblocale == NULL && dblocprovider == src_locprovider)
    dblocale = src_locale;
  if (dbicurules == NULL)
    dbicurules = src_icurules;

  /* Some encodings are client only */
  if (!PG_VALID_BE_ENCODING(encoding))
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("invalid server encoding %d", encoding)));

  /* Check that the chosen locales are valid, and get canonical spellings */
  if (!check_locale(LC_COLLATE, dbcollate, &canonname)) {
    if (dblocprovider == COLLPROVIDER_BUILTIN)
      ereport(ERROR,
              (errcode(ERRCODE_WRONG_OBJECT_TYPE),
               errmsg("invalid LC_COLLATE locale name: \"%s\"", dbcollate),
               errhint("If the locale name is specific to the builtin "
                       "provider, use BUILTIN_LOCALE.")));
    else if (dblocprovider == COLLPROVIDER_ICU)
      ereport(ERROR,
              (errcode(ERRCODE_WRONG_OBJECT_TYPE),
               errmsg("invalid LC_COLLATE locale name: \"%s\"", dbcollate),
               errhint("If the locale name is specific to the ICU provider, "
                       "use ICU_LOCALE.")));
    else
      ereport(ERROR,
              (errcode(ERRCODE_WRONG_OBJECT_TYPE),
               errmsg("invalid LC_COLLATE locale name: \"%s\"", dbcollate)));
  }
  dbcollate = canonname;
  if (!check_locale(LC_CTYPE, dbctype, &canonname)) {
    if (dblocprovider == COLLPROVIDER_BUILTIN)
      ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                      errmsg("invalid LC_CTYPE locale name: \"%s\"", dbctype),
                      errhint("If the locale name is specific to the builtin "
                              "provider, use BUILTIN_LOCALE.")));
    else if (dblocprovider == COLLPROVIDER_ICU)
      ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                      errmsg("invalid LC_CTYPE locale name: \"%s\"", dbctype),
                      errhint("If the locale name is specific to the ICU "
                              "provider, use ICU_LOCALE.")));
    else
      ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                      errmsg("invalid LC_CTYPE locale name: \"%s\"", dbctype)));
  }

  dbctype = canonname;

  check_encoding_locale_matches(encoding, dbcollate, dbctype);

  /* validate provider-specific parameters */
  if (dblocprovider != COLLPROVIDER_BUILTIN) {
    if (builtinlocaleEl)
      ereport(ERROR, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                      errmsg("BUILTIN_LOCALE cannot be specified unless locale "
                             "provider is builtin")));
  }

  if (dblocprovider != COLLPROVIDER_ICU) {
    if (iculocaleEl)
      ereport(ERROR, (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
                      errmsg("ICU locale cannot be specified unless locale "
                             "provider is ICU")));

    if (dbicurules)
      ereport(
          ERROR,
          (errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
           errmsg(
               "ICU rules cannot be specified unless locale provider is ICU")));
  }

  /* validate and canonicalize locale for the provider */
  if (dblocprovider == COLLPROVIDER_BUILTIN) {
    /*
     * This would happen if template0 uses the libc provider but the new
     * database uses builtin.
     */
    if (!dblocale)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("LOCALE or BUILTIN_LOCALE must be specified")));

    dblocale = builtin_validate_locale(encoding, dblocale);
  } else if (dblocprovider == COLLPROVIDER_ICU) {
    if (!(is_encoding_supported_by_icu(encoding)))
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("encoding \"%s\" is not supported with ICU provider",
                      pg_encoding_to_char(encoding))));

    /*
     * This would happen if template0 uses the libc provider but the new
     * database uses icu.
     */
    if (!dblocale)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("LOCALE or ICU_LOCALE must be specified")));

    /*
     * During binary upgrade, or when the locale came from the template
     * database, preserve locale string. Otherwise, canonicalize to a
     * language tag.
     */
    if (!IsBinaryUpgrade && dblocale != src_locale) {
      char *langtag = icu_language_tag(dblocale, icu_validation_level);

      if (langtag && strcmp(dblocale, langtag) != 0) {
        ereport(NOTICE,
                (errmsg("using standard form \"%s\" for ICU locale \"%s\"",
                        langtag, dblocale)));

        dblocale = langtag;
      }
    }

    icu_validate_locale(dblocale);
  }

  /* for libc, locale comes from datcollate and datctype */
  if (dblocprovider == COLLPROVIDER_LIBC)
    dblocale = NULL;

  /*
   * Check that the new encoding and locale settings match the source
   * database.  We insist on this because we simply copy the source data ---
   * any non-ASCII data would be wrongly encoded, and any indexes sorted
   * according to the source locale would be wrong.
   *
   * However, we assume that template0 doesn't contain any non-ASCII data
   * nor any indexes that depend on collation or ctype, so template0 can be
   * used as template for creating a database with any encoding or locale.
   */
  if (strcmp(dbtemplate, "template0") != 0) {
    if (encoding != src_encoding)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("new encoding (%s) is incompatible with the "
                             "encoding of the template database (%s)",
                             pg_encoding_to_char(encoding),
                             pg_encoding_to_char(src_encoding)),
                      errhint("Use the same encoding as in the template "
                              "database, or use template0 as template.")));

    if (strcmp(dbcollate, src_collate) != 0)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("new collation (%s) is incompatible with the "
                             "collation of the template database (%s)",
                             dbcollate, src_collate),
                      errhint("Use the same collation as in the template "
                              "database, or use template0 as template.")));

    if (strcmp(dbctype, src_ctype) != 0)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("new LC_CTYPE (%s) is incompatible with the "
                             "LC_CTYPE of the template database (%s)",
                             dbctype, src_ctype),
                      errhint("Use the same LC_CTYPE as in the template "
                              "database, or use template0 as template.")));

    if (dblocprovider != src_locprovider)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("new locale provider (%s) does not match locale "
                             "provider of the template database (%s)",
                             collprovider_name(dblocprovider),
                             collprovider_name(src_locprovider)),
                      errhint("Use the same locale provider as in the template "
                              "database, or use template0 as template.")));

    if (dblocprovider == COLLPROVIDER_ICU) {
      char *val1;
      char *val2;

      Assert(dblocale);
      Assert(src_locale);
      if (strcmp(dblocale, src_locale) != 0)
        ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                        errmsg("new ICU locale (%s) is incompatible with the "
                               "ICU locale of the template database (%s)",
                               dblocale, src_locale),
                        errhint("Use the same ICU locale as in the template "
                                "database, or use template0 as template.")));

      val1 = dbicurules;
      if (!val1)
        val1 = "";
      val2 = src_icurules;
      if (!val2)
        val2 = "";
      if (strcmp(val1, val2) != 0)
        ereport(ERROR,
                (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                 errmsg("new ICU collation rules (%s) are incompatible with "
                        "the ICU collation rules of the template database (%s)",
                        val1, val2),
                 errhint("Use the same ICU collation rules as in the template "
                         "database, or use template0 as template.")));
    }
  }

  /*
   * If we got a collation version for the template database, check that it
   * matches the actual OS collation version.  Otherwise error; the user
   * needs to fix the template database first.  Don't complain if a
   * collation version was specified explicitly as a statement option; that
   * is used by pg_upgrade to reproduce the old state exactly.
   *
   * (If the template database has no collation version, then either the
   * platform/provider does not support collation versioning, or it's
   * template0, for which we stipulate that it does not contain
   * collation-using objects.)
   */
  if (src_collversion && !collversionEl) {
    char *actual_versionstr;
    const char *locale;

    if (dblocprovider == COLLPROVIDER_LIBC)
      locale = dbcollate;
    else
      locale = dblocale;

    actual_versionstr = get_collation_actual_version(dblocprovider, locale);
    if (!actual_versionstr)
      ereport(ERROR,
              (errmsg("template database \"%s\" has a collation version, but "
                      "no actual collation version could be determined",
                      dbtemplate)));

    if (strcmp(actual_versionstr, src_collversion) != 0)
      ereport(
          ERROR,
          (errmsg("template database \"%s\" has a collation version mismatch",
                  dbtemplate),
           errdetail(
               "The template database was created using collation version %s, "
               "but the operating system provides version %s.",
               src_collversion, actual_versionstr),
           errhint("Rebuild all objects in the template database that use the "
                   "default collation and run "
                   "ALTER DATABASE %s REFRESH COLLATION VERSION, "
                   "or build PostgreSQL with the right library version.",
                   quote_identifier(dbtemplate))));
  }

  if (dbcollversion == NULL)
    dbcollversion = src_collversion;

  /*
   * Normally, we copy the collation version from the template database.
   * This last resort only applies if the template database does not have a
   * collation version, which is normally only the case for template0.
   */
  if (dbcollversion == NULL) {
    const char *locale;

    if (dblocprovider == COLLPROVIDER_LIBC)
      locale = dbcollate;
    else
      locale = dblocale;

    dbcollversion = get_collation_actual_version(dblocprovider, locale);
  }

  /* Resolve default tablespace for new database */
  if (tablespacenameEl && tablespacenameEl->arg) {
    char *tablespacename;
    AclResult aclresult;

    tablespacename = defGetString(tablespacenameEl);
    dst_deftablespace = get_tablespace_oid(tablespacename, false);
    /* check permissions */
    aclresult = object_aclcheck(TableSpaceRelationId, dst_deftablespace,
                                GetUserId(), ACL_CREATE);
    if (aclresult != ACLCHECK_OK)
      aclcheck_error(aclresult, OBJECT_TABLESPACE, tablespacename);

    /* pg_global must never be the default tablespace */
    if (dst_deftablespace == GLOBALTABLESPACE_OID)
      ereport(ERROR,
              (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
               errmsg("pg_global cannot be used as default tablespace")));

    /*
     * If we are trying to change the default tablespace of the template,
     * we require that the template not have any files in the new default
     * tablespace.  This is necessary because otherwise the copied
     * database would contain pg_class rows that refer to its default
     * tablespace both explicitly (by OID) and implicitly (as zero), which
     * would cause problems.  For example another CREATE DATABASE using
     * the copied database as template, and trying to change its default
     * tablespace again, would yield outright incorrect results (it would
     * improperly move tables to the new default tablespace that should
     * stay in the same tablespace).
     */
    if (dst_deftablespace != src_deftablespace) {
      char *srcpath;
      struct stat st;

      srcpath = GetDatabasePath(src_dboid, dst_deftablespace);

      if (stat(srcpath, &st) == 0 && S_ISDIR(st.st_mode) &&
          !directory_is_empty(srcpath))
        ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                        errmsg("cannot assign new default tablespace \"%s\"",
                               tablespacename),
                        errdetail("There is a conflict because database \"%s\" "
                                  "already has some tables in this tablespace.",
                                  dbtemplate)));
      pfree(srcpath);
    }
  } else {
    /* Use template database's default tablespace */
    dst_deftablespace = src_deftablespace;
    /* Note there is no additional permission check in this path */
  }

  /*
   * If built with appropriate switch, whine when regression-testing
   * conventions for database names are violated.  But don't complain during
   * initdb.
   */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
  if (IsUnderPostmaster && strstr(dbname, "regression") == NULL)
    elog(WARNING, "databases created by regression test cases should have "
                  "names including \"regression\"");
#endif

  /*
   * Check for db name conflict.  This is just to give a more friendly error
   * message than "unique index violation".  There's a race condition but
   * we're willing to accept the less friendly message in that case.
   */
  if (OidIsValid(get_database_oid(dbname, true)))
    ereport(ERROR, (errcode(ERRCODE_DUPLICATE_DATABASE),
                    errmsg("database \"%s\" already exists", dbname)));

  /*
   * The source DB can't have any active backends, except this one
   * (exception is to allow CREATE DB while connected to template1).
   * Otherwise we might copy inconsistent data.
   *
   * This should be last among the basic error checks, because it involves
   * potential waiting; we may as well throw an error first if we're gonna
   * throw one.
   */
  if (CountOtherDBBackends(src_dboid, &notherbackends, &npreparedxacts))
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_IN_USE),
             errmsg("source database \"%s\" is being accessed by other users",
                    dbtemplate),
             errdetail_busy_db(notherbackends, npreparedxacts)));

  /*
   * Select an OID for the new database, checking that it doesn't have a
   * filename conflict with anything already existing in the tablespace
   * directories.
   */
  pg_database_rel = table_open(DatabaseRelationId, RowExclusiveLock);

  /*
   * If database OID is configured, check if the OID is already in use or
   * data directory already exists.
   */
  if (OidIsValid(dboid)) {
    char *existing_dbname = get_database_name(dboid);

    if (existing_dbname != NULL)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE)),
              errmsg("database OID %u is already in use by database \"%s\"",
                     dboid, existing_dbname));

    if (check_db_file_conflict(dboid))
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE)),
              errmsg("data directory with the specified OID %u already exists",
                     dboid));
  } else {
    /* Select an OID for the new database if is not explicitly configured. */
    do {
      dboid = GetNewOidWithIndex(pg_database_rel, DatabaseOidIndexId,
                                 Anum_pg_database_oid);
    } while (check_db_file_conflict(dboid));
  }

  /*
   * Insert a new tuple into pg_database.  This establishes our ownership of
   * the new database name (anyone else trying to insert the same name will
   * block on the unique index, and fail after we commit).
   */

  Assert((dblocprovider != COLLPROVIDER_LIBC && dblocale) ||
         (dblocprovider == COLLPROVIDER_LIBC && !dblocale));

  /* Form tuple */
  new_record[Anum_pg_database_oid - 1] = ObjectIdGetDatum(dboid);
  new_record[Anum_pg_database_datname - 1] =
      DirectFunctionCall1(namein, CStringGetDatum(dbname));
  new_record[Anum_pg_database_datdba - 1] = ObjectIdGetDatum(datdba);
  new_record[Anum_pg_database_encoding - 1] = Int32GetDatum(encoding);
  new_record[Anum_pg_database_datlocprovider - 1] = CharGetDatum(dblocprovider);
  new_record[Anum_pg_database_datistemplate - 1] = BoolGetDatum(dbistemplate);
  new_record[Anum_pg_database_datallowconn - 1] =
      BoolGetDatum(dballowconnections);
  new_record[Anum_pg_database_dathasloginevt - 1] =
      BoolGetDatum(src_hasloginevt);
  new_record[Anum_pg_database_datconnlimit - 1] = Int32GetDatum(dbconnlimit);
  new_record[Anum_pg_database_datfrozenxid - 1] =
      TransactionIdGetDatum(src_frozenxid);
  new_record[Anum_pg_database_datminmxid - 1] =
      TransactionIdGetDatum(src_minmxid);
  new_record[Anum_pg_database_dattablespace - 1] =
      ObjectIdGetDatum(dst_deftablespace);
  new_record[Anum_pg_database_datcollate - 1] = CStringGetTextDatum(dbcollate);
  new_record[Anum_pg_database_datctype - 1] = CStringGetTextDatum(dbctype);
  if (dblocale)
    new_record[Anum_pg_database_datlocale - 1] = CStringGetTextDatum(dblocale);
  else
    new_record_nulls[Anum_pg_database_datlocale - 1] = true;
  if (dbicurules)
    new_record[Anum_pg_database_daticurules - 1] =
        CStringGetTextDatum(dbicurules);
  else
    new_record_nulls[Anum_pg_database_daticurules - 1] = true;
  if (dbcollversion)
    new_record[Anum_pg_database_datcollversion - 1] =
        CStringGetTextDatum(dbcollversion);
  else
    new_record_nulls[Anum_pg_database_datcollversion - 1] = true;

  /*
   * We deliberately set datacl to default (NULL), rather than copying it
   * from the template database.  Copying it would be a bad idea when the
   * owner is not the same as the template's owner.
   */
  new_record_nulls[Anum_pg_database_datacl - 1] = true;

  tuple = heap_form_tuple(RelationGetDescr(pg_database_rel), new_record,
                          new_record_nulls);

  CatalogTupleInsert(pg_database_rel, tuple);

  /*
   * Now generate additional catalog entries associated with the new DB
   */

  /* Register owner dependency */
  recordDependencyOnOwner(DatabaseRelationId, dboid, datdba);

  /* Create pg_shdepend entries for objects within database */
  copyTemplateDependencies(src_dboid, dboid);

  /* Post creation hook for new database */
  InvokeObjectPostCreateHook(DatabaseRelationId, dboid, 0);

  /*
   * Once we start copying subdirectories, we need to be able to clean 'em
   * up if we fail.  Use an ENSURE block to make sure this happens.  (This
   * is not a 100% solution, because of the possibility of failure during
   * transaction commit after we leave this routine, but it should handle
   * most scenarios.)
   */
  fparms.src_dboid = src_dboid;
  fparms.dest_dboid = dboid;

  PG_ENSURE_ERROR_CLEANUP(createdb_failure_callback, PointerGetDatum(&fparms));
  {
    /* Copy the database file by file */
    CreateDatabaseUsingFileCopy(src_dboid, dboid, src_deftablespace,
                                dst_deftablespace);

    /*
     * Close pg_database, but keep lock till commit.
     */
    table_close(pg_database_rel, NoLock);
  }
  PG_END_ENSURE_ERROR_CLEANUP(createdb_failure_callback,
                              PointerGetDatum(&fparms));

  return dboid;
}

/*
 * Check whether chosen encoding matches chosen locale settings.  This
 * restriction is necessary because libc's locale-specific code usually
 * fails when presented with data in an encoding it's not expecting. We
 * allow mismatch in four cases:
 *
 * 1. locale encoding = SQL_ASCII, which means that the locale is C/POSIX
 * which works with any encoding.
 *
 * 2. locale encoding = -1, which means that we couldn't determine the
 * locale's encoding and have to trust the user to get it right.
 *
 * 3. selected encoding is UTF8 and platform is win32. This is because
 * UTF8 is a pseudo codepage that is supported in all locales since it's
 * converted to UTF16 before being used.
 *
 * 4. selected encoding is SQL_ASCII, but only if you're a superuser. This
 * is risky but we have historically allowed it --- notably, the
 * regression tests require it.
 *
 * Note: if you change this policy, fix initdb to match.
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

/* Error cleanup callback for createdb */
static void createdb_failure_callback(int code, Datum arg) {
  createdb_failure_params *fparms =
      (createdb_failure_params *)DatumGetPointer(arg);

  /*
   * Release lock on source database before doing recursive remove. This is
   * not essential but it seems desirable to release the lock as soon as
   * possible.
   */
  UnlockSharedObject(DatabaseRelationId, fparms->src_dboid, 0, ShareLock);

  /* Throw away any successfully copied subdirectories */
  remove_dbtablespaces(fparms->dest_dboid);
}

/*
 * DROP DATABASE
 */
void dropdb(const char *dbname, bool missing_ok, bool force) {
  Oid db_id;
  bool db_istemplate;
  Relation pgdbrel;
  HeapTuple tup;
  ScanKeyData scankey;
  void *inplace_state;
  Form_pg_database datform;
  int notherbackends;
  int npreparedxacts;
  int nsubscriptions;

  /*
   * Look up the target database's OID, and get exclusive lock on it. We
   * need this to ensure that no new backend starts up in the target
   * database while we are deleting it (see postinit.c), and that no one is
   * using it as a CREATE DATABASE template or trying to delete it for
   * themselves.
   */
  pgdbrel = table_open(DatabaseRelationId, RowExclusiveLock);

  if (!get_db_info(dbname, AccessExclusiveLock, &db_id, NULL, NULL,
                   &db_istemplate, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                   NULL, NULL, NULL, NULL)) {
    if (!missing_ok) {
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                      errmsg("database \"%s\" does not exist", dbname)));
    } else {
      /* Close pg_database, release the lock, since we changed nothing */
      table_close(pgdbrel, RowExclusiveLock);
      ereport(NOTICE,
              (errmsg("database \"%s\" does not exist, skipping", dbname)));
      return;
    }
  }

  /*
   * Permission checks
   */
  if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
    aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE, dbname);

  /* DROP hook for the database being removed */
  InvokeObjectDropHook(DatabaseRelationId, db_id, 0);

  /*
   * Disallow dropping a DB that is marked istemplate.  This is just to
   * prevent people from accidentally dropping template0 or template1; they
   * can do so if they're really determined ...
   */
  if (db_istemplate)
    ereport(ERROR, (errcode(ERRCODE_WRONG_OBJECT_TYPE),
                    errmsg("cannot drop a template database")));

  /* Obviously can't drop my own database */
  if (db_id == MyDatabaseId)
    ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE),
                    errmsg("cannot drop the currently open database")));

  /*
   * Check if there are subscriptions defined in the target database.
   *
   * We can't drop them automatically because they might be holding
   * resources in other databases/instances.
   */
  if ((nsubscriptions = CountDBSubscriptions(db_id)) > 0)
    ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE),
                    errmsg("database \"%s\" is being used by logical "
                           "replication subscription",
                           dbname),
                    errdetail_plural("There is %d subscription.",
                                     "There are %d subscriptions.",
                                     nsubscriptions, nsubscriptions)));

  /*
   * Attempt to terminate all existing connections to the target database if
   * the user has requested to do so.
   */
  if (force)
    TerminateOtherDBBackends(db_id);

  /*
   * Check for other backends in the target database.  (Because we hold the
   * database lock, no new ones can start after this.)
   *
   * As in CREATE DATABASE, check this after other error conditions.
   */
  if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_IN_USE),
             errmsg("database \"%s\" is being accessed by other users", dbname),
             errdetail_busy_db(notherbackends, npreparedxacts)));

  /*
   * Delete any comments or security labels associated with the database.
   */
  DeleteSharedComments(db_id, DatabaseRelationId);
  DeleteSharedSecurityLabel(db_id, DatabaseRelationId);

  /*
   * Remove settings associated with this database
   */
  DropSetting(db_id, InvalidOid);

  /*
   * Remove shared dependency references for the database.
   */
  dropDatabaseDependencies(db_id);

  /*
   * Except for the deletion of the catalog row, subsequent actions are not
   * transactional (consider DropDatabaseBuffers() discarding modified
   * buffers). But we might crash or get interrupted below. To prevent
   * accesses to a database with invalid contents, mark the database as
   * invalid using an in-place update.
   */
  ScanKeyInit(&scankey, Anum_pg_database_datname, BTEqualStrategyNumber,
              F_NAMEEQ, CStringGetDatum(dbname));
  systable_inplace_update_begin(pgdbrel, DatabaseNameIndexId, true, NULL, 1,
                                &scankey, &tup, &inplace_state);
  if (!HeapTupleIsValid(tup))
    elog(ERROR, "cache lookup failed for database %u", db_id);
  datform = (Form_pg_database)GETSTRUCT(tup);
  datform->datconnlimit = DATCONNLIMIT_INVALID_DB;
  systable_inplace_update_finish(inplace_state, tup);

  /*
   * Also delete the tuple - transactionally. If this transaction commits,
   * the row will be gone, but if we fail, dropdb() can be invoked again.
   */
  CatalogTupleDelete(pgdbrel, &tup->t_self);
  heap_freetuple(tup);

  /*
   * Drop pages for this database that are in the shared buffer cache. This
   * is important to ensure that no remaining backend tries to write out a
   * dirty buffer to the dead database later...
   */
  DropDatabaseBuffers(db_id);

  /* Close all smgr fds in all backends. */
  WaitForProcSignalBarrier(
      EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

  /*
   * Remove all tablespace subdirs belonging to the database.
   */
  remove_dbtablespaces(db_id);

  /*
   * Close pg_database, but keep lock till commit.
   */
  table_close(pgdbrel, NoLock);
}

/*
 * Rename database
 */
ObjectAddress RenameDatabase(const char *oldname, const char *newname) {
  Oid db_id;
  HeapTuple newtup;
  ItemPointerData otid;
  Relation rel;
  int notherbackends;
  int npreparedxacts;
  ObjectAddress address;

  /*
   * Look up the target database's OID, and get exclusive lock on it. We
   * need this for the same reasons as DROP DATABASE.
   */
  rel = table_open(DatabaseRelationId, RowExclusiveLock);

  if (!get_db_info(oldname, AccessExclusiveLock, &db_id, NULL, NULL, NULL, NULL,
                   NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL))
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database \"%s\" does not exist", oldname)));

  /* must be owner */
  if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
    aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE, oldname);

  /* must have createdb rights */
  if (!have_createdb_privilege())
    ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                    errmsg("permission denied to rename database")));

    /*
     * If built with appropriate switch, whine when regression-testing
     * conventions for database names are violated.
     */
#ifdef ENFORCE_REGRESSION_TEST_NAME_RESTRICTIONS
  if (strstr(newname, "regression") == NULL)
    elog(WARNING, "databases created by regression test cases should have "
                  "names including \"regression\"");
#endif

  /*
   * Make sure the new name doesn't exist.  See notes for same error in
   * CREATE DATABASE.
   */
  if (OidIsValid(get_database_oid(newname, true)))
    ereport(ERROR, (errcode(ERRCODE_DUPLICATE_DATABASE),
                    errmsg("database \"%s\" already exists", newname)));

  /*
   * XXX Client applications probably store the current database somewhere,
   * so renaming it could cause confusion.  On the other hand, there may not
   * be an actual problem besides a little confusion, so think about this
   * and decide.
   */
  if (db_id == MyDatabaseId)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("current database cannot be renamed")));

  /*
   * Make sure the database does not have active sessions.  This is the same
   * concern as above, but applied to other sessions.
   *
   * As in CREATE DATABASE, check this after other error conditions.
   */
  if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
    ereport(ERROR, (errcode(ERRCODE_OBJECT_IN_USE),
                    errmsg("database \"%s\" is being accessed by other users",
                           oldname),
                    errdetail_busy_db(notherbackends, npreparedxacts)));

  /* rename */
  newtup = SearchSysCacheLockedCopy1(DATABASEOID, ObjectIdGetDatum(db_id));
  if (!HeapTupleIsValid(newtup))
    elog(ERROR, "cache lookup failed for database %u", db_id);
  otid = newtup->t_self;
  namestrcpy(&(((Form_pg_database)GETSTRUCT(newtup))->datname), newname);
  CatalogTupleUpdate(rel, &otid, newtup);
  UnlockTuple(rel, &otid, InplaceUpdateTupleLock);

  InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

  ObjectAddressSet(address, DatabaseRelationId, db_id);

  /*
   * Close pg_database, but keep lock till commit.
   */
  table_close(rel, NoLock);

  return address;
}

/*
 * ALTER DATABASE SET TABLESPACE
 */
static void movedb(const char *dbname, const char *tblspcname) {
  Oid db_id;
  Relation pgdbrel;
  int notherbackends;
  int npreparedxacts;
  HeapTuple oldtuple, newtuple;
  Oid src_tblspcoid, dst_tblspcoid;
  ScanKeyData scankey;
  SysScanDesc sysscan;
  AclResult aclresult;
  char *src_dbpath;
  char *dst_dbpath;
  DIR *dstdir;
  struct dirent *xlde;
  movedb_failure_params fparms;

  /*
   * Look up the target database's OID, and get exclusive lock on it. We
   * need this to ensure that no new backend starts up in the database while
   * we are moving it, and that no one is using it as a CREATE DATABASE
   * template or trying to delete it.
   */
  pgdbrel = table_open(DatabaseRelationId, RowExclusiveLock);

  if (!get_db_info(dbname, AccessExclusiveLock, &db_id, NULL, NULL, NULL, NULL,
                   NULL, NULL, NULL, &src_tblspcoid, NULL, NULL, NULL, NULL,
                   NULL, NULL))
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database \"%s\" does not exist", dbname)));

  /*
   * We actually need a session lock, so that the lock will persist across
   * the commit/restart below.  (We could almost get away with letting the
   * lock be released at commit, except that someone could try to move
   * relations of the DB back into the old directory while we rmtree() it.)
   */
  LockSharedObjectForSession(DatabaseRelationId, db_id, 0, AccessExclusiveLock);

  /*
   * Permission checks
   */
  if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
    aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE, dbname);

  /*
   * Obviously can't move the tables of my own database
   */
  if (db_id == MyDatabaseId)
    ereport(
        ERROR,
        (errcode(ERRCODE_OBJECT_IN_USE),
         errmsg(
             "cannot change the tablespace of the currently open database")));

  /*
   * Get tablespace's oid
   */
  dst_tblspcoid = get_tablespace_oid(tblspcname, false);

  /*
   * Permission checks
   */
  aclresult = object_aclcheck(TableSpaceRelationId, dst_tblspcoid, GetUserId(),
                              ACL_CREATE);
  if (aclresult != ACLCHECK_OK)
    aclcheck_error(aclresult, OBJECT_TABLESPACE, tblspcname);

  /*
   * pg_global must never be the default tablespace
   */
  if (dst_tblspcoid == GLOBALTABLESPACE_OID)
    ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                    errmsg("pg_global cannot be used as default tablespace")));

  /*
   * No-op if same tablespace
   */
  if (src_tblspcoid == dst_tblspcoid) {
    table_close(pgdbrel, NoLock);
    UnlockSharedObjectForSession(DatabaseRelationId, db_id, 0,
                                 AccessExclusiveLock);
    return;
  }

  /*
   * Check for other backends in the target database.  (Because we hold the
   * database lock, no new ones can start after this.)
   *
   * As in CREATE DATABASE, check this after other error conditions.
   */
  if (CountOtherDBBackends(db_id, &notherbackends, &npreparedxacts))
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_IN_USE),
             errmsg("database \"%s\" is being accessed by other users", dbname),
             errdetail_busy_db(notherbackends, npreparedxacts)));

  /*
   * Get old and new database paths
   */
  src_dbpath = GetDatabasePath(db_id, src_tblspcoid);
  dst_dbpath = GetDatabasePath(db_id, dst_tblspcoid);

  /*
   * Write the database's dirty buffers out before copying its files.  There
   * is no checkpoint to do it.
   */
  FlushDatabaseBuffers(db_id);

  /* Close all smgr fds in all backends. */
  WaitForProcSignalBarrier(
      EmitProcSignalBarrier(PROCSIGNAL_BARRIER_SMGRRELEASE));

  /*
   * Now drop all buffers holding data of the target database; they should
   * no longer be dirty so DropDatabaseBuffers is safe.
   *
   * It might seem that we could just let these buffers age out of shared
   * buffers naturally, since they should not get referenced anymore.  The
   * problem with that is that if the user later moves the database back to
   * its original tablespace, any still-surviving buffers would appear to
   * contain valid data again --- but they'd be missing any changes made in
   * the database while it was in the new tablespace.  In any case, freeing
   * buffers that should never be used again seems worth the cycles.
   *
   * Note: it'd be sufficient to get rid of buffers matching db_id and
   * src_tblspcoid, but bufmgr.c presently provides no API for that.
   */
  DropDatabaseBuffers(db_id);

  /*
   * Check for existence of files in the target directory, i.e., objects of
   * this database that are already in the target tablespace.  We can't
   * allow the move in such a case, because we would need to change those
   * relations' pg_class.reltablespace entries to zero, and we don't have
   * access to the DB's pg_class to do so.
   */
  dstdir = AllocateDir(dst_dbpath);
  if (dstdir != NULL) {
    while ((xlde = ReadDir(dstdir, dst_dbpath)) != NULL) {
      if (strcmp(xlde->d_name, ".") == 0 || strcmp(xlde->d_name, "..") == 0)
        continue;

      ereport(ERROR,
              (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
               errmsg("some relations of database \"%s\" are already in "
                      "tablespace \"%s\"",
                      dbname, tblspcname),
               errhint("You must move them back to the database's default "
                       "tablespace before using this command.")));
    }

    FreeDir(dstdir);

    /*
     * The directory exists but is empty. We must remove it before using
     * the copydir function.
     */
    if (rmdir(dst_dbpath) != 0)
      elog(ERROR, "could not remove directory \"%s\": %m", dst_dbpath);
  }

  /*
   * Use an ENSURE block to make sure we remove the debris if the copy fails
   * (eg, due to out-of-disk-space).  This is not a 100% solution, because
   * of the possibility of failure during transaction commit, but it should
   * handle most scenarios.
   */
  fparms.dest_dboid = db_id;
  fparms.dest_tsoid = dst_tblspcoid;
  PG_ENSURE_ERROR_CLEANUP(movedb_failure_callback, PointerGetDatum(&fparms));
  {
    Datum new_record[Natts_pg_database] = {0};
    bool new_record_nulls[Natts_pg_database] = {0};
    bool new_record_repl[Natts_pg_database] = {0};

    /*
     * Copy files from the old tablespace to the new one
     */
    copydir(src_dbpath, dst_dbpath, false);

    /*
     * Update the database's pg_database tuple
     */
    ScanKeyInit(&scankey, Anum_pg_database_datname, BTEqualStrategyNumber,
                F_NAMEEQ, CStringGetDatum(dbname));
    sysscan = systable_beginscan(pgdbrel, DatabaseNameIndexId, true, NULL, 1,
                                 &scankey);
    oldtuple = systable_getnext(sysscan);
    if (!HeapTupleIsValid(oldtuple)) /* shouldn't happen... */
      ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                      errmsg("database \"%s\" does not exist", dbname)));
    LockTuple(pgdbrel, &oldtuple->t_self, InplaceUpdateTupleLock);

    new_record[Anum_pg_database_dattablespace - 1] =
        ObjectIdGetDatum(dst_tblspcoid);
    new_record_repl[Anum_pg_database_dattablespace - 1] = true;

    newtuple = heap_modify_tuple(oldtuple, RelationGetDescr(pgdbrel),
                                 new_record, new_record_nulls, new_record_repl);
    CatalogTupleUpdate(pgdbrel, &oldtuple->t_self, newtuple);
    UnlockTuple(pgdbrel, &oldtuple->t_self, InplaceUpdateTupleLock);

    InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

    systable_endscan(sysscan);

    /*
     * Close pg_database, but keep lock till commit.
     */
    table_close(pgdbrel, NoLock);
  }
  PG_END_ENSURE_ERROR_CLEANUP(movedb_failure_callback,
                              PointerGetDatum(&fparms));

  /*
   * Commit the transaction so that the pg_database update is committed. If
   * we crash while removing files, the database won't be corrupt, we'll
   * just leave some orphaned files in the old directory.
   *
   * (This is OK because we know we aren't inside a transaction block.)
   *
   * XXX would it be safe/better to do this inside the ensure block?	Not
   * convinced it's a good idea; consider elog just after the transaction
   * really commits.
   */
  PopActiveSnapshot();
  CommitTransactionCommand();

  /* Start new transaction for the remaining work; don't need a snapshot */
  StartTransactionCommand();

  /*
   * Remove files from the old tablespace
   */
  if (!rmtree(src_dbpath, true))
    ereport(WARNING, (errmsg("some useless files may be left behind in old "
                             "database directory \"%s\"",
                             src_dbpath)));

  /* Now it's safe to release the database lock */
  UnlockSharedObjectForSession(DatabaseRelationId, db_id, 0,
                               AccessExclusiveLock);

  pfree(src_dbpath);
  pfree(dst_dbpath);
}

/* Error cleanup callback for movedb */
static void movedb_failure_callback(int code, Datum arg) {
  movedb_failure_params *fparms = (movedb_failure_params *)DatumGetPointer(arg);
  char *dstpath;

  /* Get rid of anything we managed to copy to the target directory */
  dstpath = GetDatabasePath(fparms->dest_dboid, fparms->dest_tsoid);

  (void)rmtree(dstpath, true);

  pfree(dstpath);
}

/*
 * Process options and call dropdb function.
 */
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

/*
 * ALTER DATABASE name ...
 */
Oid AlterDatabase(ParseState *pstate, AlterDatabaseStmt *stmt,
                  bool isTopLevel) {
  Relation rel;
  Oid dboid;
  HeapTuple tuple, newtuple;
  Form_pg_database datform;
  ScanKeyData scankey;
  SysScanDesc scan;
  ListCell *option;
  bool dbistemplate = false;
  bool dballowconnections = true;
  int dbconnlimit = DATCONNLIMIT_UNLIMITED;
  DefElem *distemplate = NULL;
  DefElem *dallowconnections = NULL;
  DefElem *dconnlimit = NULL;
  DefElem *dtablespace = NULL;
  Datum new_record[Natts_pg_database] = {0};
  bool new_record_nulls[Natts_pg_database] = {0};
  bool new_record_repl[Natts_pg_database] = {0};

  /* Extract options from the statement node tree */
  foreach (option, stmt->options) {
    DefElem *defel = (DefElem *)lfirst(option);

    if (strcmp(defel->defname, "is_template") == 0) {
      if (distemplate)
        errorConflictingDefElem(defel, pstate);
      distemplate = defel;
    } else if (strcmp(defel->defname, "allow_connections") == 0) {
      if (dallowconnections)
        errorConflictingDefElem(defel, pstate);
      dallowconnections = defel;
    } else if (strcmp(defel->defname, "connection_limit") == 0) {
      if (dconnlimit)
        errorConflictingDefElem(defel, pstate);
      dconnlimit = defel;
    } else if (strcmp(defel->defname, "tablespace") == 0) {
      if (dtablespace)
        errorConflictingDefElem(defel, pstate);
      dtablespace = defel;
    } else
      ereport(ERROR, (errcode(ERRCODE_SYNTAX_ERROR),
                      errmsg("option \"%s\" not recognized", defel->defname),
                      parser_errposition(pstate, defel->location)));
  }

  if (dtablespace) {
    /*
     * While the SET TABLESPACE syntax doesn't allow any other options,
     * somebody could write "WITH TABLESPACE ...".  Forbid any other
     * options from being specified in that case.
     */
    if (list_length(stmt->options) != 1)
      ereport(ERROR,
              (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
               errmsg("option \"%s\" cannot be specified with other options",
                      dtablespace->defname),
               parser_errposition(pstate, dtablespace->location)));
    /* this case isn't allowed within a transaction block */
    PreventInTransactionBlock(isTopLevel, "ALTER DATABASE SET TABLESPACE");
    movedb(stmt->dbname, defGetString(dtablespace));
    return InvalidOid;
  }

  if (distemplate && distemplate->arg)
    dbistemplate = defGetBoolean(distemplate);
  if (dallowconnections && dallowconnections->arg)
    dballowconnections = defGetBoolean(dallowconnections);
  if (dconnlimit && dconnlimit->arg) {
    dbconnlimit = defGetInt32(dconnlimit);
    if (dbconnlimit < DATCONNLIMIT_UNLIMITED)
      ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
                      errmsg("invalid connection limit: %d", dbconnlimit)));
  }

  /*
   * Get the old tuple.  We don't need a lock on the database per se,
   * because we're not going to do anything that would mess up incoming
   * connections.
   */
  rel = table_open(DatabaseRelationId, RowExclusiveLock);
  ScanKeyInit(&scankey, Anum_pg_database_datname, BTEqualStrategyNumber,
              F_NAMEEQ, CStringGetDatum(stmt->dbname));
  scan = systable_beginscan(rel, DatabaseNameIndexId, true, NULL, 1, &scankey);
  tuple = systable_getnext(scan);
  if (!HeapTupleIsValid(tuple))
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database \"%s\" does not exist", stmt->dbname)));
  LockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

  datform = (Form_pg_database)GETSTRUCT(tuple);
  dboid = datform->oid;

  if (database_is_invalid_form(datform)) {
    ereport(FATAL, errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
            errmsg("cannot alter invalid database \"%s\"", stmt->dbname),
            errhint("Use DROP DATABASE to drop invalid databases."));
  }

  if (!object_ownercheck(DatabaseRelationId, dboid, GetUserId()))
    aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE, stmt->dbname);

  /*
   * In order to avoid getting locked out and having to go through
   * standalone mode, we refuse to disallow connections to the database
   * we're currently connected to.  Lockout can still happen with concurrent
   * sessions but the likeliness of that is not high enough to worry about.
   */
  if (!dballowconnections && dboid == MyDatabaseId)
    ereport(ERROR,
            (errcode(ERRCODE_INVALID_PARAMETER_VALUE),
             errmsg("cannot disallow connections for current database")));

  /*
   * Build an updated tuple, perusing the information just obtained
   */
  if (distemplate) {
    new_record[Anum_pg_database_datistemplate - 1] = BoolGetDatum(dbistemplate);
    new_record_repl[Anum_pg_database_datistemplate - 1] = true;
  }
  if (dallowconnections) {
    new_record[Anum_pg_database_datallowconn - 1] =
        BoolGetDatum(dballowconnections);
    new_record_repl[Anum_pg_database_datallowconn - 1] = true;
  }
  if (dconnlimit) {
    new_record[Anum_pg_database_datconnlimit - 1] = Int32GetDatum(dbconnlimit);
    new_record_repl[Anum_pg_database_datconnlimit - 1] = true;
  }

  newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel), new_record,
                               new_record_nulls, new_record_repl);
  CatalogTupleUpdate(rel, &tuple->t_self, newtuple);
  UnlockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

  InvokeObjectPostAlterHook(DatabaseRelationId, dboid, 0);

  systable_endscan(scan);

  /* Close pg_database, but keep lock till commit */
  table_close(rel, NoLock);

  return dboid;
}

/*
 * ALTER DATABASE name REFRESH COLLATION VERSION
 */
ObjectAddress AlterDatabaseRefreshColl(AlterDatabaseRefreshCollStmt *stmt) {
  Relation rel;
  ScanKeyData scankey;
  SysScanDesc scan;
  Oid db_id;
  HeapTuple tuple;
  Form_pg_database datForm;
  ObjectAddress address;
  Datum datum;
  bool isnull;
  char *oldversion;
  char *newversion;

  rel = table_open(DatabaseRelationId, RowExclusiveLock);
  ScanKeyInit(&scankey, Anum_pg_database_datname, BTEqualStrategyNumber,
              F_NAMEEQ, CStringGetDatum(stmt->dbname));
  scan = systable_beginscan(rel, DatabaseNameIndexId, true, NULL, 1, &scankey);
  tuple = systable_getnext(scan);
  if (!HeapTupleIsValid(tuple))
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database \"%s\" does not exist", stmt->dbname)));

  datForm = (Form_pg_database)GETSTRUCT(tuple);
  db_id = datForm->oid;

  if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
    aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE, stmt->dbname);
  LockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

  datum = heap_getattr(tuple, Anum_pg_database_datcollversion,
                       RelationGetDescr(rel), &isnull);
  oldversion = isnull ? NULL : TextDatumGetCString(datum);

  if (datForm->datlocprovider == COLLPROVIDER_LIBC) {
    datum = heap_getattr(tuple, Anum_pg_database_datcollate,
                         RelationGetDescr(rel), &isnull);
    if (isnull)
      elog(ERROR, "unexpected null in pg_database");
  } else {
    datum = heap_getattr(tuple, Anum_pg_database_datlocale,
                         RelationGetDescr(rel), &isnull);
    if (isnull)
      elog(ERROR, "unexpected null in pg_database");
  }

  newversion = get_collation_actual_version(datForm->datlocprovider,
                                            TextDatumGetCString(datum));

  /* cannot change from NULL to non-NULL or vice versa */
  if ((!oldversion && newversion) || (oldversion && !newversion))
    elog(ERROR, "invalid collation version change");
  else if (oldversion && newversion && strcmp(newversion, oldversion) != 0) {
    bool nulls[Natts_pg_database] = {0};
    bool replaces[Natts_pg_database] = {0};
    Datum values[Natts_pg_database] = {0};
    HeapTuple newtuple;

    ereport(NOTICE,
            (errmsg("changing version from %s to %s", oldversion, newversion)));

    values[Anum_pg_database_datcollversion - 1] =
        CStringGetTextDatum(newversion);
    replaces[Anum_pg_database_datcollversion - 1] = true;

    newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel), values, nulls,
                                 replaces);
    CatalogTupleUpdate(rel, &tuple->t_self, newtuple);
    heap_freetuple(newtuple);
  } else
    ereport(NOTICE, (errmsg("version has not changed")));
  UnlockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

  InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

  ObjectAddressSet(address, DatabaseRelationId, db_id);

  systable_endscan(scan);

  table_close(rel, NoLock);

  return address;
}

/*
 * ALTER DATABASE name SET ...
 */
Oid AlterDatabaseSet(AlterDatabaseSetStmt *stmt) {
  Oid datid = get_database_oid(stmt->dbname, false);

  /*
   * Obtain a lock on the database and make sure it didn't go away in the
   * meantime.
   */
  shdepLockAndCheckObject(DatabaseRelationId, datid);

  if (!object_ownercheck(DatabaseRelationId, datid, GetUserId()))
    aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE, stmt->dbname);

  AlterSetting(datid, InvalidOid, stmt->setstmt);

  UnlockSharedObject(DatabaseRelationId, datid, 0, AccessShareLock);

  return datid;
}

/*
 * ALTER DATABASE name OWNER TO newowner
 */
ObjectAddress AlterDatabaseOwner(const char *dbname, Oid newOwnerId) {
  Oid db_id;
  HeapTuple tuple;
  Relation rel;
  ScanKeyData scankey;
  SysScanDesc scan;
  Form_pg_database datForm;
  ObjectAddress address;

  /*
   * Get the old tuple.  We don't need a lock on the database per se,
   * because we're not going to do anything that would mess up incoming
   * connections.
   */
  rel = table_open(DatabaseRelationId, RowExclusiveLock);
  ScanKeyInit(&scankey, Anum_pg_database_datname, BTEqualStrategyNumber,
              F_NAMEEQ, CStringGetDatum(dbname));
  scan = systable_beginscan(rel, DatabaseNameIndexId, true, NULL, 1, &scankey);
  tuple = systable_getnext(scan);
  if (!HeapTupleIsValid(tuple))
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database \"%s\" does not exist", dbname)));

  datForm = (Form_pg_database)GETSTRUCT(tuple);
  db_id = datForm->oid;

  /*
   * If the new owner is the same as the existing owner, consider the
   * command to have succeeded.  This is to be consistent with other
   * objects.
   */
  if (datForm->datdba != newOwnerId) {
    Datum repl_val[Natts_pg_database];
    bool repl_null[Natts_pg_database] = {0};
    bool repl_repl[Natts_pg_database] = {0};
    Acl *newAcl;
    Datum aclDatum;
    bool isNull;
    HeapTuple newtuple;

    /* Otherwise, must be owner of the existing object */
    if (!object_ownercheck(DatabaseRelationId, db_id, GetUserId()))
      aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_DATABASE, dbname);

    /* Must be able to become new owner */
    check_can_set_role(GetUserId(), newOwnerId);

    /*
     * must have createdb rights
     *
     * NOTE: This is different from other alter-owner checks in that the
     * current user is checked for createdb privileges instead of the
     * destination owner.  This is consistent with the CREATE case for
     * databases.  Because superusers will always have this right, we need
     * no special case for them.
     */
    if (!have_createdb_privilege())
      ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
                      errmsg("permission denied to change owner of database")));

    LockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

    repl_repl[Anum_pg_database_datdba - 1] = true;
    repl_val[Anum_pg_database_datdba - 1] = ObjectIdGetDatum(newOwnerId);

    /*
     * Determine the modified ACL for the new owner.  This is only
     * necessary when the ACL is non-null.
     */
    aclDatum = heap_getattr(tuple, Anum_pg_database_datacl,
                            RelationGetDescr(rel), &isNull);
    if (!isNull) {
      newAcl = aclnewowner(DatumGetAclP(aclDatum), datForm->datdba, newOwnerId);
      repl_repl[Anum_pg_database_datacl - 1] = true;
      repl_val[Anum_pg_database_datacl - 1] = PointerGetDatum(newAcl);
    }

    newtuple = heap_modify_tuple(tuple, RelationGetDescr(rel), repl_val,
                                 repl_null, repl_repl);
    CatalogTupleUpdate(rel, &newtuple->t_self, newtuple);
    UnlockTuple(rel, &tuple->t_self, InplaceUpdateTupleLock);

    heap_freetuple(newtuple);

    /* Update owner dependency reference */
    changeDependencyOnOwner(DatabaseRelationId, db_id, newOwnerId);
  }

  InvokeObjectPostAlterHook(DatabaseRelationId, db_id, 0);

  ObjectAddressSet(address, DatabaseRelationId, db_id);

  systable_endscan(scan);

  /* Close pg_database, but keep lock till commit */
  table_close(rel, NoLock);

  return address;
}

Datum pg_database_collation_actual_version(PG_FUNCTION_ARGS) {
  Oid dbid = PG_GETARG_OID(0);
  HeapTuple tp;
  char datlocprovider;
  Datum datum;
  char *version;

  tp = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
  if (!HeapTupleIsValid(tp))
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT),
                    errmsg("database with OID %u does not exist", dbid)));

  datlocprovider = ((Form_pg_database)GETSTRUCT(tp))->datlocprovider;

  if (datlocprovider == COLLPROVIDER_LIBC)
    datum =
        SysCacheGetAttrNotNull(DATABASEOID, tp, Anum_pg_database_datcollate);
  else
    datum = SysCacheGetAttrNotNull(DATABASEOID, tp, Anum_pg_database_datlocale);

  version =
      get_collation_actual_version(datlocprovider, TextDatumGetCString(datum));

  ReleaseSysCache(tp);

  if (version)
    PG_RETURN_TEXT_P(cstring_to_text(version));
  else
    PG_RETURN_NULL();
}

/*
 * Helper functions
 */

/*
 * Look up info about the database named "name".  If the database exists,
 * obtain the specified lock type on it, fill in any of the remaining
 * parameters that aren't NULL, and return true.  If no such database,
 * return false.
 */
static bool get_db_info(const char *name, LOCKMODE lockmode, Oid *dbIdP,
                        Oid *ownerIdP, int *encodingP, bool *dbIsTemplateP,
                        bool *dbAllowConnP, bool *dbHasLoginEvtP,
                        TransactionId *dbFrozenXidP, MultiXactId *dbMinMultiP,
                        Oid *dbTablespace, char **dbCollate, char **dbCtype,
                        char **dbLocale, char **dbIcurules, char *dbLocProvider,
                        char **dbCollversion) {
  bool result = false;
  Relation relation;

  Assert(name);

  /* Caller may wish to grab a better lock on pg_database beforehand... */
  relation = table_open(DatabaseRelationId, AccessShareLock);

  /*
   * Loop covers the rare case where the database is renamed before we can
   * lock it.  We try again just in case we can find a new one of the same
   * name.
   */
  for (;;) {
    ScanKeyData scanKey;
    SysScanDesc scan;
    HeapTuple tuple;
    Oid dbOid;

    /*
     * there's no syscache for database-indexed-by-name, so must do it the
     * hard way
     */
    ScanKeyInit(&scanKey, Anum_pg_database_datname, BTEqualStrategyNumber,
                F_NAMEEQ, CStringGetDatum(name));

    scan = systable_beginscan(relation, DatabaseNameIndexId, true, NULL, 1,
                              &scanKey);

    tuple = systable_getnext(scan);

    if (!HeapTupleIsValid(tuple)) {
      /* definitely no database of that name */
      systable_endscan(scan);
      break;
    }

    dbOid = ((Form_pg_database)GETSTRUCT(tuple))->oid;

    systable_endscan(scan);

    /*
     * Now that we have a database OID, we can try to lock the DB.
     */
    if (lockmode != NoLock)
      LockSharedObject(DatabaseRelationId, dbOid, 0, lockmode);

    /*
     * And now, re-fetch the tuple by OID.  If it's still there and still
     * the same name, we win; else, drop the lock and loop back to try
     * again.
     */
    tuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbOid));
    if (HeapTupleIsValid(tuple)) {
      Form_pg_database dbform = (Form_pg_database)GETSTRUCT(tuple);

      if (strcmp(name, NameStr(dbform->datname)) == 0) {
        Datum datum;
        bool isnull;

        /* oid of the database */
        if (dbIdP)
          *dbIdP = dbOid;
        /* oid of the owner */
        if (ownerIdP)
          *ownerIdP = dbform->datdba;
        /* character encoding */
        if (encodingP)
          *encodingP = dbform->encoding;
        /* allowed as template? */
        if (dbIsTemplateP)
          *dbIsTemplateP = dbform->datistemplate;
        /* Has on login event trigger? */
        if (dbHasLoginEvtP)
          *dbHasLoginEvtP = dbform->dathasloginevt;
        /* allowing connections? */
        if (dbAllowConnP)
          *dbAllowConnP = dbform->datallowconn;
        /* limit of frozen XIDs */
        if (dbFrozenXidP)
          *dbFrozenXidP = dbform->datfrozenxid;
        /* minimum MultiXactId */
        if (dbMinMultiP)
          *dbMinMultiP = dbform->datminmxid;
        /* default tablespace for this database */
        if (dbTablespace)
          *dbTablespace = dbform->dattablespace;
        /* default locale settings for this database */
        if (dbLocProvider)
          *dbLocProvider = dbform->datlocprovider;
        if (dbCollate) {
          datum = SysCacheGetAttrNotNull(DATABASEOID, tuple,
                                         Anum_pg_database_datcollate);
          *dbCollate = TextDatumGetCString(datum);
        }
        if (dbCtype) {
          datum = SysCacheGetAttrNotNull(DATABASEOID, tuple,
                                         Anum_pg_database_datctype);
          *dbCtype = TextDatumGetCString(datum);
        }
        if (dbLocale) {
          datum = SysCacheGetAttr(DATABASEOID, tuple,
                                  Anum_pg_database_datlocale, &isnull);
          if (isnull)
            *dbLocale = NULL;
          else
            *dbLocale = TextDatumGetCString(datum);
        }
        if (dbIcurules) {
          datum = SysCacheGetAttr(DATABASEOID, tuple,
                                  Anum_pg_database_daticurules, &isnull);
          if (isnull)
            *dbIcurules = NULL;
          else
            *dbIcurules = TextDatumGetCString(datum);
        }
        if (dbCollversion) {
          datum = SysCacheGetAttr(DATABASEOID, tuple,
                                  Anum_pg_database_datcollversion, &isnull);
          if (isnull)
            *dbCollversion = NULL;
          else
            *dbCollversion = TextDatumGetCString(datum);
        }
        ReleaseSysCache(tuple);
        result = true;
        break;
      }
      /* can only get here if it was just renamed */
      ReleaseSysCache(tuple);
    }

    if (lockmode != NoLock)
      UnlockSharedObject(DatabaseRelationId, dbOid, 0, lockmode);
  }

  table_close(relation, AccessShareLock);

  return result;
}

/* Check if current user has createdb privileges */
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
 * Remove tablespace directories
 *
 * We don't know what tablespaces db_id is using, so iterate through all
 * tablespaces removing <tablespace>/db_id
 */
static void remove_dbtablespaces(Oid db_id) {
  Relation rel;
  TableScanDesc scan;
  HeapTuple tuple;

  rel = table_open(TableSpaceRelationId, AccessShareLock);
  scan = table_beginscan_catalog(rel, 0, NULL);
  while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
    Form_pg_tablespace spcform = (Form_pg_tablespace)GETSTRUCT(tuple);
    Oid dsttablespace = spcform->oid;
    char *dstpath;
    struct stat st;

    /* Don't mess with the global tablespace */
    if (dsttablespace == GLOBALTABLESPACE_OID)
      continue;

    dstpath = GetDatabasePath(db_id, dsttablespace);

    if (lstat(dstpath, &st) < 0 || !S_ISDIR(st.st_mode)) {
      /* Assume we can ignore it */
      pfree(dstpath);
      continue;
    }

    if (!rmtree(dstpath, true))
      ereport(WARNING, (errmsg("some useless files may be left behind in old "
                               "database directory \"%s\"",
                               dstpath)));

    pfree(dstpath);
  }

  table_endscan(scan);
  table_close(rel, AccessShareLock);
}

/*
 * Check for existing files that conflict with a proposed new DB OID;
 * return true if there are any
 *
 * If there were a subdirectory in any tablespace matching the proposed new
 * OID, we'd get a create failure due to the duplicate name ... and then we'd
 * try to remove that already-existing subdirectory during the cleanup in
 * remove_dbtablespaces.  Nuking existing files seems like a bad idea, so
 * instead we make this extra check before settling on the OID of the new
 * database.  This exactly parallels what GetNewRelFileNumber() does for table
 * relfilenumber values.
 */
static bool check_db_file_conflict(Oid db_id) {
  bool result = false;
  Relation rel;
  TableScanDesc scan;
  HeapTuple tuple;

  rel = table_open(TableSpaceRelationId, AccessShareLock);
  scan = table_beginscan_catalog(rel, 0, NULL);
  while ((tuple = heap_getnext(scan, ForwardScanDirection)) != NULL) {
    Form_pg_tablespace spcform = (Form_pg_tablespace)GETSTRUCT(tuple);
    Oid dsttablespace = spcform->oid;
    char *dstpath;
    struct stat st;

    /* Don't mess with the global tablespace */
    if (dsttablespace == GLOBALTABLESPACE_OID)
      continue;

    dstpath = GetDatabasePath(db_id, dsttablespace);

    if (lstat(dstpath, &st) == 0) {
      /* Found a conflicting file (or directory, whatever) */
      pfree(dstpath);
      result = true;
      break;
    }

    pfree(dstpath);
  }

  table_endscan(scan);
  table_close(rel, AccessShareLock);

  return result;
}

/*
 * Issue a suitable errdetail message for a busy database
 */
static int errdetail_busy_db(int notherbackends, int npreparedxacts) {
  if (notherbackends > 0 && npreparedxacts > 0)

    /*
     * We don't deal with singular versus plural here, since gettext
     * doesn't support multiple plurals in one string.
     */
    errdetail("There are %d other session(s) and %d prepared transaction(s) "
              "using the database.",
              notherbackends, npreparedxacts);
  else if (notherbackends > 0)
    errdetail_plural("There is %d other session using the database.",
                     "There are %d other sessions using the database.",
                     notherbackends, notherbackends);
  else
    errdetail_plural("There is %d prepared transaction using the database.",
                     "There are %d prepared transactions using the database.",
                     npreparedxacts, npreparedxacts);
  return 0; /* just to keep ereport macro happy */
}

/*
 * get_database_oid - given a database name, look up the OID
 *
 * If missing_ok is false, throw an error if database name not found.  If
 * true, just return InvalidOid.
 */
Oid get_database_oid(const char *dbname, bool missing_ok) {
  Relation pg_database;
  ScanKeyData entry[1];
  SysScanDesc scan;
  HeapTuple dbtuple;
  Oid oid;

  /*
   * There's no syscache for pg_database indexed by name, so we must look
   * the hard way.
   */
  pg_database = table_open(DatabaseRelationId, AccessShareLock);
  ScanKeyInit(&entry[0], Anum_pg_database_datname, BTEqualStrategyNumber,
              F_NAMEEQ, CStringGetDatum(dbname));
  scan = systable_beginscan(pg_database, DatabaseNameIndexId, true, NULL, 1,
                            entry);

  dbtuple = systable_getnext(scan);

  /* We assume that there can be at most one matching tuple */
  if (HeapTupleIsValid(dbtuple))
    oid = ((Form_pg_database)GETSTRUCT(dbtuple))->oid;
  else
    oid = InvalidOid;

  systable_endscan(scan);
  table_close(pg_database, AccessShareLock);

  if (!OidIsValid(oid) && !missing_ok)
    ereport(ERROR, (errcode(ERRCODE_UNDEFINED_DATABASE),
                    errmsg("database \"%s\" does not exist", dbname)));

  return oid;
}

/*
 * get_database_name - given a database OID, look up the name
 *
 * Returns a palloc'd string, or NULL if no such database.
 */
char *get_database_name(Oid dbid) {
  HeapTuple dbtuple;
  char *result;

  dbtuple = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dbid));
  if (HeapTupleIsValid(dbtuple)) {
    result = pstrdup(NameStr(((Form_pg_database)GETSTRUCT(dbtuple))->datname));
    ReleaseSysCache(dbtuple);
  } else
    result = NULL;

  return result;
}

/*
 * While dropping a database the pg_database row is marked invalid, but the
 * catalog contents still exist. Connections to such a database are not
 * allowed.
 */
bool database_is_invalid_form(Form_pg_database datform) {
  return datform->datconnlimit == DATCONNLIMIT_INVALID_DB;
}

/*
 * Convenience wrapper around database_is_invalid_form()
 */
bool database_is_invalid_oid(Oid dboid) {
  HeapTuple dbtup;
  Form_pg_database dbform;
  bool invalid;

  dbtup = SearchSysCache1(DATABASEOID, ObjectIdGetDatum(dboid));
  if (!HeapTupleIsValid(dbtup))
    elog(ERROR, "cache lookup failed for database %u", dboid);
  dbform = (Form_pg_database)GETSTRUCT(dbtup);

  invalid = database_is_invalid_form(dbform);

  ReleaseSysCache(dbtup);

  return invalid;
}

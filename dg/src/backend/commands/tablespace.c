/*-------------------------------------------------------------------------
 *
 * tablespace.c
 *	  What is left of tablespaces: the two built in, by name and oid.
 *
 * digres has no tablespaces of its own: a table is sm's, and sm keeps its
 * files.  CREATE, ALTER and DROP TABLESPACE are refused.  A statement may
 * still name pg_default, where every relation is, or pg_global.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/tablespace.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "catalog/pg_tablespace.h"
#include "commands/tablespace.h"

/*
 * The oid of the tablespace named tablespacename: pg_default's or
 * pg_global's.  Any other name is an error, or InvalidOid if missing_ok.
 */
Oid get_tablespace_oid(const char *tablespacename, bool missing_ok) {
  if (strcmp(tablespacename, "pg_default") == 0)
    return DEFAULTTABLESPACE_OID;
  if (strcmp(tablespacename, "pg_global") == 0)
    return GLOBALTABLESPACE_OID;
  if (!missing_ok)
    ereport(ERROR,
            (errcode(ERRCODE_UNDEFINED_OBJECT),
             errmsg("tablespace \"%s\" does not exist", tablespacename)));
  return InvalidOid;
}

/* The name of the tablespace spc_oid, palloc'd, or NULL if there is none. */
char *get_tablespace_name(Oid spc_oid) {
  if (spc_oid == DEFAULTTABLESPACE_OID)
    return pstrdup("pg_default");
  if (spc_oid == GLOBALTABLESPACE_OID)
    return pstrdup("pg_global");
  return NULL;
}

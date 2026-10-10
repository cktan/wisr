/*-------------------------------------------------------------------------
 *
 * tablespace.h
 *		Tablespace management commands (create/drop tablespace).
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/commands/tablespace.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLESPACE_H
#define TABLESPACE_H

#include "catalog/objectaddress.h"
#include "lib/stringinfo.h"
#include "nodes/parsenodes.h"

extern thread_local /*LOCAL*/ PGDLLIMPORT char *default_tablespace;
extern thread_local /*LOCAL*/ PGDLLIMPORT char *temp_tablespaces;
extern thread_local /*LOCAL*/ PGDLLIMPORT bool allow_in_place_tablespaces;

typedef struct TableSpaceOpts {
  int32 vl_len_; /* varlena header (do not touch directly!) */
  float8 random_page_cost;
  float8 seq_page_cost;
  int effective_io_concurrency;
  int maintenance_io_concurrency;
} TableSpaceOpts;

extern Oid CreateTableSpace(CreateTableSpaceStmt *stmt);
extern void DropTableSpace(DropTableSpaceStmt *stmt);
extern ObjectAddress RenameTableSpace(const char *oldname, const char *newname);
extern Oid AlterTableSpaceOptions(AlterTableSpaceOptionsStmt *stmt);

extern void TablespaceCreateDbspace(Oid spcOid, Oid dbOid, bool isRedo);

extern Oid GetDefaultTablespace(char relpersistence, bool partitioned);

extern void PrepareTempTablespaces(void);

extern Oid get_tablespace_oid(const char *tablespacename, bool missing_ok);
extern char *get_tablespace_name(Oid spc_oid);

extern bool directory_is_empty(const char *path);

#endif /* TABLESPACE_H */

/*-------------------------------------------------------------------------
 *
 * utility.h
 *	  prototypes for utility.c.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/tcop/utility.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef UTILITY_H
#define UTILITY_H

#include "tcop/cmdtag.h"
#include "tcop/tcopprot.h"

typedef enum {
  PROCESS_UTILITY_TOPLEVEL,        /* toplevel interactive command */
  PROCESS_UTILITY_QUERY,           /* a complete query, but not toplevel */
  PROCESS_UTILITY_QUERY_NONATOMIC, /* a complete query, nonatomic
                                    * execution context */
  PROCESS_UTILITY_SUBCOMMAND,      /* a portion of a query */
} ProcessUtilityContext;

/* Info needed when recursing from ALTER TABLE */
typedef struct AlterTableUtilityContext {
  PlannedStmt *pstmt;         /* PlannedStmt for outer ALTER TABLE command */
  const char *queryString;    /* its query string */
  Oid relid;                  /* OID of ALTER's target table */
  ParamListInfo params;       /* any parameters available to ALTER TABLE */
  QueryEnvironment *queryEnv; /* execution environment for ALTER TABLE */
} AlterTableUtilityContext;

/*
 * These constants are used to describe the extent to which a particular
 * command is read-only.
 *
 * COMMAND_OK_IN_READ_ONLY_TXN means that the command is permissible even when
 * XactReadOnly is set. This bit should be set for commands that don't change
 * the state of the database (data or schema) in a way that would affect the
 * output of pg_dump.
 */
#define COMMAND_OK_IN_READ_ONLY_TXN	0x0001

/*
 * We say that a command is strictly read-only if it is sufficiently read-only
 * for all purposes. For clarity, we also have a constant for commands that are
 * in no way read-only.
 */
#define COMMAND_IS_STRICTLY_READ_ONLY	COMMAND_OK_IN_READ_ONLY_TXN
#define COMMAND_IS_NOT_READ_ONLY	0

/* Hook for plugins to get control in ProcessUtility() */
typedef void (*ProcessUtility_hook_type)(
    PlannedStmt *pstmt, const char *queryString, bool readOnlyTree,
    ProcessUtilityContext context, ParamListInfo params,
    QueryEnvironment *queryEnv, DestReceiver *dest, QueryCompletion *qc);

extern void ProcessUtility(PlannedStmt *pstmt, const char *queryString,
                           bool readOnlyTree, ProcessUtilityContext context,
                           ParamListInfo params, QueryEnvironment *queryEnv,
                           DestReceiver *dest, QueryCompletion *qc);
extern void standard_ProcessUtility(PlannedStmt *pstmt, const char *queryString,
                                    bool readOnlyTree,
                                    ProcessUtilityContext context,
                                    ParamListInfo params,
                                    QueryEnvironment *queryEnv,
                                    DestReceiver *dest, QueryCompletion *qc);

extern void ProcessUtilityForAlterTable(Node *stmt,
                                        AlterTableUtilityContext *context);

extern bool UtilityReturnsTuples(Node *parsetree);

extern TupleDesc UtilityTupleDescriptor(Node *parsetree);

extern Query *UtilityContainsQuery(Node *parsetree);

extern CommandTag CreateCommandTag(Node *parsetree);
extern CommandTag AlterObjectTypeCommandTag(ObjectType objtype);

static inline const char *CreateCommandName(Node *parsetree) {
  return GetCommandTagName(CreateCommandTag(parsetree));
}

extern LogStmtLevel GetCommandLogLevel(Node *parsetree);

extern bool CommandIsReadOnly(PlannedStmt *pstmt);

#endif /* UTILITY_H */

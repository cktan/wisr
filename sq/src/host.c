/*
 * host.c -- what sq still needs from the server.
 *
 * The raw parser and the layers under it were written inside a server,
 * and reach up into it in these places. The oracle is not a server, so
 * each is answered here the way a process with no client, no
 * transaction and no log file would answer it.
 *
 * This file is the list of what stands between sq and being a library:
 * every name here is a dependency to be cut or turned around, and the
 * file should shrink to nothing. README.md groups them.
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "fmgr.h"
#include "lib/stringinfo.h"
#include "libpq/libpq.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "nodes/value.h"
#include "pgstat.h"
#include "pgtime.h"
#include "postmaster/bgworker.h"
#include "postmaster/postmaster.h"
#include "postmaster/syslogger.h"
#include "storage/ipc.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "utils/backend_status.h"
#include "utils/guc.h"
#include "utils/json.h"
#include "utils/ps_status.h"
#include "utils/wait_event.h"

/*
 * Error reporting (utils/error) asks who and where it is, to decide
 * whether to write a message and to fill in a log line's prefix.
 */
BackendType MyBackendType = B_STANDALONE_BACKEND;
PGPROC *MyProc = NULL;
BackgroundWorker *MyBgworkerEntry = NULL;
bool ClientAuthInProgress = false;
bool redirection_done = false;
bool proc_exit_inprogress = false;
CommandDest whereToSendOutput = DestNone;
const PQcommMethods *PqCommMethods = NULL;
const char *debug_query_string = NULL;
char *application_name = NULL;
char *backtrace_functions = NULL;
int client_min_messages = NOTICE;
int log_min_messages = WARNING;
int log_min_error_statement = ERROR;
pg_tz *log_timezone = NULL;
SessionEndType pgStatSessionEndCause = DISCONNECT_NORMAL;

static uint32 local_wait_event_info;
uint32 *my_wait_event_info = &local_wait_event_info;

/* Name a backend type for a log line: there is one kind of process here. */
const char *GetBackendTypeDesc(BackendType backendType) { return "oracle"; }

/* Return the process title for a log line: there is none. */
const char *get_ps_display(int *displen) {
  *displen = 0;
  return "";
}

/* Return the running query's id for a log line: nothing is being run. */
int64 pgstat_get_my_query_id(void) { return 0; }

/* Return the transaction's id for a log line: there is no transaction. */
TransactionId GetTopTransactionIdIfAny(void) { return InvalidTransactionId; }

/* Say whether a transaction is open: there never is one. */
bool IsTransactionState(void) { return false; }

/* Write to the log collector's file: there is no collector, so stderr. */
void write_syslogger_file(const char *buffer, int count, int destination) {
  fwrite(buffer, 1, count, stderr);
}

/* Send an error to a client of the old protocol: there is no client. */
int pq_putmessage_v2(char msgtype, const char *s, size_t len) { return 0; }

/* Serve a pending interrupt: none is ever raised here. */
void ProcessInterrupts(void) {}

/* End the process, as a FATAL error does. */
void proc_exit(int code) { exit(code); }

/* Quote a string as JSON for the JSON log format, which is never chosen. */
void escape_json(StringInfo buf, const char *str) {
  appendStringInfoString(buf, str);
}

/* Open a time zone file for the log's time stamps: the log has no zone. */
int pg_open_tzfile(const char *name, char *canonname) { return -1; }

/*
 * Encoding conversion (utils/mb) looks its conversion function up in the
 * catalog and calls it through fmgr. sq is UTF-8 only and converts
 * nothing.
 */

/* Find the conversion between two encodings: there is none. */
Oid FindDefaultConversionProc(int32 for_encoding, int32 to_encoding) {
  return InvalidOid;
}

/* Call a conversion function by pointer: none can have been found. */
Datum FunctionCall6Coll(FmgrInfo *flinfo, Oid collation, Datum arg1, Datum arg2,
                        Datum arg3, Datum arg4, Datum arg5, Datum arg6) {
  elog(ERROR, "sq has no function manager");
  return (Datum)0;
}

/* Call a conversion function by oid: none can have been found. */
Datum OidFunctionCall6Coll(Oid functionId, Oid collation, Datum arg1,
                           Datum arg2, Datum arg3, Datum arg4, Datum arg5,
                           Datum arg6) {
  elog(ERROR, "sq has no function manager");
  return (Datum)0;
}

/*
 * The grammar calls one function of the catalog's, to print a qualified
 * name in an error message. It reads no catalog, and this is a copy of
 * it: it belongs with the node functions.
 */

/* Return a dotted name list as one string, for a message. */
char *NameListToString(const List *names) {
  StringInfoData string;
  ListCell *l;

  initStringInfo(&string);

  foreach (l, names) {
    Node *name = (Node *)lfirst(l);

    if (l != list_head(names))
      appendStringInfoChar(&string, '.');

    if (IsA(name, String))
      appendStringInfoString(&string, strVal(name));
    else if (IsA(name, A_Star))
      appendStringInfoChar(&string, '*');
    else
      elog(ERROR, "unexpected node type in name list: %d", (int)nodeTag(name));
  }

  return string.data;
}

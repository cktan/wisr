/*-------------------------------------------------------------------------
 *
 * sequence.c
 *	  Sequences: refused until they are sm's.
 *
 * A PostgreSQL sequence was a one-row heap relation, read and written in
 * the buffer manager; both are gone.  sm has sequences of its own
 * (sm_mkseq, sm_nextseq), which digres does not use yet, so every
 * statement and function on a sequence is refused, and a SERIAL or
 * identity column with it.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/sequence.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "commands/sequence.h"
#include "utils/fmgrprotos.h"

/* Refuse what needs a sequence. */
static void refuse(void) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("sequences are not supported yet")));
}

/* CREATE SEQUENCE: refused. */
ObjectAddress DefineSequence(ParseState *pstate, CreateSeqStmt *seq) {
  refuse();
  return InvalidObjectAddress; /* not reached */
}

/* ALTER SEQUENCE: refused. */
ObjectAddress AlterSequence(ParseState *pstate, AlterSeqStmt *stmt) {
  refuse();
  return InvalidObjectAddress; /* not reached */
}

/* ALTER SEQUENCE ... SET LOGGED or UNLOGGED: refused. */
void SequenceChangePersistence(Oid relid, char newrelpersistence) { refuse(); }

/* Drop a sequence's row: refused, as no sequence is made. */
void DeleteSequenceTuple(Oid relid) { refuse(); }

/* TRUNCATE ... RESTART IDENTITY's restart of a sequence: refused. */
void ResetSequence(Oid seq_relid) { refuse(); }

/* DISCARD SEQUENCES: there is no cache of sequence values to discard. */
void ResetSequenceCaches(void) {}

/* A sequence's options, as CREATE TABLE LIKE copies them: refused. */
List *sequence_options(Oid relid) {
  refuse();
  return NIL; /* not reached */
}

/* A sequence's next value, as a column's default takes it: refused. */
int64 nextval_internal(Oid relid, bool check_permissions) {
  refuse();
  return 0; /* not reached */
}

/* nextval(text): refused. */
Datum nextval(PG_FUNCTION_ARGS) {
  refuse();
  PG_RETURN_NULL(); /* not reached */
}

/* nextval(regclass): refused. */
Datum nextval_oid(PG_FUNCTION_ARGS) {
  refuse();
  PG_RETURN_NULL(); /* not reached */
}

/* currval(regclass): refused. */
Datum currval_oid(PG_FUNCTION_ARGS) {
  refuse();
  PG_RETURN_NULL(); /* not reached */
}

/* lastval(): refused. */
Datum lastval(PG_FUNCTION_ARGS) {
  refuse();
  PG_RETURN_NULL(); /* not reached */
}

/* setval(regclass, bigint): refused. */
Datum setval_oid(PG_FUNCTION_ARGS) {
  refuse();
  PG_RETURN_NULL(); /* not reached */
}

/* setval(regclass, bigint, boolean): refused. */
Datum setval3_oid(PG_FUNCTION_ARGS) {
  refuse();
  PG_RETURN_NULL(); /* not reached */
}

/* pg_sequence_parameters(regclass): refused. */
Datum pg_sequence_parameters(PG_FUNCTION_ARGS) {
  refuse();
  PG_RETURN_NULL(); /* not reached */
}

/* pg_get_sequence_data(regclass): refused. */
Datum pg_get_sequence_data(PG_FUNCTION_ARGS) {
  refuse();
  PG_RETURN_NULL(); /* not reached */
}

/* pg_sequence_last_value(regclass): refused. */
Datum pg_sequence_last_value(PG_FUNCTION_ARGS) {
  refuse();
  PG_RETURN_NULL(); /* not reached */
}

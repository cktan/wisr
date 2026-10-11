/*-------------------------------------------------------------------------
 *
 * sequence.c
 *	  Sequences: sm's.
 *
 * A PostgreSQL sequence was a one-row heap relation, read and written in
 * the buffer manager; both are gone.  A sequence is now an object in sm's
 * catalog, in a schema, with an sm counter behind it (access/smstore.h):
 * CREATE SEQUENCE makes one, nextval() draws from it, and currval() and
 * lastval() report what this session drew.  sm's counters only go
 * forward, so setval(), ALTER SEQUENCE and a sequence's other options are
 * refused, and a SERIAL or identity column with them.
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

#include <unordered_map>

#include "access/smstore.h"
#include "catalog/namespace.h"
#include "commands/sequence.h"
#include "utils/builtins.h"
#include "utils/control_block.h"
#include "utils/fmgrprotos.h"
#include "utils/varlena.h"

/*
 * This session's draws: the last value nextval() gave from each sequence,
 * for currval(), and the last of all, for lastval(): tcb->sequence.
 */
struct sequence_state_t {
  std::unordered_map<Oid, int64> last; /* sequence oid -> its last value */
  Oid last_oid = InvalidOid;           /* the sequence drawn from last */
};

/* This session's draws. */
static inline sequence_state_t &ss(void) { return *tcb->sequence; }

/* Make a thread's draws: none yet. */
sequence_state_t *sequence_state_new(void) { return new sequence_state_t(); }

/* Free a thread's draws. */
void sequence_state_free_t::operator()(sequence_state_t *state) const {
  delete state;
}

/* Refuse what a sequence of sm's cannot do: what, named. */
static void refuse(const char *what) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("%s is not supported yet", what),
                  errdetail("A sequence only goes forward.")));
}

/* CREATE SEQUENCE: made in sm (access/smstore.h). */
ObjectAddress DefineSequence(ParseState *pstate, CreateSeqStmt *seq) {
  return smstore_create_sequence(seq);
}

/* ALTER SEQUENCE: refused. */
ObjectAddress AlterSequence(ParseState *pstate, AlterSeqStmt *stmt) {
  refuse("ALTER SEQUENCE");
  return InvalidObjectAddress; /* not reached */
}

/* ALTER SEQUENCE ... SET LOGGED or UNLOGGED: refused. */
void SequenceChangePersistence(Oid relid, char newrelpersistence) {
  refuse("ALTER SEQUENCE");
}

/* Drop a sequence's row: a sequence in sm has none (smstore_drop_*()). */
void DeleteSequenceTuple(Oid relid) { refuse("dropping a sequence's row"); }

/* TRUNCATE ... RESTART IDENTITY's restart of a sequence: refused. */
void ResetSequence(Oid seq_relid) { refuse("restarting a sequence"); }

/* DISCARD SEQUENCES: forget what this session drew. */
void ResetSequenceCaches(void) {
  ss().last.clear();
  ss().last_oid = InvalidOid;
}

/* A sequence's options, as CREATE TABLE LIKE copies them: refused. */
List *sequence_options(Oid relid) {
  refuse("copying a sequence's options");
  return NIL; /* not reached */
}

/*
 * Draw the next value of the sequence relid, and remember it as this
 * session's last.  A sequence in sm has no privileges to check.
 */
int64 nextval_internal(Oid relid, bool check_permissions) {
  int64 value = smstore_nextval(relid);

  ss().last[relid] = value;
  ss().last_oid = relid;
  return value;
}

/* nextval(text): the sequence named, as the search path finds it. */
Datum nextval(PG_FUNCTION_ARGS) {
  text *seqin = PG_GETARG_TEXT_PP(0);
  RangeVar *sequence;
  Oid relid;

  sequence = makeRangeVarFromNameList(textToQualifiedNameList(seqin));
  relid = RangeVarGetRelid(sequence, NoLock, false);
  PG_RETURN_INT64(nextval_internal(relid, true));
}

/* nextval(regclass). */
Datum nextval_oid(PG_FUNCTION_ARGS) {
  PG_RETURN_INT64(nextval_internal(PG_GETARG_OID(0), true));
}

/* currval(regclass): what this session's last nextval() of it gave. */
Datum currval_oid(PG_FUNCTION_ARGS) {
  Oid relid = PG_GETARG_OID(0);
  auto it = ss().last.find(relid);

  if (it == ss().last.end())
    ereport(ERROR,
            (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
             errmsg("currval of sequence \"%s\" is not yet defined in this "
                    "session",
                    smstore_sequence_name(relid))));
  PG_RETURN_INT64(it->second);
}

/*
 * lastval(): what this session's last nextval() gave, of the sequence it
 * drew from last, while that sequence is still there.
 */
Datum lastval(PG_FUNCTION_ARGS) {
  Oid relid = ss().last_oid;

  if (!OidIsValid(relid) || !smstore_sequence_exists(relid))
    ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
                    errmsg("lastval is not yet defined in this session")));
  PG_RETURN_INT64(ss().last[relid]);
}

/* setval(regclass, bigint): refused. */
Datum setval_oid(PG_FUNCTION_ARGS) {
  refuse("setval()");
  PG_RETURN_NULL(); /* not reached */
}

/* setval(regclass, bigint, boolean): refused. */
Datum setval3_oid(PG_FUNCTION_ARGS) {
  refuse("setval()");
  PG_RETURN_NULL(); /* not reached */
}

/* pg_sequence_parameters(regclass): refused. */
Datum pg_sequence_parameters(PG_FUNCTION_ARGS) {
  refuse("pg_sequence_parameters()");
  PG_RETURN_NULL(); /* not reached */
}

/* pg_get_sequence_data(regclass): refused. */
Datum pg_get_sequence_data(PG_FUNCTION_ARGS) {
  refuse("pg_get_sequence_data()");
  PG_RETURN_NULL(); /* not reached */
}

/* pg_sequence_last_value(regclass): refused. */
Datum pg_sequence_last_value(PG_FUNCTION_ARGS) {
  refuse("pg_sequence_last_value()");
  PG_RETURN_NULL(); /* not reached */
}

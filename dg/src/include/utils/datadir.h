/*
 * datadir.h
 *	  What the server reads from its data directory at startup.
 *
 * src/include/utils/datadir.h
 */
#ifndef DATADIR_H
#define DATADIR_H

#include <vector>

#include "nodes/pg_list.h"

extern void datadir_load(void);
extern Oid datadir_database_oid(const char *dbname);
extern void datadir_open_session(const char *dbname);
extern char *datadir_database_name(Oid dboid);
extern List *datadir_database_names(void);
extern bool datadir_create_database(const char *dbname);

/* What datadir_drop_database() did. */
typedef enum datadir_drop_t {
  DATADIR_DROPPED,
  DATADIR_NO_DATABASE,
  DATADIR_IN_USE,
} datadir_drop_t;

extern datadir_drop_t datadir_drop_database(const char *dbname);

struct sm_session_t;
extern struct sm_session_t *datadir_sm_session(void);
extern std::vector<int> datadir_sm_blockers(int pid);
/* What an sm lock names (datadir_lock_t). */
typedef enum datadir_lockname_t {
  DATADIR_LOCK_OBJECT, /* an object by its oid: a table, index or schema */
  DATADIR_LOCK_ROW,    /* a row of a table */
  DATADIR_LOCK_KEY     /* another key: one of the catalog's DDL locks */
} datadir_lockname_t;

/* A lock of sm's, held or waited for, as pg_locks lists it. */
typedef struct datadir_lock_t {
  Oid database;            /* its database */
  datadir_lockname_t name; /* what it names */
  Oid oid;                 /* OBJECT: the object; ROW: its table, or 0 */
  const char *mode;        /* sm's name of the mode: IS, IX, S, SIX or X */
  int pid;                 /* the session's, or 0 for none */
  bool granted;            /* held; false if waited for */
} datadir_lock_t;

extern std::vector<datadir_lock_t> datadir_sm_locks(void);

#endif /* DATADIR_H */

/*
 * datadir.h
 *	  What the server reads from its data directory at startup.
 *
 * src/include/utils/datadir.h
 */
#ifndef DATADIR_H
#define DATADIR_H

#include "nodes/pg_list.h"

extern void datadir_load(void);
extern const char *datadir_superuser(void);
extern const char *datadir_superuser_secret(void);
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

#endif /* DATADIR_H */

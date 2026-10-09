/*
 * datadir.h
 *	  What the server reads from its data directory at startup.
 *
 * src/include/utils/datadir.h
 */
#ifndef DATADIR_H
#define DATADIR_H

extern void datadir_load(void);
extern const char *datadir_superuser(void);
extern const char *datadir_superuser_secret(void);
extern Oid datadir_database_oid(const char *dbname);

#endif /* DATADIR_H */

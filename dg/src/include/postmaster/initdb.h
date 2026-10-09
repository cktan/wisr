/*
 * initdb.h
 *	  "initdb", or "digres --init": make a data directory.
 *
 * src/include/postmaster/initdb.h
 */
#ifndef INITDB_H
#define INITDB_H

/* The name of the file in a data directory that names the superuser. */
#define SUPERUSER_FILE "superuser"

extern int initdb_main(int argc, char **argv);

#endif /* INITDB_H */

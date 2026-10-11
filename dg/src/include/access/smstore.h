/*-------------------------------------------------------------------------
 *
 * smstore.h
 *	  digres's way into sm, the storage stack (../sm/include/sm.h): its
 *	  catalog of what a user creates, and its tables.
 *
 * Only smstore.c includes sm.h; the rest of the backend speaks to sm in
 * PostgreSQL's types, through here.
 *
 * IDENTIFICATION
 *	  src/include/access/smstore.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMSTORE_H
#define SMSTORE_H

#include "catalog/objectaddress.h"
#include "nodes/parsenodes.h"

extern Oid smstore_namespace_oid(const char *name);
extern Oid smstore_relation_oid(const char *name, Oid nspoid);
extern ObjectAddress smstore_create_table(CreateStmt *stmt);

#endif /* SMSTORE_H */

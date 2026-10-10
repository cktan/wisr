/*
 * xlog.h
 *
 * What is left of PostgreSQL's write-ahead log manager.  digres has no WAL:
 * sm keeps what is durable (sm/PART4.md).  This is the in-memory control
 * record, and server startup and shutdown.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/xlog.h
 */
#ifndef XLOG_H
#define XLOG_H

#include "access/xlogdefs.h"
#include "datatype/timestamp.h"

extern char *GetMockAuthenticationNonce(void);
extern bool DataChecksumsEnabled(void);
extern Size XLOGShmemSize(void);
extern void XLOGShmemInit(void);
extern void LocalProcessControlFile(bool reset);
extern void StartupXLOG(void);
extern void ShutdownXLOG(int code, Datum arg);

#endif /* XLOG_H */

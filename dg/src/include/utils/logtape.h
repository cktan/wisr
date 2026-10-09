/*-------------------------------------------------------------------------
 *
 * logtape.h
 *	  Management of "logical tapes" within temporary files.
 *
 * See logtape.c for explanations.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/utils/logtape.h
 *
 *-------------------------------------------------------------------------
 */

#ifndef LOGTAPE_H
#define LOGTAPE_H

/*
 * LogicalTapeSet and LogicalTape are opaque types whose details are not
 * known outside logtape.c.
 */
typedef struct LogicalTapeSet LogicalTapeSet;
typedef struct LogicalTape LogicalTape;

/*
 * prototypes for functions in logtape.c
 */

extern LogicalTapeSet *LogicalTapeSetCreate(bool preallocate);
extern void LogicalTapeClose(LogicalTape *lt);
extern void LogicalTapeSetClose(LogicalTapeSet *lts);
extern LogicalTape *LogicalTapeCreate(LogicalTapeSet *lts);
extern void LogicalTapeSetForgetFreeSpace(LogicalTapeSet *lts);
extern size_t LogicalTapeRead(LogicalTape *lt, void *ptr, size_t size);
extern void LogicalTapeWrite(LogicalTape *lt, const void *ptr, size_t size);
extern void LogicalTapeRewindForRead(LogicalTape *lt, size_t buffer_size);
extern void LogicalTapeFreeze(LogicalTape *lt);
extern size_t LogicalTapeBackspace(LogicalTape *lt, size_t size);
extern void LogicalTapeSeek(LogicalTape *lt, int64 blocknum, int offset);
extern void LogicalTapeTell(LogicalTape *lt, int64 *blocknum, int *offset);
extern int64 LogicalTapeSetBlocks(LogicalTapeSet *lts);

#endif /* LOGTAPE_H */

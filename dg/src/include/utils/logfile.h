/*-------------------------------------------------------------------------
 *
 * logfile.h
 *	  The log file: where every log message is written, to stderr or to
 *	  a file the process rotates itself.
 *
 * Derived from PostgreSQL's postmaster/syslogger.h.
 *
 * Copyright (c) 2004-2025, PostgreSQL Global Development Group
 *
 * src/include/utils/logfile.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGFILE_H
#define LOGFILE_H

/* The settings (guc_tables.c). */
extern thread_local /*LOCAL*/ PGDLLIMPORT bool Logging_collector;
extern thread_local /*LOCAL*/ PGDLLIMPORT int Log_RotationAge;
extern thread_local /*LOCAL*/ PGDLLIMPORT int Log_RotationSize;
extern thread_local /*LOCAL*/ PGDLLIMPORT char *Log_directory;
extern thread_local /*LOCAL*/ PGDLLIMPORT char *Log_filename;
extern thread_local /*LOCAL*/ PGDLLIMPORT bool Log_truncate_on_rotation;
extern thread_local /*LOCAL*/ PGDLLIMPORT int Log_file_mode;

extern void logfile_open(void);
extern void logfile_reload(void);
extern bool logfile_rotate(void);
extern void logfile_write(const char *buf, int len);

#endif /* LOGFILE_H */

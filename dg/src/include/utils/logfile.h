/*-------------------------------------------------------------------------
 *
 * logfile.h
 *	  The log file: where every log message is written, to stderr or to
 *	  a file the process rotates itself.
 *
 * Derived from PostgreSQL's postmaster/syslogger.h.
 *
 * Copyright (c) 2004-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 * src/include/utils/logfile.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef LOGFILE_H
#define LOGFILE_H

extern void publish_log_file_settings(void);
extern void logfile_open(void);
extern void logfile_reload(void);
extern bool logfile_rotate(void);
extern void logfile_write(const char *buf, int len);

#endif /* LOGFILE_H */

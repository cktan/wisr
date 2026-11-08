/*-------------------------------------------------------------------------
 *
 * logfile.c
 *	  The log file: where every log message is written.
 *
 * Every thread writes its messages with logfile_write(), which writes them
 * to stderr, or, with logging_collector on, to a log file.  There is no
 * logger process or thread and no pipe: the caller's thread does the write
 * and, when a limit is reached, the rotation, under one mutex.  Time-based
 * rotation therefore happens only when a message arrives.
 *
 * With logging_collector on, logfile_open() points the process's stderr at
 * the log file, and each rotation points it at the next file.  What writes
 * to stderr without logfile_write() (a stdio call, RocksDB, an abort) lands
 * in the same file, but is not counted toward log_rotation_size.
 *
 * The file naming, opening and rotation are derived from PostgreSQL's
 * postmaster/syslogger.c, which is
 *
 * Copyright (c) 2004-2025, PostgreSQL Global Development Group
 *
 * Author of the original: Andreas Pflug <pgadmin@pse-consulting.de>
 *
 * IDENTIFICATION
 *	  src/backend/utils/error/logfile.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <fcntl.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <mutex>
#include <string>

#include "pgtime.h"
#include "storage/fd.h"
#include "utils/control_block.h"
#include "utils/logfile.h"

/* The settings, as guc_tables.c sets them: a thread's copy each. */
thread_local /*LOCAL*/ bool Logging_collector = false;
thread_local /*LOCAL*/ int Log_RotationAge = HOURS_PER_DAY * MINS_PER_HOUR;
thread_local /*LOCAL*/ int Log_RotationSize = 10 * 1024;
thread_local /*LOCAL*/ char *Log_directory = NULL;
thread_local /*LOCAL*/ char *Log_filename = NULL;
thread_local /*LOCAL*/ bool Log_truncate_on_rotation = false;
thread_local /*LOCAL*/ int Log_file_mode = S_IRUSR | S_IWUSR;

/*
 * The process's log file, in shmem->logfile and guarded by
 * shmem->stderr_mutex.  It holds a copy of the settings, made by the
 * postmaster: a session thread has settings of its own, which it must not
 * use to rotate.
 */
struct logfile_state_t {
  bool enabled = false;        /* logging_collector is on, and the file open */
  std::string directory;       /* Log_directory */
  std::string pattern;         /* Log_filename, a strftime() pattern */
  int mode = 0600;             /* Log_file_mode */
  int rotation_age = 0;        /* Log_RotationAge, minutes */
  int rotation_size = 0;       /* Log_RotationSize, kB */
  bool truncate = false;       /* Log_truncate_on_rotation */
  std::string name;            /* the file now written */
  off_t bytes = 0;             /* what the file holds */
  pg_time_t next_rotation = 0; /* when rotation_age next rotates */
  bool rotation_disabled = false; /* a rotation failed: no automatic one */
};

/* Make the state, empty: pcb_init() calls it. */
logfile_state_t *logfile_state_new(void) { return new logfile_state_t(); }

/* Free the state: never, as shmem is never freed. */
void logfile_free_t::operator()(logfile_state_t *state) const { delete state; }

/*
 * Write a line about the log file itself to stderr, without ereport(): the
 * caller holds the mutex, and ereport() would take it again.
 */
static void logfile_say(const char *fmt, ...) {
  char line[MAXPGPATH + 128];
  va_list ap;
  int len;
  ssize_t rc;

  len = snprintf(line, sizeof(line), "LOG:  logfile: ");
  va_start(ap, fmt);
  len += vsnprintf(line + len, sizeof(line) - len, fmt, ap);
  va_end(ap);
  if (len > (int)sizeof(line) - 2)
    len = sizeof(line) - 2;
  line[len++] = '\n';
  rc = write(STDERR_FILENO, line, len);
  (void)rc;
}

/*
 * Construct the log file name from the settings and a time: the directory,
 * and the pattern as strftime() expands it.
 */
static std::string logfile_getname(const logfile_state_t *st,
                                   pg_time_t timestamp) {
  char filename[MAXPGPATH];
  int len;

  len = snprintf(filename, MAXPGPATH, "%s/", st->directory.c_str());
  if (len >= MAXPGPATH)
    len = MAXPGPATH - 1;
  pg_strftime(filename + len, MAXPGPATH - len, st->pattern.c_str(),
              pg_localtime(&timestamp, log_timezone));
  return filename;
}

/*
 * Determine the next planned rotation time, and store it in
 * st->next_rotation.
 */
static void set_next_rotation_time(logfile_state_t *st) {
  pg_time_t now;
  struct pg_tm *tm;
  int rotinterval;

  /* nothing to do if time-based rotation is disabled */
  if (st->rotation_age <= 0)
    return;

  /*
   * The requirements here are to choose the next time > now that is a
   * "multiple" of the log rotation interval.  "Multiple" can be interpreted
   * fairly loosely.  In this version we align to log_timezone rather than
   * GMT.
   */
  rotinterval = st->rotation_age * SECS_PER_MINUTE; /* to seconds */
  now = (pg_time_t)time(NULL);
  tm = pg_localtime(&now, log_timezone);
  now += tm->tm_gmtoff;
  now -= now % rotinterval;
  now += rotinterval;
  now -= tm->tm_gmtoff;
  st->next_rotation = now;
}

/*
 * Copy the settings of this thread, the postmaster's, into the state.  The
 * caller holds the mutex.
 */
static void logfile_copy_settings(logfile_state_t *st) {
  st->directory = Log_directory;
  st->pattern = Log_filename;
  st->mode = Log_file_mode;
  st->rotation_age = Log_RotationAge;
  st->rotation_size = Log_RotationSize;
  st->truncate = Log_truncate_on_rotation;
}

/*
 * Rotate: open the next log file and point stderr at it, with the mutex
 * held.  Return false, and keep the old file, if it cannot be opened.
 *
 * 1. Name the file.  For a time-based rotation use the planned time, not
 *    the present, so the name does not slip when the rotation is late.
 * 2. Decide to truncate or append.  Truncate only if log_truncate_on_rotation
 *    is on, the rotation is time-based, and the name differs from the old.
 * 3. Open the file.  If it fails, stop automatic rotation, unless the process
 *    is out of descriptors, which will pass.
 * 4. Point stderr at the new file, which closes the old one.
 * 5. Plan the next time-based rotation.
 */
static bool logfile_rotate_locked(logfile_state_t *st, bool time_based) {
  std::string filename =
      logfile_getname(st, time_based ? st->next_rotation : time(NULL));
  bool truncate =
      st->truncate && time_based && st->enabled && filename != st->name;
  int mode = st->mode | S_IWUSR; /* the file must be ours to write */
  struct stat sb;
  int fd;

  /* Always O_APPEND, as stray writers share the open file with us. */
  fd = open(filename.c_str(),
            O_WRONLY | O_CREAT | O_APPEND | (truncate ? O_TRUNC : 0), mode);
  if (fd < 0) {
    int save_errno = errno;

    logfile_say("could not open log file \"%s\": %s", filename.c_str(),
                strerror(save_errno));
    if (ENFILE != save_errno && EMFILE != save_errno) {
      logfile_say("disabling automatic rotation (use SIGHUP to re-enable)");
      st->rotation_disabled = true;
    }
    return false;
  }
  /* The umask may have cleared bits of the mode; the setting is exact. */
  if (0 != fchmod(fd, mode))
    logfile_say("could not set the mode of log file \"%s\": %s",
                filename.c_str(), strerror(errno));
  st->bytes = 0 == fstat(fd, &sb) ? sb.st_size : 0;
  /* From now on the file is stderr: writes of any kind land in it. */
  if (dup2(fd, STDERR_FILENO) < 0)
    logfile_say("could not redirect stderr to \"%s\": %s", filename.c_str(),
                strerror(errno));
  close(fd);
  st->name = filename;
  st->enabled = true;
  set_next_rotation_time(st);
  return true;
}

/*
 * Write one log message, a whole line, to the log file, or to stderr if
 * there is none.  Any thread may call it.
 *
 * 1. Take the mutex, so that messages do not interleave.
 * 2. Rotate first if the planned time has passed.
 * 3. Write the message in one write().  A failure has nowhere to go.
 * 4. Rotate after, if the file has reached its size limit.
 */
void logfile_write(const char *buf, int len) {
  logfile_state_t *st = shmem->logfile.get();
  ssize_t rc;

  std::lock_guard<std::mutex> guard(shmem->stderr_mutex);
  if (st->enabled && !st->rotation_disabled && st->rotation_age > 0 &&
      time(NULL) >= st->next_rotation)
    logfile_rotate_locked(st, true);
  rc = write(STDERR_FILENO, buf, len);
  (void)rc;
  if (st->enabled) {
    st->bytes += len;
    if (!st->rotation_disabled && st->rotation_size > 0 &&
        st->bytes >= (off_t)st->rotation_size * 1024)
      logfile_rotate_locked(st, false);
  }
}

/*
 * Start writing the log to a file, if logging_collector is on.  The
 * postmaster calls it once its settings are read, before it attaches sm, so
 * that what sm logs as it recovers goes to the file.  A file that cannot be
 * opened is fatal.
 */
void logfile_open(void) {
  logfile_state_t *st = shmem->logfile.get();
  bool opened;

  if (!Logging_collector)
    return;

  /* The directory may exist; if not, the open below reports a failure. */
  (void)MakePGDirectory(Log_directory);

  /* Tell the old stderr where the log goes, as it is the last thing it gets. */
  ereport(LOG, (errmsg("redirecting log output to the log file"),
                errhint("Future log output will appear in directory \"%s\".",
                        Log_directory)));
  {
    std::lock_guard<std::mutex> guard(shmem->stderr_mutex);
    logfile_copy_settings(st);
    opened = logfile_rotate_locked(st, false);
  }
  if (!opened)
    ereport(FATAL, (errmsg("could not open a log file in directory \"%s\"",
                           Log_directory)));
}

/*
 * Apply the settings after the postmaster reloads its configuration.
 *
 * 1. Copy the new settings, and allow automatic rotation again.
 * 2. If the directory or the file name pattern changed, rotate to the new
 *    name, so that the log goes where the settings say.
 * 3. Otherwise plan the next time-based rotation again, as the age may have
 *    changed.
 */
void logfile_reload(void) {
  logfile_state_t *st = shmem->logfile.get();
  bool changed;

  if (!Logging_collector)
    return;

  std::lock_guard<std::mutex> guard(shmem->stderr_mutex);
  changed = st->directory != Log_directory || st->pattern != Log_filename;
  logfile_copy_settings(st);
  st->rotation_disabled = false;
  if (changed) {
    (void)MakePGDirectory(st->directory.c_str());
    logfile_rotate_locked(st, false);
  } else
    set_next_rotation_time(st);
}

/*
 * Rotate the log file now: pg_rotate_logfile() calls this.  Return false if
 * the log is not in a file, or the next file cannot be opened.  Like a
 * size-based rotation, it never truncates.
 */
bool logfile_rotate(void) {
  logfile_state_t *st = shmem->logfile.get();

  std::lock_guard<std::mutex> guard(shmem->stderr_mutex);
  if (!st->enabled)
    return false;
  return logfile_rotate_locked(st, false);
}

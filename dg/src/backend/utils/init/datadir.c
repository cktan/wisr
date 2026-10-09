/*-------------------------------------------------------------------------
 *
 * datadir.c
 *	  What the server reads from its data directory at startup.
 *
 * initdb leaves two things the server needs before it takes a connection
 * (src/backend/main/initdb.c): the superuser file, which names the one
 * role there is and holds the SCRAM secret of its password; and sm's home
 * directory, which knows the databases.
 *
 * The postmaster learns both once, before it forks a backend, and every
 * backend has them from it. sm keeps its state in the memory of the
 * process that attached the home directory (sm/PART4.md section 2), so a
 * backend cannot ask it: until connections are threads, the databases
 * are the ones there were when the server started.
 *
 * src/backend/utils/init/datadir.c
 *
 *-------------------------------------------------------------------------
 */
/* sm's header first: it includes the C++ library, which must be read
 * before PostgreSQL's macros. */
#include "sm.h"

#include <string>
#include <vector>

#include "postgres.h"

#include <sys/wait.h>
#include <unistd.h>

#include "access/transam.h"
#include "common/string.h"
#include "lib/stringinfo.h"
#include "postmaster/initdb.h"
#include "storage/fd.h"
#include "utils/datadir.h"
#include "utils/memutils.h"

/* The superuser's name, and its SCRAM secret or NULL. */
static char *superuser_name = NULL;
static char *superuser_secret = NULL;

/* The databases' names, in the order sm listed them. */
static char **database_names = NULL;
static int database_count = 0;

/* Read the superuser file of the data directory, which is the current one. */
static void load_superuser(void) {
  FILE *file = AllocateFile(SUPERUSER_FILE, "r");
  char *line;

  if (file == NULL)
    ereport(FATAL, (errcode_for_file_access(),
                    errmsg("could not open file \"%s\": %m", SUPERUSER_FILE),
                    errhint("Run initdb to make a digres data directory.")));

  line = pg_get_line(file, NULL);
  if (line == NULL || pg_strip_crlf(line) == 0)
    ereport(FATAL, (errmsg("file \"%s\" names no superuser", SUPERUSER_FILE)));
  superuser_name = MemoryContextStrdup(TopMemoryContext, line);

  line = pg_get_line(file, NULL);
  if (line != NULL && pg_strip_crlf(line) > 0)
    superuser_secret = MemoryContextStrdup(TopMemoryContext, line);
  FreeFile(file);
}

/*
 * In a child process: ask sm for the databases of the data directory, and
 * write their names to fd, each ended by a NUL. Never returns.
 */
static void list_databases(int fd) {
  sm_env_t *env = NULL;
  std::vector<std::string> names;

  /* Attach, since the databases are listed through an attached home. */
  if (sm_attach(".", &env) != 0 || sm_lsdb(env, names) != 0) {
    write_stderr("could not list the databases: %s\n", sm_errmsg());
    _exit(1);
  }
  for (const std::string &name : names)
    if (write(fd, name.c_str(), name.size() + 1) != (ssize_t)name.size() + 1)
      _exit(1);
  /* Detach, which closes the store before the process ends. */
  sm_detach(env, 0);
  _exit(0);
}

/*
 * Learn the databases of the data directory. A child process asks sm and
 * the postmaster reads the answer from a pipe: attaching leaves RocksDB's
 * threads behind in the process that did it, and the postmaster must have
 * none when it forks a backend.
 */
static void load_databases(void) {
  int fds[2];
  pid_t pid;
  int status;
  StringInfoData buf;
  char chunk[1024];
  ssize_t n;

  if (pipe(fds) != 0)
    ereport(FATAL, (errmsg("could not create pipe: %m")));
  /* Fork the child that attaches sm in the postmaster's place. */
  pid = fork();
  if (pid < 0)
    ereport(FATAL, (errmsg("could not fork: %m")));
  if (pid == 0) {
    close(fds[0]);
    list_databases(fds[1]);
  }
  close(fds[1]);

  initStringInfo(&buf);
  while ((n = read(fds[0], chunk, sizeof(chunk))) > 0)
    appendBinaryStringInfo(&buf, chunk, n);
  close(fds[0]);
  /* Wait for the child, to learn whether the list is whole. */
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
    ;
  if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    ereport(FATAL, (errmsg("could not list the databases")));

  for (int i = 0; i < buf.len; i++)
    if (buf.data[i] == '\0')
      database_count++;
  database_names = (char **)MemoryContextAlloc(
      TopMemoryContext, sizeof(char *) * (database_count + 1));
  for (int i = 0, at = 0; i < database_count; i++) {
    database_names[i] = MemoryContextStrdup(TopMemoryContext, buf.data + at);
    at += strlen(buf.data + at) + 1;
  }
  pfree(buf.data);
}

/*
 * Read the superuser file and the list of databases. The postmaster calls
 * this once it is in the data directory.
 */
void datadir_load(void) {
  load_superuser();
  load_databases();
}

/* The name of the superuser, the one role there is. */
const char *datadir_superuser(void) { return superuser_name; }

/* The SCRAM secret of the superuser's password, or NULL if it has none. */
const char *datadir_superuser_secret(void) { return superuser_secret; }

/*
 * The oid of the database dbname, or InvalidOid if there is none. sm does
 * not hand out a database's oid, so it is the database's place in the
 * list, above the oids that are built in: good for one run of the server.
 */
Oid datadir_database_oid(const char *dbname) {
  for (int i = 0; i < database_count; i++)
    if (strcmp(database_names[i], dbname) == 0)
      return FirstNormalObjectId + i;
  return InvalidOid;
}

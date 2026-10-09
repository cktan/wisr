/*-------------------------------------------------------------------------
 *
 * initdb.c
 *	  "initdb", or "digres --init": make a data directory.
 *
 * PostgreSQL's initdb is a program of its own, which runs the server
 * in bootstrap mode to load the catalog and then copies it into three
 * databases. digres has its built-in catalog compiled in and its
 * storage made by sm, so what is left is small and lives in the
 * server, which answers to the name "initdb" as well.
 *
 * A data directory is an sm home directory (sm/sm1/SM1_DESIGN.md
 * section 3.5) holding one database, "postgres", with two files of
 * digres's beside it:
 *
 *   PG_VERSION   the major version, as PostgreSQL writes it
 *   superuser    the superuser's name, and on a second line the SCRAM
 *                secret of its password, if one was given
 *
 * There is one encoding, UTF8, and one default collation, C; and
 * authentication is not configured: peer on the local socket, SCRAM
 * over TCP. So the options are few, and PostgreSQL's others are
 * rejected, not ignored.
 *
 * src/backend/main/initdb.c
 *
 *-------------------------------------------------------------------------
 */
/* sm's header first: it includes the C++ library, which must be read
 * before PostgreSQL's macros. */
#include "sm.h"

#include "postgres.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common/string.h"
#include "common/username.h"
#include "getopt_long.h"
#include "libpq/scram.h"
#include "mb/pg_wchar.h"
#include "postmaster/initdb.h"

/* The data directory, and what to undo if making it fails. */
static thread_local const char *datadir = NULL;
static thread_local bool made_datadir = false;
static thread_local bool filled_datadir = false;

/* Remove what was made so far: a failed initdb leaves nothing behind. */
static void cleanup(void) {
  if (made_datadir) {
    fprintf(stderr, "initdb: removing data directory \"%s\"\n", datadir);
    rmtree(datadir, true);
  } else if (filled_datadir) {
    fprintf(stderr, "initdb: removing contents of data directory \"%s\"\n",
            datadir);
    rmtree(datadir, false);
  }
}

/* Report an error as PostgreSQL's initdb does, clean up, and exit. */
static void pg_attribute_printf(1, 2) fatal(const char *fmt, ...) {
  va_list ap;

  fprintf(stderr, "initdb: error: ");
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  cleanup();
  exit(1);
}

/* Print the usage. */
static void usage(void) {
  printf("initdb initializes a digres data directory.\n\n");
  printf("Usage:\n");
  printf("  initdb [OPTION]... [DATADIR]\n");
  printf("  digres --init [OPTION]... [DATADIR]\n");
  printf("\nOptions:\n");
  printf(" [-D, --pgdata=]DATADIR     location for this data directory\n");
  printf("                            (default: environment variable "
         "PGDATA)\n");
  printf("  -U, --username=NAME       superuser name (default: your OS user "
         "name)\n");
  printf("  -W, --pwprompt            prompt for the superuser's password\n");
  printf("      --pwfile=FILE         read the superuser's password from a "
         "file\n");
  printf("  -E, --encoding=UTF8       text encoding; only UTF8 is "
         "supported\n");
  printf("      --locale=LOCALE       C or C.UTF-8; both mean byte-order "
         "collation\n");
  printf("                            with Unicode case rules (the "
         "default)\n");
  printf("      --no-instructions     do not print instructions for next "
         "steps\n");
  printf("  -V, --version             output version information, then "
         "exit\n");
  printf("  -?, --help                show this help, then exit\n");
  printf("\nAuthentication is not configurable:\n");
  printf("  local socket connections use peer authentication (your OS "
         "user\n");
  printf("  name must match the role name); TCP connections use "
         "SCRAM-SHA-256\n");
  printf("  and need a password set here with -W or --pwfile.\n");
  printf("\nOther options of PostgreSQL's initdb are not accepted.\n");
}

/*
 * The superuser's password: asked for twice if prompt, else the first
 * line of pwfile. An empty one is refused, since it could never be used.
 */
static char *read_password(bool prompt, const char *pwfile) {
  char *password;

  if (prompt) {
    char *again;

    password = simple_prompt("Enter new superuser password: ", false);
    again = simple_prompt("Enter it again: ", false);
    if (strcmp(password, again) != 0)
      fatal("passwords didn't match");
  } else {
    FILE *file = fopen(pwfile, "r");

    if (file == NULL)
      fatal("could not open file \"%s\" for reading: %s", pwfile,
            strerror(errno));
    password = pg_get_line(file, NULL);
    if (password == NULL) {
      if (ferror(file))
        fatal("could not read password from file \"%s\": %s", pwfile,
              strerror(errno));
      fatal("password file \"%s\" is empty", pwfile);
    }
    fclose(file);
    (void)pg_strip_crlf(password);
  }
  if (password[0] == '\0')
    fatal("the superuser's password must not be empty");
  return password;
}

/* Write text as the file name in the data directory, and to disk. */
static void write_file(const char *name, const char *text) {
  char *path = psprintf("%s/%s", datadir, name);
  FILE *file = fopen(path, "w");

  /* Sync, so that a crash after initdb cannot leave the file empty. */
  if (file == NULL || fputs(text, file) == EOF || fflush(file) != 0 ||
      fsync(fileno(file)) != 0 || fclose(file) != 0)
    fatal("could not write file \"%s\": %s", path, strerror(errno));
}

/* Sync the data directory itself, which holds the names of its files. */
static void sync_datadir(void) {
  int fd = open(datadir, O_RDONLY);

  if (fd < 0 || fsync(fd) != 0 || close(fd) != 0)
    fatal("could not sync directory \"%s\": %s", datadir, strerror(errno));
}

/*
 * Make the data directory, or take an empty one that is there, readable
 * by its owner alone.
 */
static void make_datadir(void) {
  switch (pg_check_dir(datadir)) {
  case 0:
    printf("creating directory %s ... ", datadir);
    fflush(stdout);
    if (pg_mkdir_p(pstrdup(datadir), S_IRWXU) != 0)
      fatal("could not create directory \"%s\": %s", datadir, strerror(errno));
    made_datadir = true;
    break;
  case 1:
    printf("fixing permissions on existing directory %s ... ", datadir);
    fflush(stdout);
    if (chmod(datadir, S_IRWXU) != 0)
      fatal("could not change permissions of directory \"%s\": %s", datadir,
            strerror(errno));
    filled_datadir = true;
    break;
  case 2:
  case 3:
  case 4:
    fatal("directory \"%s\" exists but is not empty", datadir);
  default:
    fatal("could not access directory \"%s\": %s", datadir, strerror(errno));
  }
  printf("ok\n");
}

/* Make the directory an sm home directory holding the database "postgres". */
static void make_storage(void) {
  sm_env_t *env = NULL;

  printf("creating storage ... ");
  fflush(stdout);
  /* Lay out the home directory: global, base and the root db. */
  if (sm_init(datadir) != 0)
    fatal("could not initialize storage: %s", sm_errmsg());
  printf("ok\n");

  printf("creating database \"postgres\" ... ");
  fflush(stdout);
  /* Attach, since a database is made through an attached home directory. */
  if (sm_attach(datadir, &env) != 0)
    fatal("could not attach storage: %s", sm_errmsg());
  if (sm_mkdb(env, "postgres", NULL) != 0)
    fatal("could not create database \"postgres\": %s", sm_errmsg());
  /* Detach, which closes the root db so that the server can open it. */
  if (sm_detach(env, 0) != 0)
    fatal("could not detach storage: %s", sm_errmsg());
  printf("ok\n");
}

/*
 * The SCRAM secret of password. Building one reports its failures with
 * ereport(), which is caught here: nothing above would.
 */
static char *build_secret(const char *password) {
  char *secret = NULL;

  PG_TRY();
  { secret = pg_be_scram_build_secret(password); }
  PG_CATCH();
  {
    ErrorData *err = CopyErrorData();

    FlushErrorState();
    fatal("could not build the password's SCRAM secret: %s", err->message);
  }
  PG_END_TRY();
  return secret;
}

/* Run initdb: argv is initdb's own, or the server's with --init first. */
int initdb_main(int argc, char **argv) {
  static thread_local struct option long_options[] = {
      {"pgdata", required_argument, NULL, 'D'},
      {"username", required_argument, NULL, 'U'},
      {"pwprompt", no_argument, NULL, 'W'},
      {"pwfile", required_argument, NULL, 1},
      {"encoding", required_argument, NULL, 'E'},
      {"locale", required_argument, NULL, 2},
      {"no-instructions", no_argument, NULL, 3},
      {"version", no_argument, NULL, 'V'},
      {"help", no_argument, NULL, '?'},
      {NULL, 0, NULL, 0}};
  const char *username = NULL;
  const char *pwfile = NULL;
  const char *owner;
  char *secret = NULL;
  bool pwprompt = false;
  bool instructions = true;
  int c;

  /* "digres --init ..." reads as "initdb ...". */
  if (argc > 1 && strcmp(argv[1], "--init") == 0) {
    argv[1] = pstrdup("initdb");
    argc--;
    argv++;
  }

  /* getopt answers "-?" for an option it does not know, too. */
  if (argc > 1 &&
      (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-?") == 0)) {
    usage();
    return 0;
  }

  while ((c = getopt_long(argc, argv, "D:U:WE:V", long_options, NULL)) != -1) {
    switch (c) {
    case 'D':
      datadir = optarg;
      break;
    case 'U':
      username = optarg;
      break;
    case 'W':
      pwprompt = true;
      break;
    case 1:
      pwfile = optarg;
      break;
    case 'E':
      if (pg_char_to_encoding(optarg) != PG_UTF8)
        fatal("encoding \"%s\" is not supported: digres stores text as UTF8",
              optarg);
      break;
    case 2:
      if (strcmp(optarg, "C") != 0 && pg_strcasecmp(optarg, "C.UTF-8") != 0)
        fatal("locale \"%s\" is not supported: the default collation is C; "
              "name another with COLLATE where it is wanted",
              optarg);
      break;
    case 3:
      instructions = false;
      break;
    case 'V':
      printf("initdb (digres) " PG_VERSION "\n");
      return 0;
    default:
      /* getopt has said which option it did not accept. */
      fprintf(stderr, "Try \"initdb --help\" for more information.\n");
      return 1;
    }
  }

  if (optind < argc && datadir == NULL)
    datadir = argv[optind++];
  if (optind < argc)
    fatal("too many command-line arguments (first is \"%s\")", argv[optind]);
  if (datadir == NULL)
    datadir = getenv("PGDATA");
  if (datadir == NULL || datadir[0] == '\0')
    fatal("no data directory specified: give it with -D or in the "
          "environment variable PGDATA");
  if (pwprompt && pwfile != NULL)
    fatal("password prompt and password file cannot be specified together");

  /* The server refuses to run as root, so a directory root made is no use. */
  if (geteuid() == 0)
    fatal("cannot be run as root");
  owner = get_user_name_or_exit("initdb");
  if (username == NULL)
    username = owner;
  if (username[0] == '\0' || strlen(username) >= NAMEDATALEN ||
      strchr(username, '\n') != NULL)
    fatal("superuser name \"%s\" is not valid", username);
  if (strncmp(username, "pg_", 3) == 0)
    fatal("superuser name \"%s\" is disallowed; role names cannot begin "
          "with \"pg_\"",
          username);

  if (pwprompt || pwfile != NULL)
    secret = build_secret(read_password(pwprompt, pwfile));

  printf("The files belonging to this database system will be owned by "
         "user \"%s\".\n",
         owner);
  printf("This user must also own the server process.\n\n");
  printf("The text encoding is \"UTF8\" and the default collation is "
         "\"C\".\n\n");

  /* Keep every file made from here on from the group and the world. */
  umask(S_IRWXG | S_IRWXO);

  make_datadir();
  make_storage();
  write_file("PG_VERSION", PG_MAJORVERSION "\n");
  write_file(SUPERUSER_FILE,
             psprintf("%s\n%s\n", username, secret ? secret : ""));
  sync_datadir();

  printf("\nSuccess. Local socket connections are allowed to OS user "
         "\"%s\"\nwithout a password.\n",
         username);
  if (secret == NULL)
    printf("TCP connections are not possible: no password was set "
           "(-W, --pwfile).\n");
  else
    printf("TCP connections as \"%s\" need the password given.\n", username);
  if (instructions)
    printf("\nYou can now start the database server using:\n\n"
           "    digres -D %s\n\n",
           datadir);
  return 0;
}

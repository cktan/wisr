# digres

A fork of PostgreSQL that runs on the storage stack in `../sm`.
`../sm/PART4.md` §10 says how it is to be made, and §9 what it
calls.

**It starts, `psql` connects to it, and its tables are sm's.**
`build/initdb DIR` makes a data directory, `build/digres -D
DIR` serves it, and PostgreSQL's own `psql` runs statements on
it. What runs so far, each a step of `WORKNOTE.md`:

- databases and schemas, created and dropped;
- tables of the types sm stores (`../sm/sm2/TYPE.md` §3):
  `bool`, the integers and floats, `numeric` as sm's
  decimals, `text`, `varchar`, `char(n)`, `name`, `bytea`,
  `date`, `time`, `timestamp`, `timestamptz`, `interval` and
  `uuid`; with NOT NULL, DEFAULT, PRIMARY KEY, UNIQUE and
  FOREIGN KEY, SERIAL and identity columns, and indexes on
  them;
- ALTER TABLE's ADD, DROP and RENAME, and CREATE TABLE AS;
- views, temporary ones too;
- temporary tables, with ON COMMIT;
- sequences;
- SELECT ... FOR UPDATE and FOR SHARE, with NOWAIT and SKIP
  LOCKED;
- INSERT ... ON CONFLICT, DO NOTHING and DO UPDATE;
- INSERT, UPDATE and DELETE, with RETURNING; TRUNCATE; COPY
  to and from a table, into an empty one as a bulk load from
  sorted files;
- sequential, index and index-only scans, joins,
  GROUP BY and ORDER BY;
- transactions and savepoints at READ COMMITTED, REPEATABLE
  READ and SERIALIZABLE, which are sm's;
- ANALYZE, and the planner reading its statistics;
- psql's `\d` commands, on catalog relations made from sm's
  catalog and the rows compiled in (`SYSCATALOG.md`).

A statement over constants also runs with no server: `echo
'select 1 + 1;' | build/digres --parse --run`.

`make -j4` generates what is generated, compiles every file as
strict C++ with no `-fpermissive`, and links `build/digres`
with `../sq` and `../sm`. The server has no catalog tables:
its catalog is being replaced, not created (`../sm/PART4.md`
§10).

- `WORKNOTE.md`: the decisions taken along the way, a step
  at a time.
- `CODE.md`: the rules for changing the code.
- `ISSUE.md`: the problems known and left unfixed.
- `UNSUPPORTED.md`: what of PostgreSQL digres refuses, has
  removed, or does otherwise.
- `PLAN.md`: the handover that took digres to one process
  with a thread a session; its §7 holds the decisions of
  record.
- `DGWORK.md`: the first recommendation, written before
  anything linked.
- `SYSCATALOG.md`: the catalog relations psql's `\d`
  commands read; being built.
- `DECIMAL.md` and `DG_DECIMAL_PLAN.md`: `numeric` as sm's
  two decimals; built.
- `STATISTICS.md`: how ANALYZE and the planner use sm's
  statistics.
- `SCHEMACACHE.md`: a session's schema kept across
  transactions; proposed.
- `SHMEM.md`: what is left of PostgreSQL's shared memory,
  locks and signals, and what replaces it.
- `WIP_THREADS.md`: the record of the step that made a
  session a thread, as it was when that step ended.

## Building

Each directory has a Makefile of a few lines that includes
`dg.mk`, which holds the rules; `Makefile` here adds the
generated sources. A file that is not compiled is named in its
directory's `SKIP`, with the reason beside it. The TLS and GSSAPI
files are the ones waiting on a decision.

Two tools made the C compile as C++, and are kept for the code
still to come from PostgreSQL: `src/tools/cxx_keywords.py`
renames identifiers that are C++ keywords, and
`src/tools/cxx_casts.py` writes the casts C does not ask for.
`src/tools/cut_builtins.py` takes built-in functions out of the
catalog data, with every row that refers to them.

## The parser

digres parses with `../sq`, a library of its own: PostgreSQL
18's grammar and scanner, which know nothing of the server.
`make` builds it and links it in. `src/backend/parser/parser.c`
is what joins the two: sq builds its tree in the current memory
context, as digres's own nodes, and the analyzer reads it as it
is. `sq_nodes.c`, which sq's generator writes, holds at compile
time that sq's nodes are digres's, and refuses what digres has
no part of.

`make test` checks it. `build/digres --parse` parses standard
input and prints the trees, with no server started, and sq's
tests hold what it prints to what PostgreSQL's parser printed.

## The catalog

PostgreSQL's catalog is being replaced, a lookup at a time
(`../sm/PART4.md` §10, `WORKNOTE.md` step 6). What is built in —
types, functions, operators — is compiled in:
`src/backend/catalog/gen_cat.py` writes `cat_builtin.c` from the
catalog data, and a caller gets a row with `cat_type(oid)`,
`cat_proc(oid)` and the like, and keeps the pointer.

`build/digres --parse --analyze`, `--plan` and `--run` take a
statement through the analyzer, the planner or the executor with
no server. A lookup that has not been moved to the new rows
stops it, and says which. `make test` runs the statements in
`test/` this way.

## The data directory

`build/initdb DIR`, or `build/digres --init DIR`, makes a data
directory: an sm home directory holding the database
`postgres`, the database `pg_global`, which holds the roles
and which no client connects to (`../sm/ROLE.md`), and a
`PG_VERSION` file. Its one role at first is the superuser,
with the SCRAM secret of its password if one is given.
`initdb` is the server under another name, and `--help` lists
its options, which are few: the encoding is UTF8, the default
collation is C, and authentication is not configured: on the
local socket, peer if the OS user is the role's name, else the
role's password; over TCP, the role's password, by
SCRAM-SHA-256 (`../sm/ROLE.md` §5). PostgreSQL's other options
are rejected (`WORKNOTE.md` step 8).

## The server

`build/digres -D DIR` starts on such a directory, with no
configuration file and no installation, and listens on TCP and
on a local socket (`-p PORT`, `-k SOCKETDIR`). It reads the
superuser and the list of databases from the directory. There
is no `pg_hba.conf`: a local socket connection is admitted if
the OS user is the superuser, and a TCP connection by the
superuser's password. `WORKNOTE.md` step 9 lists what was cut
to get here. What does not work yet fails with an error: a
statement that still looks in a catalog table, and what a
table in sm does not take, such as a column of another type,
MERGE.
`make test` starts a server and connects with `psql`, if there
is one on the path.

## Where it came from

PostgreSQL 18.6, commit `724edf9bde9d356724ad384a2e196edc3c9f80f7`,
copied from its tracked files, not branched from its history.
`COPYRIGHT` is PostgreSQL's notice, which its licence requires
to travel with the code.

## What was copied

| Path | What it is |
| --- | --- |
| `src/backend` | the server |
| `src/include` | its headers, and the catalog data, `catalog/*.dat` |
| `src/common`, `src/port` | code the server and the clients share |
| `src/timezone` | the time zone code |

## What was left out

- Every procedural language (`src/pl`), `contrib`, `ecpg`, the
  tests, the documentation, and every client: `libpq` and `psql`
  came with the copy and were removed, since PostgreSQL's own
  `psql` speaks to digres as it is.
- The time zone compiler `zic` and the time zone data: the
  server reads the system's, in `/usr/share/zoneinfo`.
- From the server: the catalog bootstrap (`bootstrap`), full-text
  search (`snowball`, `tsearch`, and the `tsvector` and `tsquery`
  types), the JIT, replication and base backup, and the index
  methods the stack does not have: `gin`, `gist`, `spgist`,
  `brin`, `hash`. Their callers, headers and built-in functions
  are removed too.
- The grammar and scanner for SQL, `gram.y` and `scan.l`: sq has
  them.
- The translations, and PostgreSQL's build files: autoconf, meson
  and every `Makefile`. digres gets Makefiles of its own.

More is to be cut, a piece at a time.

## Known incompatibilities

Where digres behaves otherwise than PostgreSQL, by choice;
`ISSUE.md` has the bugs.

- **A scan begun inside a savepoint ends at a rollback to
  it.** A FETCH inside a savepoint may begin a scan again, as
  a nested loop's inner index scan begins for each outer row.
  After `ROLLBACK TO` that savepoint, a FETCH that reads on
  from that scan fails with "cannot recreate the old view of
  a scan closed by a rollback to a savepoint" (SQLSTATE
  0A000), where PostgreSQL goes on. sm closes every scan
  begun after a savepoint at a rollback to it, as its view,
  fixed when it began, would still show the rows the rollback
  undid (`../sm/sm1/SM1_DESIGN.md` §6.5), and does not
  recreate it. A cursor whose scans all began before the
  savepoint goes on. `test/slt/point_read.test` shows both.

## Formatting

Every `.c` and `.h` file was reformatted once with the
repository's `.clang-format`, so nothing here lines up with
PostgreSQL's source any more, on purpose: this is a hard fork.

`.clang-format-ignore` lists the headers that are left as
PostgreSQL wrote them. Three Perl generators read them line by
line — `genbki.pl` and `Gen_fmgrtab.pl` the catalog headers,
`gen_node_support.pl` the node headers — and fail on the
reformatted text. They run cleanly on the headers as kept.

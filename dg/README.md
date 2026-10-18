# digres

A fork of PostgreSQL that runs on the storage stack in `../sm`.
`../sm/PART4.md` §10 says how it is to be made, and §9 what it
calls.

**It starts, `psql` connects to it, and its tables are sm's.**
`build/initdb DIR` makes a data directory, `build/digres -D
DIR` serves it, and PostgreSQL's own `psql` runs statements on
it. What runs so far, each a step of `WORKNOTE.md`:

- databases and schemas, created and dropped;
- tables of `int2`, `int4` and `int8` columns, with NOT NULL,
  PRIMARY KEY and UNIQUE, and indexes on them;
- temporary tables, with ON COMMIT;
- sequences;
- SELECT ... FOR UPDATE and FOR SHARE, with NOWAIT and SKIP
  LOCKED;
- INSERT ... ON CONFLICT, DO NOTHING and DO UPDATE;
- INSERT, UPDATE and DELETE, with RETURNING; TRUNCATE; COPY
  to and from a table, into an empty one as a bulk load from
  sorted files;
- sequential, index, index-only and bitmap scans, joins,
  GROUP BY and ORDER BY;
- transactions and savepoints at READ COMMITTED, REPEATABLE
  READ and SERIALIZABLE, which are sm's;
- ANALYZE, and the planner reading its statistics.

A statement over constants also runs with no server: `echo
'select 1 + 1;' | build/digres --parse --run`.

`make -j4` generates what is generated, compiles every file as
strict C++ with no `-fpermissive`, and links `build/digres`
with `../sq` and `../sm`. The server has no catalog tables:
its catalog is being replaced, not created (`../sm/PART4.md`
§10). `DGWORK.md` is the plan, `WORKNOTE.md` the decisions
taken along the way, `CODE.md` the rules for changing the
code, and `ISSUE.md` the problems known and left unfixed.
`SYSCATALOG.md` is a proposal, not built, for making
the catalog tables readable again, so that psql's `\dt` and
`\d` run.

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
is what joins the two, and `sq_convert.c`, which is generated,
turns sq's tree into the nodes the analyzer reads.

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
`postgres`, a `PG_VERSION` file, and a `superuser` file with
the superuser's name and the SCRAM secret of its password.
`initdb` is the server under another name, and `--help` lists
its options, which are few: the encoding is UTF8, the default
collation is C, and authentication is peer on the local socket
and SCRAM-SHA-256 over TCP. PostgreSQL's other options are
rejected (`WORKNOTE.md` step 8).

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

## Formatting

Every `.c` and `.h` file was reformatted once with the
repository's `.clang-format`, so nothing here lines up with
PostgreSQL's source any more, on purpose: this is a hard fork.

`.clang-format-ignore` lists the headers that are left as
PostgreSQL wrote them. Three Perl generators read them line by
line — `genbki.pl` and `Gen_fmgrtab.pl` the catalog headers,
`gen_node_support.pl` the node headers — and fail on the
reformatted text. They run cleanly on the headers as kept.

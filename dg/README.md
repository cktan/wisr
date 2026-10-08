# digres

A fork of PostgreSQL that runs on the storage stack in `../sm`.
`../sm/PART4.md` §10 says how it is to be made, and §9 what it
calls.

**It compiles and does not link yet.** `make -j4` generates what
is generated and compiles every file as strict C++, with no
`-fpermissive`; `make clean` and `make format` do what they say.
Nothing is linked: the server still calls what was left out
below, replication most of all, and 204 built-in functions that
`pg_proc.dat` lists and no file here defines.

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
| `src/interfaces/libpq` | the client library |
| `src/bin/psql`, `src/fe_utils` | the terminal client |
| `src/timezone` | the time zone code and data |

## What was left out

- Every procedural language (`src/pl`), `contrib`, `ecpg`, the
  tests, the documentation, and every client tool but `psql`.
- From the server: the catalog bootstrap (`bootstrap`), full-text
  search (`snowball`, `tsearch`), the JIT, replication and base
  backup, and the index methods the stack does not have: `gin`,
  `gist`, `spgist`, `brin`, `hash`.
- The translations, and PostgreSQL's build files: autoconf, meson
  and every `Makefile`. digres gets Makefiles of its own.

Headers for what was left out are still in `src/include`. More
is to be cut, a piece at a time.

## Formatting

Every `.c` and `.h` file was reformatted once with the
repository's `.clang-format`, so nothing here lines up with
PostgreSQL's source any more, on purpose: this is a hard fork.

`.clang-format-ignore` lists the headers that are left as
PostgreSQL wrote them. Three Perl generators read them line by
line — `genbki.pl` and `Gen_fmgrtab.pl` the catalog headers,
`gen_node_support.pl` the node headers — and fail on the
reformatted text. They run cleanly on the headers as kept.

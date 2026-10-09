# DESIGN — sq, the SQL front end

What sq is, where its edge is and why it is there, and what it
still takes from the server it was cut out of. `README.md` says
how to build and test it, and carries the counts; this document
argues the decisions and does not repeat the lists.

sq is PostgreSQL 18.6's raw parser and the layers under it. The
code is PostgreSQL's, unchanged in what it does. What is
designed here is the cut.

---

## 1. What sq does

```
        SQL text
           │
  ┌────────┴─────────────────────────────────────┐
  │  scanner            scan.l, scansup.c        │
  │  text → tokens; keywords from common/        │
  ├──────────────────────────────────────────────┤
  │  lookahead filter   parser.c                 │
  │  NOT, NULLS, WITH, WITHOUT, FORMAT           │
  │  renamed by the token that follows           │
  ├──────────────────────────────────────────────┤
  │  grammar            gram.y                   │
  │  tokens → one RawStmt a statement            │
  └────────┬─────────────────────────────────────┘
           │
     parse tree, a List of RawStmt
```

- **One entry point**: `raw_parser(text, mode)`, in
  `parser/parser.h`. It returns a `List` of `RawStmt`, one for
  each statement in the text.
- **The output is a node tree**, the same structures the rest of
  PostgreSQL passes about. `nodeToString()` writes one as text,
  and that text is what the tests compare.
- **No catalog is read.** The raw parser knows the grammar and
  nothing about what a name names. `SELECT a FROM t` parses
  whether or not `t` exists; `a` is a `ColumnRef` holding a
  string.
- **A parse is all or nothing.** The whole text is parsed before
  anything is returned, so one error anywhere means no tree for
  any statement in it. `test/err_syntax.sql` holds this.

The modes other than `RAW_PARSE_DEFAULT` parse a type name, or
an expression or assignment for PL/pgSQL. The type name mode
has a caller in `../dg`, `parse_type.c`. The PL/pgSQL ones are
still compiled and nothing asks for them: digres has no
procedural language (`../sm/PART4.md` §10).

## 2. Where the edge is

sq ends at the raw parse tree. The analyzer, which turns that
tree into a `Query`, stays in `../dg`.

- **The raw parser can stand alone.** What it needs is under it:
  nodes, memory, errors, encodings. The link in §5 measures how
  little it reaches above itself.
- **The analyzer cannot.** It is catalog lookups from end to
  end: every name it resolves is a question to the catalog. It
  needs about 300 names from 21 parts of the server
  (`../dg/DGWORK.md`). It moves when there is a catalog
  interface to move it onto, and that waits on sm3's catalog
  being what digres reads (`../sm/PART4.md` §10).
- **The cut has an obvious test**: text in, tree out, compared
  as text. No other piece of the server has one so cheap, which
  is why this was the first carved out.
- **It is known to work.** libpg_query makes the same cut.

So sq is two things in one directory, and only the second is
its purpose:

| Layer | What it is | Why it is in sq |
| --- | --- | --- |
| The base | nodes, memory contexts, error reporting, encodings, hash tables and lists, `common`, `port` | the parser cannot run without it; so can nothing else in digres |
| The front end | scanner, lookahead filter, grammar | what sq is for |

The base is the larger part by far, and digres needs it whether
or not it ever parses. It is in sq because it has to be in
whichever project is lowest, and today that is sq. If a second
project is cut out beside sq, the base becomes its own.

## 3. The layers under the parser

What each is for, as the parser uses it.

- **Nodes** (`backend/nodes`). The tree's structures, their
  constructors, and `copy`, `equal`, `out` and `read` over every
  node type, generated from the headers. sq compiles all of
  them, plan nodes included: the node headers are one closure
  and the generator makes one file of each kind. The link drops
  what the parser does not reach.
- **Memory contexts** (`backend/utils/mmgr`). Every node is
  allocated in the current context. A caller frees a parse tree
  by resetting the context it parsed in, never node by node.
- **Error reporting** (`backend/utils/error`). The scanner and
  grammar raise with `ereport`, which unwinds to the caller's
  `PG_TRY` by `longjmp`. The message and its position are read
  from the error state afterwards.
- **Encodings** (`backend/utils/mb`, `common/wchar.c`). The
  scanner asks how long a character is, and an error's position
  is counted in characters, not bytes. sq is UTF-8 only, as
  digres is; nothing is ever converted.
- **Keywords** (`common/keywords.c`, `kwlookup.c`). The keyword
  list and its perfect hash, shared with whatever else has to
  quote an identifier.

**What a caller must do first** is two calls, and `sqparse`'s
`main` is the whole example:

1. `MemoryContextInit()`, so that there is a context to allocate
   in and an error context to raise through.
2. `SetDatabaseEncoding(PG_UTF8)`, so that the scanner can
   measure a character.

## 4. The sources are compiled in place

sq's `Makefile` compiles the files from `../dg/src` into
`build/`. What is in `sq/` is the boundary: the list of what
belongs to it, `src/host.c`, and the tests.

The alternatives, and why this one:

| Choice | For | Against |
| --- | --- | --- |
| Move the `.c` files, leave the headers | `dg/` gets smaller at once | sq then needs digres's include tree to compile, and digres needs sq's objects: each needs the other |
| Move the `.c` files and every header they include | a real cut | the sources include well over a third of digres's headers, from `storage`, `access`, `catalog`, `executor`; each one is a decision about which side owns it, and several are generated from the catalog data |
| **Compile in place** | the boundary and its tests exist now, and nothing depends the wrong way | nothing has moved; `dg/` is no smaller, and sq cannot be built without `dg/` beside it |

The third was chosen because the dependency's direction is the
thing that is hard to undo. A boundary with tests and an honest
list of what crosses it can be tightened a header at a time. A
move that leaves the two needing each other has to be unpicked
first.

What ends it, in order:

1. Shrink the header closure, until what sq includes is what sq
   owns.
2. Move the sources and those headers here; `../dg` then
   compiles against `sq/include` and links sq's objects.

Until then sq's generated files — `gram.c`, `scan.c`, the node
functions and the node tags — are digres's to make: sq's build
runs `make -C ../dg gen` first.

## 5. The link measures the boundary

Every source is compiled with one section a function
(`-ffunction-sections -fdata-sections`) and linked with
`--gc-sections`. This is a design choice, not an optimisation.

- **Without it** the objects need everything any function in
  them names, reached or not: `dsa.c` wants shared memory,
  `portalmem.c` wants the executor. That list says what the
  files touch, which is not the question.
- **With it** the link keeps what `main` reaches and reports
  only what that needs. The undefined names are then exactly
  what the parser's own path asks of the server. `README.md`
  has both counts; the second is less than half the first.

The cost is that the boundary is drawn by reachability from one
program. A second caller that reaches further — one that prints
errors as JSON, say — will find names `sqparse` never needed.
That is the right answer: it has found a dependency that is
real for it.

## 6. host.c: what still comes from the server

`src/host.c` defines the 32 names that the link in §5 leaves
undefined, each answered as a process with no client, no
transaction and no log file would answer it. It is not a layer.
It is the list of what stands between sq and being a library,
and it should shrink to nothing.

What each group becomes:

| Who asks | Names | What it should become |
| --- | --- | --- |
| Error reporting | 24 | a small context the host fills in — who is asking, where output goes, what level to report — in place of globals read from all over the server |
| Encodings | 4 | nothing: with one encoding there is no conversion to look up, and the calls go |
| The log's formats | 3 | nothing in sq: formatting a log line is the server's work, not the parser's |
| The grammar | 1 | `NameListToString` moves in with the node functions; it reads no catalog and was only filed with it |

- **Error reporting is nearly all of it**, and it is one
  problem, not twenty-four. `elog.c` both raises an error and
  writes the log line, and the second half is what asks for the
  process, the client and the log's settings. Splitting the two
  is the cut: sq keeps the raise, the server keeps the log.
- **The same globals have to become per-thread** for a threaded
  server (`../sm/PART4.md` §2), so that work is owed already.
- `host.c`'s `NameListToString` is a copy of the one in
  `../dg`, the only duplicated code in sq. It goes when the
  original moves.

## 7. Testing

`test/NAME.sql` is run through `build/sqparse` and what it
prints is compared with `test/expected/NAME.out`.

- **The tree's text is the assertion.** Any change to what the
  parser builds shows as a difference, including one nobody
  meant to make: a field added to a node changes every tree that
  has that node.
- **That makes the tests wide and brittle together**, which is
  the trade wanted here. sq's code is not supposed to change
  what it does, so a difference is either an upstream port or a
  mistake, and either should be read.
- **Errors are tested by message and position**, the two things
  a client sees.
- **The expected trees were checked by reading**, not against a
  running PostgreSQL, which does not print the raw tree. They
  are therefore a record of what sq does, verified once by eye,
  and a guard against change after that. A wrong tree accepted
  at the start would still pass.

## 8. Known trade-offs

- **sq is not yet a library.** It builds one program. There is
  no `libsq.a` and no `sq/include`, because of §4.
- **It depends on `dg/` to build**, for the sources and for the
  generated files. The dependency runs one way only, and it is
  the wrong way for where this is going.
- **Errors unwind by `longjmp`.** The fork is to have none
  (`../sm/PART4.md` §2), and sq, compiled as C++, still raises
  as PostgreSQL does. A destructor between a raise and its
  `PG_TRY` does not run. Nothing in sq has one today; the
  decision of what replaces `ereport`'s unwinding is the
  fork's, and sq takes whatever it is.
- **Nothing is per-thread.** The current memory context, the
  error stack and the error state are process globals. Two
  threads cannot parse at once. The scanner and grammar
  themselves are reentrant and keep their state in the call.
- **More is compiled than is used**: plan nodes, `read`
  functions, shared-memory allocators, the PL/pgSQL parse modes.
  The link drops them, so the cost is build time, and trimming
  the lists by hand would have to be redone at each upstream
  port.
- **The base is carried by the wrong owner** (§2).

## 9. What is deliberately not here

- **The analyzer, rewriter and planner.** They need a catalog
  interface that does not exist yet.
- **Deparsing**, a tree back to SQL text. PostgreSQL's
  `ruleutils.c` works on analyzed trees and reads the catalog.
- **A C++ interface.** sq is PostgreSQL's C compiled as C++;
  its callers are PostgreSQL's code. Nothing wraps it.
- **Other encodings**, and the conversion procedures between
  them.

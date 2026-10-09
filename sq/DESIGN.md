# DESIGN — sq, the SQL parser

sq is a library that parses PostgreSQL 18's SQL: text in, a tree
of nodes out. It is what digres parses with. `README.md` says
how to build and test it and `include/sq.h` is the interface;
this document says how it is put together, and why.

sq knows the grammar and nothing else. It reads no catalog, so
it does not know what a name names, and it has no notion of what
its caller supports: every statement PostgreSQL's parser
accepts, sq accepts and builds the whole tree for.

---

## 1. The shape of it

```
        SQL text, and options
           │
  ┌────────┴─────────────────────────────────────┐
  │  scanner            src/scan.l               │
  │  text → tokens; keywords by a perfect hash   │
  ├──────────────────────────────────────────────┤
  │  lookahead filter   src/parser.cc            │
  │  NOT, NULLS, WITH, WITHOUT, FORMAT renamed   │
  │  by the token that follows; U& de-escaped    │
  ├──────────────────────────────────────────────┤
  │  grammar            src/gram.y               │
  │  tokens → nodes, made in the tree            │
  └────────┬─────────────────────────────────────┘
           │
     sq_tree_t: the statements, and every node
     and string they are made of
```

Two calls go down it, `sq_parse` for a list of statements and
`sq_parse_type_name` for one type name. A third, `sq_to_string`,
prints a node as PostgreSQL's `nodeToString()` would.

The interface is C in style, as `../sm/include/sm.h` is: an
`int` result, 0 or -1; the error read back from the calling
thread; names prefixed `sq_`. Three C++ types cross it and no
others: `std::string_view` for text in, `std::string` for text
out, `std::vector` for a node's lists.

## 2. What is PostgreSQL's, and what is sq's

sq is PostgreSQL's parser, kept as close to PostgreSQL's source
as its interface allows, so that a fix upstream can be carried
over by reading two files side by side.

| What | Where | How it relates to PostgreSQL |
| --- | --- | --- |
| The node definitions | `spec/*.h` | PostgreSQL's headers, unedited |
| The keyword list | `spec/kwlist.h` | PostgreSQL's, unedited |
| The scanner | `src/scan.l` | a fork: the rules are PostgreSQL's, untouched |
| The grammar | `src/gram.y` | a fork: the rules are PostgreSQL's, untouched |
| The lookahead filter | `src/parser.cc` | PostgreSQL's `parser.c`, line for line |
| Node constructors | `src/makefuncs.cc` | PostgreSQL's `makefuncs.c`, renamed |
| The nodes, their printer, copy and equality | generated | written by `gen/gen_nodes.py` from `spec/` |
| The keyword hash | generated | `gen/gen_keywords.py`: PostgreSQL's `PerfectHash.pm` in Python |
| digres's converter | generated, into `../dg` | by `gen/gen_nodes.py`: sq's nodes to PostgreSQL's own (§11) |
| The tree, errors, lists, UTF-8 | `src/tree.cc`, `error.cc`, `list.cc`, `utf8.cc` | sq's own |

What a fork changes is the code around the rules, never a rule:

- A node is one of sq's. `SelectStmt` is `sq_select_stmt_t`,
  its `targetList` is `target_list`, and the helpers have names
  to match.
- An error is raised sq's way (§5), not with `ereport`.
- Everything is made in the tree being built (§4), where
  PostgreSQL allocates in its current memory context.
- The text is UTF-8. PostgreSQL asks what the encoding is; sq
  never does.

**Porting to a later PostgreSQL** is, in order: replace the
headers in `spec/` and run `make nodes`; carry the changes to
`scan.l`, `gram.y`, `parser.c` and `makefuncs.c` into the forks
by hand; make new expected files with that PostgreSQL (§8).

## 3. The tree

`include/sq_nodes.h` has a struct for each node PostgreSQL's
grammar can build, 216 of them, and it is generated. The
generator reads PostgreSQL's own headers, the way PostgreSQL's
`gen_node_support.pl` does, so that a node's fields, their order
and what each is are PostgreSQL's by construction and not by
transcription. The header is committed, so that it can be read,
and the tests fail if it is not what the generator writes.

What changes on the way from PostgreSQL's struct to sq's:

- **Names.** Types end `_t` and carry `sq_`; fields are
  snake_case; an enum's values carry `SQ_`, which keeps them
  apart from PostgreSQL's own where digres includes both.
- **A list is a `std::vector<sq_node_t*>`**, empty where
  PostgreSQL has `NIL`. PostgreSQL also puts a list where any
  node may go — an element of another list, a field typed
  `Node *`. There it is an `sq_list_t`, a node holding a vector.
- **Text is a `const char*`**, owned by the tree. NULL is
  absent, which PostgreSQL tells apart from empty: `COMMENT ...
  IS NULL` and `IS ''` are different trees.
- **Every node derives from `sq_node_t`**, which holds the tag,
  and sets its own tag when made.
- **An `sq_a_const_t` points at its value node**; PostgreSQL's
  holds a union of them.

Fields that only PostgreSQL's analyzer fills are kept, zero or
NULL from the parser: a node is PostgreSQL's node, whole.

**The printer writes PostgreSQL's names**, not sq's:
`sq_select_stmt_t` prints as `{SELECTSTMT :distinctClause ...`.
Its text is `nodeToString()`'s byte for byte, with locations
hidden as PostgreSQL hides them. That is what makes PostgreSQL's
own output a test of sq (§8), and it is what the printer is for.

## 4. Ownership

**A tree owns everything.** `sq_tree_t` keeps a store of every
node and every string made for it and frees them all in its
destructor. Nothing in a tree is freed by hand, and nothing
points outside it: text is copied in, so a tree outlives what it
was parsed from. A tree moves and does not copy.

This is what makes cleanup after an error one line. A parse that
fails halfway has made hundreds of nodes, some reachable from
bison's stack and some from nowhere. The tree owns them all the
same, and emptying it frees them.

- **The grammar's actions make nodes in "the current tree"**, a
  per-thread pointer that `sq_parse` sets for as long as it
  runs. PostgreSQL's actions do the same with its current memory
  context. The alternative, passing the tree to each of some 800
  calls, would have rewritten every action and gained nothing: a
  parse is on one thread and builds one tree.
- **bison's stacks** come from the parse, not from `malloc`, and
  go with it. bison would free its own on a normal return, which
  an error raised from an action is not.
- **flex's state** is held by `scanner_t`, whose destructor
  releases it however the scan ends.

So RAII reaches everything, and `sq.h` has no free function.

## 5. Errors

A failed call returns -1 and leaves the error for the calling
thread to read: a code, a message, PostgreSQL's SQLSTATE, the
position in characters, and PostgreSQL's detail and hint where
it has them. A server sends those to its client as they are.

- **Three codes.** `SQ_ERR_PARSE` is the SQL's fault, and the
  SQLSTATE says how; `SQ_ERR_NOMEM` and `SQ_ERR_EXCEPTION` are
  not.
- **The message is formatted into a fixed per-thread buffer**,
  with no allocation, so that running out of memory can be
  reported.
- **Inside a parse, an error is a private exception.** A raise
  records the thread's error and throws; the call catches it
  and returns -1. PostgreSQL leaves from wherever it is by
  `longjmp`, and its helpers are written to: this keeps them the
  shape they are upstream. flex's own fatal path, which would
  call `exit()`, leaves the same way. No exception crosses the
  interface.
- **A notice is not an error.** An identifier cut to 63 bytes,
  or a warning about escapes, goes to the caller's hook as it
  happens and the parse goes on. With no hook it is dropped.

**Settings come in `sq_options_t`**: PostgreSQL's three that
change how a string literal is read, and the notice hook. NULL
is PostgreSQL's defaults.

## 6. Lists in the grammar

A node's list is a vector, but the grammar builds lists
piecemeal across many rules, passes them about, appends to them
and nests them, as PostgreSQL does with `List *`. In the grammar
a list is therefore a handle, `list_ref_t`: NIL, or the name of
an `sq_list_t` in the tree. PostgreSQL's list functions are kept
over it by their own names, so an action reads as it does
upstream.

**Assigning a handle to a node's field copies the list** into
the field's vector. The handle can go on being used; the node
has its own.

## 7. Threads

Any number of threads may parse at once, each into its own tree.
Nothing is shared between two parses: the scanner and grammar
keep their state in the call, and the three things that are per
thread — the error, the current tree, and a notice hook's
arguments — are thread-local or the caller's.

A tree, once made, may be read from any number of threads.

## 8. How it is known to be right

sq's claim is that it parses as PostgreSQL does, so the test is
PostgreSQL: what its scanner and parser print for a text is what
sq must print.

Until sq could stand alone it was built beside PostgreSQL's own
parser, compiled from `../dg`, and compared with it directly.
That parser is no longer built here. Its answers are:

- **`test/*/expected/`**: for each SQL file here, what
  PostgreSQL's parser printed — the trees, or the error with its
  position, detail and hint — and for some, what its scanner
  printed. 93 files: statements of every family, one for each
  error the scanner raises, one for each error an action of the
  grammar raises, and type names.
- **`test/regress.digest`**: for each of the 233 scripts of
  PostgreSQL's regression SQL, a digest of what its scanner
  printed for the whole script, and of what its parser printed
  for each of the script's pieces. Over 700,000 tokens and
  49,034 pieces, which include the SQL that is wrong on purpose.
  `make corpus-test` holds sq to it, given the scripts.

Neither can be made again without PostgreSQL's parser. They are
to be remade when sq is ported to a later one, with that one.

What PostgreSQL cannot show is tested by hand, in `test/*.cc`:
the notice hook, the options, positions in characters,
ownership, and that nothing leaks. Those tests are linked with a
leak checker, so a node or string a tree failed to free fails
them; `make tsan` runs them under the thread sanitizer.

Two checks are on sq's own consistency: `test/nodes.sh`, that
the committed node header is what the generator writes; and
`test/alone.sh`, that nothing sq is built from is outside `sq/`.

## 9. Known trade-offs

- **A tree holds more than its nodes need.** A list assigned to
  a field is copied (§6), and the list it came from stays in the
  tree until the tree is freed, as do the scanner's strings that
  no node kept. Moving a list where it is not used again would
  recover most of it, a rule at a time.
- **A field's list and the handle it was copied from are
  separate.** An action that appended to a list after assigning
  it would, in PostgreSQL, change the node, and here would not.
  None was found that does, and the comparison in §8 agrees, but
  the compiler cannot show it.
- **Text is assumed to be UTF-8 and is not checked.** Text that
  is not is scanned as PostgreSQL's scanner would scan it; a
  server is expected to have checked what a client sent, as
  PostgreSQL does before it parses. A NUL byte is refused.
- **The forks are forks.** A change upstream is carried over by
  hand. What sq changed is confined to the code around the
  rules, which is what keeps that tractable.
- **PostgreSQL's naming survives in two places**: the grammar's
  symbols, which are grammar and not code, and the list
  functions, kept by PostgreSQL's names so that actions read as
  they do upstream.
- **An `Oid` field is 32 bits**, as in PostgreSQL; digres's is
  to be 64 (`../sm/PART4.md` §10). Every such field in a parse
  tree is the analyzer's to fill, and zero from sq.

## 10. What is deliberately not here

- **Analysis.** Turning a tree into a query is catalog lookups
  from end to end. It is digres's.
- **Any notion of "supported".** sq parses `CREATE FUNCTION` as
  it parses anything. digres refuses what it does not handle,
  when it meets the node.
- **PostgreSQL's nodes.** digres's analyzer reads PostgreSQL's
  own node structs. sq does not know them: turning its tree into
  those is digres's (§11).
- **Deparsing**, a tree back to SQL text.
- **Other encodings**, and the PL/pgSQL parse modes: their rules
  are in the grammar, as they are in PostgreSQL's, with no call
  that reaches them.

## 11. How digres uses it

digres links `build/libsq.a` and has no grammar or scanner for
SQL of its own. Its `raw_parser()` is sq's one caller there
(`../dg/src/backend/parser/parser.c`):

- It passes the session's three settings in `sq_options_t`.
- It turns sq's tree into PostgreSQL's nodes with a converter,
  `sq_convert.c`. The converter is digres's code, and the only
  code that names both sets of nodes; sq's generator writes it,
  when digres is built, because it is the same walk over the
  same fields as the printer.
- It raises sq's error as the server's own, with its SQLSTATE,
  position, detail and hint.

Two things there follow from sq being C++ under a server that
leaves an error by `longjmp`, which runs no destructor:

- **The tree is deleted by hand**, in a `PG_FINALLY`, after it
  has been converted.
- **A notice is kept and reported after sq returns.** Reported
  from sq's hook, an error raised while reporting would jump out
  through sq.

digres is checked with sq's own tests. `postgres --parse` prints
what `build/sq_parse` prints, for `raw_parser()`, so the
expected files and digests of §8 hold digres's parser, converter
included, to what PostgreSQL's printed. They do not hold it to
the locations in a tree, which the printed text hides. Those
were compared once, PostgreSQL's parser against sq and the
converter in one process, over every text of §8, before
PostgreSQL's parser was removed from digres.

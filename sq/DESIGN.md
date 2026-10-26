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
out, `std::vector` for a tree's statements. A node's list is
PostgreSQL's `List`, under sq's name (§3).

## 2. What is PostgreSQL's, and what is sq's

sq is PostgreSQL's parser, kept as close to PostgreSQL's source
as its interface allows, so that a fix upstream can be carried
over by reading two files side by side.

| What | Where | How it relates to PostgreSQL |
| --- | --- | --- |
| The node definitions | `spec/*.h` | PostgreSQL's headers, unedited, but `nodetags.h`, digres's copy (§3) |
| The keyword list | `spec/kwlist.h` | PostgreSQL's, unedited |
| The scanner | `src/scan.l` | a fork: the rules are PostgreSQL's, untouched |
| The grammar | `src/gram.y` | a fork: the rules are PostgreSQL's, untouched |
| The lookahead filter | `src/parser.cc` | PostgreSQL's `parser.c`, line for line |
| Node constructors | `src/makefuncs.cc` | PostgreSQL's `makefuncs.c`, renamed |
| The nodes, their printer, copy and equality | generated | written by `gen/gen_nodes.py` from `spec/` |
| The keyword hash | generated | `gen/gen_keywords.py`: PostgreSQL's `PerfectHash.pm` in Python |
| digres's checks | generated, into `../dg` | by `gen/gen_nodes.py`: that sq's nodes are digres's, and what digres refuses (§11) |
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
- **A list is an `sq_list_t *`**, PostgreSQL's `List` under
  sq's name, NULL where PostgreSQL has `NIL`. A list is itself
  a node, and stands where any node may go — an element of
  another list, a field typed `Node *`.
- **Text is a `const char*`**, owned by the tree. NULL is
  absent, which PostgreSQL tells apart from empty: `COMMENT ...
  IS NULL` and `IS ''` are different trees.
- **Every node derives from `sq_node_t`**, which holds the tag,
  and sets its own tag when made; but one whose first field is
  a node, as `CreateForeignTableStmt`'s is a `CreateStmt`,
  holds its tag there, as PostgreSQL's does.

Fields that only PostgreSQL's analyzer fills are kept, zero or
NULL from the parser: a node is PostgreSQL's node, whole.

**A node is digres's, byte for byte.** Its fields are
PostgreSQL's in PostgreSQL's order, an `A_Const` holds its value
in place in PostgreSQL's union, and its tag is digres's number
for it, from `spec/nodetags.h`, a copy of digres's own (a node
digres has none of takes a number past its last). digres so
reads sq's tree as its own nodes, with nothing converted, and
holds that at compile time (§11).

**The printer writes PostgreSQL's names**, not sq's:
`sq_select_stmt_t` prints as `{SELECTSTMT :distinctClause ...`.
Its text is `nodeToString()`'s byte for byte, with locations
hidden as PostgreSQL hides them, or shown, as its
`nodeToStringWithLocations()` shows them. That is what makes
PostgreSQL's own output a test of sq (§8), and it is what the
printer is for.

## 4. Ownership

**A tree owns everything.** `sq_tree_t` keeps a store of every
node and every string made for it and frees them all in its
destructor. Nothing in a tree is freed by hand, and nothing
points outside it: text is copied in, so a tree outlives what it
was parsed from. A tree moves and does not copy.

**Nodes, lists and text are cut from the tree's arena**, as
PostgreSQL's `palloc` cuts from a memory context: blocks, the
first of 4 kB made with the first piece, each next twice the
last up to 1 MB, freed together. No node has a destructor. A list grows as
PostgreSQL's `list.c` grows one: its first cells in place after
it, then an array of its own, grown by realloc. A parse so makes
no allocation per node, list or string, which made its cost
twice PostgreSQL's. Whatever changes a list runs in a parse, or
with a tree named current.

**Or they go where the caller says.** `sq_options_t`'s
allocator, alloc and realloc, takes the arena's place: digres's
is its current memory context, so it keeps, grows and frees
sq's nodes and lists as its own. The store itself is cut from
it too, and no arena block is made, so a parse asks `malloc`
for none of its nodes. What the allocator hands out the tree
never frees. A failed alloc returns NULL, and sq reports running
out of memory as it does any error.

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
- **flex's state** is held by `scanner_t`, in room inside it,
  as is the copy of the text flex scans when the text is short:
  a scan asks `malloc` for none of it. Its destructor releases
  what a long text took, however the scan ends.

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
change how a string literal is read, the notice hook, and the
allocator (§4). NULL is PostgreSQL's defaults.

## 6. Lists in the grammar

A node's list is an `sq_list_t *`, and the grammar builds lists
piecemeal across many rules, passes them about, appends to them
and nests them, as PostgreSQL does with `List *`. In the grammar
a list is therefore a handle, `list_ref_t`: NIL, or the name of
an `sq_list_t` in the tree. PostgreSQL's list functions are kept
over it by their own names, so an action reads as it does
upstream.

**Assigning a handle to a node's field shares the list**, as
PostgreSQL's assignment shares a `List *`: an action written
upstream reads the same here.

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
  position, detail and hint — and for some, the trees again with
  every location shown, and what its scanner printed. 93 files:
  statements of every family, one for each error the scanner
  raises, one for each error an action of the grammar raises,
  and type names.
- **`test/regress.digest`**: for each of the 233 scripts of
  PostgreSQL's regression SQL, a digest of what its scanner
  printed for the whole script, of what its parser printed for
  each of the script's pieces, and of the same trees with their
  locations. Over 700,000 tokens and 49,034 pieces, which
  include the SQL that is wrong on purpose. `make corpus-test`
  holds sq to it, given the scripts.

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

- **A tree holds a little more than its nodes need**: a list's
  cells outgrown, in place or in an array the arena does not
  give back, and the scanner's strings no node kept, until the
  tree goes. In digres's memory context, a realloc gives an
  outgrown array back.
- **sq's tags are digres's.** A node that digres adds or drops
  before the value nodes in its headers moves their numbers;
  digres's build then fails its checks until `spec/nodetags.h`
  is copied over again and `make nodes` run.
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
- **PostgreSQL's headers.** sq's nodes are laid out as
  digres's, but sq includes none of digres's headers; the
  layout comes from the generator, and digres checks it (§11).
- **Deparsing**, a tree back to SQL text.
- **Other encodings**, and the PL/pgSQL parse modes: their rules
  are in the grammar, as they are in PostgreSQL's, with no call
  that reaches them.

## 11. How digres uses it

digres links `build/libsq.a` and has no grammar or scanner for
SQL of its own. Its `raw_parser()` is sq's one caller there
(`../dg/src/backend/parser/parser.c`):

- It passes the session's three settings in `sq_options_t`,
  and an allocator that is its current memory context, by
  `palloc` and `repalloc` that return NULL when out of memory.
- It hands sq's statements to the analyzer as they are: they
  are digres's nodes. `sq_nodes.c`, which sq's generator writes
  when digres is built, holds that at compile time, a
  `static_assert` for each node's tag and size, each field's
  offset and each enum's value; and refuses what digres has no
  part of: a publication's or subscription's statement, and any
  statement naming one as an object (`REFUSED` and
  `REFUSED_OBJECTS` in `gen/gen_nodes.py`), in a statement or a
  SQL function's body.
- It raises sq's error as the server's own, with its SQLSTATE,
  position, detail and hint.

digres raises an error as a C++ exception, so the tree is an
ordinary object there, freed by its destructor however
`raw_parser()` leaves; its nodes stay in the memory context.

- **A notice is kept and reported after sq returns.** Reported
  from sq's hook, an error raised while reporting would be an
  exception inside sq, which catches everything and would report
  it as its own.

digres is checked with sq's own tests. `digres --parse` prints
what `build/sq_parse` prints, for `raw_parser()`, so the
expected files and digests of §8 hold digres's parser to what
PostgreSQL's printed, locations among it.

# DESIGN — sq, the SQL parser

sq is a library that parses PostgreSQL 18's SQL: text goes in,
and a tree of nodes comes out. digres parses with it.

- [`README.md`](README.md) says how to build and test it.
- `include/sq.h` is the interface.
- This document says how sq is put together, and why.

sq knows the grammar and nothing else. It reads no catalog, so it
does not know what a name refers to. It also has no notion of
what its caller supports. Every statement that PostgreSQL's
parser accepts, sq accepts, and builds the whole tree for. sq
accepts one thing more: digres's `ON UPDATE` after a column's
default (§2).

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

Two calls go down this path:

- `sq_parse` parses a list of statements.
- `sq_parse_type_name` parses one type name.

A third call, `sq_to_string`, prints a node as PostgreSQL's
`nodeToString()` would.

The interface is C in style, as `../sm/include/sm.h` is:

- a call returns an `int`, 0 or -1;
- the calling thread reads the error back;
- names start with `sq_`.

Three C++ types cross the interface, and no others:

- `std::string_view`, for text in;
- `std::string`, for text out;
- `std::vector`, for a tree's statements.

A node's list is PostgreSQL's `List`, under sq's name (§3).

## 2. What is PostgreSQL's, and what is sq's

sq is PostgreSQL's parser, kept as close to PostgreSQL's source
as its interface allows. So a fix upstream can be carried over
by reading two files side by side.

| What | Where | How it relates to PostgreSQL |
| --- | --- | --- |
| The node definitions | `spec/*.h` | PostgreSQL's headers, unedited, except `nodetags.h`, which is digres's copy (§3) |
| The keyword list | `spec/kwlist.h` | PostgreSQL's, unedited |
| The scanner | `src/scan.l` | a fork; the rules are PostgreSQL's, untouched |
| The grammar | `src/gram.y` | a fork; the rules are PostgreSQL's, untouched |
| The lookahead filter | `src/parser.cc` | PostgreSQL's `parser.c`, line for line |
| Node constructors | `src/makefuncs.cc` | PostgreSQL's `makefuncs.c`, renamed |
| The nodes, their printer, copy and equality | generated | written by `gen/gen_nodes.py` from `spec/` |
| The keyword hash | generated | `gen/gen_keywords.py`: PostgreSQL's `PerfectHash.pm` in Python |
| digres's checks | generated, into `../dg` | by `gen/gen_nodes.py`: checks that sq's nodes are digres's, and what digres refuses (§11) |
| The tree, errors, lists, UTF-8 | `src/tree.cc`, `error.cc`, `list.cc`, `utf8.cc` | sq's own |

A fork changes the code around the rules, never a rule:

- A node is one of sq's. `SelectStmt` is `sq_select_stmt_t`, its
  `targetList` is `target_list`, and the helpers have matching
  names.
- An error is raised sq's way (§5), not with `ereport`.
- Everything is made in the tree being built (§4). PostgreSQL
  allocates in its current memory context instead.
- The text is UTF-8. PostgreSQL asks what the encoding is; sq
  never does.

**One rule is digres's, not PostgreSQL's**: `DEFAULT b_expr ON
UPDATE` in `ColConstraintElem`. This is a column's update
default: what an UPDATE writes in the column when it does not
assign it ([`../dg/ONUPDATE.md`](../dg/ONUPDATE.md)).

- The rule makes the same `Constraint` that `DEFAULT b_expr`
  makes, with `generated_when` set to `'u'`. A default does not
  otherwise use that field, so no node changes.
- It needs no token of its own, and no change to the lookahead
  filter. After a default, nothing in PostgreSQL's grammar
  continues with `ON`.
- A port carries the rule over, as it carries the forks.

**To port sq to a later PostgreSQL:**

1. Replace the headers in `spec/`, and run `make nodes`.
2. Carry the changes to `scan.l`, `gram.y`, `parser.c` and
   `makefuncs.c` into the forks, by hand.
3. Make new expected files with that PostgreSQL (§8).

## 3. The tree

`include/sq_nodes.h` has a struct for each node that
PostgreSQL's grammar can build: 216 of them. The header is
generated. The generator reads PostgreSQL's own headers, as
PostgreSQL's `gen_node_support.pl` does. So a node's fields,
their order and their types are PostgreSQL's by construction,
not by copying them by hand. The header is committed, so it can
be read, and the tests fail if it differs from what the
generator writes.

These things change from PostgreSQL's struct to sq's:

- **Names.** Types start with `sq_` and end with `_t`. Fields
  are snake_case. An enum's values start with `SQ_`, which keeps
  them apart from PostgreSQL's own where digres includes both.
- **A list is an `sq_list_t *`**: PostgreSQL's `List` under sq's
  name. It is NULL where PostgreSQL has `NIL`. A list is itself
  a node, and can stand where any node can: as an element of
  another list, or in a field of type `Node *`.
- **Text is a `const char*`**, owned by the tree. NULL means
  absent, which PostgreSQL tells apart from empty: `COMMENT ...
  IS NULL` and `IS ''` are different trees.
- **Every node derives from `sq_node_t`**, which holds the tag.
  Each node sets its own tag when it is made. The exception is a
  node whose first field is a node, as `CreateForeignTableStmt`'s
  first field is a `CreateStmt`. That node holds its tag in the
  first field, as PostgreSQL's does.

sq keeps the fields that only PostgreSQL's analyzer fills. The
parser leaves them zero or NULL. A node is PostgreSQL's node,
whole.

**A node is digres's, byte for byte.**

- Its fields are PostgreSQL's, in PostgreSQL's order.
- An `A_Const` holds its value in place, in PostgreSQL's union.
- Its tag is digres's number for it, from `spec/nodetags.h`,
  which is a copy of digres's own. A node that digres does not
  have takes a number after digres's last one.

So digres reads sq's tree as its own nodes, with nothing
converted. digres checks this at compile time (§11).

**The printer writes PostgreSQL's names**, not sq's:
`sq_select_stmt_t` prints as `{SELECTSTMT :distinctClause ...`.
Its text is `nodeToString()`'s, byte for byte. Locations are
hidden as PostgreSQL hides them, or shown as PostgreSQL's
`nodeToStringWithLocations()` shows them. This makes
PostgreSQL's own output a test of sq (§8), which is what the
printer is for.

## 4. Ownership

**A tree owns everything.** `sq_tree_t` keeps a store of every
node and every string made for it, and frees them all in its
destructor. Nothing in a tree is freed by hand, and nothing
points outside it. Text is copied in, so a tree outlives the
text it was parsed from. A tree can be moved, but not copied.

**Nodes, lists and text are cut from the tree's arena**, as
PostgreSQL's `palloc` cuts them from a memory context.

- The arena is a series of blocks, freed together. The first
  block is 4 kB, made with the first piece. Each next block is
  twice the last, up to 1 MB.
- No node has a destructor.
- A list grows as PostgreSQL's `list.c` grows one. Its first
  cells are in place after it. Then it gets an array of its own,
  grown by realloc.
- So a parse makes no allocation for each node, list or string.
  Those allocations made a parse cost twice PostgreSQL's.
- Code that changes a list runs in a parse, or with a tree set
  as the current tree.

**Or they go where the caller says.** The allocator in
`sq_options_t`, an alloc and a realloc, takes the place of the
arena.

- digres's allocator is its current memory context. So digres
  keeps, grows and frees sq's nodes and lists as its own.
- The tree's store is also cut from the allocator, and no arena
  block is made. So a parse asks `malloc` for none of its nodes.
- The tree never frees what the allocator gives out.
- A failed alloc returns NULL. sq reports running out of memory
  as it reports any error.

**WARNING: do not put digres's nodes in an arena.** digres needs
the allocator, because PostgreSQL does not treat the tree as
read-only. A branch that removed the allocator, `wip-sq-arena`,
was dropped on 2026-11-04, for these reasons:

- **The tree is not const.** `sq_parse()` gives the caller a tree
  that it may change, and digres's analyzer changes it.
- **Each node and each list's array must be `palloc` memory.**
  PostgreSQL grows and frees them itself: `list.c` calls
  `repalloc`, `pfree` and `GetMemoryChunkContext()`. Each of
  these reads the chunk header in front of the pointer. A piece
  cut from an arena has no chunk header, even if the arena's
  block came from `palloc`.
- **PostgreSQL writes into the nodes.** Parse analysis writes in
  them. `parse_utilcmd.c` appends to the `constraints` list of a
  `ColumnDef` that the grammar made. `plancache.c` copies a raw
  tree, because "the parser tends to scribble on its input".
- **PostgreSQL copies the nodes.** `copyObject()` copies a raw
  tree, or a part of one, into the current memory context, as
  `plancache.c` does. So each node must have the tag and the
  layout that PostgreSQL's copy functions expect.

The arena is for a caller that only reads the tree, as sq's tools
and tests do.

**Ownership makes cleanup after an error one line.** A parse that
fails halfway has made hundreds of nodes. Some are reachable from
bison's stack, and some from nowhere. The tree owns them all the
same, and emptying the tree frees them. Three things make this
work:

- **The grammar's actions make nodes in "the current tree"**: a
  per-thread pointer that `sq_parse` sets while it runs.
  PostgreSQL's actions do the same with its current memory
  context. The alternative was to pass the tree to each of about
  800 calls. That would have rewritten every action and gained
  nothing, because a parse runs on one thread and builds one
  tree.
- **bison's stacks** come from the parse, not from `malloc`, and
  go with it. bison frees its own stacks only on a normal return,
  and an error raised from an action is not one.
- **flex's state** is held in room inside `scanner_t`. So is
  flex's copy of the text, when the text is short. So a scan asks
  `malloc` for none of it. The destructor of `scanner_t` releases
  what a long text took, however the scan ends.

So RAII reaches everything, and `sq.h` has no free function.

## 5. Errors

A failed call returns -1, and leaves the error for the calling
thread to read. The error has:

- a code;
- a message;
- PostgreSQL's SQLSTATE;
- the position, in characters;
- PostgreSQL's detail and hint, where it has them.

A server sends these to its client as they are.

- **Three codes.** `SQ_ERR_PARSE` means the SQL is at fault, and
  the SQLSTATE says how. `SQ_ERR_NOMEM` and `SQ_ERR_EXCEPTION`
  are not the SQL's fault.
- **The message is formatted into a fixed per-thread buffer**,
  with no allocation, so that sq can report running out of
  memory.
- **Inside a parse, an error is a private exception.**
  1. A raise records the thread's error, and throws.
  2. The call catches the exception, and returns -1.

  PostgreSQL leaves from wherever it is by `longjmp`, and its
  helpers are written for that. The exception keeps them in the
  shape they have upstream. flex's own fatal path, which would
  call `exit()`, leaves the same way. No exception crosses the
  interface.
- **A notice is not an error.** A notice is, for example, an
  identifier cut to 63 bytes, or a warning about escapes. It
  goes to the caller's hook as it happens, and the parse goes on.
  With no hook, the notice is dropped.

**Settings come in `sq_options_t`**: PostgreSQL's three settings
that change how a string literal is read, the notice hook, and
the allocator (§4). NULL means PostgreSQL's defaults.

## 6. Lists in the grammar

A node's list is an `sq_list_t *`. The grammar builds lists
piece by piece across many rules. It passes them around, appends
to them and nests them, as PostgreSQL does with `List *`. So in
the grammar, a list is a handle, `list_ref_t`: NIL, or the name
of an `sq_list_t` in the tree. PostgreSQL's list functions are
kept over the handle, by their own names, so an action reads as
it does upstream.

**Assigning a handle to a node's field shares the list**, as
PostgreSQL's assignment shares a `List *`. So an action written
upstream reads the same here.

## 7. Threads

Any number of threads may parse at once, each into its own tree.
Two parses share nothing. The scanner and grammar keep their
state in the call. Three things are per thread: the error, the
current tree, and a notice hook's arguments. Each of them is
thread-local, or belongs to the caller.

A tree, once made, may be read from any number of threads.

## 8. How it is known to be right

sq claims to parse as PostgreSQL does. So the test is
PostgreSQL: what PostgreSQL's scanner and parser print for a
text is what sq must print.

The one rule of digres's (§2) is the exception.
`test/on_update.sql` and the two `test/err_on_update_*.sql`
files expect sq's own output. The corpus below shows that
PostgreSQL's text still parses as it did.

Until sq could stand alone, it was built beside PostgreSQL's own
parser, compiled from `../dg`, and compared with it directly.
That parser is no longer built here. Its answers are kept:

- **`test/*/expected/`**: for each SQL file here, what
  PostgreSQL's parser printed: the trees, or the error with its
  position, detail and hint. For some files, it also holds the
  trees again with every location shown, and what PostgreSQL's
  scanner printed. There are 96 files: statements of every
  family, one for each error the scanner raises, one for each
  error that a grammar action raises, and type names.
- **`test/regress.digest`**: for each of the 233 scripts of
  PostgreSQL's regression SQL, three digests:
  - of what PostgreSQL's scanner printed for the whole script;
  - of what its parser printed for each piece of the script;
  - of the same trees, with their locations.

  That is over 700,000 tokens and 49,034 pieces, including the
  SQL that is wrong on purpose. Given the scripts,
  `make corpus-test` holds sq to the digests.
- **Locations.** `sq_to_string_with_locations` prints them.
  `expected/*.loc`, and the third digest in
  `test/regress.digest`, hold what PostgreSQL's parser gave, for
  the 28 files of statements here and the 49,034 pieces.
  PostgreSQL's parser was built again from this repository's
  history to write them.

Neither the expected files nor the digests can be made again
without PostgreSQL's parser. When sq is ported to a later
PostgreSQL, they are to be made again with that one.

What PostgreSQL cannot show is tested by hand, in `test/*.cc`:
the notice hook, the options, positions in characters,
ownership, and that nothing leaks. Those tests are linked with a
leak checker, so a node or string that a tree failed to free
fails them. `make tsan` runs them under the thread sanitizer.

Two checks cover sq's own consistency:

- `test/nodes.sh` checks that the committed node header is what
  the generator writes.
- `test/alone.sh` checks that nothing sq is built from is
  outside `sq/`.

## 9. Known trade-offs

- **A tree holds a little more memory than its nodes need.** It
  keeps a list's outgrown cells, in place or in an array that
  the arena does not give back. It also keeps the scanner's
  strings that no node kept. Both stay until the tree goes. In
  digres's memory context, a realloc gives an outgrown array
  back.
- **sq's tags are digres's.** If digres adds or drops a node
  before the value nodes in its headers, their numbers move.
  digres's build then fails its checks, until `spec/nodetags.h`
  is copied over again and `make nodes` is run.
- **Text is assumed to be UTF-8, and is not checked.** Text that
  is not UTF-8 is scanned as PostgreSQL's scanner would scan it.
  A server is expected to check what a client sent, as
  PostgreSQL does before it parses. A NUL byte is refused.
- **The forks are forks.** A change upstream is carried over by
  hand. sq's changes are confined to the code around the rules,
  which keeps this manageable. The one exception is digres's
  rule (§2).
- **PostgreSQL's naming survives in two places**: the grammar's
  symbols, which are grammar and not code; and the list
  functions, which keep PostgreSQL's names so that actions read
  as they do upstream.
- **An `Oid` field is 32 bits**, as in PostgreSQL. digres's is
  to be 64 bits
  ([`../sm/PART4.md`](../sm/PART4.md#10-digres-how-the-fork-is-to-be-made)
  §10). The analyzer fills every such field in a parse tree, and
  sq leaves them zero.

## 10. What is deliberately not here

- **Analysis.** Turning a tree into a query is catalog lookups
  from end to end. It belongs to digres.
- **Any notion of "supported".** sq parses `CREATE FUNCTION` as
  it parses anything else. digres refuses what it does not
  handle, when it meets the node.
- **PostgreSQL's headers.** sq's nodes are laid out as digres's,
  but sq includes none of digres's headers. The layout comes from
  the generator, and digres checks it (§11).
- **Deparsing**: turning a tree back into SQL text.
- **Other encodings, and the PL/pgSQL parse modes.** Their rules
  are in the grammar, as they are in PostgreSQL's, but no call
  reaches them.

## 11. How digres uses it

digres links `build/libsq.a`, and has no SQL grammar or scanner
of its own. Its `raw_parser()` is sq's one caller in digres
(`../dg/src/backend/parser/parser.c`). `raw_parser()`:

- **passes the session's three settings** in `sq_options_t`. It
  also passes an allocator that is its current memory context:
  `palloc` and `repalloc`, which return NULL when out of memory.
- **hands sq's statements to the analyzer as they are**, because
  they are digres's nodes. sq's generator writes `sq_nodes.c`
  when digres is built. It checks this at compile time, with a
  `static_assert` for each node's tag and size, each field's
  offset, and each enum's value. It also refuses the statements
  that digres has no part in: a publication's or subscription's
  statement, and any statement that names one as an object
  (`REFUSED` and `REFUSED_OBJECTS` in `gen/gen_nodes.py`). It
  refuses them in a statement, or in a SQL function's body.
- **raises sq's error as the server's own**, with its SQLSTATE,
  position, detail and hint.
- **keeps a notice, and reports it after sq returns.** If digres
  reported it from sq's hook, an error raised while reporting
  would be an exception inside sq. sq catches every exception,
  and would report that error as its own.

digres raises an error as a C++ exception. So in digres the tree
is an ordinary object, freed by its destructor however
`raw_parser()` leaves. Its nodes stay in the memory context.

digres is checked with sq's own tests. `digres --parse` prints
what `build/sq_parse` prints, for `raw_parser()`. So the expected
files and digests of §8 hold digres's parser to what
PostgreSQL's printed, locations included.
`digres --parse --locations` is held to the same files.

## What is left

None of it is owed to a step. Each item is a choice made on the
way, and written where it was made:

- **A tree holds a little more memory than its nodes need**: a
  list's outgrown cells, and the scanner's strings that no node
  kept (§9). A list assigned to a field is shared, not copied
  (§6).
- **The expected files and digests cannot be made again**
  without PostgreSQL's parser. A port of sq to a later
  PostgreSQL begins by making them with that one (§2, §8).
- **`make corpus-test` needs PostgreSQL 18.6's regression SQL**,
  which is not in this repository.
- **An `Oid` field is 32 bits**; digres's is to be 64 (§9).
- **Text is not checked to be UTF-8** (§9).
- **`spi.c` in digres** can still be handed a PL/pgSQL parse
  mode, which `raw_parser()` now refuses.

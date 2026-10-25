# sq

The SQL parser: PostgreSQL 18's SQL in, a tree of nodes out. It
is a library, `build/libsq.a`, with its interface in
`include/sq.h`, and it is what digres parses with.

```
make -j4         build build/libsq.a, its tests, and their tools
make test        build, and run the tests in test/
make corpus-test check sq over PostgreSQL's regression SQL
make tsan        the unit tests under the thread sanitizer
make clean       remove build/
make format      clang-format src/ and test/
make nodes       rewrite include/sq_nodes.h from spec/
```

It needs a C++20 compiler, flex, bison and Python 3, and nothing
else: nothing sq is built from is outside this directory.

`DESIGN.md` says how it is put together and why. `PLAN.md` is
the record of how it was made, and of what is left. digres uses
it through `raw_parser()`, in `../dg/src/backend/parser`.

## Using it

```cpp
#include "sq.h"

sq_tree_t tree;
if (sq_parse("SELECT a FROM t WHERE b = 1;", nullptr, tree) != 0) {
    fprintf(stderr, "%s at character %d\n", sq_errmsg(), sq_errpos());
    return;
}
for (const sq_raw_stmt_t* raw : tree.stmts) {
    if (raw->stmt->tag == SQ_T_SELECT_STMT) {
        auto* sel = static_cast<const sq_select_stmt_t*>(raw->stmt);
        /* sel->target_list, sel->from_clause, sel->where_clause ... */
    }
}
```

- **`sq_parse`** parses a list of statements into a tree.
  **`sq_parse_type_name`** parses one type name.
  **`sq_to_string`** prints a node as PostgreSQL's
  `nodeToString()` would, and
  **`sq_to_string_with_locations`** with every location shown.
- **A call returns 0 or -1.** After -1 the calling thread reads
  what went wrong: `sq_errmsg`, `sq_sqlstate`, `sq_errpos`, and
  PostgreSQL's `sq_errdetail` and `sq_errhint`.
- **The tree owns every node and string in it** and frees them
  when it is destroyed. Nothing is freed by hand. Given an
  allocator, as digres gives its memory context, the tree
  frees none of the nodes: they are the caller's.
- **The nodes are PostgreSQL's**, in `include/sq_nodes.h`, by
  sq's names: `SelectStmt` is `sq_select_stmt_t` and its
  `targetList` is `target_list`, laid out as digres's own. A
  list is an `sq_list_t*`, PostgreSQL's `List`; text is a
  `const char*`, NULL where absent.
- **Options** are an `sq_options_t`, or NULL for PostgreSQL's
  defaults: its three settings that change how a string literal
  is read, a hook for notices, and an allocator for the nodes
  in place of the tree's own arena.
- **Any number of threads** may parse at once.

sq parses and no more. It reads no catalog, and it has no notion
of what its caller supports: `CREATE FUNCTION` parses to its
nodes like any other statement.

## What is in it

| Where | What |
| --- | --- |
| `include/sq.h`, `sq_error.h` | the interface |
| `include/sq_nodes.h` | the nodes; generated, and committed |
| `spec/` | PostgreSQL 18.6's node headers and keyword list, unedited, but `nodetags.h`, digres's copy; and the list of the nodes its grammar can build |
| `gen/` | the generators: the nodes and the functions over them, the keyword hash, and digres's checks that sq's nodes are its own |
| `src/scan.l`, `src/gram.y` | PostgreSQL's scanner and grammar, forked: the rules are PostgreSQL's, the code around them is sq's |
| `src/parser.cc` | the two parsing calls, and the filter between scanner and grammar |
| `src/tree.cc`, `error.cc`, `list.cc`, `utf8.cc` | what a tree owns, the thread's error, lists while the grammar builds them, UTF-8 |
| `src/makefuncs.cc`, `scansup.cc`, `keywords.cc` | PostgreSQL's helpers for the grammar and the scanner |
| `src/print.cc` | `sq_to_string`: PostgreSQL's `outfuncs.c` as far as a parse tree needs it; the rest is generated |
| `test/tool/` | `sq_parse` and `sq_tokens`, which the tests run |

## Tests

sq is right if it parses as PostgreSQL does, so the tests hold
it to what PostgreSQL's own scanner and parser printed.

- **`test/run.sh`**: each SQL file here is parsed, and for some
  scanned, and what sq prints is compared with
  `expected/NAME.out`, or `NAME.loc` for trees with their
  locations, or `NAME.tok` for tokens. 93 files:
  - `test/*.sql`: statements of every family, and three errors;
  - `test/scan/`: literals and operators, and one file for each
    error the scanner raises;
  - `test/errors/`: one statement for each error an action of
    the grammar raises;
  - `test/types/`: type names.
- **`test/corpus.py`**, by `make corpus-test`: all 233 scripts
  of PostgreSQL 18.6's regression SQL, over 700,000 tokens and
  49,034 pieces, against `test/regress.digest`. It needs the
  scripts, which are not here: `PG_SQL` says where they are.
- **`build/test_sq`**: the unit tests, written with doctest and
  linked with a leak checker, for what PostgreSQL cannot show:
  ownership, the notice hook, the options, threads.
- **`test/nodes.sh`**: `include/sq_nodes.h` is what the
  generator writes today.
- **`test/alone.sh`**: nothing sq is built from is outside
  `sq/`.

**The expected files and the digests are PostgreSQL's answers,
not sq's.** They were written by PostgreSQL's parser, which was
built here beside sq's until sq could stand alone, and they
cannot be made again without it. `test/run.sh --accept` would
replace them with sq's own output, which checks nothing: use it
only for a difference that has been read and is known to be
PostgreSQL's too.

# PLAN — sq

Objective: make this a module with clear C-like interface,
exposing only C++ vector, C++ string and C++ string-view.

1. the sq/ project should look like sm/
2. a include/sq.h file
3. replace the elog with another mechanism similar to sm/ that
   propagate error back via return value and thread local
   storage
4. replace PG data structure that is awkward (e.g. List) with
   vector
5. adopt naming like sm/ i.e. lower case with _ , add
   namespace, etc.

---

## Decisions taken

- **sq has its own tree.** It is not PostgreSQL's node tree. A
  converter in `../dg` turns it into PostgreSQL's nodes for the
  analyzer, and is written last (step 7).
- **The base goes back to digres.** sq stops using `elog`,
  `List`, memory contexts and PostgreSQL's node structs, so it
  needs none of `nodes`, `mmgr`, `error`, `mb`, `common` or
  `port`. They are `../dg`'s alone again. `DESIGN.md` §2
  describes the arrangement this ends.
- **What `sq.h` exposes is prefixed `sq_`**, as `sm.h`'s is
  `sm_`: `sq_parse`, `sq_tree_t`, `sq_node_t`, and `SQ_ERR_`
  for the error codes. Nothing exposed is in a namespace.
- **Inside, sq uses `namespace sq`**, as the levels of `sm/` use
  `sm1`, `sm2` and `sm3`. The scanner, the grammar's helpers and
  the printer live there and are not in `sq.h`.
- **Types end in `_t`**, as in `sm/`: `sq_node_t`,
  `sq_select_stmt_t`, `sq_a_expr_t`. Fields are snake_case:
  `target_list`, not `targetList`.
- **Memory is RAII first.** An `sq_tree_t` owns every node of a
  parse and destroys them in its destructor. Where RAII cannot
  reach, there is a free function. None is foreseen in `sq.h`.
- **Inside a parse, an error is a private exception.** A raise
  records the thread's error and throws; `sq_parse` catches it
  and returns -1. It is how the scanner and grammar leave from
  wherever they are, as PostgreSQL's leave by `longjmp`, and it
  keeps their helpers shaped as PostgreSQL's are. flex's own
  fatal path, which would `exit()`, leaves the same way. None
  is ever seen outside sq.
- **A notice goes to a callback.** PostgreSQL's parser says
  things that are not errors: an identifier cut to 63 bytes,
  warnings about escapes. The caller's hook is called for each
  as it happens; with no hook they are dropped.
- **Settings come in an options struct**, `sq_options_t`:
  PostgreSQL's `standard_conforming_strings`, `backslash_quote`
  and `escape_string_warning`, and the notice hook. NULL is the
  defaults.
- **Bison keeps its `%union` of plain pointers.** The values on
  the parser's stack point at nodes the `sq_tree_t` already owns,
  so a parse that fails halfway is cleaned up by one destructor
  and no rule needs a cleanup of its own.
- **sq has its own error codes**, in `include/sq_error.h`, not
  `sm_error.h`: sq links nothing of `sm/`. An error carries the
  SQLSTATE and the character position, because digres sends
  both to the client.
- **The printer writes PostgreSQL's `nodeToString` text**,
  exactly. The struct is `sq_select_stmt_t` and it prints as
  `{SELECTSTMT`. This is for testing: it makes the old parser an
  oracle (step 0).
- **sq is a library digres links**, `libsq.a`, and it is how
  digres parses PostgreSQL's SQL. digres has no other parser
  after step 7.
- **sq is a full parser of PostgreSQL 18's SQL.** Every
  statement PostgreSQL accepts, sq accepts, and builds the whole
  tree for; every one it refuses as a syntax error, sq refuses.
  Its output is a bag of nodes and nothing more.
- **sq knows nothing of what digres supports.** There is no
  "unsupported" in sq: `CREATE FUNCTION` parses to its nodes
  like any other statement. digres decides, when it meets a
  node it does not handle, and the error is digres's to raise.
- **sq parses what a server is sent, and nothing else.**
  - psql's backslash commands are psql's. It runs them itself
    and splits its input into statements with its own scanner;
    the server never sees one. Stock PostgreSQL 18 `psql` is
    digres's client (`../dg/DGWORK.md` §1).
  - `COPY t FROM stdin` is a statement, and sq parses it. The
    rows that follow are not SQL: the client sends them as
    protocol messages, and digres's `COPY` code reads them.
    `\copy` is psql's.
- **The nodes are generated from PostgreSQL's own headers.**
  `spec/` holds `parsenodes.h`, `primnodes.h` and `value.h`
  unedited; `gen/gen_nodes.py` writes `include/sq_nodes.h` and
  the printer from them, and is to write digres's converter.
  Porting to a later PostgreSQL is replacing the headers.
- **The node header is committed**, so the API can be read. The
  tests fail when it is not what the generator writes.
- **Text in a node is a `const char*`**, owned by the tree. NULL
  means absent, which PostgreSQL tells apart from empty:
  `COMMENT ... IS NULL` and `IS ''` are different trees.

## The interface, in outline

```cpp
struct sq_node_t { sq_nodetag_t tag; };   // sq_nodes.h has the rest

struct sq_tree_t {                   // owns every node; move-only
    std::vector<sq_node_t*> stmts;   // one per statement
    ~sq_tree_t();
};

int sq_parse(std::string_view sql, const sq_options_t* opt,
             sq_tree_t& out);        // 0, or -1
sq_errnum_t sq_errnum(void);         // the calling thread's last error
const char* sq_errmsg(void);
const char* sq_sqlstate(void);       // five characters
int sq_errpos(void);                 // character position, or 0
const char* sq_errdetail(void);      // PostgreSQL's detail, or ""
const char* sq_errhint(void);        // PostgreSQL's hint, or ""
int sq_to_string(const sq_node_t* node, std::string& out);
```

`sq_parse` takes a list of statements. digres also parses a type
name by itself (`parse_type.c`), so a second entry,
`sq_parse_type_name`, is owed by step 5. PostgreSQL's parse
modes for PL/pgSQL are not ported.

- A function returning `int` returns 0 on success and -1 on
  error, as `sm.h`'s do.
- No exception crosses it: `std::bad_alloc` becomes an
  out-of-memory error at `sq_parse`.
- A parse is all or nothing. On -1, `out` is empty.

## 0. Where it starts, and the oracle

Measured on 2026-10-09.

- sq compiles 131 files from `../dg/src` in place. `src/host.c`
  answers 32 names. Eight tests pass.
- The grammar builds 198 node types at 832 `makeNode` sites. It
  builds or tests a `List` on about 640 lines. `gram.y`,
  `scan.l`, `parser.c` and `scansup.c` raise on about 105.

Before anything changes, today's `build/sqparse` is kept as the
oracle: PostgreSQL's own parser printing PostgreSQL's own text.

- It gains one option, `--split DIR`, to write each statement's
  source text and its tree as a pair of files, from `RawStmt`'s
  `stmt_location` and `stmt_len`. That splits a SQL file into
  statements without a second scanner.
- The corpus is the eight tests here and PostgreSQL's
  regression SQL, in `~/p/postgres18`. `make corpus` builds it
  in `build/corpus`.
- Verify: the oracle builds as `build/oracle`; its output on the
  eight tests is the expected output, unchanged.

**Done.** `src/sqparse.c` is `src/oracle.c`; `test/split.sh`
tests the option.

- **The corpus is 13,179 statements of 71 kinds, from 111 of
  the 233 files.** The regression files are scripts for psql,
  not text a server is sent. The other 122 hold backslash
  commands, rows of `COPY` data, or a syntax error put there on
  purpose, and PostgreSQL's parser returns nothing for a text
  with an error in it.
- **The 122 are not lost for good.** What a server is sent from
  them is what psql makes of them. The way to get it is to run
  them through psql against a stock PostgreSQL 18 with
  `log_statement = all` and take the statements from its log.
  Not built: no PostgreSQL server is installed here, and 111
  files cover every statement family of step 4. To be done if
  step 4 finds a family the corpus is thin on, or to bring the
  deliberate syntax errors in.

Every later step is checked against it: for a statement sq
supports, the new parser's text equals the oracle's, byte for
byte.

## 1. The skeleton, shaped like sm/

- `include/sq.h` and `include/sq_error.h`; `src/`; `test/`; a
  Makefile with `all`, `test`, `format` and `clean` that builds
  `build/libsq.a`. Tests use doctest, as sm1's do, beside the
  `.sql` files.
- The thread-local error state, and `sq_parse` returning -1 with
  "not implemented" for every input.
- The oracle keeps building beside it, from the old list.
- Verify: `libsq.a` and its test link with no object and no
  header from outside `sq/`.

**Done.** `test/alone.sh` holds the verification, and runs with
the tests.

- **Three error codes**, and no more: `SQ_ERR_PARSE`,
  `SQ_ERR_NOMEM`, `SQ_ERR_EXCEPTION`. What was wrong with the
  SQL is in the SQLSTATE, of which the grammar and scanner raise
  nine.
- **Owed to the error state, and not built**: PostgreSQL's
  detail and hint, which the grammar and scanner attach to some
  errors and a client is shown. They are added when the first
  ported rule needs them, in step 3 or 4.
- The library is `src/*.cc`, the oracle `src/*.c`: they share
  the directory until step 6.
- sq is independent of `sm/` as well as of `../dg`: it has its
  own copy of `doctest.h`, in `test/`, and `test/alone.sh` fails
  on a dependency on any file outside `sq/`.

## 2. The tree

- `sq_node_t`, and the node structs the first grammar slice
  needs: constants, column references, operators, function
  calls, `sq_select_stmt_t` and what hangs from it.
- A list is a `std::vector<sq_node_t*>`; text is a
  `const char*`.
- `sq_tree_t` records each node as it is made and destroys them
  all.
- The printer, with its table of PostgreSQL's names.
- Then decide: hand-written or generated, for the rest.
- Verify: tests build a tree by hand, print it, and compare with
  the oracle's text for the same statement; a leak checker finds
  nothing after the tree is destroyed.

**Done**, and for every node, not the first slice: generating
them made all 216 cost what a dozen would.

- **216 structs and 55 enums**, in `include/sq_nodes.h`. They
  are the structs the grammar can make and all those point at,
  listed in `spec/nodes.list`.
- **The printer is checked two ways.** Trees built by hand print
  as the oracle prints the same statements. And for each of the
  208 structs the generator prints, its fields, their order and
  their kinds are those of PostgreSQL's own generated printer
  (`test/spec.sh`). The other eight are printed by hand, as
  PostgreSQL prints them by hand.
- **The tests are linked with a leak checker.** Made to leak on
  purpose, the tree failed them.
- `sq_to_string` fills a `std::string` and returns `int`, as
  every other call does: it can run out of memory.
- A list that stands where any node may — an element of another
  list, or a field typed `sq_node_t*` — is an `sq_list_t`.
- `test/spec.sh` and `gen/check_pg.py` read `../dg`. They go
  with the oracle in step 6.

Owed to later steps:

- **PostgreSQL's `#define` constants** that the grammar stores
  in nodes — `CURSOR_OPT_`, `FRAMEOPTION_` and the like — are
  not generated yet. Step 4 needs them.
- **`sq_tree_t::stmts` holds statements, not `RawStmt`s.**
  digres wants each statement's position and length, which
  `sq_raw_stmt_t` has. Settled when the grammar's top rule is
  ported.
- **An `Oid` field is 32 bits**, as in PostgreSQL. digres's is
  to be 64 (`../sm/PART4.md` §10). Every such field in a parse
  tree is the analyzer's to fill and zero from sq.

## 3. The scanner

- sq's own copies of `scan.l`, `scansup.c` and the keyword
  table. They are forks from here on: `../dg` keeps its own
  until step 7.
- Each raise becomes: set the thread's error, return an error
  token. `palloc` becomes the tree's allocation. Character
  length is UTF-8's, written here.
- Flex's scanner state is held by a guard object, so that every
  exit releases it.
- Verify: a token-dump test over the corpus; the
  unterminated-string test reports the same message and
  position as the oracle.

**Done.** `src/scan.l` is sq's fork; its rules are PostgreSQL's
and unchanged. A raise throws, as decided above, and does not
return an error token.

- **Checked against PostgreSQL's scanner**, not by reading:
  `oracle --tokens` and `build/sq_tokens` print each token's
  number, position and value, or the error with its position,
  detail and hint. They agree on all 233 files of the regression
  SQL, 716,066 tokens (`make corpus-test`), and on the 25 files
  here (`make test`). The scanner reads any text, so the 122
  files the parser cannot split are in this.
- `test/scan/` holds one file for each of the scanner's errors,
  since a scan stops at its first.
- **What the oracle cannot show is in `test/test_scan.cc`**: the
  notice hook, the three settings, positions in characters, and
  that a scan ended by an error frees everything.
- **The tokens are declared in `src/gram.y`** already, with no
  rules: the scanner returns the grammar's numbers. They are in
  PostgreSQL's order, so each has PostgreSQL's number.
- **Keywords are found with a perfect hash**, as PostgreSQL
  finds them: one hash and one comparison, with no probing and
  no collisions. `gen/gen_keywords.py` is PostgreSQL's
  `PerfectHash.pm` in Python, and what it writes from
  `spec/kwlist.h` is PostgreSQL's own table, number for number
  (`test/spec.sh`). Measured at `-O2` on keywords and plain
  identifiers in equal parts: 8 ns a lookup, against 39 ns for
  the binary search it replaced.
- **Text with a NUL byte is refused**, as PostgreSQL refuses it
  from a client. Text that is not valid UTF-8 is not refused: sq
  scans it as PostgreSQL's scanner would, and digres is expected
  to have checked what a client sent.
- `scansup.c` and the pieces of `wchar.c`, `mbutils.c` and
  `numutils.c` the scanner calls are in `src/scansup.cc` and
  `src/utf8.cc`, for UTF-8 only.
- flex and bison are now needed to build sq.

Owed to later steps:

- **The lookahead filter** of PostgreSQL's `parser.c`, between
  scanner and grammar, comes with the grammar in step 4.
- `sq_parse` takes its options and still refuses every input.

## 4. The grammar, one family at a time

The order, and each is its own cycle of port, test, commit:

1. Expressions and `SELECT`.
2. `INSERT`, `UPDATE`, `DELETE`, `MERGE`.
3. Table and index DDL.
4. Transactions, `SET`, `EXPLAIN`, `COPY`, cursors, `VACUUM`
   and the other utility statements.
5. Every remaining statement: functions, types, operators,
   roles and privileges, publications and the rest.

In each:

- `makeNode(X)` becomes an allocation in the tree.
- A `List` becomes a vector.
- A raise becomes the thread's error and `YYABORT`. A helper
  that raised returns -1, and the rule that called it aborts.
- Verify: for every corpus statement in the family, sq's text
  equals the oracle's; for every statement the oracle refuses,
  sq refuses with the same message and position; a failed parse
  leaks nothing.
- Verify, at the end of 5: for all 13,179 corpus statements,
  sq's text equals the oracle's.

**Done, and not a family at a time.** The whole of `gram.y` was
converted at once, by a script, and then made to compile by
hand; a bison grammar does not come apart into families, and a
conversion that is mechanical is the same work for one rule as
for all. The families were how it was to be checked, and the
check below covers them all.

- **`src/gram.y` is sq's fork** of PostgreSQL's. The rules are
  PostgreSQL's, untouched. Inside the actions every type, field
  and helper has sq's name, and that renaming is all the script
  did. It was run once and is not kept: the fork is the source
  from here on.
- **`sq_parse` parses.** `src/parser.cc` is PostgreSQL's
  `parser.c`: the filter that looks one token ahead, and the
  de-escaping of `U&` strings.
- **Checked against PostgreSQL's parser three ways:**
  - every statement of the corpus, 13,179 of them, prints the
    tree the oracle prints (`test/trees.sh --corpus`);
  - every piece of every one of the 233 regression scripts,
    49,034 pieces, gets the same answer from both: the same
    trees, or the same error with its position, detail and
    hint (`test/trees.sh --chunks`). `test/chunks.py` cuts the
    scripts at semicolons, cruder than a statement and enough to
    compare by. This is where the SQL that is wrong on purpose
    is, which the corpus cannot hold (step 0);
  - the 25 files here, on every `make test`.
- **A list in the grammar is a handle**, `list_ref_t`, where
  PostgreSQL has a `List *`. Assigned to a node's field it is
  copied into the field's vector. PostgreSQL's list functions
  are kept, by its names, over it (`src/list.h`).
- **The grammar's actions make nodes in "the current tree"**, a
  per-thread pointer `sq_parse` sets for as long as it runs, as
  PostgreSQL's allocate in its current memory context. Passing
  the tree to each of 800 calls would have rewritten every
  action.
- **bison's stacks come from the parse** and are freed with it,
  so that an error raised from an action leaks nothing.
- **`sq_tree_t::stmts` holds `sq_raw_stmt_t`s**: each statement
  with where it begins and how long it is, as PostgreSQL's
  parser returns them.
- `copy_node` and `equal_nodes`, PostgreSQL's `copyObject` and
  `equal`, are generated with the printer: the grammar calls
  each once.
- PostgreSQL's constants for node fields, its `#define`s in
  `parsenodes.h`, are generated into `sq_nodes.h` with `SQ_`
  before them. The few from its catalog headers that the
  grammar stores are in `src/pg_consts.h`, by hand.
- Step 5's threads test is already here: eight threads parsing
  at once, each reading its own error.

Known, and accepted:

- **A list assigned to a field is copied**, and the list it came
  from stays in the tree, unused, until the tree is freed. A
  tree therefore holds more memory than its nodes need. Moving
  the list where it is not used again would fix it, a rule at a
  time.
- **A field's list and the handle it was copied from are
  separate.** PostgreSQL's actions could append to a list after
  assigning it and have the node see it. None was found to, and
  49,034 comparisons agree, but the compiler cannot show it.

## 5. The surface

- `sq.h` is settled and its comment states the conventions, as
  `sm.h`'s does.
- Verify: a test parses from several threads at once, under the
  thread sanitizer; each thread reads its own error.

## 6. The old boundary goes

- `sqparse` is rewritten on `sq.h`. The eight expected files
  must not change.
- sq's Makefile compiles nothing from `../dg/src`. `src/host.c`
  and the oracle are deleted.
- Verify: `make -C sq test` passes from clean with `../dg`
  absent.

The oracle is deleted here and not sooner. After this the
comparison lives in `../dg`, in step 7.

## 7. The converter, in dg

- `../dg` gains one file: an sq tree in, PostgreSQL's nodes
  out. It is the only code that knows both. A node digres does
  not handle is refused there or in the analyzer, with
  "not supported", SQLSTATE `0A000`.
- Verify: over the corpus, `nodeToString` of the converted tree
  equals `nodeToString` of what `raw_parser` returns.
- Then digres's callers of `raw_parser` call sq and the
  converter, and `gram.y`, `scan.l`, `parser.c` and `scansup.c`
  are deleted from `../dg`.
- Verify: `make -C ../dg -j4` links.

## 8. Documents

A step is not done while a document says what is no longer
true.

- Step 1: `README.md`'s build section.
- Step 6: `README.md` and `DESIGN.md` are rewritten. Most of
  both describes the boundary that step ends.
- Step 7: `../dg/README.md`, and the root `README.md`.

## Not planned

- **The analyzer.** It stays in `../dg`, reading PostgreSQL's
  nodes.
- **Deparsing**, a tree back to SQL text.
- **Other encodings.** UTF-8 only.
- **Carving the base out as its own project.** Considered, and
  dropped: nothing below digres needs it once sq does not.

## Against the spike

The first target is `SELECT 1 + 1` running in digres
(`../sm/PART4.md` §10). Nothing here is on that path until step
7, and digres can run on its own parser until then. This plan
gives way to the spike whenever the two compete.

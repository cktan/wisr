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
- **The whole of PostgreSQL's grammar is ported.** Every
  statement PostgreSQL accepts, sq accepts; every one it refuses
  as a syntax error, sq refuses.
- **A statement digres does not support parses to
  `sq_unsupported_stmt_t`**, which names the statement:
  `CREATE FUNCTION` is one. Its syntax is still checked in full.
  It is a node, not an error from `sq_parse`: the other
  statements in the same text still parse, and digres raises
  "not supported", SQLSTATE `0A000`, when it reaches that one —
  as PostgreSQL raises such errors a statement at a time. No
  node struct, printer or converter is written for what is
  under it.
- **sq parses what a server is sent, and nothing else.**
  - psql's backslash commands are psql's. It runs them itself
    and splits its input into statements with its own scanner;
    the server never sees one. Stock PostgreSQL 18 `psql` is
    digres's client (`../dg/DGWORK.md` §1).
  - `COPY t FROM stdin` is a statement, and sq parses it. The
    rows that follow are not SQL: the client sends them as
    protocol messages, and digres's `COPY` code reads them.
    `\copy` is psql's.
- **Node structs are written by hand to begin with.** Whether to
  generate them, and the printer, is decided in step 2 after the
  first dozen.

## The interface, in outline

```cpp
struct sq_node_t { sq_nodetag_t tag; int location; };

struct sq_tree_t {                   // owns every node; move-only
    std::vector<sq_node_t*> stmts;   // one per statement
    ~sq_tree_t();
};

int sq_parse(std::string_view sql, sq_tree_t& out);   // 0, or -1
sq_errnum_t sq_errnum(void);         // the calling thread's last error
const char* sq_errmsg(void);
const char* sq_sqlstate(void);       // five characters
int sq_errpos(void);                 // character position, or 0
std::string sq_to_string(const sq_node_t* node);
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
  "not supported" for every input.
- The oracle keeps building beside it, from the old list.
- Verify: `libsq.a` and its test link with no object and no
  header from `../dg`.

## 2. The tree

- `sq_node_t`, and the node structs the first grammar slice
  needs: constants, column references, operators, function
  calls, `sq_select_stmt_t` and what hangs from it.
- A list is a `std::vector<sq_node_t*>`; text is a
  `std::string`.
- `sq_tree_t` records each node as it is made and destroys them
  all.
- The printer, with its table of PostgreSQL's names.
- Then decide: hand-written or generated, for the rest.
- Verify: tests build a tree by hand, print it, and compare with
  the oracle's text for the same statement; a leak checker finds
  nothing after the tree is destroyed.

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

## 4. The grammar, one family at a time

The order, and each is its own cycle of port, test, commit:

1. Expressions and `SELECT`.
2. `INSERT`, `UPDATE`, `DELETE`, `MERGE`.
3. Table and index DDL.
4. Transactions, `SET`, `EXPLAIN`, `COPY`, and whatever else
   digres runs. The list is drawn up at the start of this step
   from what `../dg` has cut.
5. Every remaining statement, with its rules in full and its
   actions building nothing, as `sq_unsupported_stmt_t`.

In each:

- `makeNode(X)` becomes an allocation in the tree.
- A `List` becomes a vector.
- A raise becomes the thread's error and `YYABORT`. A helper
  that raised returns -1, and the rule that called it aborts.
- Verify: for every corpus statement in the family, sq's text
  equals the oracle's; for every statement the oracle refuses,
  sq refuses with the same message and position; a failed parse
  leaks nothing.
- Verify, at the end of 5: sq accepts all 13,179 corpus
  statements; each parses to the oracle's text or to
  `sq_unsupported_stmt_t`, and the list of which is in
  `README.md`.

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
  out. It is the only code that knows both.
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

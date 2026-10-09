/*-------------------------------------------------------------------------
 *
 * parser.c
 *		Main entry point/driver for the parser
 *
 * digres parses with sq (../sq): PostgreSQL 18's grammar and scanner
 * as a library that knows nothing of the server. raw_parser() is what
 * stands between the two. It hands sq the text and the session's
 * settings, turns sq's tree into PostgreSQL's nodes for the analyzer,
 * and reports sq's error as a server reports one.
 *
 * Note that the grammar is not allowed to perform any table access
 * (since we need to be able to do basic parsing even while inside an
 * aborted transaction).  Therefore, the data structures returned by
 * the grammar are "raw" parsetrees that still need to be analyzed by
 * analyze.c and related files.
 *
 * IDENTIFICATION
 *	  src/backend/parser/parser.c
 *
 *-------------------------------------------------------------------------
 */

/* sq's header first: it includes the C++ library, which must be read
 * before postgres.h defines macros over names of its own. */
#include "sq.h"

#include <string>
#include <vector>

#include "postgres.h"

#include "nodes/makefuncs.h"
#include "parser/parser.h"
#include "parser/sq_convert.h"

/*
 * The session's settings that change how a string literal is read.
 * They are PostgreSQL's, and it is a wart of PostgreSQL's that the
 * parser depends on them: raw_parser() hands them to sq.
 */
int backslash_quote = BACKSLASH_QUOTE_SAFE_ENCODING;
bool escape_string_warning = true;
bool standard_conforming_strings = true;

/* Something sq had to say that was not an error, kept until it is safe to
 * say it. */
struct pending_notice_t {
  sq_notice_level_t level;
  std::string sqlstate;
  std::string message;
  std::string hint;
  bool has_hint;
  int pos;
};

/*
 * One parse's tree and notices. They are C++ objects, freed by their
 * destructors: an ERROR raised while they are held is an exception, and
 * unwinds through them.
 */
struct parse_job_t {
  sq_tree_t tree;
  std::vector<pending_notice_t> notices;
};

/*
 * sq's notice hook: keep the notice for after the parse. Reporting it
 * from here would be reporting from inside sq, which an error raised
 * while reporting would then jump out of.
 */
static void keep_notice(void *arg, const sq_notice_t *notice) {
  parse_job_t *job = (parse_job_t *)arg;

  /* The hook may not throw; a notice that cannot be kept is dropped. */
  try {
    job->notices.push_back({notice->level, notice->sqlstate, notice->message,
                            notice->hint ? notice->hint : "",
                            notice->hint != NULL, notice->pos});
  } catch (...) {
  }
}

/* PostgreSQL's error code for a SQLSTATE of five characters. */
static int sqlstate_code(const char *state) {
  return MAKE_SQLSTATE(state[0], state[1], state[2], state[3], state[4]);
}

/* Report what sq said along the way, as the server's own NOTICE or WARNING. */
static void report_notices(const parse_job_t *job) {
  for (const pending_notice_t &n : job->notices)
    ereport(n.level == SQ_WARNING ? WARNING : NOTICE,
            (errcode(sqlstate_code(n.sqlstate.c_str())),
             errmsg_internal("%s", n.message.c_str()),
             n.has_hint ? errhint_internal("%s", n.hint.c_str()) : 0,
             n.pos > 0 ? errposition(n.pos) : 0));
}

/* Raise the calling thread's sq error as the server's own ERROR. */
static void report_error(void) {
  ereport(ERROR,
          (errcode(sqlstate_code(sq_sqlstate())),
           errmsg_internal("%s", sq_errmsg()),
           sq_errdetail()[0] ? errdetail_internal("%s", sq_errdetail()) : 0,
           sq_errhint()[0] ? errhint_internal("%s", sq_errhint()) : 0,
           sq_errpos() > 0 ? errposition(sq_errpos()) : 0));
}

/*
 * raw_parser
 *		Given a query in string form, do lexical and grammatical
 *analysis.
 *
 * Returns a list of raw (un-analyzed) parse trees.  The contents of the
 * list have the form required by the specified RawParseMode: a RawStmt
 * for each statement, or one TypeName.
 */
List *raw_parser(const char *str, RawParseMode mode) {
  sq_options_t options;
  parse_job_t job;
  sq_type_name_t *type_name = NULL;
  List *result = NIL;
  int rc;

  /* digres has no procedural language to ask for the other modes. */
  if (mode != RAW_PARSE_DEFAULT && mode != RAW_PARSE_TYPE_NAME)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("PL/pgSQL parse modes are not supported")));

  /* The settings are the session's; their values are PostgreSQL's. */
  options.standard_conforming_strings = standard_conforming_strings;
  options.backslash_quote = (sq_backslash_quote_t)backslash_quote;
  options.escape_string_warning = escape_string_warning;
  options.notice = keep_notice;
  options.notice_arg = &job;

  /* Nothing in sq raises a server error, or throws: it returns. */
  if (mode == RAW_PARSE_TYPE_NAME)
    rc = sq_parse_type_name(str, &options, job.tree, &type_name);
  else
    rc = sq_parse(str, &options, job.tree);

  /* What sq said on the way comes before what stopped it. */
  report_notices(&job);
  if (rc != 0)
    report_error();

  /* The analyzer reads PostgreSQL's nodes, not sq's. */
  if (mode == RAW_PARSE_TYPE_NAME)
    result = list_make1(sq_convert(type_name));
  else
    for (const sq_raw_stmt_t *raw : job.tree.stmts)
      result = lappend(result, sq_convert(raw));

  /* sq's tree, which no memory context knows of, goes with job. */
  return result;
}

/* SystemFuncName()
 * Build a properly-qualified reference to a built-in function.
 */
List *SystemFuncName(char *name) {
  return list_make2(makeString("pg_catalog"), makeString(name));
}

/* SystemTypeName()
 * Build a properly-qualified reference to a built-in type.
 *
 * typmod is defaulted, but may be changed afterwards by caller.
 * Likewise for the location.
 */
TypeName *SystemTypeName(char *name) {
  return makeTypeNameFromNameList(
      list_make2(makeString("pg_catalog"), makeString(name)));
}

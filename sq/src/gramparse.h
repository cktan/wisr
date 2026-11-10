/*
 * gramparse.h -- what the grammar, the scanner and the parser's
 * driver share: PostgreSQL's gramparse.h.
 *
 * It is included by the bison header, build/gen/gram.hh, so that
 * whatever includes that has the types its union names.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) 2026, CK Tan: the changes for sq,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 */
#ifndef SQ_SRC_GRAMPARSE_H
#define SQ_SRC_GRAMPARSE_H

#include <cstddef>
#include <memory>
#include <vector>

#include "list.h"
#include "scan.h"
#include "sq_nodes.h"

namespace sq {

/* flex's handle on a scan, which the grammar passes about. */
typedef void *core_yyscan_t;

/*
 * Things the grammar's actions pass between rules that are not nodes.
 * Each is made in the tree, as a node is.
 */
struct priv_target_t {
  sq_grant_target_type_t targtype;
  sq_object_type_t objtype;
  list_ref_t objs;
};

struct import_qual_t {
  sq_import_foreign_schema_type_t type;
  list_ref_t table_names;
};

struct select_limit_t {
  sq_node_t *limit_offset;
  sq_node_t *limit_count;
  sq_limit_option_t limit_option; /* indicates presence of WITH TIES */
  int offset_loc;                 /* location of OFFSET token, if present */
  int count_loc;  /* location of LIMIT/FETCH token, if present */
  int option_loc; /* location of WITH TIES, if present */
};

struct group_clause_t {
  bool distinct;
  list_ref_t list;
};

struct key_action_t {
  char action;
  list_ref_t cols;
};

struct key_actions_t {
  key_action_t *update_action;
  key_action_t *delete_action;
};

/*
 * The state of one parse beyond the scanner's: PostgreSQL's
 * base_yy_extra_type, less the scanner's part of it.
 */
struct parse_state_t {
  /* The filter between scanner and grammar looks one token ahead. */
  bool have_lookahead = false;     /* is lookahead info valid? */
  int lookahead_token = 0;         /* one-token lookahead */
  scan_value_t lookahead_yylval{}; /* yylval for lookahead token */
  int lookahead_yylloc = 0;        /* yylloc for lookahead token */
  char *lookahead_end = nullptr;   /* end of current token */
  char lookahead_hold_char = 0;    /* to be put back at *lookahead_end */

  /* The final parse result is delivered here. */
  list_ref_t parsetree = NIL;

  /* bison's stacks, when they outgrow its own: freed with the parse. */
  std::vector<std::unique_ptr<char[]>> scratch;
};

/* The parse a scan belongs to. */
inline parse_state_t *parse_state(core_yyscan_t yyscanner) {
  return scanner_extra(yyscanner)->parse;
}

/* Memory for bison's stacks, released when the parse ends. */
void *parse_alloc(core_yyscan_t yyscanner, size_t size);

} // namespace sq

#endif /* SQ_SRC_GRAMPARSE_H */

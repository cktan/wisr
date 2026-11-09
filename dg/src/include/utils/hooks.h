/*-------------------------------------------------------------------------
 *
 * hooks.h
 *	  The hooks a loaded library sets: this thread's, tcb->hooks.
 *
 * A library loaded by shared_preload_libraries sets its hooks in the
 * postmaster, before any session starts, and every session starts with
 * them: session_thread_start() copies the parent's into the new thread
 * (utils/control_block.h).  A library a session LOADs sets that session's
 * alone, but a session may not load one (ISSUE.md dg issue 1).
 *
 * src/include/utils/hooks.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef HOOKS_H
#define HOOKS_H

#include "catalog/objectaccess.h"
#include "commands/explain.h"
#include "commands/explain_state.h"
#include "commands/user.h"
#include "executor/executor.h"
#include "fmgr.h"
#include "libpq/auth.h"
#include "optimizer/paths.h"
#include "optimizer/plancat.h"
#include "optimizer/planner.h"
#include "parser/analyze.h"
#include "rewrite/rowsecurity.h"
#include "tcop/utility.h"
#include "utils/control_block.h"
#include "utils/elog.h"
#include "utils/lsyscache.h"
#include "utils/selfuncs.h"

/* Each hook, NULL until a library sets it; its module calls it. */
struct hooks_t {
  /* catalog/objectaccess.c */
  object_access_hook_type object_access_hook = NULL;
  object_access_hook_type_str object_access_hook_str = NULL;

  /* commands/explain.c, explain_state.c */
  ExplainOneQuery_hook_type ExplainOneQuery_hook = NULL;
  explain_per_plan_hook_type explain_per_plan_hook = NULL;
  explain_per_node_hook_type explain_per_node_hook = NULL;
  explain_get_index_name_hook_type explain_get_index_name_hook = NULL;
  explain_validate_options_hook_type explain_validate_options_hook = NULL;

  /* commands/user.c */
  check_password_hook_type check_password_hook = NULL;

  /* executor/execMain.c */
  ExecutorStart_hook_type ExecutorStart_hook = NULL;
  ExecutorRun_hook_type ExecutorRun_hook = NULL;
  ExecutorFinish_hook_type ExecutorFinish_hook = NULL;
  ExecutorEnd_hook_type ExecutorEnd_hook = NULL;
  ExecutorCheckPerms_hook_type ExecutorCheckPerms_hook = NULL;

  /* libpq/auth.c */
  ClientAuthentication_hook_type ClientAuthentication_hook = NULL;

  /* optimizer */
  set_rel_pathlist_hook_type set_rel_pathlist_hook = NULL;
  set_join_pathlist_hook_type set_join_pathlist_hook = NULL;
  join_search_hook_type join_search_hook = NULL;
  get_relation_info_hook_type get_relation_info_hook = NULL;
  planner_hook_type planner_hook = NULL;
  create_upper_paths_hook_type create_upper_paths_hook = NULL;

  /* parser/analyze.c */
  post_parse_analyze_hook_type post_parse_analyze_hook = NULL;

  /* rewrite/rowsecurity.c */
  row_security_policy_hook_type row_security_policy_hook_permissive = NULL;
  row_security_policy_hook_type row_security_policy_hook_restrictive = NULL;

  /* tcop/utility.c */
  ProcessUtility_hook_type ProcessUtility_hook = NULL;

  /* utils: elog.c, fmgr.c, lsyscache.c, selfuncs.c */
  emit_log_hook_type emit_log_hook = NULL;
  needs_fmgr_hook_type needs_fmgr_hook = NULL;
  fmgr_hook_type fmgr_hook = NULL;
  get_attavgwidth_hook_type get_attavgwidth_hook = NULL;
  get_relation_stats_hook_type get_relation_stats_hook = NULL;
  get_index_stats_hook_type get_index_stats_hook = NULL;
};

#endif /* HOOKS_H */

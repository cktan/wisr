%code requires {
/* What the semantic values are made of. */
#include "gramparse.h"
}

%code {

/*
 * This file is sq's fork of PostgreSQL 18.6's src/backend/parser/gram.y.
 * The rules are PostgreSQL's, and so is what each action does. What
 * differs is how an action says it:
 *
 *   - a node is one of sq's (include/sq_nodes.h): SelectStmt is
 *     sq_select_stmt_t and its targetList is target_list, and the
 *     helpers here have sq's names to match;
 *   - a list is passed about as a list_ref_t (src/list.h), where
 *     PostgreSQL has a List *, and is shared with a node's field, an
 *     sq_list_t *, when it is assigned there, as PostgreSQL shares it;
 *   - an error is raised with sq::raise (src/error.h), not ereport, and
 *     a warning goes to the options' notice hook;
 *   - everything is made in the tree being built (src/tree.h), where
 *     PostgreSQL allocates in its current memory context.
 *
 * PostgreSQL's own heading follows.
 */

/*#define YYDEBUG 1*/
/*-------------------------------------------------------------------------
 *
 * gram.y
 *	  POSTGRESQL BISON rules/actions
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/parser/gram.y
 *
 * HISTORY
 *	  AUTHOR			DATE			MAJOR EVENT
 *	  Andrew Yu			Sept, 1994		POSTQUEL to SQL conversion
 *	  Andrew Yu			Oct, 1994		lispy code conversion
 *
 * NOTES
 *	  CAPITALS are used to represent terminal symbols.
 *	  non-capitals are used to represent non-terminals.
 *
 *	  In general, nothing in this file should initiate database accesses
 *	  nor depend on changeable state (such as SET variables).  If you do
 *	  database accesses, your code will fail when we have aborted the
 *	  current transaction and are just parsing commands to find the next
 *	  ROLLBACK or COMMIT.  If you make use of SET variables, then you
 *	  will do the wrong thing in multi-query strings like this:
 *			SET constraint_exclusion TO off; SELECT * FROM foo;
 *	  because the entire string is parsed by gram.y before the SET gets
 *	  executed.  Anything that depends on the database or changeable state
 *	  should be handled during parse analysis so that it happens at the
 *	  right time not the wrong time.
 *
 * WARNINGS
 *	  If you use a list, make sure the datum is a node so that the printing
 *	  routines work.
 *
 *	  Sometimes we assign constants to makeStrings. Make sure we don't free
 *	  those.
 *
 *-------------------------------------------------------------------------
 */

#include <ctype.h>
#include <limits.h>
#include <string.h>

#include <cassert>

#include "error.h"
#include "gramparse.h"
#include "nodefuncs.h"
#include "pg_consts.h"
#include "scansup.h"

using namespace sq;


/*
 * Location tracking support.  Unlike bison's default, we only want
 * to track the start position not the end position of each nonterminal.
 * Nonterminals that reduce to empty receive position "-1".  Since a
 * production's leading RHS nonterminal(s) may have reduced to empty,
 * we have to scan to find the first one that's not -1.
 */
#define YYLLOC_DEFAULT(current, rhs, N) \
	do { \
		(current) = (-1); \
		for (int _i = 1; _i <= (N); _i++) \
		{ \
			if ((rhs)[_i] >= 0) \
			{ \
				(current) = (rhs)[_i]; \
				break; \
			} \
		} \
	} while (0)

/*
 * Bison doesn't allocate anything that needs to live across parser calls,
 * so we can easily have it take its stacks from the parse, which frees
 * them when it ends.  This prevents memory leaks if we error out during
 * parsing.
 */
#define YYMALLOC(size) parse_alloc(yyscanner, (size))
#define YYFREE(ptr)    ((void) (ptr))


/* ConstraintAttributeSpec yields an integer bitmask of these flags: */
#define CAS_NOT_DEFERRABLE			0x01
#define CAS_DEFERRABLE				0x02
#define CAS_INITIALLY_IMMEDIATE		0x04
#define CAS_INITIALLY_DEFERRED		0x08
#define CAS_NOT_VALID				0x10
#define CAS_NO_INHERIT				0x20
#define CAS_NOT_ENFORCED			0x40
#define CAS_ENFORCED				0x80


#define parser_yyerror(msg)  scanner_yyerror(msg, yyscanner)
#define parser_errposition(pos)  scanner_errposition(pos, yyscanner)

static void sq_base_yyerror(YYLTYPE *yylloc, core_yyscan_t yyscanner,
							const char *msg);
static list_ref_t system_func_name(const char *name);
static sq_type_name_t *system_type_name(const char *name);
static sq_raw_stmt_t *make_raw_stmt(sq_node_t *stmt, int stmt_location);
static void update_raw_stmt_end(sq_raw_stmt_t *rs, int end_location);
static sq_node_t *make_column_ref(const char *colname, list_ref_t indirection,
						   int location, core_yyscan_t yyscanner);
static sq_node_t *make_type_cast(sq_node_t *arg, sq_type_name_t *typename_, int location);
static sq_node_t *make_string_const_cast(char *str, int location, sq_type_name_t *typename_);
static sq_node_t *make_int_const(int val, int location);
static sq_node_t *make_float_const(const char *str, int location);
static sq_node_t *make_bool_a_const(bool state, int location);
static sq_node_t *make_bit_string_const(char *str, int location);
static sq_node_t *make_null_a_const(int location);
static sq_node_t *make_a_const(sq_node_t *v, int location);
static sq_role_spec_t *make_role_spec(sq_role_spec_type_t type, int location);
static void check_qualified_name(list_ref_t names, core_yyscan_t yyscanner);
static list_ref_t check_func_name(list_ref_t names, core_yyscan_t yyscanner);
static list_ref_t check_indirection(list_ref_t indirection, core_yyscan_t yyscanner);
static list_ref_t extract_arg_types(list_ref_t parameters);
static list_ref_t extract_aggr_arg_types(list_ref_t aggrargs);
static list_ref_t make_ordered_set_args(list_ref_t directargs, list_ref_t orderedargs,
								core_yyscan_t yyscanner);
static void insert_select_options(sq_select_stmt_t *stmt,
								list_ref_t sort_clause, list_ref_t locking_clause,
								select_limit_t *limit_clause,
								sq_with_clause_t *with_clause,
								core_yyscan_t yyscanner);
static sq_node_t *make_set_op(sq_set_operation_t op, bool all, sq_node_t *larg, sq_node_t *rarg);
static sq_node_t *do_negate(sq_node_t *n, int location);
static void do_negate_float(sq_float_t *v);
static sq_node_t *make_and_expr(sq_node_t *lexpr, sq_node_t *rexpr, int location);
static sq_node_t *make_or_expr(sq_node_t *lexpr, sq_node_t *rexpr, int location);
static sq_node_t *make_not_expr(sq_node_t *expr, int location);
static sq_node_t *make_a_array_expr(list_ref_t elements, int location, int end_location);
static sq_node_t *make_sql_value_function(sq_sql_value_function_op_t op, int32_t typmod,
								  int location);
static sq_node_t *make_xml_expr(sq_xml_expr_op_t op, char *name, list_ref_t named_args,
						 list_ref_t args, int location);
static list_ref_t merge_table_func_parameters(list_ref_t func_args, list_ref_t columns, core_yyscan_t yyscanner);
static sq_type_name_t *table_func_type_name(list_ref_t columns);
static sq_range_var_t *make_range_var_from_any_name(list_ref_t names, int position, core_yyscan_t yyscanner);
static sq_range_var_t *make_range_var_from_qualified_name(char *name, list_ref_t namelist, int location,
											   core_yyscan_t yyscanner);
static void split_col_qual_list(list_ref_t qual_list,
							 sq_list_t **constraint_list, sq_collate_clause_t **coll_clause,
							 core_yyscan_t yyscanner);
static void process_ca_sbits(int cas_bits, int location, const char *constr_type,
			   bool *deferrable, bool *initdeferred, bool *is_enforced,
			   bool *not_valid, bool *no_inherit, core_yyscan_t yyscanner);
static sq_partition_strategy_t parse_partition_strategy(char *strategy, int location,
												core_yyscan_t yyscanner);
static void preprocess_pubobj_list(list_ref_t pubobjspec_list,
								   core_yyscan_t yyscanner);
static sq_node_t *make_recursive_view_select(const char *relname, list_ref_t aliases, sq_node_t *query);

}

%define api.pure full
%expect 0
%define api.prefix {sq_base_yy}
%define api.location.type {int}
%locations

%parse-param {sq::core_yyscan_t yyscanner}
%lex-param   {sq::core_yyscan_t yyscanner}

%code provides {
/*
 * The grammar's source of tokens: the scanner, through the filter that
 * looks one token ahead (src/parser.cc).
 */
int sq_base_yylex(SQ_BASE_YYSTYPE *lvalp, int *llocp,
				  sq::core_yyscan_t yyscanner);
}

%union
{
	sq::scan_value_t core_yystype;
	/* these fields must match core_YYSTYPE: */
	int			ival;
	char	   *str;
	const char *keyword;

	char		chr;
	bool		boolean;
	sq_join_type_t	jtype;
	sq_drop_behavior_t dbehavior;
	sq_on_commit_action_t oncommit;
	sq::list_ref_t	   list;
	sq_node_t	   *node;
	sq_object_type_t	objtype;
	sq_type_name_t   *typnam;
	sq_function_parameter_t *fun_param;
	sq_function_parameter_mode_t fun_param_mode;
	sq_object_with_args_t *objwithargs;
	sq_def_elem_t	   *defelt;
	sq_sort_by_t	   *sortby;
	sq_window_def_t  *windef;
	sq_join_expr_t   *jexpr;
	sq_index_elem_t  *ielem;
	sq_stats_elem_t  *selem;
	sq_alias_t	   *alias;
	sq_range_var_t   *range;
	sq_into_clause_t *into;
	sq_with_clause_t *with;
	sq_infer_clause_t	*infer;
	sq_on_conflict_clause_t *onconflict;
	sq_a_indices_t  *aind;
	sq_res_target_t  *target;
	sq::priv_target_t *privtarget;
	sq_access_priv_t *accesspriv;
	sq::import_qual_t *importqual;
	sq_insert_stmt_t *istmt;
	sq_variable_set_stmt_t *vsetstmt;
	sq_partition_elem_t *partelem;
	sq_partition_spec_t *partspec;
	sq_partition_bound_spec_t *partboundspec;
	sq_role_spec_t   *rolespec;
	sq_publication_obj_spec_t *publicationobjectspec;
	sq::select_limit_t *selectlimit;
	sq_set_quantifier_t setquantifier;
	sq::group_clause_t *groupclause;
	sq_merge_match_kind_t mergematch;
	sq_merge_when_clause_t *mergewhen;
	sq::key_actions_t *keyactions;
	sq::key_action_t *keyaction;
	sq_returning_clause_t *retclause;
	sq_returning_option_kind_t retoptionkind;
}

%type <node>	stmt toplevel_stmt schema_stmt routine_body_stmt
		AlterEventTrigStmt AlterCollationStmt
		AlterDatabaseStmt AlterDatabaseSetStmt AlterDomainStmt AlterEnumStmt
		AlterFdwStmt AlterForeignServerStmt AlterGroupStmt
		AlterObjectDependsStmt AlterObjectSchemaStmt AlterOwnerStmt
		AlterOperatorStmt AlterTypeStmt AlterSeqStmt AlterSystemStmt AlterTableStmt
		AlterTblSpcStmt AlterExtensionStmt AlterExtensionContentsStmt
		AlterCompositeTypeStmt AlterUserMappingStmt
		AlterRoleStmt AlterRoleSetStmt AlterPolicyStmt AlterStatsStmt
		AlterDefaultPrivilegesStmt DefACLAction
		AnalyzeStmt CallStmt ClosePortalStmt ClusterStmt CommentStmt
		ConstraintsSetStmt CopyStmt CreateAsStmt CreateCastStmt
		CreateDomainStmt CreateExtensionStmt CreateGroupStmt CreateOpClassStmt
		CreateOpFamilyStmt AlterOpFamilyStmt CreatePLangStmt
		CreateSchemaStmt CreateSeqStmt CreateStmt CreateStatsStmt CreateTableSpaceStmt
		CreateFdwStmt CreateForeignServerStmt CreateForeignTableStmt
		CreateAssertionStmt CreateTransformStmt CreateTrigStmt CreateEventTrigStmt
		CreateUserStmt CreateUserMappingStmt CreateRoleStmt CreatePolicyStmt
		CreatedbStmt DeclareCursorStmt DefineStmt DeleteStmt DiscardStmt DoStmt
		DropOpClassStmt DropOpFamilyStmt DropStmt
		DropCastStmt DropRoleStmt
		DropdbStmt DropTableSpaceStmt
		DropTransformStmt
		DropUserMappingStmt ExplainStmt FetchStmt
		GrantStmt GrantRoleStmt ImportForeignSchemaStmt IndexStmt InsertStmt
		ListenStmt LoadStmt LockStmt MergeStmt NotifyStmt ExplainableStmt PreparableStmt
		CreateFunctionStmt AlterFunctionStmt ReindexStmt RemoveAggrStmt
		RemoveFuncStmt RemoveOperStmt RenameStmt ReturnStmt RevokeStmt RevokeRoleStmt
		RuleActionStmt RuleActionStmtOrEmpty RuleStmt
		SecLabelStmt SelectStmt TransactionStmt TransactionStmtLegacy TruncateStmt
		UnlistenStmt UpdateStmt VacuumStmt
		VariableResetStmt VariableSetStmt VariableShowStmt
		ViewStmt CheckPointStmt CreateConversionStmt
		DeallocateStmt PrepareStmt ExecuteStmt
		DropOwnedStmt ReassignOwnedStmt
		AlterTSConfigurationStmt AlterTSDictionaryStmt
		CreateMatViewStmt RefreshMatViewStmt CreateAmStmt
		CreatePublicationStmt AlterPublicationStmt
		CreateSubscriptionStmt AlterSubscriptionStmt DropSubscriptionStmt

%type <node>	select_no_parens select_with_parens select_clause
				simple_select values_clause
				PLpgSQL_Expr PLAssignStmt

%type <str>			opt_single_name
%type <list>		opt_qualified_name
%type <boolean>		opt_concurrently
%type <dbehavior>	opt_drop_behavior

%type <node>	alter_column_default opclass_item opclass_drop alter_using
%type <ival>	add_drop opt_asc_desc opt_nulls_order

%type <node>	alter_table_cmd alter_type_cmd opt_collate_clause
	   replica_identity partition_cmd index_partition_cmd
%type <list>	alter_table_cmds alter_type_cmds
%type <list>    alter_identity_column_option_list
%type <defelt>  alter_identity_column_option
%type <node>	set_statistics_value
%type <str>		set_access_method_name

%type <list>	createdb_opt_list createdb_opt_items copy_opt_list
				transaction_mode_list
				create_extension_opt_list alter_extension_opt_list
%type <defelt>	createdb_opt_item copy_opt_item
				transaction_mode_item
				create_extension_opt_item alter_extension_opt_item

%type <ival>	opt_lock lock_type cast_context
%type <str>		utility_option_name
%type <defelt>	utility_option_elem
%type <list>	utility_option_list
%type <node>	utility_option_arg
%type <defelt>	drop_option
%type <boolean>	opt_or_replace opt_no
				opt_grant_grant_option
				opt_nowait opt_if_exists opt_with_data
				opt_transaction_chain
%type <list>	grant_role_opt_list
%type <defelt>	grant_role_opt
%type <node>	grant_role_opt_value
%type <ival>	opt_nowait_or_skip

%type <list>	OptRoleList AlterOptRoleList
%type <defelt>	CreateOptRoleElem AlterOptRoleElem

%type <str>		opt_type
%type <str>		foreign_server_version opt_foreign_server_version
%type <str>		opt_in_database

%type <str>		parameter_name
%type <list>	OptSchemaEltList parameter_name_list

%type <chr>		am_type

%type <boolean> TriggerForSpec TriggerForType
%type <ival>	TriggerActionTime
%type <list>	TriggerEvents TriggerOneEvent
%type <node>	TriggerFuncArg
%type <node>	TriggerWhen
%type <str>		TransitionRelName
%type <boolean>	TransitionRowOrTable TransitionOldOrNew
%type <node>	TriggerTransition

%type <list>	event_trigger_when_list event_trigger_value_list
%type <defelt>	event_trigger_when_item
%type <chr>		enable_trigger

%type <str>		copy_file_name
				access_method_clause attr_name
				table_access_method_clause name cursor_name file_name
				cluster_index_specification

%type <list>	func_name handler_name qual_Op qual_all_Op subquery_Op
				opt_inline_handler opt_validator validator_clause
				opt_collate

%type <range>	qualified_name insert_target OptConstrFromTable

%type <str>		all_Op MathOp

%type <str>		row_security_cmd RowSecurityDefaultForCmd
%type <boolean> RowSecurityDefaultPermissive
%type <node>	RowSecurityOptionalWithCheck RowSecurityOptionalExpr
%type <list>	RowSecurityDefaultToRole RowSecurityOptionalToRole

%type <str>		iso_level opt_encoding
%type <rolespec> grantee
%type <list>	grantee_list
%type <accesspriv> privilege
%type <list>	privileges privilege_list
%type <privtarget> privilege_target
%type <objwithargs> function_with_argtypes aggregate_with_argtypes operator_with_argtypes
%type <list>	function_with_argtypes_list aggregate_with_argtypes_list operator_with_argtypes_list
%type <ival>	defacl_privilege_target
%type <defelt>	DefACLOption
%type <list>	DefACLOptionList
%type <ival>	import_qualification_type
%type <importqual> import_qualification
%type <node>	vacuum_relation
%type <selectlimit> opt_select_limit select_limit limit_clause

%type <list>	parse_toplevel stmtmulti routine_body_stmt_list
				OptTableElementList TableElementList OptInherit definition
				OptTypedTableElementList TypedTableElementList
				reloptions opt_reloptions
				OptWith opt_definition func_args func_args_list
				func_args_with_defaults func_args_with_defaults_list
				aggr_args aggr_args_list
				func_as createfunc_opt_list opt_createfunc_opt_list alterfunc_opt_list
				old_aggr_definition old_aggr_list
				oper_argtypes RuleActionList RuleActionMulti
				opt_column_list columnList opt_name_list
				sort_clause opt_sort_clause sortby_list index_params
				stats_params
				opt_include opt_c_include index_including_params
				name_list role_list from_clause from_list opt_array_bounds
				qualified_name_list any_name any_name_list type_name_list
				any_operator expr_list attrs
				distinct_clause opt_distinct_clause
				target_list opt_target_list insert_column_list set_target_list
				merge_values_clause
				set_clause_list set_clause
				def_list operator_def_list indirection opt_indirection
				reloption_list TriggerFuncArgs opclass_item_list opclass_drop_list
				opclass_purpose opt_opfamily transaction_mode_list_or_empty
				OptTableFuncElementList TableFuncElementList opt_type_modifiers
				prep_type_clause
				execute_param_clause using_clause
				returning_with_clause returning_options
				opt_enum_val_list enum_val_list table_func_column_list
				create_generic_options alter_generic_options
				relation_expr_list dostmt_opt_list
				transform_element_list transform_type_list
				TriggerTransitions TriggerReferencing
				vacuum_relation_list opt_vacuum_relation_list
				drop_option_list pub_obj_list

%type <retclause> returning_clause
%type <node>	returning_option
%type <retoptionkind> returning_option_kind
%type <node>	opt_routine_body
%type <groupclause> group_clause
%type <list>	group_by_list
%type <node>	group_by_item empty_grouping_set rollup_clause cube_clause
%type <node>	grouping_sets_clause

%type <list>	opt_fdw_options fdw_options
%type <defelt>	fdw_option

%type <range>	OptTempTableName
%type <into>	into_clause create_as_target create_mv_target

%type <defelt>	createfunc_opt_item common_func_opt_item dostmt_opt_item
%type <fun_param> func_arg func_arg_with_default table_func_column aggr_arg
%type <fun_param_mode> arg_class
%type <typnam>	func_return func_type

%type <boolean>  opt_trusted opt_restart_seqs
%type <ival>	 OptTemp
%type <ival>	 OptNoLog
%type <oncommit> OnCommitOption

%type <ival>	for_locking_strength
%type <node>	for_locking_item
%type <list>	for_locking_clause opt_for_locking_clause for_locking_items
%type <list>	locked_rels_list
%type <setquantifier> set_quantifier

%type <node>	join_qual
%type <jtype>	join_type

%type <list>	extract_list overlay_list position_list
%type <list>	substr_list trim_list
%type <list>	opt_interval interval_second
%type <str>		unicode_normal_form

%type <boolean> opt_instead
%type <boolean> opt_unique opt_verbose opt_full
%type <boolean> opt_freeze opt_analyze opt_default
%type <defelt>	opt_binary copy_delimiter

%type <boolean> copy_from opt_program

%type <ival>	event cursor_options opt_hold opt_set_data
%type <objtype>	object_type_any_name object_type_name object_type_name_on_any_name
				drop_type_name

%type <node>	fetch_args select_limit_value
				offset_clause select_offset_value
				select_fetch_first_value I_or_F_const
%type <ival>	row_or_rows first_or_next

%type <list>	OptSeqOptList SeqOptList OptParenthesizedSeqOptList
%type <defelt>	SeqOptElem

%type <istmt>	insert_rest
%type <infer>	opt_conf_expr
%type <onconflict> opt_on_conflict
%type <mergewhen>	merge_insert merge_update merge_delete

%type <mergematch> merge_when_tgt_matched merge_when_tgt_not_matched
%type <node>	merge_when_clause opt_merge_when_condition
%type <list>	merge_when_list

%type <vsetstmt> generic_set set_rest set_rest_more generic_reset reset_rest
				 SetResetClause FunctionSetResetClause

%type <node>	TableElement TypedTableElement ConstraintElem DomainConstraintElem TableFuncElement
%type <node>	columnDef columnOptions optionalPeriodName
%type <defelt>	def_elem reloption_elem old_aggr_elem operator_def_elem
%type <node>	def_arg columnElem where_clause where_or_current_clause
				a_expr b_expr c_expr AexprConst indirection_el opt_slice_bound
				columnref having_clause func_table xmltable array_expr
				OptWhereClause operator_def_arg
%type <list>	opt_column_and_period_list
%type <list>	rowsfrom_item rowsfrom_list opt_col_def_list
%type <boolean> opt_ordinality opt_without_overlaps
%type <list>	ExclusionConstraintList ExclusionConstraintElem
%type <list>	func_arg_list func_arg_list_opt
%type <node>	func_arg_expr
%type <list>	row explicit_row implicit_row type_list array_expr_list
%type <node>	case_expr case_arg when_clause case_default
%type <list>	when_clause_list
%type <node>	opt_search_clause opt_cycle_clause
%type <ival>	sub_type opt_materialized
%type <node>	NumericOnly
%type <list>	NumericOnly_list
%type <alias>	alias_clause opt_alias_clause opt_alias_clause_for_join_using
%type <list>	func_alias_clause
%type <sortby>	sortby
%type <ielem>	index_elem index_elem_options
%type <selem>	stats_param
%type <node>	table_ref
%type <jexpr>	joined_table
%type <range>	relation_expr
%type <range>	extended_relation_expr
%type <range>	relation_expr_opt_alias
%type <node>	tablesample_clause opt_repeatable_clause
%type <target>	target_el set_target insert_column_item

%type <str>		generic_option_name
%type <node>	generic_option_arg
%type <defelt>	generic_option_elem alter_generic_option_elem
%type <list>	generic_option_list alter_generic_option_list

%type <ival>	reindex_target_relation reindex_target_all
%type <list>	opt_reindex_option_list

%type <node>	copy_generic_opt_arg copy_generic_opt_arg_list_item
%type <defelt>	copy_generic_opt_elem
%type <list>	copy_generic_opt_list copy_generic_opt_arg_list
%type <list>	copy_options

%type <typnam>	Typename SimpleTypename ConstTypename
				GenericType Numeric opt_float JsonType
				Character ConstCharacter
				CharacterWithLength CharacterWithoutLength
				ConstDatetime ConstInterval
				Bit ConstBit BitWithLength BitWithoutLength
%type <str>		character
%type <str>		extract_arg
%type <boolean> opt_varying opt_timezone opt_no_inherit

%type <ival>	Iconst SignedIconst
%type <str>		Sconst comment_text notify_payload
%type <str>		RoleId opt_boolean_or_string
%type <list>	var_list
%type <str>		ColId ColLabel BareColLabel
%type <str>		NonReservedWord NonReservedWord_or_Sconst
%type <str>		var_name type_function_name param_name
%type <str>		createdb_opt_name plassign_target
%type <node>	var_value zone_value
%type <rolespec> auth_ident RoleSpec opt_granted_by
%type <publicationobjectspec> PublicationObjSpec

%type <keyword> unreserved_keyword type_func_name_keyword
%type <keyword> col_name_keyword reserved_keyword
%type <keyword> bare_label_keyword

%type <node>	DomainConstraint TableConstraint TableLikeClause
%type <ival>	TableLikeOptionList TableLikeOption
%type <str>		column_compression opt_column_compression column_storage opt_column_storage
%type <list>	ColQualList
%type <node>	ColConstraint ColConstraintElem ConstraintAttr
%type <ival>	key_match
%type <keyaction> key_delete key_update key_action
%type <keyactions> key_actions
%type <ival>	ConstraintAttributeSpec ConstraintAttributeElem
%type <str>		ExistingIndex

%type <list>	constraints_set_list
%type <boolean> constraints_set_mode
%type <str>		OptTableSpace OptConsTableSpace
%type <rolespec> OptTableSpaceOwner
%type <ival>	opt_check_option

%type <str>		opt_provider security_label

%type <target>	xml_attribute_el
%type <list>	xml_attribute_list xml_attributes
%type <node>	xml_root_version opt_xml_root_standalone
%type <node>	xmlexists_argument
%type <ival>	document_or_content
%type <boolean>	xml_indent_option xml_whitespace_option
%type <list>	xmltable_column_list xmltable_column_option_list
%type <node>	xmltable_column_el
%type <defelt>	xmltable_column_option_el
%type <list>	xml_namespace_list
%type <target>	xml_namespace_el

%type <node>	func_application func_expr_common_subexpr
%type <node>	func_expr func_expr_windowless
%type <node>	common_table_expr
%type <with>	with_clause opt_with_clause
%type <list>	cte_list

%type <list>	within_group_clause
%type <node>	filter_clause
%type <list>	window_clause window_definition_list opt_partition_clause
%type <windef>	window_definition over_clause window_specification
				opt_frame_clause frame_extent frame_bound
%type <ival>	opt_window_exclusion_clause
%type <str>		opt_existing_window_name
%type <boolean> opt_if_not_exists
%type <boolean> opt_unique_null_treatment
%type <ival>	generated_when override_kind opt_virtual_or_stored
%type <partspec>	PartitionSpec OptPartitionSpec
%type <partelem>	part_elem
%type <list>		part_params
%type <partboundspec> PartitionBoundSpec
%type <list>		hash_partbound
%type <defelt>		hash_partbound_elem

%type <node>	json_format_clause
				json_format_clause_opt
				json_value_expr
				json_returning_clause_opt
				json_name_and_value
				json_aggregate_func
				json_argument
				json_behavior
				json_on_error_clause_opt
				json_table
				json_table_column_definition
				json_table_column_path_clause_opt
%type <list>	json_name_and_value_list
				json_value_expr_list
				json_array_aggregate_order_by_clause_opt
				json_arguments
				json_behavior_clause_opt
				json_passing_clause_opt
				json_table_column_definition_list
%type <str>		json_table_path_name_opt
%type <ival>	json_behavior_type
				json_predicate_type_constraint
				json_quotes_clause_opt
				json_wrapper_behavior
%type <boolean>	json_key_uniqueness_constraint_opt
				json_object_constructor_null_clause_opt
				json_array_constructor_null_clause_opt


/*
 * Non-keyword token types.  These are hard-wired into the "flex" lexer.
 * They must be listed first so that their numeric codes do not depend on
 * the set of keywords.  PL/pgSQL depends on this so that it can share the
 * same lexer.  If you add/change tokens here, fix PL/pgSQL to match!
 *
 * UIDENT and USCONST are reduced to IDENT and SCONST in parser.c, so that
 * they need no productions here; but we must assign token codes to them.
 *
 * DOT_DOT is unused in the core SQL grammar, and so will always provoke
 * parse errors.  It is needed by PL/pgSQL.
 */
%token <str>	IDENT UIDENT FCONST SCONST USCONST BCONST XCONST Op
%token <ival>	ICONST PARAM
%token			TYPECAST DOT_DOT COLON_EQUALS EQUALS_GREATER
%token			LESS_EQUALS GREATER_EQUALS NOT_EQUALS

/*
 * If you want to make any keyword changes, update the keyword table in
 * src/include/parser/kwlist.h and add new keywords to the appropriate one
 * of the reserved-or-not-so-reserved keyword lists, below; search
 * this file for "Keyword category lists".
 */

/* ordinary key words in alphabetical order */
%token <keyword> ABORT_P ABSENT ABSOLUTE_P ACCESS ACTION ADD_P ADMIN AFTER
	AGGREGATE ALL ALSO ALTER ALWAYS ANALYSE ANALYZE AND ANY ARRAY AS ASC
	ASENSITIVE ASSERTION ASSIGNMENT ASYMMETRIC ATOMIC AT ATTACH ATTRIBUTE AUTHORIZATION

	BACKWARD BEFORE BEGIN_P BETWEEN BIGINT BINARY BIT
	BOOLEAN_P BOTH BREADTH BY

	CACHE CALL CALLED CASCADE CASCADED CASE CAST CATALOG_P CHAIN CHAR_P
	CHARACTER CHARACTERISTICS CHECK CHECKPOINT CLASS CLOSE
	CLUSTER COALESCE COLLATE COLLATION COLUMN COLUMNS COMMENT COMMENTS COMMIT
	COMMITTED COMPRESSION CONCURRENTLY CONDITIONAL CONFIGURATION CONFLICT
	CONNECTION CONSTRAINT CONSTRAINTS CONTENT_P CONTINUE_P CONVERSION_P COPY
	COST CREATE CROSS CSV CUBE CURRENT_P
	CURRENT_CATALOG CURRENT_DATE CURRENT_ROLE CURRENT_SCHEMA
	CURRENT_TIME CURRENT_TIMESTAMP CURRENT_USER CURSOR CYCLE

	DATA_P DATABASE DAY_P DEALLOCATE DEC DECIMAL_P DECLARE DEFAULT DEFAULTS
	DEFERRABLE DEFERRED DEFINER DELETE_P DELIMITER DELIMITERS DEPENDS DEPTH DESC
	DETACH DICTIONARY DISABLE_P DISCARD DISTINCT DO DOCUMENT_P DOMAIN_P
	DOUBLE_P DROP

	EACH ELSE EMPTY_P ENABLE_P ENCODING ENCRYPTED END_P ENFORCED ENUM_P ERROR_P
	ESCAPE EVENT EXCEPT EXCLUDE EXCLUDING EXCLUSIVE EXECUTE EXISTS EXPLAIN
	EXPRESSION EXTENSION EXTERNAL EXTRACT

	FALSE_P FAMILY FETCH FILTER FINALIZE FIRST_P FLOAT_P FOLLOWING FOR
	FORCE FOREIGN FORMAT FORWARD FREEZE FROM FULL FUNCTION FUNCTIONS

	GENERATED GLOBAL GRANT GRANTED GREATEST GROUP_P GROUPING GROUPS

	HANDLER HAVING HEADER_P HOLD HOUR_P

	IDENTITY_P IF_P ILIKE IMMEDIATE IMMUTABLE IMPLICIT_P IMPORT_P IN_P INCLUDE
	INCLUDING INCREMENT INDENT INDEX INDEXES INHERIT INHERITS INITIALLY INLINE_P
	INNER_P INOUT INPUT_P INSENSITIVE INSERT INSTEAD INT_P INTEGER
	INTERSECT INTERVAL INTO INVOKER IS ISNULL ISOLATION

	JOIN JSON JSON_ARRAY JSON_ARRAYAGG JSON_EXISTS JSON_OBJECT JSON_OBJECTAGG
	JSON_QUERY JSON_SCALAR JSON_SERIALIZE JSON_TABLE JSON_VALUE

	KEEP KEY KEYS

	LABEL LANGUAGE LARGE_P LAST_P LATERAL_P
	LEADING LEAKPROOF LEAST LEFT LEVEL LIKE LIMIT LISTEN LOAD LOCAL
	LOCALTIME LOCALTIMESTAMP LOCATION LOCK_P LOCKED LOGGED

	MAPPING MATCH MATCHED MATERIALIZED MAXVALUE MERGE MERGE_ACTION METHOD
	MINUTE_P MINVALUE MODE MONTH_P MOVE

	NAME_P NAMES NATIONAL NATURAL NCHAR NESTED NEW NEXT NFC NFD NFKC NFKD NO
	NONE NORMALIZE NORMALIZED
	NOT NOTHING NOTIFY NOTNULL NOWAIT NULL_P NULLIF
	NULLS_P NUMERIC

	OBJECT_P OBJECTS_P OF OFF OFFSET OIDS OLD OMIT ON ONLY OPERATOR OPTION OPTIONS OR
	ORDER ORDINALITY OTHERS OUT_P OUTER_P
	OVER OVERLAPS OVERLAY OVERRIDING OWNED OWNER

	PARALLEL PARAMETER PARSER PARTIAL PARTITION PASSING PASSWORD PATH
	PERIOD PLACING PLAN PLANS POLICY
	POSITION PRECEDING PRECISION PRESERVE PREPARE PREPARED PRIMARY
	PRIOR PRIVILEGES PROCEDURAL PROCEDURE PROCEDURES PROGRAM PUBLICATION

	QUOTE QUOTES

	RANGE READ REAL REASSIGN RECURSIVE REF_P REFERENCES REFERENCING
	REFRESH REINDEX RELATIVE_P RELEASE RENAME REPEATABLE REPLACE REPLICA
	RESET RESTART RESTRICT RETURN RETURNING RETURNS REVOKE RIGHT ROLE ROLLBACK ROLLUP
	ROUTINE ROUTINES ROW ROWS RULE

	SAVEPOINT SCALAR SCHEMA SCHEMAS SCROLL SEARCH SECOND_P SECURITY SELECT
	SEQUENCE SEQUENCES
	SERIALIZABLE SERVER SESSION SESSION_USER SET SETS SETOF SHARE SHOW
	SIMILAR SIMPLE SKIP SMALLINT SNAPSHOT SOME SOURCE SQL_P STABLE STANDALONE_P
	START STATEMENT STATISTICS STDIN STDOUT STORAGE STORED STRICT_P STRING_P STRIP_P
	SUBSCRIPTION SUBSTRING SUPPORT SYMMETRIC SYSID SYSTEM_P SYSTEM_USER

	TABLE TABLES TABLESAMPLE TABLESPACE TARGET TEMP TEMPLATE TEMPORARY TEXT_P THEN
	TIES TIME TIMESTAMP TO TRAILING TRANSACTION TRANSFORM
	TREAT TRIGGER TRIM TRUE_P
	TRUNCATE TRUSTED TYPE_P TYPES_P

	UESCAPE UNBOUNDED UNCONDITIONAL UNCOMMITTED UNENCRYPTED UNION UNIQUE UNKNOWN
	UNLISTEN UNLOGGED UNTIL UPDATE USER USING

	VACUUM VALID VALIDATE VALIDATOR VALUE_P VALUES VARCHAR VARIADIC VARYING
	VERBOSE VERSION_P VIEW VIEWS VIRTUAL VOLATILE

	WHEN WHERE WHITESPACE_P WINDOW WITH WITHIN WITHOUT WORK WRAPPER WRITE

	XML_P XMLATTRIBUTES XMLCONCAT XMLELEMENT XMLEXISTS XMLFOREST XMLNAMESPACES
	XMLPARSE XMLPI XMLROOT XMLSERIALIZE XMLTABLE

	YEAR_P YES_P

	ZONE

/*
 * The grammar thinks these are keywords, but they are not in the kwlist.h
 * list and so can never be entered directly.  The filter in parser.c
 * creates these tokens when required (based on looking one token ahead).
 *
 * NOT_LA exists so that productions such as NOT LIKE can be given the same
 * precedence as LIKE; otherwise they'd effectively have the same precedence
 * as NOT, at least with respect to their left-hand subexpression.
 * FORMAT_LA, NULLS_LA, WITH_LA, and WITHOUT_LA are needed to make the grammar
 * LALR(1).
 */
%token		FORMAT_LA NOT_LA NULLS_LA WITH_LA WITHOUT_LA

/*
 * The grammar likewise thinks these tokens are keywords, but they are never
 * generated by the scanner.  Rather, they can be injected by parser.c as
 * the initial token of the string (using the lookahead-token mechanism
 * implemented there).  This provides a way to tell the grammar to parse
 * something other than the usual list of SQL commands.
 */
%token		MODE_TYPE_NAME
%token		MODE_PLPGSQL_EXPR
%token		MODE_PLPGSQL_ASSIGN1
%token		MODE_PLPGSQL_ASSIGN2
%token		MODE_PLPGSQL_ASSIGN3


/* Precedence: lowest to highest */
%left		UNION EXCEPT
%left		INTERSECT
%left		OR
%left		AND
%right		NOT
%nonassoc	IS ISNULL NOTNULL	/* IS sets precedence for IS NULL, etc */
%nonassoc	'<' '>' '=' LESS_EQUALS GREATER_EQUALS NOT_EQUALS
%nonassoc	BETWEEN IN_P LIKE ILIKE SIMILAR NOT_LA
%nonassoc	ESCAPE			/* ESCAPE must be just above LIKE/ILIKE/SIMILAR */

/*
 * Sometimes it is necessary to assign precedence to keywords that are not
 * really part of the operator hierarchy, in order to resolve grammar
 * ambiguities.  It's best to avoid doing so whenever possible, because such
 * assignments have global effect and may hide ambiguities besides the one
 * you intended to solve.  (Attaching a precedence to a single rule with
 * %prec is far safer and should be preferred.)  If you must give precedence
 * to a new keyword, try very hard to give it the same precedence as IDENT.
 * If the keyword has IDENT's precedence then it clearly acts the same as
 * non-keywords and other similar keywords, thus reducing the risk of
 * unexpected precedence effects.
 *
 * We used to need to assign IDENT an explicit precedence just less than Op,
 * to support target_el without AS.  While that's not really necessary since
 * we removed postfix operators, we continue to do so because it provides a
 * reference point for a precedence level that we can assign to other
 * keywords that lack a natural precedence level.
 *
 * We need to do this for PARTITION, RANGE, ROWS, and GROUPS to support
 * opt_existing_window_name (see comment there).
 *
 * The frame_bound productions UNBOUNDED PRECEDING and UNBOUNDED FOLLOWING
 * are even messier: since UNBOUNDED is an unreserved keyword (per spec!),
 * there is no principled way to distinguish these from the productions
 * a_expr PRECEDING/FOLLOWING.  We hack this up by giving UNBOUNDED slightly
 * lower precedence than PRECEDING and FOLLOWING.  At present this doesn't
 * appear to cause UNBOUNDED to be treated differently from other unreserved
 * keywords anywhere else in the grammar, but it's definitely risky.  We can
 * blame any funny behavior of UNBOUNDED on the SQL standard, though.
 *
 * To support CUBE and ROLLUP in GROUP BY without reserving them, we give them
 * an explicit priority lower than '(', so that a rule with CUBE '(' will shift
 * rather than reducing a conflicting rule that takes CUBE as a function name.
 * Using the same precedence as IDENT seems right for the reasons given above.
 *
 * SET is likewise assigned the same precedence as IDENT, to support the
 * relation_expr_opt_alias production (see comment there).
 *
 * KEYS, OBJECT_P, SCALAR, VALUE_P, WITH, and WITHOUT are similarly assigned
 * the same precedence as IDENT.  This allows resolving conflicts in the
 * json_predicate_type_constraint and json_key_uniqueness_constraint_opt
 * productions (see comments there).
 *
 * Like the UNBOUNDED PRECEDING/FOLLOWING case, NESTED is assigned a lower
 * precedence than PATH to fix ambiguity in the json_table production.
 */
%nonassoc	UNBOUNDED NESTED /* ideally would have same precedence as IDENT */
%nonassoc	IDENT PARTITION RANGE ROWS GROUPS PRECEDING FOLLOWING CUBE ROLLUP
			SET KEYS OBJECT_P SCALAR VALUE_P WITH WITHOUT PATH
%left		Op OPERATOR		/* multi-character ops and user-defined operators */
%left		'+' '-'
%left		'*' '/' '%'
%left		'^'
/* Unary Operators */
%left		AT				/* sets precedence for AT TIME ZONE, AT LOCAL */
%left		COLLATE
%right		UMINUS
%left		'[' ']'
%left		'(' ')'
%left		TYPECAST
%left		'.'
/*
 * These might seem to be low-precedence, but actually they are not part
 * of the arithmetic hierarchy at all in their use as JOIN operators.
 * We make them high-precedence to support their use as function names.
 * They wouldn't be given a precedence at all, were it not that we need
 * left-associativity among the JOIN rules themselves.
 */
%left		JOIN CROSS LEFT FULL RIGHT INNER_P NATURAL

%%

/*
 *	The target production for the whole parse.
 *
 * Ordinarily we parse a list of statements, but if we see one of the
 * special MODE_XXX symbols as first token, we parse something else.
 * The options here correspond to enum RawParseMode, which see for details.
 */
parse_toplevel:
			stmtmulti
			{
				parse_state(yyscanner)->parsetree = $1;
				(void) yynerrs;		/* suppress compiler warning */
			}
			| MODE_TYPE_NAME Typename
			{
				parse_state(yyscanner)->parsetree = list_make1($2);
			}
			| MODE_PLPGSQL_EXPR PLpgSQL_Expr
			{
				parse_state(yyscanner)->parsetree =
					list_make1(make_raw_stmt($2, @2));
			}
			| MODE_PLPGSQL_ASSIGN1 PLAssignStmt
			{
				sq_pl_assign_stmt_t *n = (sq_pl_assign_stmt_t *) $2;

				n->nnames = 1;
				parse_state(yyscanner)->parsetree =
					list_make1(make_raw_stmt((sq_node_t *) n, @2));
			}
			| MODE_PLPGSQL_ASSIGN2 PLAssignStmt
			{
				sq_pl_assign_stmt_t *n = (sq_pl_assign_stmt_t *) $2;

				n->nnames = 2;
				parse_state(yyscanner)->parsetree =
					list_make1(make_raw_stmt((sq_node_t *) n, @2));
			}
			| MODE_PLPGSQL_ASSIGN3 PLAssignStmt
			{
				sq_pl_assign_stmt_t *n = (sq_pl_assign_stmt_t *) $2;

				n->nnames = 3;
				parse_state(yyscanner)->parsetree =
					list_make1(make_raw_stmt((sq_node_t *) n, @2));
			}
		;

/*
 * At top level, we wrap each stmt with a RawStmt node carrying start location
 * and length of the stmt's text.
 * We also take care to discard empty statements entirely (which among other
 * things dodges the problem of assigning them a location).
 */
stmtmulti:	stmtmulti ';' toplevel_stmt
				{
					if ($1 != NIL)
					{
						/* update length of previous stmt */
						update_raw_stmt_end(llast_node(sq_raw_stmt_t, $1), @2);
					}
					if ($3 != NULL)
						$$ = lappend($1, make_raw_stmt($3, @3));
					else
						$$ = $1;
				}
			| toplevel_stmt
				{
					if ($1 != NULL)
						$$ = list_make1(make_raw_stmt($1, @1));
					else
						$$ = NIL;
				}
		;

/*
 * toplevel_stmt includes BEGIN and END.  stmt does not include them, because
 * those words have different meanings in function bodies.
 */
toplevel_stmt:
			stmt
			| TransactionStmtLegacy
		;

stmt:
			AlterEventTrigStmt
			| AlterCollationStmt
			| AlterDatabaseStmt
			| AlterDatabaseSetStmt
			| AlterDefaultPrivilegesStmt
			| AlterDomainStmt
			| AlterEnumStmt
			| AlterExtensionStmt
			| AlterExtensionContentsStmt
			| AlterFdwStmt
			| AlterForeignServerStmt
			| AlterFunctionStmt
			| AlterGroupStmt
			| AlterObjectDependsStmt
			| AlterObjectSchemaStmt
			| AlterOwnerStmt
			| AlterOperatorStmt
			| AlterTypeStmt
			| AlterPolicyStmt
			| AlterSeqStmt
			| AlterSystemStmt
			| AlterTableStmt
			| AlterTblSpcStmt
			| AlterCompositeTypeStmt
			| AlterPublicationStmt
			| AlterRoleSetStmt
			| AlterRoleStmt
			| AlterSubscriptionStmt
			| AlterStatsStmt
			| AlterTSConfigurationStmt
			| AlterTSDictionaryStmt
			| AlterUserMappingStmt
			| AnalyzeStmt
			| CallStmt
			| CheckPointStmt
			| ClosePortalStmt
			| ClusterStmt
			| CommentStmt
			| ConstraintsSetStmt
			| CopyStmt
			| CreateAmStmt
			| CreateAsStmt
			| CreateAssertionStmt
			| CreateCastStmt
			| CreateConversionStmt
			| CreateDomainStmt
			| CreateExtensionStmt
			| CreateFdwStmt
			| CreateForeignServerStmt
			| CreateForeignTableStmt
			| CreateFunctionStmt
			| CreateGroupStmt
			| CreateMatViewStmt
			| CreateOpClassStmt
			| CreateOpFamilyStmt
			| CreatePublicationStmt
			| AlterOpFamilyStmt
			| CreatePolicyStmt
			| CreatePLangStmt
			| CreateSchemaStmt
			| CreateSeqStmt
			| CreateStmt
			| CreateSubscriptionStmt
			| CreateStatsStmt
			| CreateTableSpaceStmt
			| CreateTransformStmt
			| CreateTrigStmt
			| CreateEventTrigStmt
			| CreateRoleStmt
			| CreateUserStmt
			| CreateUserMappingStmt
			| CreatedbStmt
			| DeallocateStmt
			| DeclareCursorStmt
			| DefineStmt
			| DeleteStmt
			| DiscardStmt
			| DoStmt
			| DropCastStmt
			| DropOpClassStmt
			| DropOpFamilyStmt
			| DropOwnedStmt
			| DropStmt
			| DropSubscriptionStmt
			| DropTableSpaceStmt
			| DropTransformStmt
			| DropRoleStmt
			| DropUserMappingStmt
			| DropdbStmt
			| ExecuteStmt
			| ExplainStmt
			| FetchStmt
			| GrantStmt
			| GrantRoleStmt
			| ImportForeignSchemaStmt
			| IndexStmt
			| InsertStmt
			| ListenStmt
			| RefreshMatViewStmt
			| LoadStmt
			| LockStmt
			| MergeStmt
			| NotifyStmt
			| PrepareStmt
			| ReassignOwnedStmt
			| ReindexStmt
			| RemoveAggrStmt
			| RemoveFuncStmt
			| RemoveOperStmt
			| RenameStmt
			| RevokeStmt
			| RevokeRoleStmt
			| RuleStmt
			| SecLabelStmt
			| SelectStmt
			| TransactionStmt
			| TruncateStmt
			| UnlistenStmt
			| UpdateStmt
			| VacuumStmt
			| VariableResetStmt
			| VariableSetStmt
			| VariableShowStmt
			| ViewStmt
			| /*EMPTY*/
				{ $$ = NULL; }
		;

/*
 * Generic supporting productions for DDL
 */
opt_single_name:
			ColId							{ $$ = $1; }
			| /* EMPTY */					{ $$ = NULL; }
		;

opt_qualified_name:
			any_name						{ $$ = $1; }
			| /*EMPTY*/						{ $$ = NIL; }
		;

opt_concurrently:
			CONCURRENTLY					{ $$ = true; }
			| /*EMPTY*/						{ $$ = false; }
		;

opt_drop_behavior:
			CASCADE							{ $$ = SQ_DROP_CASCADE; }
			| RESTRICT						{ $$ = SQ_DROP_RESTRICT; }
			| /* EMPTY */					{ $$ = SQ_DROP_RESTRICT; /* default */ }
		;

/*****************************************************************************
 *
 * CALL statement
 *
 *****************************************************************************/

CallStmt:	CALL func_application
				{
					sq_call_stmt_t   *n = make_node<sq_call_stmt_t>();

					n->funccall = cast_node(sq_func_call_t, $2);
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 * Create a new Postgres DBMS role
 *
 *****************************************************************************/

CreateRoleStmt:
			CREATE ROLE RoleId opt_with OptRoleList
				{
					sq_create_role_stmt_t *n = make_node<sq_create_role_stmt_t>();

					n->stmt_type = SQ_ROLESTMT_ROLE;
					n->role = $3;
					n->options = $5;
					$$ = (sq_node_t *) n;
				}
		;


opt_with:	WITH
			| WITH_LA
			| /*EMPTY*/
		;

/*
 * Options for CREATE ROLE and ALTER ROLE (also used by CREATE/ALTER USER
 * for backwards compatibility).  Note: the only option required by SQL99
 * is "WITH ADMIN name".
 */
OptRoleList:
			OptRoleList CreateOptRoleElem			{ $$ = lappend($1, $2); }
			| /* EMPTY */							{ $$ = NIL; }
		;

AlterOptRoleList:
			AlterOptRoleList AlterOptRoleElem		{ $$ = lappend($1, $2); }
			| /* EMPTY */							{ $$ = NIL; }
		;

AlterOptRoleElem:
			PASSWORD Sconst
				{
					$$ = make_def_elem("password",
									 (sq_node_t *) make_string($2), @1);
				}
			| PASSWORD NULL_P
				{
					$$ = make_def_elem("password", NULL, @1);
				}
			| ENCRYPTED PASSWORD Sconst
				{
					/*
					 * These days, passwords are always stored in encrypted
					 * form, so there is no difference between PASSWORD and
					 * ENCRYPTED PASSWORD.
					 */
					$$ = make_def_elem("password",
									 (sq_node_t *) make_string($3), @1);
				}
			| UNENCRYPTED PASSWORD Sconst
				{
					raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@1), .hint = "Remove UNENCRYPTED to store the password in encrypted form instead."}, "UNENCRYPTED PASSWORD is no longer supported");
				}
			| INHERIT
				{
					$$ = make_def_elem("inherit", (sq_node_t *) make_boolean(true), @1);
				}
			| CONNECTION LIMIT SignedIconst
				{
					$$ = make_def_elem("connectionlimit", (sq_node_t *) make_integer($3), @1);
				}
			| VALID UNTIL Sconst
				{
					$$ = make_def_elem("validUntil", (sq_node_t *) make_string($3), @1);
				}
		/*	Supported but not documented for roles, for use by ALTER GROUP. */
			| USER role_list
				{
					$$ = make_def_elem("rolemembers", (sq_node_t *) $2, @1);
				}
			| IDENT
				{
					/*
					 * We handle identifiers that aren't parser keywords with
					 * the following special-case codes, to avoid bloating the
					 * size of the main parser.
					 */
					if (strcmp($1, "superuser") == 0)
						$$ = make_def_elem("superuser", (sq_node_t *) make_boolean(true), @1);
					else if (strcmp($1, "nosuperuser") == 0)
						$$ = make_def_elem("superuser", (sq_node_t *) make_boolean(false), @1);
					else if (strcmp($1, "createrole") == 0)
						$$ = make_def_elem("createrole", (sq_node_t *) make_boolean(true), @1);
					else if (strcmp($1, "nocreaterole") == 0)
						$$ = make_def_elem("createrole", (sq_node_t *) make_boolean(false), @1);
					else if (strcmp($1, "replication") == 0)
						$$ = make_def_elem("isreplication", (sq_node_t *) make_boolean(true), @1);
					else if (strcmp($1, "noreplication") == 0)
						$$ = make_def_elem("isreplication", (sq_node_t *) make_boolean(false), @1);
					else if (strcmp($1, "createdb") == 0)
						$$ = make_def_elem("createdb", (sq_node_t *) make_boolean(true), @1);
					else if (strcmp($1, "nocreatedb") == 0)
						$$ = make_def_elem("createdb", (sq_node_t *) make_boolean(false), @1);
					else if (strcmp($1, "login") == 0)
						$$ = make_def_elem("canlogin", (sq_node_t *) make_boolean(true), @1);
					else if (strcmp($1, "nologin") == 0)
						$$ = make_def_elem("canlogin", (sq_node_t *) make_boolean(false), @1);
					else if (strcmp($1, "bypassrls") == 0)
						$$ = make_def_elem("bypassrls", (sq_node_t *) make_boolean(true), @1);
					else if (strcmp($1, "nobypassrls") == 0)
						$$ = make_def_elem("bypassrls", (sq_node_t *) make_boolean(false), @1);
					else if (strcmp($1, "noinherit") == 0)
					{
						/*
						 * Note that INHERIT is a keyword, so it's handled by main parser, but
						 * NOINHERIT is handled here.
						 */
						$$ = make_def_elem("inherit", (sq_node_t *) make_boolean(false), @1);
					}
					else
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@1)}, "unrecognized role option \"{}\"", $1);
				}
		;

CreateOptRoleElem:
			AlterOptRoleElem			{ $$ = $1; }
			/* The following are not supported by ALTER ROLE/USER/GROUP */
			| SYSID Iconst
				{
					$$ = make_def_elem("sysid", (sq_node_t *) make_integer($2), @1);
				}
			| ADMIN role_list
				{
					$$ = make_def_elem("adminmembers", (sq_node_t *) $2, @1);
				}
			| ROLE role_list
				{
					$$ = make_def_elem("rolemembers", (sq_node_t *) $2, @1);
				}
			| IN_P ROLE role_list
				{
					$$ = make_def_elem("addroleto", (sq_node_t *) $3, @1);
				}
			| IN_P GROUP_P role_list
				{
					$$ = make_def_elem("addroleto", (sq_node_t *) $3, @1);
				}
		;


/*****************************************************************************
 *
 * Create a new Postgres DBMS user (role with implied login ability)
 *
 *****************************************************************************/

CreateUserStmt:
			CREATE USER RoleId opt_with OptRoleList
				{
					sq_create_role_stmt_t *n = make_node<sq_create_role_stmt_t>();

					n->stmt_type = SQ_ROLESTMT_USER;
					n->role = $3;
					n->options = $5;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 * Alter a postgresql DBMS role
 *
 *****************************************************************************/

AlterRoleStmt:
			ALTER ROLE RoleSpec opt_with AlterOptRoleList
				 {
					sq_alter_role_stmt_t *n = make_node<sq_alter_role_stmt_t>();

					n->role = $3;
					n->action = +1;	/* add, if there are members */
					n->options = $5;
					$$ = (sq_node_t *) n;
				 }
			| ALTER USER RoleSpec opt_with AlterOptRoleList
				 {
					sq_alter_role_stmt_t *n = make_node<sq_alter_role_stmt_t>();

					n->role = $3;
					n->action = +1;	/* add, if there are members */
					n->options = $5;
					$$ = (sq_node_t *) n;
				 }
		;

opt_in_database:
			   /* EMPTY */					{ $$ = NULL; }
			| IN_P DATABASE name	{ $$ = $3; }
		;

AlterRoleSetStmt:
			ALTER ROLE RoleSpec opt_in_database SetResetClause
				{
					sq_alter_role_set_stmt_t *n = make_node<sq_alter_role_set_stmt_t>();

					n->role = $3;
					n->database = $4;
					n->setstmt = $5;
					$$ = (sq_node_t *) n;
				}
			| ALTER ROLE ALL opt_in_database SetResetClause
				{
					sq_alter_role_set_stmt_t *n = make_node<sq_alter_role_set_stmt_t>();

					n->role = NULL;
					n->database = $4;
					n->setstmt = $5;
					$$ = (sq_node_t *) n;
				}
			| ALTER USER RoleSpec opt_in_database SetResetClause
				{
					sq_alter_role_set_stmt_t *n = make_node<sq_alter_role_set_stmt_t>();

					n->role = $3;
					n->database = $4;
					n->setstmt = $5;
					$$ = (sq_node_t *) n;
				}
			| ALTER USER ALL opt_in_database SetResetClause
				{
					sq_alter_role_set_stmt_t *n = make_node<sq_alter_role_set_stmt_t>();

					n->role = NULL;
					n->database = $4;
					n->setstmt = $5;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 * Drop a postgresql DBMS role
 *
 * XXX Ideally this would have CASCADE/RESTRICT options, but a role
 * might own objects in multiple databases, and there is presently no way to
 * implement cascading to other databases.  So we always behave as RESTRICT.
 *****************************************************************************/

DropRoleStmt:
			DROP ROLE role_list
				{
					sq_drop_role_stmt_t *n = make_node<sq_drop_role_stmt_t>();

					n->missing_ok = false;
					n->roles = $3;
					$$ = (sq_node_t *) n;
				}
			| DROP ROLE IF_P EXISTS role_list
				{
					sq_drop_role_stmt_t *n = make_node<sq_drop_role_stmt_t>();

					n->missing_ok = true;
					n->roles = $5;
					$$ = (sq_node_t *) n;
				}
			| DROP USER role_list
				{
					sq_drop_role_stmt_t *n = make_node<sq_drop_role_stmt_t>();

					n->missing_ok = false;
					n->roles = $3;
					$$ = (sq_node_t *) n;
				}
			| DROP USER IF_P EXISTS role_list
				{
					sq_drop_role_stmt_t *n = make_node<sq_drop_role_stmt_t>();

					n->roles = $5;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| DROP GROUP_P role_list
				{
					sq_drop_role_stmt_t *n = make_node<sq_drop_role_stmt_t>();

					n->missing_ok = false;
					n->roles = $3;
					$$ = (sq_node_t *) n;
				}
			| DROP GROUP_P IF_P EXISTS role_list
				{
					sq_drop_role_stmt_t *n = make_node<sq_drop_role_stmt_t>();

					n->missing_ok = true;
					n->roles = $5;
					$$ = (sq_node_t *) n;
				}
			;


/*****************************************************************************
 *
 * Create a postgresql group (role without login ability)
 *
 *****************************************************************************/

CreateGroupStmt:
			CREATE GROUP_P RoleId opt_with OptRoleList
				{
					sq_create_role_stmt_t *n = make_node<sq_create_role_stmt_t>();

					n->stmt_type = SQ_ROLESTMT_GROUP;
					n->role = $3;
					n->options = $5;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 * Alter a postgresql group
 *
 *****************************************************************************/

AlterGroupStmt:
			ALTER GROUP_P RoleSpec add_drop USER role_list
				{
					sq_alter_role_stmt_t *n = make_node<sq_alter_role_stmt_t>();

					n->role = $3;
					n->action = $4;
					n->options = list_make1(make_def_elem("rolemembers",
														(sq_node_t *) $6, @6));
					$$ = (sq_node_t *) n;
				}
		;

add_drop:	ADD_P									{ $$ = +1; }
			| DROP									{ $$ = -1; }
		;


/*****************************************************************************
 *
 * Manipulate a schema
 *
 *****************************************************************************/

CreateSchemaStmt:
			CREATE SCHEMA opt_single_name AUTHORIZATION RoleSpec OptSchemaEltList
				{
					sq_create_schema_stmt_t *n = make_node<sq_create_schema_stmt_t>();

					/* One can omit the schema name or the authorization id. */
					n->schemaname = $3;
					n->authrole = $5;
					n->schema_elts = $6;
					n->if_not_exists = false;
					$$ = (sq_node_t *) n;
				}
			| CREATE SCHEMA ColId OptSchemaEltList
				{
					sq_create_schema_stmt_t *n = make_node<sq_create_schema_stmt_t>();

					/* ...but not both */
					n->schemaname = $3;
					n->authrole = NULL;
					n->schema_elts = $4;
					n->if_not_exists = false;
					$$ = (sq_node_t *) n;
				}
			| CREATE SCHEMA IF_P NOT EXISTS opt_single_name AUTHORIZATION RoleSpec OptSchemaEltList
				{
					sq_create_schema_stmt_t *n = make_node<sq_create_schema_stmt_t>();

					/* schema name can be omitted here, too */
					n->schemaname = $6;
					n->authrole = $8;
					if ($9 != NIL)
						raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@9)}, "CREATE SCHEMA IF NOT EXISTS cannot include schema elements");
					n->schema_elts = $9;
					n->if_not_exists = true;
					$$ = (sq_node_t *) n;
				}
			| CREATE SCHEMA IF_P NOT EXISTS ColId OptSchemaEltList
				{
					sq_create_schema_stmt_t *n = make_node<sq_create_schema_stmt_t>();

					/* ...but not here */
					n->schemaname = $6;
					n->authrole = NULL;
					if ($7 != NIL)
						raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@7)}, "CREATE SCHEMA IF NOT EXISTS cannot include schema elements");
					n->schema_elts = $7;
					n->if_not_exists = true;
					$$ = (sq_node_t *) n;
				}
		;

OptSchemaEltList:
			OptSchemaEltList schema_stmt
				{
					$$ = lappend($1, $2);
				}
			| /* EMPTY */
				{ $$ = NIL; }
		;

/*
 *	schema_stmt are the ones that can show up inside a CREATE SCHEMA
 *	statement (in addition to by themselves).
 */
schema_stmt:
			CreateStmt
			| IndexStmt
			| CreateSeqStmt
			| CreateTrigStmt
			| GrantStmt
			| ViewStmt
		;


/*****************************************************************************
 *
 * Set PG internal variable
 *	  SET name TO 'var_value'
 * Include SQL syntax (thomas 1997-10-22):
 *	  SET TIME ZONE 'var_value'
 *
 *****************************************************************************/

VariableSetStmt:
			SET set_rest
				{
					sq_variable_set_stmt_t *n = $2;

					n->is_local = false;
					$$ = (sq_node_t *) n;
				}
			| SET LOCAL set_rest
				{
					sq_variable_set_stmt_t *n = $3;

					n->is_local = true;
					$$ = (sq_node_t *) n;
				}
			| SET SESSION set_rest
				{
					sq_variable_set_stmt_t *n = $3;

					n->is_local = false;
					$$ = (sq_node_t *) n;
				}
		;

set_rest:
			TRANSACTION transaction_mode_list
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_MULTI;
					n->name = "TRANSACTION";
					n->args = $2;
					n->jumble_args = true;
					n->location = -1;
					$$ = n;
				}
			| SESSION CHARACTERISTICS AS TRANSACTION transaction_mode_list
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_MULTI;
					n->name = "SESSION CHARACTERISTICS";
					n->args = $5;
					n->jumble_args = true;
					n->location = -1;
					$$ = n;
				}
			| set_rest_more
			;

generic_set:
			var_name TO var_list
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_VALUE;
					n->name = $1;
					n->args = $3;
					n->location = @3;
					$$ = n;
				}
			| var_name '=' var_list
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_VALUE;
					n->name = $1;
					n->args = $3;
					n->location = @3;
					$$ = n;
				}
			| var_name TO DEFAULT
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_DEFAULT;
					n->name = $1;
					n->location = -1;
					$$ = n;
				}
			| var_name '=' DEFAULT
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_DEFAULT;
					n->name = $1;
					n->location = -1;
					$$ = n;
				}
		;

set_rest_more:	/* Generic SET syntaxes: */
			generic_set							{$$ = $1;}
			| var_name FROM CURRENT_P
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_CURRENT;
					n->name = $1;
					n->location = -1;
					$$ = n;
				}
			/* Special syntaxes mandated by SQL standard: */
			| TIME ZONE zone_value
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_VALUE;
					n->name = "timezone";
					n->location = -1;
					n->jumble_args = true;
					if ($3 != NULL)
						n->args = list_make1($3);
					else
						n->kind = SQ_VAR_SET_DEFAULT;
					$$ = n;
				}
			| CATALOG_P Sconst
				{
					raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@2)}, "current database cannot be changed");
					$$ = NULL; /*not reached*/
				}
			| SCHEMA Sconst
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_VALUE;
					n->name = "search_path";
					n->args = list_make1(make_string_const($2, @2));
					n->location = @2;
					$$ = n;
				}
			| NAMES opt_encoding
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_VALUE;
					n->name = "client_encoding";
					n->location = @2;
					if ($2 != NULL)
						n->args = list_make1(make_string_const($2, @2));
					else
						n->kind = SQ_VAR_SET_DEFAULT;
					$$ = n;
				}
			| ROLE NonReservedWord_or_Sconst
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_VALUE;
					n->name = "role";
					n->args = list_make1(make_string_const($2, @2));
					n->location = @2;
					$$ = n;
				}
			| SESSION AUTHORIZATION NonReservedWord_or_Sconst
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_VALUE;
					n->name = "session_authorization";
					n->args = list_make1(make_string_const($3, @3));
					n->location = @3;
					$$ = n;
				}
			| SESSION AUTHORIZATION DEFAULT
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_DEFAULT;
					n->name = "session_authorization";
					n->location = -1;
					$$ = n;
				}
			| XML_P OPTION document_or_content
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_VALUE;
					n->name = "xmloption";
					n->args = list_make1(make_string_const((char *) ($3 == SQ_XMLOPTION_DOCUMENT ? "DOCUMENT" : "CONTENT"), @3));
					n->jumble_args = true;
					n->location = -1;
					$$ = n;
				}
			/* Special syntaxes invented by PostgreSQL: */
			| TRANSACTION SNAPSHOT Sconst
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_SET_MULTI;
					n->name = "TRANSACTION SNAPSHOT";
					n->args = list_make1(make_string_const($3, @3));
					n->location = @3;
					$$ = n;
				}
		;

var_name:	ColId								{ $$ = $1; }
			| var_name '.' ColId
				{ $$ = tree_format("{}.{}", $1, $3); }
		;

var_list:	var_value								{ $$ = list_make1($1); }
			| var_list ',' var_value				{ $$ = lappend($1, $3); }
		;

var_value:	opt_boolean_or_string
				{ $$ = make_string_const($1, @1); }
			| NumericOnly
				{ $$ = make_a_const($1, @1); }
		;

iso_level:	READ UNCOMMITTED						{ $$ = "read uncommitted"; }
			| READ COMMITTED						{ $$ = "read committed"; }
			| REPEATABLE READ						{ $$ = "repeatable read"; }
			| SERIALIZABLE							{ $$ = "serializable"; }
		;

opt_boolean_or_string:
			TRUE_P									{ $$ = "true"; }
			| FALSE_P								{ $$ = "false"; }
			| ON									{ $$ = "on"; }
			/*
			 * OFF is also accepted as a boolean value, but is handled by
			 * the NonReservedWord rule.  The action for booleans and strings
			 * is the same, so we don't need to distinguish them here.
			 */
			| NonReservedWord_or_Sconst				{ $$ = $1; }
		;

/* Timezone values can be:
 * - a string such as 'pst8pdt'
 * - an identifier such as "pst8pdt"
 * - an integer or floating point number
 * - a time interval per SQL99
 * ColId gives reduce/reduce errors against ConstInterval and LOCAL,
 * so use IDENT (meaning we reject anything that is a key word).
 */
zone_value:
			Sconst
				{
					$$ = make_string_const($1, @1);
				}
			| IDENT
				{
					$$ = make_string_const($1, @1);
				}
			| ConstInterval Sconst opt_interval
				{
					sq_type_name_t   *t = $1;

					if ($3 != NIL)
					{
						sq_a_const_t	   *n = (sq_a_const_t *) linitial($3);

						if ((int_val(&n->val.node) & ~(INTERVAL_MASK(HOUR) | INTERVAL_MASK(MINUTE))) != 0)
							raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@3)}, "time zone interval must be HOUR or HOUR TO MINUTE");
					}
					t->typmods = $3;
					$$ = make_string_const_cast($2, @2, t);
				}
			| ConstInterval '(' Iconst ')' Sconst
				{
					sq_type_name_t   *t = $1;

					t->typmods = list_make2(make_int_const(INTERVAL_FULL_RANGE, -1),
											make_int_const($3, @3));
					$$ = make_string_const_cast($5, @5, t);
				}
			| NumericOnly							{ $$ = make_a_const($1, @1); }
			| DEFAULT								{ $$ = NULL; }
			| LOCAL									{ $$ = NULL; }
		;

opt_encoding:
			Sconst									{ $$ = $1; }
			| DEFAULT								{ $$ = NULL; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

NonReservedWord_or_Sconst:
			NonReservedWord							{ $$ = $1; }
			| Sconst								{ $$ = $1; }
		;

VariableResetStmt:
			RESET reset_rest						{ $$ = (sq_node_t *) $2; }
		;

reset_rest:
			generic_reset							{ $$ = $1; }
			| TIME ZONE
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_RESET;
					n->name = "timezone";
					n->location = -1;
					$$ = n;
				}
			| TRANSACTION ISOLATION LEVEL
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_RESET;
					n->name = "transaction_isolation";
					n->location = -1;
					$$ = n;
				}
			| SESSION AUTHORIZATION
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_RESET;
					n->name = "session_authorization";
					n->location = -1;
					$$ = n;
				}
		;

generic_reset:
			var_name
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_RESET;
					n->name = $1;
					n->location = -1;
					$$ = n;
				}
			| ALL
				{
					sq_variable_set_stmt_t *n = make_node<sq_variable_set_stmt_t>();

					n->kind = SQ_VAR_RESET_ALL;
					n->location = -1;
					$$ = n;
				}
		;

/* SetResetClause allows SET or RESET without LOCAL */
SetResetClause:
			SET set_rest					{ $$ = $2; }
			| VariableResetStmt				{ $$ = (sq_variable_set_stmt_t *) $1; }
		;

/* SetResetClause allows SET or RESET without LOCAL */
FunctionSetResetClause:
			SET set_rest_more				{ $$ = $2; }
			| VariableResetStmt				{ $$ = (sq_variable_set_stmt_t *) $1; }
		;


VariableShowStmt:
			SHOW var_name
				{
					sq_variable_show_stmt_t *n = make_node<sq_variable_show_stmt_t>();

					n->name = $2;
					$$ = (sq_node_t *) n;
				}
			| SHOW TIME ZONE
				{
					sq_variable_show_stmt_t *n = make_node<sq_variable_show_stmt_t>();

					n->name = "timezone";
					$$ = (sq_node_t *) n;
				}
			| SHOW TRANSACTION ISOLATION LEVEL
				{
					sq_variable_show_stmt_t *n = make_node<sq_variable_show_stmt_t>();

					n->name = "transaction_isolation";
					$$ = (sq_node_t *) n;
				}
			| SHOW SESSION AUTHORIZATION
				{
					sq_variable_show_stmt_t *n = make_node<sq_variable_show_stmt_t>();

					n->name = "session_authorization";
					$$ = (sq_node_t *) n;
				}
			| SHOW ALL
				{
					sq_variable_show_stmt_t *n = make_node<sq_variable_show_stmt_t>();

					n->name = "all";
					$$ = (sq_node_t *) n;
				}
		;


ConstraintsSetStmt:
			SET CONSTRAINTS constraints_set_list constraints_set_mode
				{
					sq_constraints_set_stmt_t *n = make_node<sq_constraints_set_stmt_t>();

					n->constraints = $3;
					n->deferred = $4;
					$$ = (sq_node_t *) n;
				}
		;

constraints_set_list:
			ALL										{ $$ = NIL; }
			| qualified_name_list					{ $$ = $1; }
		;

constraints_set_mode:
			DEFERRED								{ $$ = true; }
			| IMMEDIATE								{ $$ = false; }
		;


/*
 * Checkpoint statement
 */
CheckPointStmt:
			CHECKPOINT
				{
					sq_check_point_stmt_t *n = make_node<sq_check_point_stmt_t>();

					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 * DISCARD { ALL | TEMP | PLANS | SEQUENCES }
 *
 *****************************************************************************/

DiscardStmt:
			DISCARD ALL
				{
					sq_discard_stmt_t *n = make_node<sq_discard_stmt_t>();

					n->target = SQ_DISCARD_ALL;
					$$ = (sq_node_t *) n;
				}
			| DISCARD TEMP
				{
					sq_discard_stmt_t *n = make_node<sq_discard_stmt_t>();

					n->target = SQ_DISCARD_TEMP;
					$$ = (sq_node_t *) n;
				}
			| DISCARD TEMPORARY
				{
					sq_discard_stmt_t *n = make_node<sq_discard_stmt_t>();

					n->target = SQ_DISCARD_TEMP;
					$$ = (sq_node_t *) n;
				}
			| DISCARD PLANS
				{
					sq_discard_stmt_t *n = make_node<sq_discard_stmt_t>();

					n->target = SQ_DISCARD_PLANS;
					$$ = (sq_node_t *) n;
				}
			| DISCARD SEQUENCES
				{
					sq_discard_stmt_t *n = make_node<sq_discard_stmt_t>();

					n->target = SQ_DISCARD_SEQUENCES;
					$$ = (sq_node_t *) n;
				}

		;


/*****************************************************************************
 *
 *	ALTER [ TABLE | INDEX | SEQUENCE | VIEW | MATERIALIZED VIEW | FOREIGN TABLE ] variations
 *
 * Note: we accept all subcommands for each of the variants, and sort
 * out what's really legal at execution time.
 *****************************************************************************/

AlterTableStmt:
			ALTER TABLE relation_expr alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $3;
					n->cmds = $4;
					n->objtype = SQ_OBJECT_TABLE;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
		|	ALTER TABLE IF_P EXISTS relation_expr alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $5;
					n->cmds = $6;
					n->objtype = SQ_OBJECT_TABLE;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
		|	ALTER TABLE relation_expr partition_cmd
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $3;
					n->cmds = list_make1($4);
					n->objtype = SQ_OBJECT_TABLE;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
		|	ALTER TABLE IF_P EXISTS relation_expr partition_cmd
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $5;
					n->cmds = list_make1($6);
					n->objtype = SQ_OBJECT_TABLE;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
		|	ALTER TABLE ALL IN_P TABLESPACE name SET TABLESPACE name opt_nowait
				{
					sq_alter_table_move_all_stmt_t *n =
						make_node<sq_alter_table_move_all_stmt_t>();

					n->orig_tablespacename = $6;
					n->objtype = SQ_OBJECT_TABLE;
					n->roles = NIL;
					n->new_tablespacename = $9;
					n->nowait = $10;
					$$ = (sq_node_t *) n;
				}
		|	ALTER TABLE ALL IN_P TABLESPACE name OWNED BY role_list SET TABLESPACE name opt_nowait
				{
					sq_alter_table_move_all_stmt_t *n =
						make_node<sq_alter_table_move_all_stmt_t>();

					n->orig_tablespacename = $6;
					n->objtype = SQ_OBJECT_TABLE;
					n->roles = $9;
					n->new_tablespacename = $12;
					n->nowait = $13;
					$$ = (sq_node_t *) n;
				}
		|	ALTER INDEX qualified_name alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $3;
					n->cmds = $4;
					n->objtype = SQ_OBJECT_INDEX;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
		|	ALTER INDEX IF_P EXISTS qualified_name alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $5;
					n->cmds = $6;
					n->objtype = SQ_OBJECT_INDEX;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
		|	ALTER INDEX qualified_name index_partition_cmd
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $3;
					n->cmds = list_make1($4);
					n->objtype = SQ_OBJECT_INDEX;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
		|	ALTER INDEX ALL IN_P TABLESPACE name SET TABLESPACE name opt_nowait
				{
					sq_alter_table_move_all_stmt_t *n =
						make_node<sq_alter_table_move_all_stmt_t>();

					n->orig_tablespacename = $6;
					n->objtype = SQ_OBJECT_INDEX;
					n->roles = NIL;
					n->new_tablespacename = $9;
					n->nowait = $10;
					$$ = (sq_node_t *) n;
				}
		|	ALTER INDEX ALL IN_P TABLESPACE name OWNED BY role_list SET TABLESPACE name opt_nowait
				{
					sq_alter_table_move_all_stmt_t *n =
						make_node<sq_alter_table_move_all_stmt_t>();

					n->orig_tablespacename = $6;
					n->objtype = SQ_OBJECT_INDEX;
					n->roles = $9;
					n->new_tablespacename = $12;
					n->nowait = $13;
					$$ = (sq_node_t *) n;
				}
		|	ALTER SEQUENCE qualified_name alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $3;
					n->cmds = $4;
					n->objtype = SQ_OBJECT_SEQUENCE;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
		|	ALTER SEQUENCE IF_P EXISTS qualified_name alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $5;
					n->cmds = $6;
					n->objtype = SQ_OBJECT_SEQUENCE;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
		|	ALTER VIEW qualified_name alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $3;
					n->cmds = $4;
					n->objtype = SQ_OBJECT_VIEW;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
		|	ALTER VIEW IF_P EXISTS qualified_name alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $5;
					n->cmds = $6;
					n->objtype = SQ_OBJECT_VIEW;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
		|	ALTER MATERIALIZED VIEW qualified_name alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $4;
					n->cmds = $5;
					n->objtype = SQ_OBJECT_MATVIEW;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
		|	ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $6;
					n->cmds = $7;
					n->objtype = SQ_OBJECT_MATVIEW;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
		|	ALTER MATERIALIZED VIEW ALL IN_P TABLESPACE name SET TABLESPACE name opt_nowait
				{
					sq_alter_table_move_all_stmt_t *n =
						make_node<sq_alter_table_move_all_stmt_t>();

					n->orig_tablespacename = $7;
					n->objtype = SQ_OBJECT_MATVIEW;
					n->roles = NIL;
					n->new_tablespacename = $10;
					n->nowait = $11;
					$$ = (sq_node_t *) n;
				}
		|	ALTER MATERIALIZED VIEW ALL IN_P TABLESPACE name OWNED BY role_list SET TABLESPACE name opt_nowait
				{
					sq_alter_table_move_all_stmt_t *n =
						make_node<sq_alter_table_move_all_stmt_t>();

					n->orig_tablespacename = $7;
					n->objtype = SQ_OBJECT_MATVIEW;
					n->roles = $10;
					n->new_tablespacename = $13;
					n->nowait = $14;
					$$ = (sq_node_t *) n;
				}
		|	ALTER FOREIGN TABLE relation_expr alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $4;
					n->cmds = $5;
					n->objtype = SQ_OBJECT_FOREIGN_TABLE;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
		|	ALTER FOREIGN TABLE IF_P EXISTS relation_expr alter_table_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					n->relation = $6;
					n->cmds = $7;
					n->objtype = SQ_OBJECT_FOREIGN_TABLE;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
		;

alter_table_cmds:
			alter_table_cmd							{ $$ = list_make1($1); }
			| alter_table_cmds ',' alter_table_cmd	{ $$ = lappend($1, $3); }
		;

partition_cmd:
			/* ALTER TABLE <name> ATTACH PARTITION <table_name> FOR VALUES */
			ATTACH PARTITION qualified_name PartitionBoundSpec
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();
					sq_partition_cmd_t *cmd = make_node<sq_partition_cmd_t>();

					n->subtype = SQ_AT_AttachPartition;
					cmd->name = $3;
					cmd->bound = $4;
					cmd->concurrent = false;
					n->def = (sq_node_t *) cmd;

					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> DETACH PARTITION <partition_name> [CONCURRENTLY] */
			| DETACH PARTITION qualified_name opt_concurrently
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();
					sq_partition_cmd_t *cmd = make_node<sq_partition_cmd_t>();

					n->subtype = SQ_AT_DetachPartition;
					cmd->name = $3;
					cmd->bound = NULL;
					cmd->concurrent = $4;
					n->def = (sq_node_t *) cmd;

					$$ = (sq_node_t *) n;
				}
			| DETACH PARTITION qualified_name FINALIZE
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();
					sq_partition_cmd_t *cmd = make_node<sq_partition_cmd_t>();

					n->subtype = SQ_AT_DetachPartitionFinalize;
					cmd->name = $3;
					cmd->bound = NULL;
					cmd->concurrent = false;
					n->def = (sq_node_t *) cmd;
					$$ = (sq_node_t *) n;
				}
		;

index_partition_cmd:
			/* ALTER INDEX <name> ATTACH PARTITION <index_name> */
			ATTACH PARTITION qualified_name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();
					sq_partition_cmd_t *cmd = make_node<sq_partition_cmd_t>();

					n->subtype = SQ_AT_AttachPartition;
					cmd->name = $3;
					cmd->bound = NULL;
					cmd->concurrent = false;
					n->def = (sq_node_t *) cmd;

					$$ = (sq_node_t *) n;
				}
		;

alter_table_cmd:
			/* ALTER TABLE <name> ADD <coldef> */
			ADD_P columnDef
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_AddColumn;
					n->def = $2;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ADD IF NOT EXISTS <coldef> */
			| ADD_P IF_P NOT EXISTS columnDef
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_AddColumn;
					n->def = $5;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ADD COLUMN <coldef> */
			| ADD_P COLUMN columnDef
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_AddColumn;
					n->def = $3;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ADD COLUMN IF NOT EXISTS <coldef> */
			| ADD_P COLUMN IF_P NOT EXISTS columnDef
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_AddColumn;
					n->def = $6;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> {SET DEFAULT <expr>|DROP DEFAULT} */
			| ALTER opt_column ColId alter_column_default
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_ColumnDefault;
					n->name = $3;
					n->def = $4;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> DROP NOT NULL */
			| ALTER opt_column ColId DROP NOT NULL_P
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropNotNull;
					n->name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET NOT NULL */
			| ALTER opt_column ColId SET NOT NULL_P
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetNotNull;
					n->name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET EXPRESSION AS <expr> */
			| ALTER opt_column ColId SET EXPRESSION AS '(' a_expr ')'
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetExpression;
					n->name = $3;
					n->def = $8;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> DROP EXPRESSION */
			| ALTER opt_column ColId DROP EXPRESSION
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropExpression;
					n->name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> DROP EXPRESSION IF EXISTS */
			| ALTER opt_column ColId DROP EXPRESSION IF_P EXISTS
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropExpression;
					n->name = $3;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET STATISTICS */
			| ALTER opt_column ColId SET STATISTICS set_statistics_value
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetStatistics;
					n->name = $3;
					n->def = $6;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colnum> SET STATISTICS */
			| ALTER opt_column Iconst SET STATISTICS set_statistics_value
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					if ($3 <= 0 || $3 > PG_INT16_MAX)
						raise({.sqlstate = sqlstate_invalid_parameter_value, .pos = parser_errposition(@3)}, "column number must be in range from 1 to {}", PG_INT16_MAX);

					n->subtype = SQ_AT_SetStatistics;
					n->num = (int16_t) $3;
					n->def = $6;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET ( column_parameter = value [, ... ] ) */
			| ALTER opt_column ColId SET reloptions
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetOptions;
					n->name = $3;
					n->def = (sq_node_t *) $5;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> RESET ( column_parameter [, ... ] ) */
			| ALTER opt_column ColId RESET reloptions
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_ResetOptions;
					n->name = $3;
					n->def = (sq_node_t *) $5;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET STORAGE <storagemode> */
			| ALTER opt_column ColId SET column_storage
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetStorage;
					n->name = $3;
					n->def = (sq_node_t *) make_string($5);
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET COMPRESSION <cm> */
			| ALTER opt_column ColId SET column_compression
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetCompression;
					n->name = $3;
					n->def = (sq_node_t *) make_string($5);
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> ADD GENERATED ... AS IDENTITY ... */
			| ALTER opt_column ColId ADD_P GENERATED generated_when AS IDENTITY_P OptParenthesizedSeqOptList
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();
					sq_constraint_t *c = make_node<sq_constraint_t>();

					c->contype = SQ_CONSTR_IDENTITY;
					c->generated_when = $6;
					c->options = $9;
					c->location = @5;

					n->subtype = SQ_AT_AddIdentity;
					n->name = $3;
					n->def = (sq_node_t *) c;

					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> SET <sequence options>/RESET */
			| ALTER opt_column ColId alter_identity_column_option_list
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetIdentity;
					n->name = $3;
					n->def = (sq_node_t *) $4;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> DROP IDENTITY */
			| ALTER opt_column ColId DROP IDENTITY_P
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropIdentity;
					n->name = $3;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER [COLUMN] <colname> DROP IDENTITY IF EXISTS */
			| ALTER opt_column ColId DROP IDENTITY_P IF_P EXISTS
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropIdentity;
					n->name = $3;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> DROP [COLUMN] IF EXISTS <colname> [RESTRICT|CASCADE] */
			| DROP opt_column IF_P EXISTS ColId opt_drop_behavior
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropColumn;
					n->name = $5;
					n->behavior = $6;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> DROP [COLUMN] <colname> [RESTRICT|CASCADE] */
			| DROP opt_column ColId opt_drop_behavior
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropColumn;
					n->name = $3;
					n->behavior = $4;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			/*
			 * ALTER TABLE <name> ALTER [COLUMN] <colname> [SET DATA] TYPE <typename>
			 *		[ USING <expression> ]
			 */
			| ALTER opt_column ColId opt_set_data TYPE_P Typename opt_collate_clause alter_using
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();
					sq_column_def_t *def = make_node<sq_column_def_t>();

					n->subtype = SQ_AT_AlterColumnType;
					n->name = $3;
					n->def = (sq_node_t *) def;
					/* We only use these fields of the ColumnDef node */
					def->type_name = $6;
					def->coll_clause = (sq_collate_clause_t *) $7;
					def->raw_default = $8;
					def->location = @3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER FOREIGN TABLE <name> ALTER [COLUMN] <colname> OPTIONS */
			| ALTER opt_column ColId alter_generic_options
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_AlterColumnGenericOptions;
					n->name = $3;
					n->def = (sq_node_t *) $4;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ADD CONSTRAINT ... */
			| ADD_P TableConstraint
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_AddConstraint;
					n->def = $2;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER CONSTRAINT ... */
			| ALTER CONSTRAINT name ConstraintAttributeSpec
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();
					sq_at_alter_constraint_t *c = make_node<sq_at_alter_constraint_t>();

					n->subtype = SQ_AT_AlterConstraint;
					n->def = (sq_node_t *) c;
					c->conname = $3;
					if ($4 & (CAS_NOT_ENFORCED | CAS_ENFORCED))
						c->alter_enforceability = true;
					if ($4 & (CAS_DEFERRABLE | CAS_NOT_DEFERRABLE |
							  CAS_INITIALLY_DEFERRED | CAS_INITIALLY_IMMEDIATE))
						c->alter_deferrability = true;
					if ($4 & CAS_NO_INHERIT)
						c->alter_inheritability = true;
					/* handle unsupported case with specific error message */
					if ($4 & CAS_NOT_VALID)
						raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@4)}, "constraints cannot be altered to be NOT VALID");
					process_ca_sbits($4, @4, "FOREIGN KEY",
									&c->deferrable,
									&c->initdeferred,
									&c->is_enforced,
									NULL,
									&c->noinherit,
									yyscanner);
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ALTER CONSTRAINT INHERIT */
			| ALTER CONSTRAINT name INHERIT
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();
					sq_at_alter_constraint_t *c = make_node<sq_at_alter_constraint_t>();

					n->subtype = SQ_AT_AlterConstraint;
					n->def = (sq_node_t *) c;
					c->conname = $3;
					c->alter_inheritability = true;
					c->noinherit = false;

					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> VALIDATE CONSTRAINT ... */
			| VALIDATE CONSTRAINT name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_ValidateConstraint;
					n->name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> DROP CONSTRAINT IF EXISTS <name> [RESTRICT|CASCADE] */
			| DROP CONSTRAINT IF_P EXISTS name opt_drop_behavior
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropConstraint;
					n->name = $5;
					n->behavior = $6;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> DROP CONSTRAINT <name> [RESTRICT|CASCADE] */
			| DROP CONSTRAINT name opt_drop_behavior
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropConstraint;
					n->name = $3;
					n->behavior = $4;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> SET WITHOUT OIDS, for backward compat */
			| SET WITHOUT OIDS
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropOids;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> CLUSTER ON <indexname> */
			| CLUSTER ON name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_ClusterOn;
					n->name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> SET WITHOUT CLUSTER */
			| SET WITHOUT CLUSTER
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropCluster;
					n->name = NULL;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> SET LOGGED */
			| SET LOGGED
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetLogged;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> SET UNLOGGED */
			| SET UNLOGGED
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetUnLogged;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ENABLE TRIGGER <trig> */
			| ENABLE_P TRIGGER name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_EnableTrig;
					n->name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ENABLE ALWAYS TRIGGER <trig> */
			| ENABLE_P ALWAYS TRIGGER name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_EnableAlwaysTrig;
					n->name = $4;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ENABLE REPLICA TRIGGER <trig> */
			| ENABLE_P REPLICA TRIGGER name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_EnableReplicaTrig;
					n->name = $4;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ENABLE TRIGGER ALL */
			| ENABLE_P TRIGGER ALL
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_EnableTrigAll;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ENABLE TRIGGER USER */
			| ENABLE_P TRIGGER USER
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_EnableTrigUser;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> DISABLE TRIGGER <trig> */
			| DISABLE_P TRIGGER name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DisableTrig;
					n->name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> DISABLE TRIGGER ALL */
			| DISABLE_P TRIGGER ALL
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DisableTrigAll;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> DISABLE TRIGGER USER */
			| DISABLE_P TRIGGER USER
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DisableTrigUser;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ENABLE RULE <rule> */
			| ENABLE_P RULE name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_EnableRule;
					n->name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ENABLE ALWAYS RULE <rule> */
			| ENABLE_P ALWAYS RULE name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_EnableAlwaysRule;
					n->name = $4;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ENABLE REPLICA RULE <rule> */
			| ENABLE_P REPLICA RULE name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_EnableReplicaRule;
					n->name = $4;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> DISABLE RULE <rule> */
			| DISABLE_P RULE name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DisableRule;
					n->name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> INHERIT <parent> */
			| INHERIT qualified_name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_AddInherit;
					n->def = (sq_node_t *) $2;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> NO INHERIT <parent> */
			| NO INHERIT qualified_name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropInherit;
					n->def = (sq_node_t *) $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> OF <type_name> */
			| OF any_name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();
					sq_type_name_t   *def = make_type_name_from_name_list($2);

					def->location = @2;
					n->subtype = SQ_AT_AddOf;
					n->def = (sq_node_t *) def;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> NOT OF */
			| NOT OF
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropOf;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> OWNER TO RoleSpec */
			| OWNER TO RoleSpec
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_ChangeOwner;
					n->newowner = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> SET ACCESS METHOD { <amname> | DEFAULT } */
			| SET ACCESS METHOD set_access_method_name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetAccessMethod;
					n->name = $4;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> SET TABLESPACE <tablespacename> */
			| SET TABLESPACE name
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetTableSpace;
					n->name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> SET (...) */
			| SET reloptions
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_SetRelOptions;
					n->def = (sq_node_t *) $2;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> RESET (...) */
			| RESET reloptions
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_ResetRelOptions;
					n->def = (sq_node_t *) $2;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> REPLICA IDENTITY */
			| REPLICA IDENTITY_P replica_identity
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_ReplicaIdentity;
					n->def = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> ENABLE ROW LEVEL SECURITY */
			| ENABLE_P ROW LEVEL SECURITY
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_EnableRowSecurity;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> DISABLE ROW LEVEL SECURITY */
			| DISABLE_P ROW LEVEL SECURITY
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DisableRowSecurity;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> FORCE ROW LEVEL SECURITY */
			| FORCE ROW LEVEL SECURITY
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_ForceRowSecurity;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TABLE <name> NO FORCE ROW LEVEL SECURITY */
			| NO FORCE ROW LEVEL SECURITY
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_NoForceRowSecurity;
					$$ = (sq_node_t *) n;
				}
			| alter_generic_options
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_GenericOptions;
					n->def = (sq_node_t *) $1;
					$$ = (sq_node_t *) n;
				}
		;

alter_column_default:
			SET DEFAULT a_expr			{ $$ = $3; }
			| DROP DEFAULT				{ $$ = NULL; }
		;

opt_collate_clause:
			COLLATE any_name
				{
					sq_collate_clause_t *n = make_node<sq_collate_clause_t>();

					n->arg = NULL;
					n->collname = $2;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| /* EMPTY */				{ $$ = NULL; }
		;

alter_using:
			USING a_expr				{ $$ = $2; }
			| /* EMPTY */				{ $$ = NULL; }
		;

replica_identity:
			NOTHING
				{
					sq_replica_identity_stmt_t *n = make_node<sq_replica_identity_stmt_t>();

					n->identity_type = REPLICA_IDENTITY_NOTHING;
					n->name = NULL;
					$$ = (sq_node_t *) n;
				}
			| FULL
				{
					sq_replica_identity_stmt_t *n = make_node<sq_replica_identity_stmt_t>();

					n->identity_type = REPLICA_IDENTITY_FULL;
					n->name = NULL;
					$$ = (sq_node_t *) n;
				}
			| DEFAULT
				{
					sq_replica_identity_stmt_t *n = make_node<sq_replica_identity_stmt_t>();

					n->identity_type = REPLICA_IDENTITY_DEFAULT;
					n->name = NULL;
					$$ = (sq_node_t *) n;
				}
			| USING INDEX name
				{
					sq_replica_identity_stmt_t *n = make_node<sq_replica_identity_stmt_t>();

					n->identity_type = REPLICA_IDENTITY_INDEX;
					n->name = $3;
					$$ = (sq_node_t *) n;
				}
;

reloptions:
			'(' reloption_list ')'					{ $$ = $2; }
		;

opt_reloptions:		WITH reloptions					{ $$ = $2; }
			 |		/* EMPTY */						{ $$ = NIL; }
		;

reloption_list:
			reloption_elem							{ $$ = list_make1($1); }
			| reloption_list ',' reloption_elem		{ $$ = lappend($1, $3); }
		;

/* This should match def_elem and also allow qualified names */
reloption_elem:
			ColLabel '=' def_arg
				{
					$$ = make_def_elem($1, (sq_node_t *) $3, @1);
				}
			| ColLabel
				{
					$$ = make_def_elem($1, NULL, @1);
				}
			| ColLabel '.' ColLabel '=' def_arg
				{
					$$ = make_def_elem_extended($1, $3, (sq_node_t *) $5,
											 SQ_DEFELEM_UNSPEC, @1);
				}
			| ColLabel '.' ColLabel
				{
					$$ = make_def_elem_extended($1, $3, NULL, SQ_DEFELEM_UNSPEC, @1);
				}
		;

alter_identity_column_option_list:
			alter_identity_column_option
				{ $$ = list_make1($1); }
			| alter_identity_column_option_list alter_identity_column_option
				{ $$ = lappend($1, $2); }
		;

alter_identity_column_option:
			RESTART
				{
					$$ = make_def_elem("restart", NULL, @1);
				}
			| RESTART opt_with NumericOnly
				{
					$$ = make_def_elem("restart", (sq_node_t *) $3, @1);
				}
			| SET SeqOptElem
				{
					if (strcmp($2->defname, "as") == 0 ||
						strcmp($2->defname, "restart") == 0 ||
						strcmp($2->defname, "owned_by") == 0)
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@2)}, "sequence option \"{}\" not supported here", $2->defname);
					$$ = $2;
				}
			| SET GENERATED generated_when
				{
					$$ = make_def_elem("generated", (sq_node_t *) make_integer($3), @1);
				}
		;

set_statistics_value:
			SignedIconst					{ $$ = (sq_node_t *) make_integer($1); }
			| DEFAULT						{ $$ = NULL; }
		;

set_access_method_name:
			ColId							{ $$ = $1; }
			| DEFAULT						{ $$ = NULL; }
		;

PartitionBoundSpec:
			/* a HASH partition */
			FOR VALUES WITH '(' hash_partbound ')'
				{
					list_cell_t   lc;
					sq_partition_bound_spec_t *n = make_node<sq_partition_bound_spec_t>();

					n->strategy = SQ_PARTITION_STRATEGY_HASH;
					n->modulus = n->remainder = -1;

					foreach (lc, $5)
					{
						sq_def_elem_t    *opt = lfirst_node(sq_def_elem_t, lc);

						if (strcmp(opt->defname, "modulus") == 0)
						{
							if (n->modulus != -1)
								raise({.sqlstate = sqlstate_duplicate_object, .pos = parser_errposition(opt->location)}, "modulus for hash partition provided more than once");
							n->modulus = def_get_int32(opt);
						}
						else if (strcmp(opt->defname, "remainder") == 0)
						{
							if (n->remainder != -1)
								raise({.sqlstate = sqlstate_duplicate_object, .pos = parser_errposition(opt->location)}, "remainder for hash partition provided more than once");
							n->remainder = def_get_int32(opt);
						}
						else
							raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(opt->location)}, "unrecognized hash partition bound specification \"{}\"",
											opt->defname);
					}

					if (n->modulus == -1)
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@3)}, "modulus for hash partition must be specified");
					if (n->remainder == -1)
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@3)}, "remainder for hash partition must be specified");

					n->location = @3;

					$$ = n;
				}

			/* a LIST partition */
			| FOR VALUES IN_P '(' expr_list ')'
				{
					sq_partition_bound_spec_t *n = make_node<sq_partition_bound_spec_t>();

					n->strategy = SQ_PARTITION_STRATEGY_LIST;
					n->is_default = false;
					n->listdatums = $5;
					n->location = @3;

					$$ = n;
				}

			/* a RANGE partition */
			| FOR VALUES FROM '(' expr_list ')' TO '(' expr_list ')'
				{
					sq_partition_bound_spec_t *n = make_node<sq_partition_bound_spec_t>();

					n->strategy = SQ_PARTITION_STRATEGY_RANGE;
					n->is_default = false;
					n->lowerdatums = $5;
					n->upperdatums = $9;
					n->location = @3;

					$$ = n;
				}

			/* a DEFAULT partition */
			| DEFAULT
				{
					sq_partition_bound_spec_t *n = make_node<sq_partition_bound_spec_t>();

					n->is_default = true;
					n->location = @1;

					$$ = n;
				}
		;

hash_partbound_elem:
		NonReservedWord Iconst
			{
				$$ = make_def_elem($1, (sq_node_t *) make_integer($2), @1);
			}
		;

hash_partbound:
		hash_partbound_elem
			{
				$$ = list_make1($1);
			}
		| hash_partbound ',' hash_partbound_elem
			{
				$$ = lappend($1, $3);
			}
		;

/*****************************************************************************
 *
 *	ALTER TYPE
 *
 * really variants of the ALTER TABLE subcommands with different spellings
 *****************************************************************************/

AlterCompositeTypeStmt:
			ALTER TYPE_P any_name alter_type_cmds
				{
					sq_alter_table_stmt_t *n = make_node<sq_alter_table_stmt_t>();

					/* can't use qualified_name, sigh */
					n->relation = make_range_var_from_any_name($3, @3, yyscanner);
					n->cmds = $4;
					n->objtype = SQ_OBJECT_TYPE;
					$$ = (sq_node_t *) n;
				}
			;

alter_type_cmds:
			alter_type_cmd							{ $$ = list_make1($1); }
			| alter_type_cmds ',' alter_type_cmd	{ $$ = lappend($1, $3); }
		;

alter_type_cmd:
			/* ALTER TYPE <name> ADD ATTRIBUTE <coldef> [RESTRICT|CASCADE] */
			ADD_P ATTRIBUTE TableFuncElement opt_drop_behavior
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_AddColumn;
					n->def = $3;
					n->behavior = $4;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TYPE <name> DROP ATTRIBUTE IF EXISTS <attname> [RESTRICT|CASCADE] */
			| DROP ATTRIBUTE IF_P EXISTS ColId opt_drop_behavior
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropColumn;
					n->name = $5;
					n->behavior = $6;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TYPE <name> DROP ATTRIBUTE <attname> [RESTRICT|CASCADE] */
			| DROP ATTRIBUTE ColId opt_drop_behavior
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();

					n->subtype = SQ_AT_DropColumn;
					n->name = $3;
					n->behavior = $4;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			/* ALTER TYPE <name> ALTER ATTRIBUTE <attname> [SET DATA] TYPE <typename> [RESTRICT|CASCADE] */
			| ALTER ATTRIBUTE ColId opt_set_data TYPE_P Typename opt_collate_clause opt_drop_behavior
				{
					sq_alter_table_cmd_t *n = make_node<sq_alter_table_cmd_t>();
					sq_column_def_t *def = make_node<sq_column_def_t>();

					n->subtype = SQ_AT_AlterColumnType;
					n->name = $3;
					n->def = (sq_node_t *) def;
					n->behavior = $8;
					/* We only use these fields of the ColumnDef node */
					def->type_name = $6;
					def->coll_clause = (sq_collate_clause_t *) $7;
					def->raw_default = NULL;
					def->location = @3;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				close <portalname>
 *
 *****************************************************************************/

ClosePortalStmt:
			CLOSE cursor_name
				{
					sq_close_portal_stmt_t *n = make_node<sq_close_portal_stmt_t>();

					n->portalname = $2;
					$$ = (sq_node_t *) n;
				}
			| CLOSE ALL
				{
					sq_close_portal_stmt_t *n = make_node<sq_close_portal_stmt_t>();

					n->portalname = NULL;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				COPY relname [(columnList)] FROM/TO file [WITH] [(options)]
 *				COPY ( query ) TO file	[WITH] [(options)]
 *
 *				where 'query' can be one of:
 *				{ SELECT | UPDATE | INSERT | DELETE }
 *
 *				and 'file' can be one of:
 *				{ PROGRAM 'command' | STDIN | STDOUT | 'filename' }
 *
 *				In the preferred syntax the options are comma-separated
 *				and use generic identifiers instead of keywords.  The pre-9.0
 *				syntax had a hard-wired, space-separated set of options.
 *
 *				Really old syntax, from versions 7.2 and prior:
 *				COPY [ BINARY ] table FROM/TO file
 *					[ [ USING ] DELIMITERS 'delimiter' ] ]
 *					[ WITH NULL AS 'null string' ]
 *				This option placement is not supported with COPY (query...).
 *
 *****************************************************************************/

CopyStmt:	COPY opt_binary qualified_name opt_column_list
			copy_from opt_program copy_file_name copy_delimiter opt_with
			copy_options where_clause
				{
					sq_copy_stmt_t *n = make_node<sq_copy_stmt_t>();

					n->relation = $3;
					n->query = NULL;
					n->attlist = $4;
					n->is_from = $5;
					n->is_program = $6;
					n->filename = $7;
					n->where_clause = $11;

					if (n->is_program && n->filename == NULL)
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@8)}, "STDIN/STDOUT not allowed with PROGRAM");

					if (!n->is_from && n->where_clause != NULL)
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@11)}, "WHERE clause not allowed with COPY TO");

					n->options = NIL;
					/* Concatenate user-supplied flags */
					if ($2)
						n->options = lappend(n->options, $2);
					if ($8)
						n->options = lappend(n->options, $8);
					if ($10)
						n->options = list_concat(n->options, $10);
					$$ = (sq_node_t *) n;
				}
			| COPY '(' PreparableStmt ')' TO opt_program copy_file_name opt_with copy_options
				{
					sq_copy_stmt_t *n = make_node<sq_copy_stmt_t>();

					n->relation = NULL;
					n->query = $3;
					n->attlist = NIL;
					n->is_from = false;
					n->is_program = $6;
					n->filename = $7;
					n->options = $9;

					if (n->is_program && n->filename == NULL)
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@5)}, "STDIN/STDOUT not allowed with PROGRAM");

					$$ = (sq_node_t *) n;
				}
		;

copy_from:
			FROM									{ $$ = true; }
			| TO									{ $$ = false; }
		;

opt_program:
			PROGRAM									{ $$ = true; }
			| /* EMPTY */							{ $$ = false; }
		;

/*
 * copy_file_name NULL indicates stdio is used. Whether stdin or stdout is
 * used depends on the direction. (It really doesn't make sense to copy from
 * stdout. We silently correct the "typo".)		 - AY 9/94
 */
copy_file_name:
			Sconst									{ $$ = $1; }
			| STDIN									{ $$ = NULL; }
			| STDOUT								{ $$ = NULL; }
		;

copy_options: copy_opt_list							{ $$ = $1; }
			| '(' copy_generic_opt_list ')'			{ $$ = $2; }
		;

/* old COPY option syntax */
copy_opt_list:
			copy_opt_list copy_opt_item				{ $$ = lappend($1, $2); }
			| /* EMPTY */							{ $$ = NIL; }
		;

copy_opt_item:
			BINARY
				{
					$$ = make_def_elem("format", (sq_node_t *) make_string("binary"), @1);
				}
			| FREEZE
				{
					$$ = make_def_elem("freeze", (sq_node_t *) make_boolean(true), @1);
				}
			| DELIMITER opt_as Sconst
				{
					$$ = make_def_elem("delimiter", (sq_node_t *) make_string($3), @1);
				}
			| NULL_P opt_as Sconst
				{
					$$ = make_def_elem("null", (sq_node_t *) make_string($3), @1);
				}
			| CSV
				{
					$$ = make_def_elem("format", (sq_node_t *) make_string("csv"), @1);
				}
			| HEADER_P
				{
					$$ = make_def_elem("header", (sq_node_t *) make_boolean(true), @1);
				}
			| QUOTE opt_as Sconst
				{
					$$ = make_def_elem("quote", (sq_node_t *) make_string($3), @1);
				}
			| ESCAPE opt_as Sconst
				{
					$$ = make_def_elem("escape", (sq_node_t *) make_string($3), @1);
				}
			| FORCE QUOTE columnList
				{
					$$ = make_def_elem("force_quote", (sq_node_t *) $3, @1);
				}
			| FORCE QUOTE '*'
				{
					$$ = make_def_elem("force_quote", (sq_node_t *) make_node<sq_a_star_t>(), @1);
				}
			| FORCE NOT NULL_P columnList
				{
					$$ = make_def_elem("force_not_null", (sq_node_t *) $4, @1);
				}
			| FORCE NOT NULL_P '*'
				{
					$$ = make_def_elem("force_not_null", (sq_node_t *) make_node<sq_a_star_t>(), @1);
				}
			| FORCE NULL_P columnList
				{
					$$ = make_def_elem("force_null", (sq_node_t *) $3, @1);
				}
			| FORCE NULL_P '*'
				{
					$$ = make_def_elem("force_null", (sq_node_t *) make_node<sq_a_star_t>(), @1);
				}
			| ENCODING Sconst
				{
					$$ = make_def_elem("encoding", (sq_node_t *) make_string($2), @1);
				}
		;

/* The following exist for backward compatibility with very old versions */

opt_binary:
			BINARY
				{
					$$ = make_def_elem("format", (sq_node_t *) make_string("binary"), @1);
				}
			| /*EMPTY*/								{ $$ = NULL; }
		;

copy_delimiter:
			opt_using DELIMITERS Sconst
				{
					$$ = make_def_elem("delimiter", (sq_node_t *) make_string($3), @2);
				}
			| /*EMPTY*/								{ $$ = NULL; }
		;

opt_using:
			USING
			| /*EMPTY*/
		;

/* new COPY option syntax */
copy_generic_opt_list:
			copy_generic_opt_elem
				{
					$$ = list_make1($1);
				}
			| copy_generic_opt_list ',' copy_generic_opt_elem
				{
					$$ = lappend($1, $3);
				}
		;

copy_generic_opt_elem:
			ColLabel copy_generic_opt_arg
				{
					$$ = make_def_elem($1, $2, @1);
				}
		;

copy_generic_opt_arg:
			opt_boolean_or_string			{ $$ = (sq_node_t *) make_string($1); }
			| NumericOnly					{ $$ = (sq_node_t *) $1; }
			| '*'							{ $$ = (sq_node_t *) make_node<sq_a_star_t>(); }
			| DEFAULT                       { $$ = (sq_node_t *) make_string("default"); }
			| '(' copy_generic_opt_arg_list ')'		{ $$ = (sq_node_t *) $2; }
			| /* EMPTY */					{ $$ = NULL; }
		;

copy_generic_opt_arg_list:
			  copy_generic_opt_arg_list_item
				{
					$$ = list_make1($1);
				}
			| copy_generic_opt_arg_list ',' copy_generic_opt_arg_list_item
				{
					$$ = lappend($1, $3);
				}
		;

/* beware of emitting non-string list elements here; see commands/define.c */
copy_generic_opt_arg_list_item:
			opt_boolean_or_string	{ $$ = (sq_node_t *) make_string($1); }
		;


/*****************************************************************************
 *
 *		QUERY :
 *				CREATE TABLE relname
 *
 *****************************************************************************/

CreateStmt:	CREATE OptTemp TABLE qualified_name '(' OptTableElementList ')'
			OptInherit OptPartitionSpec table_access_method_clause OptWith
			OnCommitOption OptTableSpace
				{
					sq_create_stmt_t *n = make_node<sq_create_stmt_t>();

					$4->relpersistence = $2;
					n->relation = $4;
					n->table_elts = $6;
					n->inh_relations = $8;
					n->partspec = $9;
					n->of_typename = NULL;
					n->constraints = NIL;
					n->access_method = $10;
					n->options = $11;
					n->oncommit = $12;
					n->tablespacename = $13;
					n->if_not_exists = false;
					$$ = (sq_node_t *) n;
				}
		| CREATE OptTemp TABLE IF_P NOT EXISTS qualified_name '('
			OptTableElementList ')' OptInherit OptPartitionSpec table_access_method_clause
			OptWith OnCommitOption OptTableSpace
				{
					sq_create_stmt_t *n = make_node<sq_create_stmt_t>();

					$7->relpersistence = $2;
					n->relation = $7;
					n->table_elts = $9;
					n->inh_relations = $11;
					n->partspec = $12;
					n->of_typename = NULL;
					n->constraints = NIL;
					n->access_method = $13;
					n->options = $14;
					n->oncommit = $15;
					n->tablespacename = $16;
					n->if_not_exists = true;
					$$ = (sq_node_t *) n;
				}
		| CREATE OptTemp TABLE qualified_name OF any_name
			OptTypedTableElementList OptPartitionSpec table_access_method_clause
			OptWith OnCommitOption OptTableSpace
				{
					sq_create_stmt_t *n = make_node<sq_create_stmt_t>();

					$4->relpersistence = $2;
					n->relation = $4;
					n->table_elts = $7;
					n->inh_relations = NIL;
					n->partspec = $8;
					n->of_typename = make_type_name_from_name_list($6);
					n->of_typename->location = @6;
					n->constraints = NIL;
					n->access_method = $9;
					n->options = $10;
					n->oncommit = $11;
					n->tablespacename = $12;
					n->if_not_exists = false;
					$$ = (sq_node_t *) n;
				}
		| CREATE OptTemp TABLE IF_P NOT EXISTS qualified_name OF any_name
			OptTypedTableElementList OptPartitionSpec table_access_method_clause
			OptWith OnCommitOption OptTableSpace
				{
					sq_create_stmt_t *n = make_node<sq_create_stmt_t>();

					$7->relpersistence = $2;
					n->relation = $7;
					n->table_elts = $10;
					n->inh_relations = NIL;
					n->partspec = $11;
					n->of_typename = make_type_name_from_name_list($9);
					n->of_typename->location = @9;
					n->constraints = NIL;
					n->access_method = $12;
					n->options = $13;
					n->oncommit = $14;
					n->tablespacename = $15;
					n->if_not_exists = true;
					$$ = (sq_node_t *) n;
				}
		| CREATE OptTemp TABLE qualified_name PARTITION OF qualified_name
			OptTypedTableElementList PartitionBoundSpec OptPartitionSpec
			table_access_method_clause OptWith OnCommitOption OptTableSpace
				{
					sq_create_stmt_t *n = make_node<sq_create_stmt_t>();

					$4->relpersistence = $2;
					n->relation = $4;
					n->table_elts = $8;
					n->inh_relations = list_make1($7);
					n->partbound = $9;
					n->partspec = $10;
					n->of_typename = NULL;
					n->constraints = NIL;
					n->access_method = $11;
					n->options = $12;
					n->oncommit = $13;
					n->tablespacename = $14;
					n->if_not_exists = false;
					$$ = (sq_node_t *) n;
				}
		| CREATE OptTemp TABLE IF_P NOT EXISTS qualified_name PARTITION OF
			qualified_name OptTypedTableElementList PartitionBoundSpec OptPartitionSpec
			table_access_method_clause OptWith OnCommitOption OptTableSpace
				{
					sq_create_stmt_t *n = make_node<sq_create_stmt_t>();

					$7->relpersistence = $2;
					n->relation = $7;
					n->table_elts = $11;
					n->inh_relations = list_make1($10);
					n->partbound = $12;
					n->partspec = $13;
					n->of_typename = NULL;
					n->constraints = NIL;
					n->access_method = $14;
					n->options = $15;
					n->oncommit = $16;
					n->tablespacename = $17;
					n->if_not_exists = true;
					$$ = (sq_node_t *) n;
				}
		;

/*
 * Redundancy here is needed to avoid shift/reduce conflicts,
 * since TEMP is not a reserved word.  See also OptTempTableName.
 *
 * NOTE: we accept both GLOBAL and LOCAL options.  They currently do nothing,
 * but future versions might consider GLOBAL to request SQL-spec-compliant
 * temp table behavior, so warn about that.  Since we have no modules the
 * LOCAL keyword is really meaningless; furthermore, some other products
 * implement LOCAL as meaning the same as our default temp table behavior,
 * so we'll probably continue to treat LOCAL as a noise word.
 */
OptTemp:	TEMPORARY					{ $$ = RELPERSISTENCE_TEMP; }
			| TEMP						{ $$ = RELPERSISTENCE_TEMP; }
			| LOCAL TEMPORARY			{ $$ = RELPERSISTENCE_TEMP; }
			| LOCAL TEMP				{ $$ = RELPERSISTENCE_TEMP; }
			| GLOBAL TEMPORARY
				{
					notify(*scanner_extra(yyscanner), SQ_WARNING, sqlstate_warning,
						   parser_errposition(@1), "GLOBAL is deprecated in temporary table creation", nullptr);
					$$ = RELPERSISTENCE_TEMP;
				}
			| GLOBAL TEMP
				{
					notify(*scanner_extra(yyscanner), SQ_WARNING, sqlstate_warning,
						   parser_errposition(@1), "GLOBAL is deprecated in temporary table creation", nullptr);
					$$ = RELPERSISTENCE_TEMP;
				}
			| UNLOGGED					{ $$ = RELPERSISTENCE_UNLOGGED; }
			| /*EMPTY*/					{ $$ = RELPERSISTENCE_PERMANENT; }
		;

OptTableElementList:
			TableElementList					{ $$ = $1; }
			| /*EMPTY*/							{ $$ = NIL; }
		;

OptTypedTableElementList:
			'(' TypedTableElementList ')'		{ $$ = $2; }
			| /*EMPTY*/							{ $$ = NIL; }
		;

TableElementList:
			TableElement
				{
					$$ = list_make1($1);
				}
			| TableElementList ',' TableElement
				{
					$$ = lappend($1, $3);
				}
		;

TypedTableElementList:
			TypedTableElement
				{
					$$ = list_make1($1);
				}
			| TypedTableElementList ',' TypedTableElement
				{
					$$ = lappend($1, $3);
				}
		;

TableElement:
			columnDef							{ $$ = $1; }
			| TableLikeClause					{ $$ = $1; }
			| TableConstraint					{ $$ = $1; }
		;

TypedTableElement:
			columnOptions						{ $$ = $1; }
			| TableConstraint					{ $$ = $1; }
		;

columnDef:	ColId Typename opt_column_storage opt_column_compression create_generic_options ColQualList
				{
					sq_column_def_t *n = make_node<sq_column_def_t>();

					n->colname = $1;
					n->type_name = $2;
					n->storage_name = $3;
					n->compression = $4;
					n->inhcount = 0;
					n->is_local = true;
					n->is_not_null = false;
					n->is_from_type = false;
					n->storage = 0;
					n->raw_default = NULL;
					n->cooked_default = NULL;
					n->coll_oid = invalid_oid;
					n->fdwoptions = $5;
					split_col_qual_list($6, &n->constraints, &n->coll_clause,
									 yyscanner);
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
		;

columnOptions:	ColId ColQualList
				{
					sq_column_def_t *n = make_node<sq_column_def_t>();

					n->colname = $1;
					n->type_name = NULL;
					n->inhcount = 0;
					n->is_local = true;
					n->is_not_null = false;
					n->is_from_type = false;
					n->storage = 0;
					n->raw_default = NULL;
					n->cooked_default = NULL;
					n->coll_oid = invalid_oid;
					split_col_qual_list($2, &n->constraints, &n->coll_clause,
									 yyscanner);
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
				| ColId WITH OPTIONS ColQualList
				{
					sq_column_def_t *n = make_node<sq_column_def_t>();

					n->colname = $1;
					n->type_name = NULL;
					n->inhcount = 0;
					n->is_local = true;
					n->is_not_null = false;
					n->is_from_type = false;
					n->storage = 0;
					n->raw_default = NULL;
					n->cooked_default = NULL;
					n->coll_oid = invalid_oid;
					split_col_qual_list($4, &n->constraints, &n->coll_clause,
									 yyscanner);
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
		;

column_compression:
			COMPRESSION ColId						{ $$ = $2; }
			| COMPRESSION DEFAULT					{ $$ = tree_strdup("default"); }
		;

opt_column_compression:
			column_compression						{ $$ = $1; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

column_storage:
			STORAGE ColId							{ $$ = $2; }
			| STORAGE DEFAULT						{ $$ = tree_strdup("default"); }
		;

opt_column_storage:
			column_storage							{ $$ = $1; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

ColQualList:
			ColQualList ColConstraint				{ $$ = lappend($1, $2); }
			| /*EMPTY*/								{ $$ = NIL; }
		;

ColConstraint:
			CONSTRAINT name ColConstraintElem
				{
					sq_constraint_t *n = cast_node(sq_constraint_t, $3);

					n->conname = $2;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| ColConstraintElem						{ $$ = $1; }
			| ConstraintAttr						{ $$ = $1; }
			| COLLATE any_name
				{
					/*
					 * Note: the CollateClause is momentarily included in
					 * the list built by ColQualList, but we split it out
					 * again in SplitColQualList.
					 */
					sq_collate_clause_t *n = make_node<sq_collate_clause_t>();

					n->arg = NULL;
					n->collname = $2;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
		;

/* DEFAULT NULL is already the default for Postgres.
 * But define it here and carry it forward into the system
 * to make it explicit.
 * - thomas 1998-09-13
 *
 * WITH NULL and NULL are not SQL-standard syntax elements,
 * so leave them out. Use DEFAULT NULL to explicitly indicate
 * that a column may have that value. WITH NULL leads to
 * shift/reduce conflicts with WITH TIME ZONE anyway.
 * - thomas 1999-01-08
 *
 * DEFAULT expression must be b_expr not a_expr to prevent shift/reduce
 * conflict on NOT (since NOT might start a subsequent NOT NULL constraint,
 * or be part of a_expr NOT LIKE or similar constructs).
 */
ColConstraintElem:
			NOT NULL_P opt_no_inherit
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_NOTNULL;
					n->location = @1;
					n->is_no_inherit = $3;
					n->is_enforced = true;
					n->skip_validation = false;
					n->initially_valid = true;
					$$ = (sq_node_t *) n;
				}
			| NULL_P
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_NULL;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| UNIQUE opt_unique_null_treatment opt_definition OptConsTableSpace
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_UNIQUE;
					n->location = @1;
					n->nulls_not_distinct = !$2;
					n->keys = NIL;
					n->options = $3;
					n->indexname = NULL;
					n->indexspace = $4;
					$$ = (sq_node_t *) n;
				}
			| PRIMARY KEY opt_definition OptConsTableSpace
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_PRIMARY;
					n->location = @1;
					n->keys = NIL;
					n->options = $3;
					n->indexname = NULL;
					n->indexspace = $4;
					$$ = (sq_node_t *) n;
				}
			| CHECK '(' a_expr ')' opt_no_inherit
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_CHECK;
					n->location = @1;
					n->is_no_inherit = $5;
					n->raw_expr = $3;
					n->cooked_expr = NULL;
					n->is_enforced = true;
					n->skip_validation = false;
					n->initially_valid = true;
					$$ = (sq_node_t *) n;
				}
			| DEFAULT b_expr
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_DEFAULT;
					n->location = @1;
					n->raw_expr = $2;
					n->cooked_expr = NULL;
					$$ = (sq_node_t *) n;
				}
			/*
			 * digres's, not PostgreSQL's: a column's update default, what
			 * an UPDATE that does not assign the column writes in it
			 * (../../dg/ONUPDATE.md). Marked by generated_when, which a
			 * default does not use.
			 */
			| DEFAULT b_expr ON UPDATE
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_DEFAULT;
					n->location = @1;
					n->raw_expr = $2;
					n->cooked_expr = NULL;
					n->generated_when = 'u';
					$$ = (sq_node_t *) n;
				}
			| GENERATED generated_when AS IDENTITY_P OptParenthesizedSeqOptList
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_IDENTITY;
					n->generated_when = $2;
					n->options = $5;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| GENERATED generated_when AS '(' a_expr ')' opt_virtual_or_stored
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_GENERATED;
					n->generated_when = $2;
					n->raw_expr = $5;
					n->cooked_expr = NULL;
					n->generated_kind = $7;
					n->location = @1;

					/*
					 * Can't do this in the grammar because of shift/reduce
					 * conflicts.  (IDENTITY allows both ALWAYS and BY
					 * DEFAULT, but generated columns only allow ALWAYS.)  We
					 * can also give a more useful error message and location.
					 */
					if ($2 != ATTRIBUTE_IDENTITY_ALWAYS)
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@2)}, "for a generated column, GENERATED ALWAYS must be specified");

					$$ = (sq_node_t *) n;
				}
			| REFERENCES qualified_name opt_column_list key_match key_actions
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_FOREIGN;
					n->location = @1;
					n->pktable = $2;
					n->fk_attrs = NIL;
					n->pk_attrs = $3;
					n->fk_matchtype = $4;
					n->fk_upd_action = ($5)->update_action->action;
					n->fk_del_action = ($5)->delete_action->action;
					n->fk_del_set_cols = ($5)->delete_action->cols;
					n->is_enforced = true;
					n->skip_validation = false;
					n->initially_valid = true;
					$$ = (sq_node_t *) n;
				}
		;

opt_unique_null_treatment:
			NULLS_P DISTINCT		{ $$ = true; }
			| NULLS_P NOT DISTINCT	{ $$ = false; }
			| /*EMPTY*/				{ $$ = true; }
		;

generated_when:
			ALWAYS			{ $$ = ATTRIBUTE_IDENTITY_ALWAYS; }
			| BY DEFAULT	{ $$ = ATTRIBUTE_IDENTITY_BY_DEFAULT; }
		;

opt_virtual_or_stored:
			STORED			{ $$ = ATTRIBUTE_GENERATED_STORED; }
			| VIRTUAL		{ $$ = ATTRIBUTE_GENERATED_VIRTUAL; }
			| /*EMPTY*/		{ $$ = ATTRIBUTE_GENERATED_VIRTUAL; }
		;

/*
 * ConstraintAttr represents constraint attributes, which we parse as if
 * they were independent constraint clauses, in order to avoid shift/reduce
 * conflicts (since NOT might start either an independent NOT NULL clause
 * or an attribute).  parse_utilcmd.c is responsible for attaching the
 * attribute information to the preceding "real" constraint node, and for
 * complaining if attribute clauses appear in the wrong place or wrong
 * combinations.
 *
 * See also ConstraintAttributeSpec, which can be used in places where
 * there is no parsing conflict.  (Note: currently, NOT VALID and NO INHERIT
 * are allowed clauses in ConstraintAttributeSpec, but not here.  Someday we
 * might need to allow them here too, but for the moment it doesn't seem
 * useful in the statements that use ConstraintAttr.)
 */
ConstraintAttr:
			DEFERRABLE
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_ATTR_DEFERRABLE;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| NOT DEFERRABLE
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_ATTR_NOT_DEFERRABLE;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| INITIALLY DEFERRED
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_ATTR_DEFERRED;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| INITIALLY IMMEDIATE
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_ATTR_IMMEDIATE;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| ENFORCED
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_ATTR_ENFORCED;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| NOT ENFORCED
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_ATTR_NOT_ENFORCED;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
		;


TableLikeClause:
			LIKE qualified_name TableLikeOptionList
				{
					sq_table_like_clause_t *n = make_node<sq_table_like_clause_t>();

					n->relation = $2;
					n->options = $3;
					n->relation_oid = invalid_oid;
					$$ = (sq_node_t *) n;
				}
		;

TableLikeOptionList:
				TableLikeOptionList INCLUDING TableLikeOption	{ $$ = $1 | $3; }
				| TableLikeOptionList EXCLUDING TableLikeOption	{ $$ = $1 & ~$3; }
				| /* EMPTY */						{ $$ = 0; }
		;

TableLikeOption:
				COMMENTS			{ $$ = SQ_CREATE_TABLE_LIKE_COMMENTS; }
				| COMPRESSION		{ $$ = SQ_CREATE_TABLE_LIKE_COMPRESSION; }
				| CONSTRAINTS		{ $$ = SQ_CREATE_TABLE_LIKE_CONSTRAINTS; }
				| DEFAULTS			{ $$ = SQ_CREATE_TABLE_LIKE_DEFAULTS; }
				| IDENTITY_P		{ $$ = SQ_CREATE_TABLE_LIKE_IDENTITY; }
				| GENERATED			{ $$ = SQ_CREATE_TABLE_LIKE_GENERATED; }
				| INDEXES			{ $$ = SQ_CREATE_TABLE_LIKE_INDEXES; }
				| STATISTICS		{ $$ = SQ_CREATE_TABLE_LIKE_STATISTICS; }
				| STORAGE			{ $$ = SQ_CREATE_TABLE_LIKE_STORAGE; }
				| ALL				{ $$ = SQ_CREATE_TABLE_LIKE_ALL; }
		;


/* ConstraintElem specifies constraint syntax which is not embedded into
 *	a column definition. ColConstraintElem specifies the embedded form.
 * - thomas 1997-12-03
 */
TableConstraint:
			CONSTRAINT name ConstraintElem
				{
					sq_constraint_t *n = cast_node(sq_constraint_t, $3);

					n->conname = $2;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| ConstraintElem						{ $$ = $1; }
		;

ConstraintElem:
			CHECK '(' a_expr ')' ConstraintAttributeSpec
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_CHECK;
					n->location = @1;
					n->raw_expr = $3;
					n->cooked_expr = NULL;
					process_ca_sbits($5, @5, "CHECK",
								   NULL, NULL, &n->is_enforced, &n->skip_validation,
								   &n->is_no_inherit, yyscanner);
					n->initially_valid = !n->skip_validation;
					$$ = (sq_node_t *) n;
				}
			| NOT NULL_P ColId ConstraintAttributeSpec
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_NOTNULL;
					n->location = @1;
					n->keys = list_make1(make_string($3));
					process_ca_sbits($4, @4, "NOT NULL",
								   NULL, NULL, NULL, &n->skip_validation,
								   &n->is_no_inherit, yyscanner);
					n->initially_valid = !n->skip_validation;
					$$ = (sq_node_t *) n;
				}
			| UNIQUE opt_unique_null_treatment '(' columnList opt_without_overlaps ')' opt_c_include opt_definition OptConsTableSpace
				ConstraintAttributeSpec
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_UNIQUE;
					n->location = @1;
					n->nulls_not_distinct = !$2;
					n->keys = $4;
					n->without_overlaps = $5;
					n->including = $7;
					n->options = $8;
					n->indexname = NULL;
					n->indexspace = $9;
					process_ca_sbits($10, @10, "UNIQUE",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					$$ = (sq_node_t *) n;
				}
			| UNIQUE ExistingIndex ConstraintAttributeSpec
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_UNIQUE;
					n->location = @1;
					n->keys = NIL;
					n->including = NIL;
					n->options = NIL;
					n->indexname = $2;
					n->indexspace = NULL;
					process_ca_sbits($3, @3, "UNIQUE",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					$$ = (sq_node_t *) n;
				}
			| PRIMARY KEY '(' columnList opt_without_overlaps ')' opt_c_include opt_definition OptConsTableSpace
				ConstraintAttributeSpec
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_PRIMARY;
					n->location = @1;
					n->keys = $4;
					n->without_overlaps = $5;
					n->including = $7;
					n->options = $8;
					n->indexname = NULL;
					n->indexspace = $9;
					process_ca_sbits($10, @10, "PRIMARY KEY",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					$$ = (sq_node_t *) n;
				}
			| PRIMARY KEY ExistingIndex ConstraintAttributeSpec
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_PRIMARY;
					n->location = @1;
					n->keys = NIL;
					n->including = NIL;
					n->options = NIL;
					n->indexname = $3;
					n->indexspace = NULL;
					process_ca_sbits($4, @4, "PRIMARY KEY",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					$$ = (sq_node_t *) n;
				}
			| EXCLUDE access_method_clause '(' ExclusionConstraintList ')'
				opt_c_include opt_definition OptConsTableSpace OptWhereClause
				ConstraintAttributeSpec
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_EXCLUSION;
					n->location = @1;
					n->access_method = $2;
					n->exclusions = $4;
					n->including = $6;
					n->options = $7;
					n->indexname = NULL;
					n->indexspace = $8;
					n->where_clause = $9;
					process_ca_sbits($10, @10, "EXCLUDE",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					$$ = (sq_node_t *) n;
				}
			| FOREIGN KEY '(' columnList optionalPeriodName ')' REFERENCES qualified_name
				opt_column_and_period_list key_match key_actions ConstraintAttributeSpec
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_FOREIGN;
					n->location = @1;
					n->pktable = $8;
					n->fk_attrs = $4;
					if ($5)
					{
						n->fk_attrs = lappend(n->fk_attrs, $5);
						n->fk_with_period = true;
					}
					n->pk_attrs = (list_ref_t ) linitial($9);
					if (lsecond($9))
					{
						n->pk_attrs = lappend(n->pk_attrs, lsecond($9));
						n->pk_with_period = true;
					}
					n->fk_matchtype = $10;
					n->fk_upd_action = ($11)->update_action->action;
					n->fk_del_action = ($11)->delete_action->action;
					n->fk_del_set_cols = ($11)->delete_action->cols;
					process_ca_sbits($12, @12, "FOREIGN KEY",
								   &n->deferrable, &n->initdeferred,
								   &n->is_enforced, &n->skip_validation, NULL,
								   yyscanner);
					n->initially_valid = !n->skip_validation;
					$$ = (sq_node_t *) n;
				}
		;

/*
 * DomainConstraint is separate from TableConstraint because the syntax for
 * NOT NULL constraints is different.  For table constraints, we need to
 * accept a column name, but for domain constraints, we don't.  (We could
 * accept something like NOT NULL VALUE, but that seems weird.)  CREATE DOMAIN
 * (which uses ColQualList) has for a long time accepted NOT NULL without a
 * column name, so it makes sense that ALTER DOMAIN (which uses
 * DomainConstraint) does as well.  None of these syntaxes are per SQL
 * standard; we are just living with the bits of inconsistency that have built
 * up over time.
 */
DomainConstraint:
			CONSTRAINT name DomainConstraintElem
				{
					sq_constraint_t *n = cast_node(sq_constraint_t, $3);

					n->conname = $2;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| DomainConstraintElem					{ $$ = $1; }
		;

DomainConstraintElem:
			CHECK '(' a_expr ')' ConstraintAttributeSpec
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_CHECK;
					n->location = @1;
					n->raw_expr = $3;
					n->cooked_expr = NULL;
					process_ca_sbits($5, @5, "CHECK",
								   NULL, NULL, NULL, &n->skip_validation,
								   &n->is_no_inherit, yyscanner);
					n->is_enforced = true;
					n->initially_valid = !n->skip_validation;
					$$ = (sq_node_t *) n;
				}
			| NOT NULL_P ConstraintAttributeSpec
				{
					sq_constraint_t *n = make_node<sq_constraint_t>();

					n->contype = SQ_CONSTR_NOTNULL;
					n->location = @1;
					n->keys = list_make1(make_string("value"));
					/* no NOT VALID, NO INHERIT support */
					process_ca_sbits($3, @3, "NOT NULL",
								   NULL, NULL, NULL,
								   NULL, NULL, yyscanner);
					n->initially_valid = true;
					$$ = (sq_node_t *) n;
				}
		;

opt_no_inherit:	NO INHERIT							{  $$ = true; }
			| /* EMPTY */							{  $$ = false; }
		;

opt_without_overlaps:
			WITHOUT OVERLAPS						{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
	;

opt_column_list:
			'(' columnList ')'						{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

columnList:
			columnElem								{ $$ = list_make1($1); }
			| columnList ',' columnElem				{ $$ = lappend($1, $3); }
		;

optionalPeriodName:
			',' PERIOD columnElem { $$ = $3; }
			| /*EMPTY*/               { $$ = NULL; }
	;

opt_column_and_period_list:
			'(' columnList optionalPeriodName ')'			{ $$ = list_make2($2, $3); }
			| /*EMPTY*/								{ $$ = list_make2(NIL, NULL); }
		;

columnElem: ColId
				{
					$$ = (sq_node_t *) make_string($1);
				}
		;

opt_c_include:	INCLUDE '(' columnList ')'			{ $$ = $3; }
			 |		/* EMPTY */						{ $$ = NIL; }
		;

key_match:  MATCH FULL
			{
				$$ = SQ_FKCONSTR_MATCH_FULL;
			}
		| MATCH PARTIAL
			{
				raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@1)}, "MATCH PARTIAL not yet implemented");
				$$ = SQ_FKCONSTR_MATCH_PARTIAL;
			}
		| MATCH SIMPLE
			{
				$$ = SQ_FKCONSTR_MATCH_SIMPLE;
			}
		| /*EMPTY*/
			{
				$$ = SQ_FKCONSTR_MATCH_SIMPLE;
			}
		;

ExclusionConstraintList:
			ExclusionConstraintElem					{ $$ = list_make1($1); }
			| ExclusionConstraintList ',' ExclusionConstraintElem
													{ $$ = lappend($1, $3); }
		;

ExclusionConstraintElem: index_elem WITH any_operator
			{
				$$ = list_make2($1, $3);
			}
			/* allow OPERATOR() decoration for the benefit of ruleutils.c */
			| index_elem WITH OPERATOR '(' any_operator ')'
			{
				$$ = list_make2($1, $5);
			}
		;

OptWhereClause:
			WHERE '(' a_expr ')'					{ $$ = $3; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

key_actions:
			key_update
				{
					key_actions_t *n = make_node<key_actions_t>();

					n->update_action = $1;
					n->delete_action = make_node<key_action_t>();
					n->delete_action->action = SQ_FKCONSTR_ACTION_NOACTION;
					n->delete_action->cols = NIL;
					$$ = n;
				}
			| key_delete
				{
					key_actions_t *n = make_node<key_actions_t>();

					n->update_action = make_node<key_action_t>();
					n->update_action->action = SQ_FKCONSTR_ACTION_NOACTION;
					n->update_action->cols = NIL;
					n->delete_action = $1;
					$$ = n;
				}
			| key_update key_delete
				{
					key_actions_t *n = make_node<key_actions_t>();

					n->update_action = $1;
					n->delete_action = $2;
					$$ = n;
				}
			| key_delete key_update
				{
					key_actions_t *n = make_node<key_actions_t>();

					n->update_action = $2;
					n->delete_action = $1;
					$$ = n;
				}
			| /*EMPTY*/
				{
					key_actions_t *n = make_node<key_actions_t>();

					n->update_action = make_node<key_action_t>();
					n->update_action->action = SQ_FKCONSTR_ACTION_NOACTION;
					n->update_action->cols = NIL;
					n->delete_action = make_node<key_action_t>();
					n->delete_action->action = SQ_FKCONSTR_ACTION_NOACTION;
					n->delete_action->cols = NIL;
					$$ = n;
				}
		;

key_update: ON UPDATE key_action
				{
					if (($3)->cols)
						raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@1)}, "a column list with {} is only supported for ON DELETE actions",
										($3)->action == SQ_FKCONSTR_ACTION_SETNULL ? "SET NULL" : "SET DEFAULT");
					$$ = $3;
				}
		;

key_delete: ON DELETE_P key_action
				{
					$$ = $3;
				}
		;

key_action:
			NO ACTION
				{
					key_action_t *n = make_node<key_action_t>();

					n->action = SQ_FKCONSTR_ACTION_NOACTION;
					n->cols = NIL;
					$$ = n;
				}
			| RESTRICT
				{
					key_action_t *n = make_node<key_action_t>();

					n->action = SQ_FKCONSTR_ACTION_RESTRICT;
					n->cols = NIL;
					$$ = n;
				}
			| CASCADE
				{
					key_action_t *n = make_node<key_action_t>();

					n->action = SQ_FKCONSTR_ACTION_CASCADE;
					n->cols = NIL;
					$$ = n;
				}
			| SET NULL_P opt_column_list
				{
					key_action_t *n = make_node<key_action_t>();

					n->action = SQ_FKCONSTR_ACTION_SETNULL;
					n->cols = $3;
					$$ = n;
				}
			| SET DEFAULT opt_column_list
				{
					key_action_t *n = make_node<key_action_t>();

					n->action = SQ_FKCONSTR_ACTION_SETDEFAULT;
					n->cols = $3;
					$$ = n;
				}
		;

OptInherit: INHERITS '(' qualified_name_list ')'	{ $$ = $3; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

/* Optional partition key specification */
OptPartitionSpec: PartitionSpec	{ $$ = $1; }
			| /*EMPTY*/			{ $$ = NULL; }
		;

PartitionSpec: PARTITION BY ColId '(' part_params ')'
				{
					sq_partition_spec_t *n = make_node<sq_partition_spec_t>();

					n->strategy = parse_partition_strategy($3, @3, yyscanner);
					n->part_params = $5;
					n->location = @1;

					$$ = n;
				}
		;

part_params:	part_elem						{ $$ = list_make1($1); }
			| part_params ',' part_elem			{ $$ = lappend($1, $3); }
		;

part_elem: ColId opt_collate opt_qualified_name
				{
					sq_partition_elem_t *n = make_node<sq_partition_elem_t>();

					n->name = $1;
					n->expr = NULL;
					n->collation = $2;
					n->opclass = $3;
					n->location = @1;
					$$ = n;
				}
			| func_expr_windowless opt_collate opt_qualified_name
				{
					sq_partition_elem_t *n = make_node<sq_partition_elem_t>();

					n->name = NULL;
					n->expr = $1;
					n->collation = $2;
					n->opclass = $3;
					n->location = @1;
					$$ = n;
				}
			| '(' a_expr ')' opt_collate opt_qualified_name
				{
					sq_partition_elem_t *n = make_node<sq_partition_elem_t>();

					n->name = NULL;
					n->expr = $2;
					n->collation = $4;
					n->opclass = $5;
					n->location = @1;
					$$ = n;
				}
		;

table_access_method_clause:
			USING name							{ $$ = $2; }
			| /*EMPTY*/							{ $$ = NULL; }
		;

/* WITHOUT OIDS is legacy only */
OptWith:
			WITH reloptions				{ $$ = $2; }
			| WITHOUT OIDS				{ $$ = NIL; }
			| /*EMPTY*/					{ $$ = NIL; }
		;

OnCommitOption:  ON COMMIT DROP				{ $$ = SQ_ONCOMMIT_DROP; }
			| ON COMMIT DELETE_P ROWS		{ $$ = SQ_ONCOMMIT_DELETE_ROWS; }
			| ON COMMIT PRESERVE ROWS		{ $$ = SQ_ONCOMMIT_PRESERVE_ROWS; }
			| /*EMPTY*/						{ $$ = SQ_ONCOMMIT_NOOP; }
		;

OptTableSpace:   TABLESPACE name					{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

OptConsTableSpace:   USING INDEX TABLESPACE name	{ $$ = $4; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

ExistingIndex:   USING INDEX name					{ $$ = $3; }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				CREATE STATISTICS [[IF NOT EXISTS] stats_name] [(stat types)]
 *					ON expression-list FROM from_list
 *
 * Note: the expectation here is that the clauses after ON are a subset of
 * SELECT syntax, allowing for expressions and joined tables, and probably
 * someday a WHERE clause.  Much less than that is currently implemented,
 * but the grammar accepts it and then we'll throw FEATURE_NOT_SUPPORTED
 * errors as necessary at execution.
 *
 * Statistics name is optional unless IF NOT EXISTS is specified.
 *
 *****************************************************************************/

CreateStatsStmt:
			CREATE STATISTICS opt_qualified_name
			opt_name_list ON stats_params FROM from_list
				{
					sq_create_stats_stmt_t *n = make_node<sq_create_stats_stmt_t>();

					n->defnames = $3;
					n->stat_types = $4;
					n->exprs = $6;
					n->relations = $8;
					n->stxcomment = NULL;
					n->if_not_exists = false;
					$$ = (sq_node_t *) n;
				}
			| CREATE STATISTICS IF_P NOT EXISTS any_name
			opt_name_list ON stats_params FROM from_list
				{
					sq_create_stats_stmt_t *n = make_node<sq_create_stats_stmt_t>();

					n->defnames = $6;
					n->stat_types = $7;
					n->exprs = $9;
					n->relations = $11;
					n->stxcomment = NULL;
					n->if_not_exists = true;
					$$ = (sq_node_t *) n;
				}
			;

/*
 * Statistics attributes can be either simple column references, or arbitrary
 * expressions in parens.  For compatibility with index attributes permitted
 * in CREATE INDEX, we allow an expression that's just a function call to be
 * written without parens.
 */

stats_params:	stats_param							{ $$ = list_make1($1); }
			| stats_params ',' stats_param			{ $$ = lappend($1, $3); }
		;

stats_param:	ColId
				{
					$$ = make_node<sq_stats_elem_t>();
					$$->name = $1;
					$$->expr = NULL;
				}
			| func_expr_windowless
				{
					$$ = make_node<sq_stats_elem_t>();
					$$->name = NULL;
					$$->expr = $1;
				}
			| '(' a_expr ')'
				{
					$$ = make_node<sq_stats_elem_t>();
					$$->name = NULL;
					$$->expr = $2;
				}
		;

/*****************************************************************************
 *
 *		QUERY :
 *				ALTER STATISTICS [IF EXISTS] stats_name
 *					SET STATISTICS  <SignedIconst>
 *
 *****************************************************************************/

AlterStatsStmt:
			ALTER STATISTICS any_name SET STATISTICS set_statistics_value
				{
					sq_alter_stats_stmt_t *n = make_node<sq_alter_stats_stmt_t>();

					n->defnames = $3;
					n->missing_ok = false;
					n->stxstattarget = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER STATISTICS IF_P EXISTS any_name SET STATISTICS set_statistics_value
				{
					sq_alter_stats_stmt_t *n = make_node<sq_alter_stats_stmt_t>();

					n->defnames = $5;
					n->missing_ok = true;
					n->stxstattarget = $8;
					$$ = (sq_node_t *) n;
				}
			;

/*****************************************************************************
 *
 *		QUERY :
 *				CREATE TABLE relname AS SelectStmt [ WITH [NO] DATA ]
 *
 *
 * Note: SELECT ... INTO is a now-deprecated alternative for this.
 *
 *****************************************************************************/

CreateAsStmt:
		CREATE OptTemp TABLE create_as_target AS SelectStmt opt_with_data
				{
					sq_create_table_as_stmt_t *ctas = make_node<sq_create_table_as_stmt_t>();

					ctas->query = $6;
					ctas->into = $4;
					ctas->objtype = SQ_OBJECT_TABLE;
					ctas->is_select_into = false;
					ctas->if_not_exists = false;
					/* cram additional flags into the IntoClause */
					$4->rel->relpersistence = $2;
					$4->skip_data = !($7);
					$$ = (sq_node_t *) ctas;
				}
		| CREATE OptTemp TABLE IF_P NOT EXISTS create_as_target AS SelectStmt opt_with_data
				{
					sq_create_table_as_stmt_t *ctas = make_node<sq_create_table_as_stmt_t>();

					ctas->query = $9;
					ctas->into = $7;
					ctas->objtype = SQ_OBJECT_TABLE;
					ctas->is_select_into = false;
					ctas->if_not_exists = true;
					/* cram additional flags into the IntoClause */
					$7->rel->relpersistence = $2;
					$7->skip_data = !($10);
					$$ = (sq_node_t *) ctas;
				}
		;

create_as_target:
			qualified_name opt_column_list table_access_method_clause
			OptWith OnCommitOption OptTableSpace
				{
					$$ = make_node<sq_into_clause_t>();
					$$->rel = $1;
					$$->col_names = $2;
					$$->access_method = $3;
					$$->options = $4;
					$$->on_commit = $5;
					$$->table_space_name = $6;
					$$->view_query = NULL;
					$$->skip_data = false;		/* might get changed later */
				}
		;

opt_with_data:
			WITH DATA_P								{ $$ = true; }
			| WITH NO DATA_P						{ $$ = false; }
			| /*EMPTY*/								{ $$ = true; }
		;


/*****************************************************************************
 *
 *		QUERY :
 *				CREATE MATERIALIZED VIEW relname AS SelectStmt
 *
 *****************************************************************************/

CreateMatViewStmt:
		CREATE OptNoLog MATERIALIZED VIEW create_mv_target AS SelectStmt opt_with_data
				{
					sq_create_table_as_stmt_t *ctas = make_node<sq_create_table_as_stmt_t>();

					ctas->query = $7;
					ctas->into = $5;
					ctas->objtype = SQ_OBJECT_MATVIEW;
					ctas->is_select_into = false;
					ctas->if_not_exists = false;
					/* cram additional flags into the IntoClause */
					$5->rel->relpersistence = $2;
					$5->skip_data = !($8);
					$$ = (sq_node_t *) ctas;
				}
		| CREATE OptNoLog MATERIALIZED VIEW IF_P NOT EXISTS create_mv_target AS SelectStmt opt_with_data
				{
					sq_create_table_as_stmt_t *ctas = make_node<sq_create_table_as_stmt_t>();

					ctas->query = $10;
					ctas->into = $8;
					ctas->objtype = SQ_OBJECT_MATVIEW;
					ctas->is_select_into = false;
					ctas->if_not_exists = true;
					/* cram additional flags into the IntoClause */
					$8->rel->relpersistence = $2;
					$8->skip_data = !($11);
					$$ = (sq_node_t *) ctas;
				}
		;

create_mv_target:
			qualified_name opt_column_list table_access_method_clause opt_reloptions OptTableSpace
				{
					$$ = make_node<sq_into_clause_t>();
					$$->rel = $1;
					$$->col_names = $2;
					$$->access_method = $3;
					$$->options = $4;
					$$->on_commit = SQ_ONCOMMIT_NOOP;
					$$->table_space_name = $5;
					$$->view_query = NULL;		/* filled at analysis time */
					$$->skip_data = false;		/* might get changed later */
				}
		;

OptNoLog:	UNLOGGED					{ $$ = RELPERSISTENCE_UNLOGGED; }
			| /*EMPTY*/					{ $$ = RELPERSISTENCE_PERMANENT; }
		;


/*****************************************************************************
 *
 *		QUERY :
 *				REFRESH MATERIALIZED VIEW qualified_name
 *
 *****************************************************************************/

RefreshMatViewStmt:
			REFRESH MATERIALIZED VIEW opt_concurrently qualified_name opt_with_data
				{
					sq_refresh_mat_view_stmt_t *n = make_node<sq_refresh_mat_view_stmt_t>();

					n->concurrent = $4;
					n->relation = $5;
					n->skip_data = !($6);
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				CREATE SEQUENCE seqname
 *				ALTER SEQUENCE seqname
 *
 *****************************************************************************/

CreateSeqStmt:
			CREATE OptTemp SEQUENCE qualified_name OptSeqOptList
				{
					sq_create_seq_stmt_t *n = make_node<sq_create_seq_stmt_t>();

					$4->relpersistence = $2;
					n->sequence = $4;
					n->options = $5;
					n->owner_id = invalid_oid;
					n->if_not_exists = false;
					$$ = (sq_node_t *) n;
				}
			| CREATE OptTemp SEQUENCE IF_P NOT EXISTS qualified_name OptSeqOptList
				{
					sq_create_seq_stmt_t *n = make_node<sq_create_seq_stmt_t>();

					$7->relpersistence = $2;
					n->sequence = $7;
					n->options = $8;
					n->owner_id = invalid_oid;
					n->if_not_exists = true;
					$$ = (sq_node_t *) n;
				}
		;

AlterSeqStmt:
			ALTER SEQUENCE qualified_name SeqOptList
				{
					sq_alter_seq_stmt_t *n = make_node<sq_alter_seq_stmt_t>();

					n->sequence = $3;
					n->options = $4;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER SEQUENCE IF_P EXISTS qualified_name SeqOptList
				{
					sq_alter_seq_stmt_t *n = make_node<sq_alter_seq_stmt_t>();

					n->sequence = $5;
					n->options = $6;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}

		;

OptSeqOptList: SeqOptList							{ $$ = $1; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

OptParenthesizedSeqOptList: '(' SeqOptList ')'		{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

SeqOptList: SeqOptElem								{ $$ = list_make1($1); }
			| SeqOptList SeqOptElem					{ $$ = lappend($1, $2); }
		;

SeqOptElem: AS SimpleTypename
				{
					$$ = make_def_elem("as", (sq_node_t *) $2, @1);
				}
			| CACHE NumericOnly
				{
					$$ = make_def_elem("cache", (sq_node_t *) $2, @1);
				}
			| CYCLE
				{
					$$ = make_def_elem("cycle", (sq_node_t *) make_boolean(true), @1);
				}
			| NO CYCLE
				{
					$$ = make_def_elem("cycle", (sq_node_t *) make_boolean(false), @1);
				}
			| INCREMENT opt_by NumericOnly
				{
					$$ = make_def_elem("increment", (sq_node_t *) $3, @1);
				}
			| LOGGED
				{
					$$ = make_def_elem("logged", NULL, @1);
				}
			| MAXVALUE NumericOnly
				{
					$$ = make_def_elem("maxvalue", (sq_node_t *) $2, @1);
				}
			| MINVALUE NumericOnly
				{
					$$ = make_def_elem("minvalue", (sq_node_t *) $2, @1);
				}
			| NO MAXVALUE
				{
					$$ = make_def_elem("maxvalue", NULL, @1);
				}
			| NO MINVALUE
				{
					$$ = make_def_elem("minvalue", NULL, @1);
				}
			| OWNED BY any_name
				{
					$$ = make_def_elem("owned_by", (sq_node_t *) $3, @1);
				}
			| SEQUENCE NAME_P any_name
				{
					$$ = make_def_elem("sequence_name", (sq_node_t *) $3, @1);
				}
			| START opt_with NumericOnly
				{
					$$ = make_def_elem("start", (sq_node_t *) $3, @1);
				}
			| RESTART
				{
					$$ = make_def_elem("restart", NULL, @1);
				}
			| RESTART opt_with NumericOnly
				{
					$$ = make_def_elem("restart", (sq_node_t *) $3, @1);
				}
			| UNLOGGED
				{
					$$ = make_def_elem("unlogged", NULL, @1);
				}
		;

opt_by:		BY
			| /* EMPTY */
	  ;

NumericOnly:
			FCONST								{ $$ = (sq_node_t *) make_float($1); }
			| '+' FCONST						{ $$ = (sq_node_t *) make_float($2); }
			| '-' FCONST
				{
					sq_float_t	   *f = make_float($2);

					do_negate_float(f);
					$$ = (sq_node_t *) f;
				}
			| SignedIconst						{ $$ = (sq_node_t *) make_integer($1); }
		;

NumericOnly_list:	NumericOnly						{ $$ = list_make1($1); }
				| NumericOnly_list ',' NumericOnly	{ $$ = lappend($1, $3); }
		;

/*****************************************************************************
 *
 *		QUERIES :
 *				CREATE [OR REPLACE] [TRUSTED] [PROCEDURAL] LANGUAGE ...
 *				DROP [PROCEDURAL] LANGUAGE ...
 *
 *****************************************************************************/

CreatePLangStmt:
			CREATE opt_or_replace opt_trusted opt_procedural LANGUAGE name
			{
				/*
				 * We now interpret parameterless CREATE LANGUAGE as
				 * CREATE EXTENSION.  "OR REPLACE" is silently translated
				 * to "IF NOT EXISTS", which isn't quite the same, but
				 * seems more useful than throwing an error.  We just
				 * ignore TRUSTED, as the previous code would have too.
				 */
				sq_create_extension_stmt_t *n = make_node<sq_create_extension_stmt_t>();

				n->if_not_exists = $2;
				n->extname = $6;
				n->options = NIL;
				$$ = (sq_node_t *) n;
			}
			| CREATE opt_or_replace opt_trusted opt_procedural LANGUAGE name
			  HANDLER handler_name opt_inline_handler opt_validator
			{
				sq_create_p_lang_stmt_t *n = make_node<sq_create_p_lang_stmt_t>();

				n->replace = $2;
				n->plname = $6;
				n->plhandler = $8;
				n->plinline = $9;
				n->plvalidator = $10;
				n->pltrusted = $3;
				$$ = (sq_node_t *) n;
			}
		;

opt_trusted:
			TRUSTED									{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
		;

/* This ought to be just func_name, but that causes reduce/reduce conflicts
 * (CREATE LANGUAGE is the only place where func_name isn't followed by '(').
 * Work around by using simple names, instead.
 */
handler_name:
			name						{ $$ = list_make1(make_string($1)); }
			| name attrs				{ $$ = lcons(make_string($1), $2); }
		;

opt_inline_handler:
			INLINE_P handler_name					{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

validator_clause:
			VALIDATOR handler_name					{ $$ = $2; }
			| NO VALIDATOR							{ $$ = NIL; }
		;

opt_validator:
			validator_clause						{ $$ = $1; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

opt_procedural:
			PROCEDURAL
			| /*EMPTY*/
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE TABLESPACE tablespace LOCATION '/path/to/tablespace/'
 *
 *****************************************************************************/

CreateTableSpaceStmt: CREATE TABLESPACE name OptTableSpaceOwner LOCATION Sconst opt_reloptions
				{
					sq_create_table_space_stmt_t *n = make_node<sq_create_table_space_stmt_t>();

					n->tablespacename = $3;
					n->owner = $4;
					n->location = $6;
					n->options = $7;
					$$ = (sq_node_t *) n;
				}
		;

OptTableSpaceOwner: OWNER RoleSpec		{ $$ = $2; }
			| /*EMPTY */				{ $$ = NULL; }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				DROP TABLESPACE <tablespace>
 *
 *		No need for drop behaviour as we cannot implement dependencies for
 *		objects in other databases; we can only support RESTRICT.
 *
 ****************************************************************************/

DropTableSpaceStmt: DROP TABLESPACE name
				{
					sq_drop_table_space_stmt_t *n = make_node<sq_drop_table_space_stmt_t>();

					n->tablespacename = $3;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
				|  DROP TABLESPACE IF_P EXISTS name
				{
					sq_drop_table_space_stmt_t *n = make_node<sq_drop_table_space_stmt_t>();

					n->tablespacename = $5;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE EXTENSION extension
 *             [ WITH ] [ SCHEMA schema ] [ VERSION version ]
 *
 *****************************************************************************/

CreateExtensionStmt: CREATE EXTENSION name opt_with create_extension_opt_list
				{
					sq_create_extension_stmt_t *n = make_node<sq_create_extension_stmt_t>();

					n->extname = $3;
					n->if_not_exists = false;
					n->options = $5;
					$$ = (sq_node_t *) n;
				}
				| CREATE EXTENSION IF_P NOT EXISTS name opt_with create_extension_opt_list
				{
					sq_create_extension_stmt_t *n = make_node<sq_create_extension_stmt_t>();

					n->extname = $6;
					n->if_not_exists = true;
					n->options = $8;
					$$ = (sq_node_t *) n;
				}
		;

create_extension_opt_list:
			create_extension_opt_list create_extension_opt_item
				{ $$ = lappend($1, $2); }
			| /* EMPTY */
				{ $$ = NIL; }
		;

create_extension_opt_item:
			SCHEMA name
				{
					$$ = make_def_elem("schema", (sq_node_t *) make_string($2), @1);
				}
			| VERSION_P NonReservedWord_or_Sconst
				{
					$$ = make_def_elem("new_version", (sq_node_t *) make_string($2), @1);
				}
			| FROM NonReservedWord_or_Sconst
				{
					raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@1)}, "CREATE EXTENSION ... FROM is no longer supported");
				}
			| CASCADE
				{
					$$ = make_def_elem("cascade", (sq_node_t *) make_boolean(true), @1);
				}
		;

/*****************************************************************************
 *
 * ALTER EXTENSION name UPDATE [ TO version ]
 *
 *****************************************************************************/

AlterExtensionStmt: ALTER EXTENSION name UPDATE alter_extension_opt_list
				{
					sq_alter_extension_stmt_t *n = make_node<sq_alter_extension_stmt_t>();

					n->extname = $3;
					n->options = $5;
					$$ = (sq_node_t *) n;
				}
		;

alter_extension_opt_list:
			alter_extension_opt_list alter_extension_opt_item
				{ $$ = lappend($1, $2); }
			| /* EMPTY */
				{ $$ = NIL; }
		;

alter_extension_opt_item:
			TO NonReservedWord_or_Sconst
				{
					$$ = make_def_elem("new_version", (sq_node_t *) make_string($2), @1);
				}
		;

/*****************************************************************************
 *
 * ALTER EXTENSION name ADD/DROP object-identifier
 *
 *****************************************************************************/

AlterExtensionContentsStmt:
			ALTER EXTENSION name add_drop object_type_name name
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = $5;
					n->object = (sq_node_t *) make_string($6);
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop object_type_any_name any_name
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = $5;
					n->object = (sq_node_t *) $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop AGGREGATE aggregate_with_argtypes
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = SQ_OBJECT_AGGREGATE;
					n->object = (sq_node_t *) $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop CAST '(' Typename AS Typename ')'
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = SQ_OBJECT_CAST;
					n->object = (sq_node_t *) list_make2($7, $9);
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop DOMAIN_P Typename
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = SQ_OBJECT_DOMAIN;
					n->object = (sq_node_t *) $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop FUNCTION function_with_argtypes
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = SQ_OBJECT_FUNCTION;
					n->object = (sq_node_t *) $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop OPERATOR operator_with_argtypes
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = SQ_OBJECT_OPERATOR;
					n->object = (sq_node_t *) $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop OPERATOR CLASS any_name USING name
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = SQ_OBJECT_OPCLASS;
					n->object = (sq_node_t *) lcons(make_string($9), $7);
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop OPERATOR FAMILY any_name USING name
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = SQ_OBJECT_OPFAMILY;
					n->object = (sq_node_t *) lcons(make_string($9), $7);
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop PROCEDURE function_with_argtypes
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = SQ_OBJECT_PROCEDURE;
					n->object = (sq_node_t *) $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop ROUTINE function_with_argtypes
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = SQ_OBJECT_ROUTINE;
					n->object = (sq_node_t *) $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop TRANSFORM FOR Typename LANGUAGE name
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = SQ_OBJECT_TRANSFORM;
					n->object = (sq_node_t *) list_make2($7, make_string($9));
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name add_drop TYPE_P Typename
				{
					sq_alter_extension_contents_stmt_t *n = make_node<sq_alter_extension_contents_stmt_t>();

					n->extname = $3;
					n->action = $4;
					n->objtype = SQ_OBJECT_TYPE;
					n->object = (sq_node_t *) $6;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE FOREIGN DATA WRAPPER name options
 *
 *****************************************************************************/

CreateFdwStmt: CREATE FOREIGN DATA_P WRAPPER name opt_fdw_options create_generic_options
				{
					sq_create_fdw_stmt_t *n = make_node<sq_create_fdw_stmt_t>();

					n->fdwname = $5;
					n->func_options = $6;
					n->options = $7;
					$$ = (sq_node_t *) n;
				}
		;

fdw_option:
			HANDLER handler_name				{ $$ = make_def_elem("handler", (sq_node_t *) $2, @1); }
			| NO HANDLER						{ $$ = make_def_elem("handler", NULL, @1); }
			| VALIDATOR handler_name			{ $$ = make_def_elem("validator", (sq_node_t *) $2, @1); }
			| NO VALIDATOR						{ $$ = make_def_elem("validator", NULL, @1); }
		;

fdw_options:
			fdw_option							{ $$ = list_make1($1); }
			| fdw_options fdw_option			{ $$ = lappend($1, $2); }
		;

opt_fdw_options:
			fdw_options							{ $$ = $1; }
			| /*EMPTY*/							{ $$ = NIL; }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				ALTER FOREIGN DATA WRAPPER name options
 *
 ****************************************************************************/

AlterFdwStmt: ALTER FOREIGN DATA_P WRAPPER name opt_fdw_options alter_generic_options
				{
					sq_alter_fdw_stmt_t *n = make_node<sq_alter_fdw_stmt_t>();

					n->fdwname = $5;
					n->func_options = $6;
					n->options = $7;
					$$ = (sq_node_t *) n;
				}
			| ALTER FOREIGN DATA_P WRAPPER name fdw_options
				{
					sq_alter_fdw_stmt_t *n = make_node<sq_alter_fdw_stmt_t>();

					n->fdwname = $5;
					n->func_options = $6;
					n->options = NIL;
					$$ = (sq_node_t *) n;
				}
		;

/* Options definition for CREATE FDW, SERVER and USER MAPPING */
create_generic_options:
			OPTIONS '(' generic_option_list ')'			{ $$ = $3; }
			| /*EMPTY*/									{ $$ = NIL; }
		;

generic_option_list:
			generic_option_elem
				{
					$$ = list_make1($1);
				}
			| generic_option_list ',' generic_option_elem
				{
					$$ = lappend($1, $3);
				}
		;

/* Options definition for ALTER FDW, SERVER and USER MAPPING */
alter_generic_options:
			OPTIONS	'(' alter_generic_option_list ')'		{ $$ = $3; }
		;

alter_generic_option_list:
			alter_generic_option_elem
				{
					$$ = list_make1($1);
				}
			| alter_generic_option_list ',' alter_generic_option_elem
				{
					$$ = lappend($1, $3);
				}
		;

alter_generic_option_elem:
			generic_option_elem
				{
					$$ = $1;
				}
			| SET generic_option_elem
				{
					$$ = $2;
					$$->defaction = SQ_DEFELEM_SET;
				}
			| ADD_P generic_option_elem
				{
					$$ = $2;
					$$->defaction = SQ_DEFELEM_ADD;
				}
			| DROP generic_option_name
				{
					$$ = make_def_elem_extended(NULL, $2, NULL, SQ_DEFELEM_DROP, @2);
				}
		;

generic_option_elem:
			generic_option_name generic_option_arg
				{
					$$ = make_def_elem($1, $2, @1);
				}
		;

generic_option_name:
				ColLabel			{ $$ = $1; }
		;

/* We could use def_arg here, but the spec only requires string literals */
generic_option_arg:
				Sconst				{ $$ = (sq_node_t *) make_string($1); }
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE SERVER name [TYPE] [VERSION] [OPTIONS]
 *
 *****************************************************************************/

CreateForeignServerStmt: CREATE SERVER name opt_type opt_foreign_server_version
						 FOREIGN DATA_P WRAPPER name create_generic_options
				{
					sq_create_foreign_server_stmt_t *n = make_node<sq_create_foreign_server_stmt_t>();

					n->servername = $3;
					n->servertype = $4;
					n->version = $5;
					n->fdwname = $9;
					n->options = $10;
					n->if_not_exists = false;
					$$ = (sq_node_t *) n;
				}
				| CREATE SERVER IF_P NOT EXISTS name opt_type opt_foreign_server_version
						 FOREIGN DATA_P WRAPPER name create_generic_options
				{
					sq_create_foreign_server_stmt_t *n = make_node<sq_create_foreign_server_stmt_t>();

					n->servername = $6;
					n->servertype = $7;
					n->version = $8;
					n->fdwname = $12;
					n->options = $13;
					n->if_not_exists = true;
					$$ = (sq_node_t *) n;
				}
		;

opt_type:
			TYPE_P Sconst			{ $$ = $2; }
			| /*EMPTY*/				{ $$ = NULL; }
		;


foreign_server_version:
			VERSION_P Sconst		{ $$ = $2; }
		|	VERSION_P NULL_P		{ $$ = NULL; }
		;

opt_foreign_server_version:
			foreign_server_version	{ $$ = $1; }
			| /*EMPTY*/				{ $$ = NULL; }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				ALTER SERVER name [VERSION] [OPTIONS]
 *
 ****************************************************************************/

AlterForeignServerStmt: ALTER SERVER name foreign_server_version alter_generic_options
				{
					sq_alter_foreign_server_stmt_t *n = make_node<sq_alter_foreign_server_stmt_t>();

					n->servername = $3;
					n->version = $4;
					n->options = $5;
					n->has_version = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER SERVER name foreign_server_version
				{
					sq_alter_foreign_server_stmt_t *n = make_node<sq_alter_foreign_server_stmt_t>();

					n->servername = $3;
					n->version = $4;
					n->has_version = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER SERVER name alter_generic_options
				{
					sq_alter_foreign_server_stmt_t *n = make_node<sq_alter_foreign_server_stmt_t>();

					n->servername = $3;
					n->options = $4;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE FOREIGN TABLE relname (...) SERVER name (...)
 *
 *****************************************************************************/

CreateForeignTableStmt:
		CREATE FOREIGN TABLE qualified_name
			'(' OptTableElementList ')'
			OptInherit SERVER name create_generic_options
				{
					sq_create_foreign_table_stmt_t *n = make_node<sq_create_foreign_table_stmt_t>();

					$4->relpersistence = RELPERSISTENCE_PERMANENT;
					n->base.relation = $4;
					n->base.table_elts = $6;
					n->base.inh_relations = $8;
					n->base.of_typename = NULL;
					n->base.constraints = NIL;
					n->base.options = NIL;
					n->base.oncommit = SQ_ONCOMMIT_NOOP;
					n->base.tablespacename = NULL;
					n->base.if_not_exists = false;
					/* FDW-specific data */
					n->servername = $10;
					n->options = $11;
					$$ = (sq_node_t *) n;
				}
		| CREATE FOREIGN TABLE IF_P NOT EXISTS qualified_name
			'(' OptTableElementList ')'
			OptInherit SERVER name create_generic_options
				{
					sq_create_foreign_table_stmt_t *n = make_node<sq_create_foreign_table_stmt_t>();

					$7->relpersistence = RELPERSISTENCE_PERMANENT;
					n->base.relation = $7;
					n->base.table_elts = $9;
					n->base.inh_relations = $11;
					n->base.of_typename = NULL;
					n->base.constraints = NIL;
					n->base.options = NIL;
					n->base.oncommit = SQ_ONCOMMIT_NOOP;
					n->base.tablespacename = NULL;
					n->base.if_not_exists = true;
					/* FDW-specific data */
					n->servername = $13;
					n->options = $14;
					$$ = (sq_node_t *) n;
				}
		| CREATE FOREIGN TABLE qualified_name
			PARTITION OF qualified_name OptTypedTableElementList PartitionBoundSpec
			SERVER name create_generic_options
				{
					sq_create_foreign_table_stmt_t *n = make_node<sq_create_foreign_table_stmt_t>();

					$4->relpersistence = RELPERSISTENCE_PERMANENT;
					n->base.relation = $4;
					n->base.inh_relations = list_make1($7);
					n->base.table_elts = $8;
					n->base.partbound = $9;
					n->base.of_typename = NULL;
					n->base.constraints = NIL;
					n->base.options = NIL;
					n->base.oncommit = SQ_ONCOMMIT_NOOP;
					n->base.tablespacename = NULL;
					n->base.if_not_exists = false;
					/* FDW-specific data */
					n->servername = $11;
					n->options = $12;
					$$ = (sq_node_t *) n;
				}
		| CREATE FOREIGN TABLE IF_P NOT EXISTS qualified_name
			PARTITION OF qualified_name OptTypedTableElementList PartitionBoundSpec
			SERVER name create_generic_options
				{
					sq_create_foreign_table_stmt_t *n = make_node<sq_create_foreign_table_stmt_t>();

					$7->relpersistence = RELPERSISTENCE_PERMANENT;
					n->base.relation = $7;
					n->base.inh_relations = list_make1($10);
					n->base.table_elts = $11;
					n->base.partbound = $12;
					n->base.of_typename = NULL;
					n->base.constraints = NIL;
					n->base.options = NIL;
					n->base.oncommit = SQ_ONCOMMIT_NOOP;
					n->base.tablespacename = NULL;
					n->base.if_not_exists = true;
					/* FDW-specific data */
					n->servername = $14;
					n->options = $15;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				IMPORT FOREIGN SCHEMA remote_schema
 *				[ { LIMIT TO | EXCEPT } ( table_list ) ]
 *				FROM SERVER server_name INTO local_schema [ OPTIONS (...) ]
 *
 ****************************************************************************/

ImportForeignSchemaStmt:
		IMPORT_P FOREIGN SCHEMA name import_qualification
		  FROM SERVER name INTO name create_generic_options
			{
				sq_import_foreign_schema_stmt_t *n = make_node<sq_import_foreign_schema_stmt_t>();

				n->server_name = $8;
				n->remote_schema = $4;
				n->local_schema = $10;
				n->list_type = $5->type;
				n->table_list = $5->table_names;
				n->options = $11;
				$$ = (sq_node_t *) n;
			}
		;

import_qualification_type:
		LIMIT TO				{ $$ = SQ_FDW_IMPORT_SCHEMA_LIMIT_TO; }
		| EXCEPT				{ $$ = SQ_FDW_IMPORT_SCHEMA_EXCEPT; }
		;

import_qualification:
		import_qualification_type '(' relation_expr_list ')'
			{
				import_qual_t *n = make_node<import_qual_t>();

				n->type = (sq_import_foreign_schema_type_t) $1;
				n->table_names = $3;
				$$ = n;
			}
		| /*EMPTY*/
			{
				import_qual_t *n = make_node<import_qual_t>();
				n->type = SQ_FDW_IMPORT_SCHEMA_ALL;
				n->table_names = NIL;
				$$ = n;
			}
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE USER MAPPING FOR auth_ident SERVER name [OPTIONS]
 *
 *****************************************************************************/

CreateUserMappingStmt: CREATE USER MAPPING FOR auth_ident SERVER name create_generic_options
				{
					sq_create_user_mapping_stmt_t *n = make_node<sq_create_user_mapping_stmt_t>();

					n->user = $5;
					n->servername = $7;
					n->options = $8;
					n->if_not_exists = false;
					$$ = (sq_node_t *) n;
				}
				| CREATE USER MAPPING IF_P NOT EXISTS FOR auth_ident SERVER name create_generic_options
				{
					sq_create_user_mapping_stmt_t *n = make_node<sq_create_user_mapping_stmt_t>();

					n->user = $8;
					n->servername = $10;
					n->options = $11;
					n->if_not_exists = true;
					$$ = (sq_node_t *) n;
				}
		;

/* User mapping authorization identifier */
auth_ident: RoleSpec			{ $$ = $1; }
			| USER				{ $$ = make_role_spec(SQ_ROLESPEC_CURRENT_USER, @1); }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				DROP USER MAPPING FOR auth_ident SERVER name
 *
 * XXX you'd think this should have a CASCADE/RESTRICT option, even if it's
 * only pro forma; but the SQL standard doesn't show one.
 ****************************************************************************/

DropUserMappingStmt: DROP USER MAPPING FOR auth_ident SERVER name
				{
					sq_drop_user_mapping_stmt_t *n = make_node<sq_drop_user_mapping_stmt_t>();

					n->user = $5;
					n->servername = $7;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
				|  DROP USER MAPPING IF_P EXISTS FOR auth_ident SERVER name
				{
					sq_drop_user_mapping_stmt_t *n = make_node<sq_drop_user_mapping_stmt_t>();

					n->user = $7;
					n->servername = $9;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY :
 *				ALTER USER MAPPING FOR auth_ident SERVER name OPTIONS
 *
 ****************************************************************************/

AlterUserMappingStmt: ALTER USER MAPPING FOR auth_ident SERVER name alter_generic_options
				{
					sq_alter_user_mapping_stmt_t *n = make_node<sq_alter_user_mapping_stmt_t>();

					n->user = $5;
					n->servername = $7;
					n->options = $8;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERIES:
 *				CREATE POLICY name ON table
 *					[AS { PERMISSIVE | RESTRICTIVE } ]
 *					[FOR { SELECT | INSERT | UPDATE | DELETE } ]
 *					[TO role, ...]
 *					[USING (qual)] [WITH CHECK (with check qual)]
 *				ALTER POLICY name ON table [TO role, ...]
 *					[USING (qual)] [WITH CHECK (with check qual)]
 *
 *****************************************************************************/

CreatePolicyStmt:
			CREATE POLICY name ON qualified_name RowSecurityDefaultPermissive
				RowSecurityDefaultForCmd RowSecurityDefaultToRole
				RowSecurityOptionalExpr RowSecurityOptionalWithCheck
				{
					sq_create_policy_stmt_t *n = make_node<sq_create_policy_stmt_t>();

					n->policy_name = $3;
					n->table = $5;
					n->permissive = $6;
					n->cmd_name = $7;
					n->roles = $8;
					n->qual = $9;
					n->with_check = $10;
					$$ = (sq_node_t *) n;
				}
		;

AlterPolicyStmt:
			ALTER POLICY name ON qualified_name RowSecurityOptionalToRole
				RowSecurityOptionalExpr RowSecurityOptionalWithCheck
				{
					sq_alter_policy_stmt_t *n = make_node<sq_alter_policy_stmt_t>();

					n->policy_name = $3;
					n->table = $5;
					n->roles = $6;
					n->qual = $7;
					n->with_check = $8;
					$$ = (sq_node_t *) n;
				}
		;

RowSecurityOptionalExpr:
			USING '(' a_expr ')'	{ $$ = $3; }
			| /* EMPTY */			{ $$ = NULL; }
		;

RowSecurityOptionalWithCheck:
			WITH CHECK '(' a_expr ')'		{ $$ = $4; }
			| /* EMPTY */					{ $$ = NULL; }
		;

RowSecurityDefaultToRole:
			TO role_list			{ $$ = $2; }
			| /* EMPTY */			{ $$ = list_make1(make_role_spec(SQ_ROLESPEC_PUBLIC, -1)); }
		;

RowSecurityOptionalToRole:
			TO role_list			{ $$ = $2; }
			| /* EMPTY */			{ $$ = NULL; }
		;

RowSecurityDefaultPermissive:
			AS IDENT
				{
					if (strcmp($2, "permissive") == 0)
						$$ = true;
					else if (strcmp($2, "restrictive") == 0)
						$$ = false;
					else
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@2), .hint = "Only PERMISSIVE or RESTRICTIVE policies are supported currently."}, "unrecognized row security option \"{}\"", $2);

				}
			| /* EMPTY */			{ $$ = true; }
		;

RowSecurityDefaultForCmd:
			FOR row_security_cmd	{ $$ = $2; }
			| /* EMPTY */			{ $$ = "all"; }
		;

row_security_cmd:
			ALL				{ $$ = "all"; }
		|	SELECT			{ $$ = "select"; }
		|	INSERT			{ $$ = "insert"; }
		|	UPDATE			{ $$ = "update"; }
		|	DELETE_P		{ $$ = "delete"; }
		;

/*****************************************************************************
 *
 *		QUERY:
 *             CREATE ACCESS METHOD name HANDLER handler_name
 *
 *****************************************************************************/

CreateAmStmt: CREATE ACCESS METHOD name TYPE_P am_type HANDLER handler_name
				{
					sq_create_am_stmt_t *n = make_node<sq_create_am_stmt_t>();

					n->amname = $4;
					n->handler_name = $8;
					n->amtype = $6;
					$$ = (sq_node_t *) n;
				}
		;

am_type:
			INDEX			{ $$ = AMTYPE_INDEX; }
		|	TABLE			{ $$ = AMTYPE_TABLE; }
		;

/*****************************************************************************
 *
 *		QUERIES :
 *				CREATE TRIGGER ...
 *
 *****************************************************************************/

CreateTrigStmt:
			CREATE opt_or_replace TRIGGER name TriggerActionTime TriggerEvents ON
			qualified_name TriggerReferencing TriggerForSpec TriggerWhen
			EXECUTE FUNCTION_or_PROCEDURE func_name '(' TriggerFuncArgs ')'
				{
					sq_create_trig_stmt_t *n = make_node<sq_create_trig_stmt_t>();

					n->replace = $2;
					n->isconstraint = false;
					n->trigname = $4;
					n->relation = $8;
					n->funcname = $14;
					n->args = $16;
					n->row = $10;
					n->timing = $5;
					n->events = int_val(linitial($6));
					n->columns = (list_ref_t ) lsecond($6);
					n->when_clause = $11;
					n->transition_rels = $9;
					n->deferrable = false;
					n->initdeferred = false;
					n->constrrel = NULL;
					$$ = (sq_node_t *) n;
				}
		  | CREATE opt_or_replace CONSTRAINT TRIGGER name AFTER TriggerEvents ON
			qualified_name OptConstrFromTable ConstraintAttributeSpec
			FOR EACH ROW TriggerWhen
			EXECUTE FUNCTION_or_PROCEDURE func_name '(' TriggerFuncArgs ')'
				{
					sq_create_trig_stmt_t *n = make_node<sq_create_trig_stmt_t>();

					n->replace = $2;
					if (n->replace) /* not supported, see CreateTrigger */
						raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@1)}, "CREATE OR REPLACE CONSTRAINT TRIGGER is not supported");
					n->isconstraint = true;
					n->trigname = $5;
					n->relation = $9;
					n->funcname = $18;
					n->args = $20;
					n->row = true;
					n->timing = TRIGGER_TYPE_AFTER;
					n->events = int_val(linitial($7));
					n->columns = (list_ref_t ) lsecond($7);
					n->when_clause = $15;
					n->transition_rels = NIL;
					process_ca_sbits($11, @11, "TRIGGER",
								   &n->deferrable, &n->initdeferred, NULL,
								   NULL, NULL, yyscanner);
					n->constrrel = $10;
					$$ = (sq_node_t *) n;
				}
		;

TriggerActionTime:
			BEFORE								{ $$ = TRIGGER_TYPE_BEFORE; }
			| AFTER								{ $$ = TRIGGER_TYPE_AFTER; }
			| INSTEAD OF						{ $$ = TRIGGER_TYPE_INSTEAD; }
		;

TriggerEvents:
			TriggerOneEvent
				{ $$ = $1; }
			| TriggerEvents OR TriggerOneEvent
				{
					int			events1 = int_val(linitial($1));
					int			events2 = int_val(linitial($3));
					list_ref_t	   columns1 = (list_ref_t ) lsecond($1);
					list_ref_t	   columns2 = (list_ref_t ) lsecond($3);

					if (events1 & events2)
						parser_yyerror("duplicate trigger events specified");
					/*
					 * concat'ing the columns lists loses information about
					 * which columns went with which event, but so long as
					 * only UPDATE carries columns and we disallow multiple
					 * UPDATE items, it doesn't matter.  Command execution
					 * should just ignore the columns for non-UPDATE events.
					 */
					$$ = list_make2(make_integer(events1 | events2),
									list_concat(columns1, columns2));
				}
		;

TriggerOneEvent:
			INSERT
				{ $$ = list_make2(make_integer(TRIGGER_TYPE_INSERT), NIL); }
			| DELETE_P
				{ $$ = list_make2(make_integer(TRIGGER_TYPE_DELETE), NIL); }
			| UPDATE
				{ $$ = list_make2(make_integer(TRIGGER_TYPE_UPDATE), NIL); }
			| UPDATE OF columnList
				{ $$ = list_make2(make_integer(TRIGGER_TYPE_UPDATE), $3); }
			| TRUNCATE
				{ $$ = list_make2(make_integer(TRIGGER_TYPE_TRUNCATE), NIL); }
		;

TriggerReferencing:
			REFERENCING TriggerTransitions			{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

TriggerTransitions:
			TriggerTransition						{ $$ = list_make1($1); }
			| TriggerTransitions TriggerTransition	{ $$ = lappend($1, $2); }
		;

TriggerTransition:
			TransitionOldOrNew TransitionRowOrTable opt_as TransitionRelName
				{
					sq_trigger_transition_t *n = make_node<sq_trigger_transition_t>();

					n->name = $4;
					n->is_new = $1;
					n->is_table = $2;
					$$ = (sq_node_t *) n;
				}
		;

TransitionOldOrNew:
			NEW										{ $$ = true; }
			| OLD									{ $$ = false; }
		;

TransitionRowOrTable:
			TABLE									{ $$ = true; }
			/*
			 * According to the standard, lack of a keyword here implies ROW.
			 * Support for that would require prohibiting ROW entirely here,
			 * reserving the keyword ROW, and/or requiring AS (instead of
			 * allowing it to be optional, as the standard specifies) as the
			 * next token.  Requiring ROW seems cleanest and easiest to
			 * explain.
			 */
			| ROW									{ $$ = false; }
		;

TransitionRelName:
			ColId									{ $$ = $1; }
		;

TriggerForSpec:
			FOR TriggerForOptEach TriggerForType
				{
					$$ = $3;
				}
			| /* EMPTY */
				{
					/*
					 * If ROW/STATEMENT not specified, default to
					 * STATEMENT, per SQL
					 */
					$$ = false;
				}
		;

TriggerForOptEach:
			EACH
			| /*EMPTY*/
		;

TriggerForType:
			ROW										{ $$ = true; }
			| STATEMENT								{ $$ = false; }
		;

TriggerWhen:
			WHEN '(' a_expr ')'						{ $$ = $3; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

FUNCTION_or_PROCEDURE:
			FUNCTION
		|	PROCEDURE
		;

TriggerFuncArgs:
			TriggerFuncArg							{ $$ = list_make1($1); }
			| TriggerFuncArgs ',' TriggerFuncArg	{ $$ = lappend($1, $3); }
			| /*EMPTY*/								{ $$ = NIL; }
		;

TriggerFuncArg:
			Iconst
				{
					$$ = (sq_node_t *) make_string(tree_format("{}", $1));
				}
			| FCONST								{ $$ = (sq_node_t *) make_string($1); }
			| Sconst								{ $$ = (sq_node_t *) make_string($1); }
			| ColLabel								{ $$ = (sq_node_t *) make_string($1); }
		;

OptConstrFromTable:
			FROM qualified_name						{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

ConstraintAttributeSpec:
			/*EMPTY*/
				{ $$ = 0; }
			| ConstraintAttributeSpec ConstraintAttributeElem
				{
					/*
					 * We must complain about conflicting options.
					 * We could, but choose not to, complain about redundant
					 * options (ie, where $2's bit is already set in $1).
					 */
					int		newspec = $1 | $2;

					/* special message for this case */
					if ((newspec & (CAS_NOT_DEFERRABLE | CAS_INITIALLY_DEFERRED)) == (CAS_NOT_DEFERRABLE | CAS_INITIALLY_DEFERRED))
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@2)}, "constraint declared INITIALLY DEFERRED must be DEFERRABLE");
					/* generic message for other conflicts */
					if ((newspec & (CAS_NOT_DEFERRABLE | CAS_DEFERRABLE)) == (CAS_NOT_DEFERRABLE | CAS_DEFERRABLE) ||
						(newspec & (CAS_INITIALLY_IMMEDIATE | CAS_INITIALLY_DEFERRED)) == (CAS_INITIALLY_IMMEDIATE | CAS_INITIALLY_DEFERRED) ||
						(newspec & (CAS_NOT_ENFORCED | CAS_ENFORCED)) == (CAS_NOT_ENFORCED | CAS_ENFORCED))
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@2)}, "conflicting constraint properties");
					$$ = newspec;
				}
		;

ConstraintAttributeElem:
			NOT DEFERRABLE					{ $$ = CAS_NOT_DEFERRABLE; }
			| DEFERRABLE					{ $$ = CAS_DEFERRABLE; }
			| INITIALLY IMMEDIATE			{ $$ = CAS_INITIALLY_IMMEDIATE; }
			| INITIALLY DEFERRED			{ $$ = CAS_INITIALLY_DEFERRED; }
			| NOT VALID						{ $$ = CAS_NOT_VALID; }
			| NO INHERIT					{ $$ = CAS_NO_INHERIT; }
			| NOT ENFORCED					{ $$ = CAS_NOT_ENFORCED; }
			| ENFORCED						{ $$ = CAS_ENFORCED; }
		;


/*****************************************************************************
 *
 *		QUERIES :
 *				CREATE EVENT TRIGGER ...
 *				ALTER EVENT TRIGGER ...
 *
 *****************************************************************************/

CreateEventTrigStmt:
			CREATE EVENT TRIGGER name ON ColLabel
			EXECUTE FUNCTION_or_PROCEDURE func_name '(' ')'
				{
					sq_create_event_trig_stmt_t *n = make_node<sq_create_event_trig_stmt_t>();

					n->trigname = $4;
					n->eventname = $6;
					n->whenclause = NIL;
					n->funcname = $9;
					$$ = (sq_node_t *) n;
				}
		  | CREATE EVENT TRIGGER name ON ColLabel
			WHEN event_trigger_when_list
			EXECUTE FUNCTION_or_PROCEDURE func_name '(' ')'
				{
					sq_create_event_trig_stmt_t *n = make_node<sq_create_event_trig_stmt_t>();

					n->trigname = $4;
					n->eventname = $6;
					n->whenclause = $8;
					n->funcname = $11;
					$$ = (sq_node_t *) n;
				}
		;

event_trigger_when_list:
		  event_trigger_when_item
			{ $$ = list_make1($1); }
		| event_trigger_when_list AND event_trigger_when_item
			{ $$ = lappend($1, $3); }
		;

event_trigger_when_item:
		ColId IN_P '(' event_trigger_value_list ')'
			{ $$ = make_def_elem($1, (sq_node_t *) $4, @1); }
		;

event_trigger_value_list:
		  SCONST
			{ $$ = list_make1(make_string($1)); }
		| event_trigger_value_list ',' SCONST
			{ $$ = lappend($1, make_string($3)); }
		;

AlterEventTrigStmt:
			ALTER EVENT TRIGGER name enable_trigger
				{
					sq_alter_event_trig_stmt_t *n = make_node<sq_alter_event_trig_stmt_t>();

					n->trigname = $4;
					n->tgenabled = $5;
					$$ = (sq_node_t *) n;
				}
		;

enable_trigger:
			ENABLE_P					{ $$ = TRIGGER_FIRES_ON_ORIGIN; }
			| ENABLE_P REPLICA			{ $$ = TRIGGER_FIRES_ON_REPLICA; }
			| ENABLE_P ALWAYS			{ $$ = TRIGGER_FIRES_ALWAYS; }
			| DISABLE_P					{ $$ = TRIGGER_DISABLED; }
		;

/*****************************************************************************
 *
 *		QUERY :
 *				CREATE ASSERTION ...
 *
 *****************************************************************************/

CreateAssertionStmt:
			CREATE ASSERTION any_name CHECK '(' a_expr ')' ConstraintAttributeSpec
				{
					raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@1)}, "CREATE ASSERTION is not yet implemented");

					$$ = NULL;
				}
		;


/*****************************************************************************
 *
 *		QUERY :
 *				define (aggregate,operator,type)
 *
 *****************************************************************************/

DefineStmt:
			CREATE opt_or_replace AGGREGATE func_name aggr_args definition
				{
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_AGGREGATE;
					n->oldstyle = false;
					n->replace = $2;
					n->defnames = $4;
					n->args = $5;
					n->definition = $6;
					$$ = (sq_node_t *) n;
				}
			| CREATE opt_or_replace AGGREGATE func_name old_aggr_definition
				{
					/* old-style (pre-8.2) syntax for CREATE AGGREGATE */
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_AGGREGATE;
					n->oldstyle = true;
					n->replace = $2;
					n->defnames = $4;
					n->args = NIL;
					n->definition = $5;
					$$ = (sq_node_t *) n;
				}
			| CREATE OPERATOR any_operator definition
				{
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_OPERATOR;
					n->oldstyle = false;
					n->defnames = $3;
					n->args = NIL;
					n->definition = $4;
					$$ = (sq_node_t *) n;
				}
			| CREATE TYPE_P any_name definition
				{
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_TYPE;
					n->oldstyle = false;
					n->defnames = $3;
					n->args = NIL;
					n->definition = $4;
					$$ = (sq_node_t *) n;
				}
			| CREATE TYPE_P any_name
				{
					/* Shell type (identified by lack of definition) */
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_TYPE;
					n->oldstyle = false;
					n->defnames = $3;
					n->args = NIL;
					n->definition = NIL;
					$$ = (sq_node_t *) n;
				}
			| CREATE TYPE_P any_name AS '(' OptTableFuncElementList ')'
				{
					sq_composite_type_stmt_t *n = make_node<sq_composite_type_stmt_t>();

					/* can't use qualified_name, sigh */
					n->typevar = make_range_var_from_any_name($3, @3, yyscanner);
					n->coldeflist = $6;
					$$ = (sq_node_t *) n;
				}
			| CREATE TYPE_P any_name AS ENUM_P '(' opt_enum_val_list ')'
				{
					sq_create_enum_stmt_t *n = make_node<sq_create_enum_stmt_t>();

					n->type_name = $3;
					n->vals = $7;
					$$ = (sq_node_t *) n;
				}
			| CREATE TYPE_P any_name AS RANGE definition
				{
					sq_create_range_stmt_t *n = make_node<sq_create_range_stmt_t>();

					n->type_name = $3;
					n->params = $6;
					$$ = (sq_node_t *) n;
				}
			| CREATE TEXT_P SEARCH PARSER any_name definition
				{
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_TSPARSER;
					n->args = NIL;
					n->defnames = $5;
					n->definition = $6;
					$$ = (sq_node_t *) n;
				}
			| CREATE TEXT_P SEARCH DICTIONARY any_name definition
				{
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_TSDICTIONARY;
					n->args = NIL;
					n->defnames = $5;
					n->definition = $6;
					$$ = (sq_node_t *) n;
				}
			| CREATE TEXT_P SEARCH TEMPLATE any_name definition
				{
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_TSTEMPLATE;
					n->args = NIL;
					n->defnames = $5;
					n->definition = $6;
					$$ = (sq_node_t *) n;
				}
			| CREATE TEXT_P SEARCH CONFIGURATION any_name definition
				{
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_TSCONFIGURATION;
					n->args = NIL;
					n->defnames = $5;
					n->definition = $6;
					$$ = (sq_node_t *) n;
				}
			| CREATE COLLATION any_name definition
				{
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_COLLATION;
					n->args = NIL;
					n->defnames = $3;
					n->definition = $4;
					$$ = (sq_node_t *) n;
				}
			| CREATE COLLATION IF_P NOT EXISTS any_name definition
				{
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_COLLATION;
					n->args = NIL;
					n->defnames = $6;
					n->definition = $7;
					n->if_not_exists = true;
					$$ = (sq_node_t *) n;
				}
			| CREATE COLLATION any_name FROM any_name
				{
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_COLLATION;
					n->args = NIL;
					n->defnames = $3;
					n->definition = list_make1(make_def_elem("from", (sq_node_t *) $5, @5));
					$$ = (sq_node_t *) n;
				}
			| CREATE COLLATION IF_P NOT EXISTS any_name FROM any_name
				{
					sq_define_stmt_t *n = make_node<sq_define_stmt_t>();

					n->kind = SQ_OBJECT_COLLATION;
					n->args = NIL;
					n->defnames = $6;
					n->definition = list_make1(make_def_elem("from", (sq_node_t *) $8, @8));
					n->if_not_exists = true;
					$$ = (sq_node_t *) n;
				}
		;

definition: '(' def_list ')'						{ $$ = $2; }
		;

def_list:	def_elem								{ $$ = list_make1($1); }
			| def_list ',' def_elem					{ $$ = lappend($1, $3); }
		;

def_elem:	ColLabel '=' def_arg
				{
					$$ = make_def_elem($1, (sq_node_t *) $3, @1);
				}
			| ColLabel
				{
					$$ = make_def_elem($1, NULL, @1);
				}
		;

/* Note: any simple identifier will be returned as a type name! */
def_arg:	func_type						{ $$ = (sq_node_t *) $1; }
			| reserved_keyword				{ $$ = (sq_node_t *) make_string(tree_strdup($1)); }
			| qual_all_Op					{ $$ = (sq_node_t *) $1; }
			| NumericOnly					{ $$ = (sq_node_t *) $1; }
			| Sconst						{ $$ = (sq_node_t *) make_string($1); }
			| NONE							{ $$ = (sq_node_t *) make_string(tree_strdup($1)); }
		;

old_aggr_definition: '(' old_aggr_list ')'			{ $$ = $2; }
		;

old_aggr_list: old_aggr_elem						{ $$ = list_make1($1); }
			| old_aggr_list ',' old_aggr_elem		{ $$ = lappend($1, $3); }
		;

/*
 * Must use IDENT here to avoid reduce/reduce conflicts; fortunately none of
 * the item names needed in old aggregate definitions are likely to become
 * SQL keywords.
 */
old_aggr_elem:  IDENT '=' def_arg
				{
					$$ = make_def_elem($1, (sq_node_t *) $3, @1);
				}
		;

opt_enum_val_list:
		enum_val_list							{ $$ = $1; }
		| /*EMPTY*/								{ $$ = NIL; }
		;

enum_val_list:	Sconst
				{ $$ = list_make1(make_string($1)); }
			| enum_val_list ',' Sconst
				{ $$ = lappend($1, make_string($3)); }
		;

/*****************************************************************************
 *
 *	ALTER TYPE enumtype ADD ...
 *
 *****************************************************************************/

AlterEnumStmt:
		ALTER TYPE_P any_name ADD_P VALUE_P opt_if_not_exists Sconst
			{
				sq_alter_enum_stmt_t *n = make_node<sq_alter_enum_stmt_t>();

				n->type_name = $3;
				n->old_val = NULL;
				n->new_val = $7;
				n->new_val_neighbor = NULL;
				n->new_val_is_after = true;
				n->skip_if_new_val_exists = $6;
				$$ = (sq_node_t *) n;
			}
		 | ALTER TYPE_P any_name ADD_P VALUE_P opt_if_not_exists Sconst BEFORE Sconst
			{
				sq_alter_enum_stmt_t *n = make_node<sq_alter_enum_stmt_t>();

				n->type_name = $3;
				n->old_val = NULL;
				n->new_val = $7;
				n->new_val_neighbor = $9;
				n->new_val_is_after = false;
				n->skip_if_new_val_exists = $6;
				$$ = (sq_node_t *) n;
			}
		 | ALTER TYPE_P any_name ADD_P VALUE_P opt_if_not_exists Sconst AFTER Sconst
			{
				sq_alter_enum_stmt_t *n = make_node<sq_alter_enum_stmt_t>();

				n->type_name = $3;
				n->old_val = NULL;
				n->new_val = $7;
				n->new_val_neighbor = $9;
				n->new_val_is_after = true;
				n->skip_if_new_val_exists = $6;
				$$ = (sq_node_t *) n;
			}
		 | ALTER TYPE_P any_name RENAME VALUE_P Sconst TO Sconst
			{
				sq_alter_enum_stmt_t *n = make_node<sq_alter_enum_stmt_t>();

				n->type_name = $3;
				n->old_val = $6;
				n->new_val = $8;
				n->new_val_neighbor = NULL;
				n->new_val_is_after = false;
				n->skip_if_new_val_exists = false;
				$$ = (sq_node_t *) n;
			}
		 | ALTER TYPE_P any_name DROP VALUE_P Sconst
			{
				/*
				 * The following problems must be solved before this can be
				 * implemented:
				 *
				 * - There must be no instance of the target value in
				 *   any table.
				 *
				 * - The value must not appear in any catalog metadata,
				 *   such as stored view expressions or column defaults.
				 *
				 * - The value must not appear in any non-leaf page of a
				 *   btree (and similar issues with other index types).
				 *   This is problematic because a value could persist
				 *   there long after it's gone from user-visible data.
				 *
				 * - Concurrent sessions must not be able to insert the
				 *   value while the preceding conditions are being checked.
				 *
				 * - Possibly more...
				 */
				raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@4)}, "dropping an enum value is not implemented");
			}
		 ;

opt_if_not_exists: IF_P NOT EXISTS              { $$ = true; }
		| /* EMPTY */                          { $$ = false; }
		;


/*****************************************************************************
 *
 *		QUERIES :
 *				CREATE OPERATOR CLASS ...
 *				CREATE OPERATOR FAMILY ...
 *				ALTER OPERATOR FAMILY ...
 *				DROP OPERATOR CLASS ...
 *				DROP OPERATOR FAMILY ...
 *
 *****************************************************************************/

CreateOpClassStmt:
			CREATE OPERATOR CLASS any_name opt_default FOR TYPE_P Typename
			USING name opt_opfamily AS opclass_item_list
				{
					sq_create_op_class_stmt_t *n = make_node<sq_create_op_class_stmt_t>();

					n->opclassname = $4;
					n->is_default = $5;
					n->datatype = $8;
					n->amname = $10;
					n->opfamilyname = $11;
					n->items = $13;
					$$ = (sq_node_t *) n;
				}
		;

opclass_item_list:
			opclass_item							{ $$ = list_make1($1); }
			| opclass_item_list ',' opclass_item	{ $$ = lappend($1, $3); }
		;

opclass_item:
			OPERATOR Iconst any_operator opclass_purpose
				{
					sq_create_op_class_item_t *n = make_node<sq_create_op_class_item_t>();
					sq_object_with_args_t *owa = make_node<sq_object_with_args_t>();

					owa->objname = $3;
					owa->objargs = NIL;
					n->itemtype = SQ_OPCLASS_ITEM_OPERATOR;
					n->name = owa;
					n->number = $2;
					n->order_family = $4;
					$$ = (sq_node_t *) n;
				}
			| OPERATOR Iconst operator_with_argtypes opclass_purpose
				{
					sq_create_op_class_item_t *n = make_node<sq_create_op_class_item_t>();

					n->itemtype = SQ_OPCLASS_ITEM_OPERATOR;
					n->name = $3;
					n->number = $2;
					n->order_family = $4;
					$$ = (sq_node_t *) n;
				}
			| FUNCTION Iconst function_with_argtypes
				{
					sq_create_op_class_item_t *n = make_node<sq_create_op_class_item_t>();

					n->itemtype = SQ_OPCLASS_ITEM_FUNCTION;
					n->name = $3;
					n->number = $2;
					$$ = (sq_node_t *) n;
				}
			| FUNCTION Iconst '(' type_list ')' function_with_argtypes
				{
					sq_create_op_class_item_t *n = make_node<sq_create_op_class_item_t>();

					n->itemtype = SQ_OPCLASS_ITEM_FUNCTION;
					n->name = $6;
					n->number = $2;
					n->class_args = $4;
					$$ = (sq_node_t *) n;
				}
			| STORAGE Typename
				{
					sq_create_op_class_item_t *n = make_node<sq_create_op_class_item_t>();

					n->itemtype = SQ_OPCLASS_ITEM_STORAGETYPE;
					n->storedtype = $2;
					$$ = (sq_node_t *) n;
				}
		;

opt_default:	DEFAULT						{ $$ = true; }
			| /*EMPTY*/						{ $$ = false; }
		;

opt_opfamily:	FAMILY any_name				{ $$ = $2; }
			| /*EMPTY*/						{ $$ = NIL; }
		;

opclass_purpose: FOR SEARCH					{ $$ = NIL; }
			| FOR ORDER BY any_name			{ $$ = $4; }
			| /*EMPTY*/						{ $$ = NIL; }
		;


CreateOpFamilyStmt:
			CREATE OPERATOR FAMILY any_name USING name
				{
					sq_create_op_family_stmt_t *n = make_node<sq_create_op_family_stmt_t>();

					n->opfamilyname = $4;
					n->amname = $6;
					$$ = (sq_node_t *) n;
				}
		;

AlterOpFamilyStmt:
			ALTER OPERATOR FAMILY any_name USING name ADD_P opclass_item_list
				{
					sq_alter_op_family_stmt_t *n = make_node<sq_alter_op_family_stmt_t>();

					n->opfamilyname = $4;
					n->amname = $6;
					n->is_drop = false;
					n->items = $8;
					$$ = (sq_node_t *) n;
				}
			| ALTER OPERATOR FAMILY any_name USING name DROP opclass_drop_list
				{
					sq_alter_op_family_stmt_t *n = make_node<sq_alter_op_family_stmt_t>();

					n->opfamilyname = $4;
					n->amname = $6;
					n->is_drop = true;
					n->items = $8;
					$$ = (sq_node_t *) n;
				}
		;

opclass_drop_list:
			opclass_drop							{ $$ = list_make1($1); }
			| opclass_drop_list ',' opclass_drop	{ $$ = lappend($1, $3); }
		;

opclass_drop:
			OPERATOR Iconst '(' type_list ')'
				{
					sq_create_op_class_item_t *n = make_node<sq_create_op_class_item_t>();

					n->itemtype = SQ_OPCLASS_ITEM_OPERATOR;
					n->number = $2;
					n->class_args = $4;
					$$ = (sq_node_t *) n;
				}
			| FUNCTION Iconst '(' type_list ')'
				{
					sq_create_op_class_item_t *n = make_node<sq_create_op_class_item_t>();

					n->itemtype = SQ_OPCLASS_ITEM_FUNCTION;
					n->number = $2;
					n->class_args = $4;
					$$ = (sq_node_t *) n;
				}
		;


DropOpClassStmt:
			DROP OPERATOR CLASS any_name USING name opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->objects = list_make1(lcons(make_string($6), $4));
					n->remove_type = SQ_OBJECT_OPCLASS;
					n->behavior = $7;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP OPERATOR CLASS IF_P EXISTS any_name USING name opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->objects = list_make1(lcons(make_string($8), $6));
					n->remove_type = SQ_OBJECT_OPCLASS;
					n->behavior = $9;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
		;

DropOpFamilyStmt:
			DROP OPERATOR FAMILY any_name USING name opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->objects = list_make1(lcons(make_string($6), $4));
					n->remove_type = SQ_OBJECT_OPFAMILY;
					n->behavior = $7;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP OPERATOR FAMILY IF_P EXISTS any_name USING name opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->objects = list_make1(lcons(make_string($8), $6));
					n->remove_type = SQ_OBJECT_OPFAMILY;
					n->behavior = $9;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *
 *		DROP OWNED BY username [, username ...] [ RESTRICT | CASCADE ]
 *		REASSIGN OWNED BY username [, username ...] TO username
 *
 *****************************************************************************/
DropOwnedStmt:
			DROP OWNED BY role_list opt_drop_behavior
				{
					sq_drop_owned_stmt_t *n = make_node<sq_drop_owned_stmt_t>();

					n->roles = $4;
					n->behavior = $5;
					$$ = (sq_node_t *) n;
				}
		;

ReassignOwnedStmt:
			REASSIGN OWNED BY role_list TO RoleSpec
				{
					sq_reassign_owned_stmt_t *n = make_node<sq_reassign_owned_stmt_t>();

					n->roles = $4;
					n->newrole = $6;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *
 *		DROP itemtype [ IF EXISTS ] itemname [, itemname ...]
 *           [ RESTRICT | CASCADE ]
 *
 *****************************************************************************/

DropStmt:	DROP object_type_any_name IF_P EXISTS any_name_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = $2;
					n->missing_ok = true;
					n->objects = $5;
					n->behavior = $6;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP object_type_any_name any_name_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = $2;
					n->missing_ok = false;
					n->objects = $3;
					n->behavior = $4;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP drop_type_name IF_P EXISTS name_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = $2;
					n->missing_ok = true;
					n->objects = $5;
					n->behavior = $6;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP drop_type_name name_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = $2;
					n->missing_ok = false;
					n->objects = $3;
					n->behavior = $4;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP object_type_name_on_any_name name ON any_name opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = $2;
					n->objects = list_make1(lappend($5, make_string($3)));
					n->behavior = $6;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP object_type_name_on_any_name IF_P EXISTS name ON any_name opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = $2;
					n->objects = list_make1(lappend($7, make_string($5)));
					n->behavior = $8;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP TYPE_P type_name_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_TYPE;
					n->missing_ok = false;
					n->objects = $3;
					n->behavior = $4;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP TYPE_P IF_P EXISTS type_name_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_TYPE;
					n->missing_ok = true;
					n->objects = $5;
					n->behavior = $6;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP DOMAIN_P type_name_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_DOMAIN;
					n->missing_ok = false;
					n->objects = $3;
					n->behavior = $4;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP DOMAIN_P IF_P EXISTS type_name_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_DOMAIN;
					n->missing_ok = true;
					n->objects = $5;
					n->behavior = $6;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP INDEX CONCURRENTLY any_name_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_INDEX;
					n->missing_ok = false;
					n->objects = $4;
					n->behavior = $5;
					n->concurrent = true;
					$$ = (sq_node_t *) n;
				}
			| DROP INDEX CONCURRENTLY IF_P EXISTS any_name_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_INDEX;
					n->missing_ok = true;
					n->objects = $6;
					n->behavior = $7;
					n->concurrent = true;
					$$ = (sq_node_t *) n;
				}
		;

/* object types taking any_name/any_name_list */
object_type_any_name:
			TABLE									{ $$ = SQ_OBJECT_TABLE; }
			| SEQUENCE								{ $$ = SQ_OBJECT_SEQUENCE; }
			| VIEW									{ $$ = SQ_OBJECT_VIEW; }
			| MATERIALIZED VIEW						{ $$ = SQ_OBJECT_MATVIEW; }
			| INDEX									{ $$ = SQ_OBJECT_INDEX; }
			| FOREIGN TABLE							{ $$ = SQ_OBJECT_FOREIGN_TABLE; }
			| COLLATION								{ $$ = SQ_OBJECT_COLLATION; }
			| CONVERSION_P							{ $$ = SQ_OBJECT_CONVERSION; }
			| STATISTICS							{ $$ = SQ_OBJECT_STATISTIC_EXT; }
			| TEXT_P SEARCH PARSER					{ $$ = SQ_OBJECT_TSPARSER; }
			| TEXT_P SEARCH DICTIONARY				{ $$ = SQ_OBJECT_TSDICTIONARY; }
			| TEXT_P SEARCH TEMPLATE				{ $$ = SQ_OBJECT_TSTEMPLATE; }
			| TEXT_P SEARCH CONFIGURATION			{ $$ = SQ_OBJECT_TSCONFIGURATION; }
		;

/*
 * object types taking name/name_list
 *
 * DROP handles some of them separately
 */

object_type_name:
			drop_type_name							{ $$ = $1; }
			| DATABASE								{ $$ = SQ_OBJECT_DATABASE; }
			| ROLE									{ $$ = SQ_OBJECT_ROLE; }
			| SUBSCRIPTION							{ $$ = SQ_OBJECT_SUBSCRIPTION; }
			| TABLESPACE							{ $$ = SQ_OBJECT_TABLESPACE; }
		;

drop_type_name:
			ACCESS METHOD							{ $$ = SQ_OBJECT_ACCESS_METHOD; }
			| EVENT TRIGGER							{ $$ = SQ_OBJECT_EVENT_TRIGGER; }
			| EXTENSION								{ $$ = SQ_OBJECT_EXTENSION; }
			| FOREIGN DATA_P WRAPPER				{ $$ = SQ_OBJECT_FDW; }
			| opt_procedural LANGUAGE				{ $$ = SQ_OBJECT_LANGUAGE; }
			| PUBLICATION							{ $$ = SQ_OBJECT_PUBLICATION; }
			| SCHEMA								{ $$ = SQ_OBJECT_SCHEMA; }
			| SERVER								{ $$ = SQ_OBJECT_FOREIGN_SERVER; }
		;

/* object types attached to a table */
object_type_name_on_any_name:
			POLICY									{ $$ = SQ_OBJECT_POLICY; }
			| RULE									{ $$ = SQ_OBJECT_RULE; }
			| TRIGGER								{ $$ = SQ_OBJECT_TRIGGER; }
		;

any_name_list:
			any_name								{ $$ = list_make1($1); }
			| any_name_list ',' any_name			{ $$ = lappend($1, $3); }
		;

any_name:	ColId						{ $$ = list_make1(make_string($1)); }
			| ColId attrs				{ $$ = lcons(make_string($1), $2); }
		;

attrs:		'.' attr_name
					{ $$ = list_make1(make_string($2)); }
			| attrs '.' attr_name
					{ $$ = lappend($1, make_string($3)); }
		;

type_name_list:
			Typename								{ $$ = list_make1($1); }
			| type_name_list ',' Typename			{ $$ = lappend($1, $3); }
		;

/*****************************************************************************
 *
 *		QUERY:
 *				truncate table relname1, relname2, ...
 *
 *****************************************************************************/

TruncateStmt:
			TRUNCATE opt_table relation_expr_list opt_restart_seqs opt_drop_behavior
				{
					sq_truncate_stmt_t *n = make_node<sq_truncate_stmt_t>();

					n->relations = $3;
					n->restart_seqs = $4;
					n->behavior = $5;
					$$ = (sq_node_t *) n;
				}
		;

opt_restart_seqs:
			CONTINUE_P IDENTITY_P		{ $$ = false; }
			| RESTART IDENTITY_P		{ $$ = true; }
			| /* EMPTY */				{ $$ = false; }
		;

/*****************************************************************************
 *
 * COMMENT ON <object> IS <text>
 *
 *****************************************************************************/

CommentStmt:
			COMMENT ON object_type_any_name any_name IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = $3;
					n->object = (sq_node_t *) $4;
					n->comment = $6;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON COLUMN any_name IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_COLUMN;
					n->object = (sq_node_t *) $4;
					n->comment = $6;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON object_type_name name IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = $3;
					n->object = (sq_node_t *) make_string($4);
					n->comment = $6;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON TYPE_P Typename IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_TYPE;
					n->object = (sq_node_t *) $4;
					n->comment = $6;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON DOMAIN_P Typename IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_DOMAIN;
					n->object = (sq_node_t *) $4;
					n->comment = $6;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON AGGREGATE aggregate_with_argtypes IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_AGGREGATE;
					n->object = (sq_node_t *) $4;
					n->comment = $6;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON FUNCTION function_with_argtypes IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_FUNCTION;
					n->object = (sq_node_t *) $4;
					n->comment = $6;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON OPERATOR operator_with_argtypes IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_OPERATOR;
					n->object = (sq_node_t *) $4;
					n->comment = $6;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON CONSTRAINT name ON any_name IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_TABCONSTRAINT;
					n->object = (sq_node_t *) lappend($6, make_string($4));
					n->comment = $8;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON CONSTRAINT name ON DOMAIN_P any_name IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_DOMCONSTRAINT;
					/*
					 * should use Typename not any_name in the production, but
					 * there's a shift/reduce conflict if we do that, so fix it
					 * up here.
					 */
					n->object = (sq_node_t *) list_make2(make_type_name_from_name_list($7), make_string($4));
					n->comment = $9;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON object_type_name_on_any_name name ON any_name IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = $3;
					n->object = (sq_node_t *) lappend($6, make_string($4));
					n->comment = $8;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON PROCEDURE function_with_argtypes IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_PROCEDURE;
					n->object = (sq_node_t *) $4;
					n->comment = $6;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON ROUTINE function_with_argtypes IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_ROUTINE;
					n->object = (sq_node_t *) $4;
					n->comment = $6;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON TRANSFORM FOR Typename LANGUAGE name IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_TRANSFORM;
					n->object = (sq_node_t *) list_make2($5, make_string($7));
					n->comment = $9;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON OPERATOR CLASS any_name USING name IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_OPCLASS;
					n->object = (sq_node_t *) lcons(make_string($7), $5);
					n->comment = $9;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON OPERATOR FAMILY any_name USING name IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_OPFAMILY;
					n->object = (sq_node_t *) lcons(make_string($7), $5);
					n->comment = $9;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON LARGE_P OBJECT_P NumericOnly IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_LARGEOBJECT;
					n->object = (sq_node_t *) $5;
					n->comment = $7;
					$$ = (sq_node_t *) n;
				}
			| COMMENT ON CAST '(' Typename AS Typename ')' IS comment_text
				{
					sq_comment_stmt_t *n = make_node<sq_comment_stmt_t>();

					n->objtype = SQ_OBJECT_CAST;
					n->object = (sq_node_t *) list_make2($5, $7);
					n->comment = $10;
					$$ = (sq_node_t *) n;
				}
		;

comment_text:
			Sconst								{ $$ = $1; }
			| NULL_P							{ $$ = NULL; }
		;


/*****************************************************************************
 *
 *  SECURITY LABEL [FOR <provider>] ON <object> IS <label>
 *
 *  As with COMMENT ON, <object> can refer to various types of database
 *  objects (e.g. TABLE, COLUMN, etc.).
 *
 *****************************************************************************/

SecLabelStmt:
			SECURITY LABEL opt_provider ON object_type_any_name any_name
			IS security_label
				{
					sq_sec_label_stmt_t *n = make_node<sq_sec_label_stmt_t>();

					n->provider = $3;
					n->objtype = $5;
					n->object = (sq_node_t *) $6;
					n->label = $8;
					$$ = (sq_node_t *) n;
				}
			| SECURITY LABEL opt_provider ON COLUMN any_name
			  IS security_label
				{
					sq_sec_label_stmt_t *n = make_node<sq_sec_label_stmt_t>();

					n->provider = $3;
					n->objtype = SQ_OBJECT_COLUMN;
					n->object = (sq_node_t *) $6;
					n->label = $8;
					$$ = (sq_node_t *) n;
				}
			| SECURITY LABEL opt_provider ON object_type_name name
			  IS security_label
				{
					sq_sec_label_stmt_t *n = make_node<sq_sec_label_stmt_t>();

					n->provider = $3;
					n->objtype = $5;
					n->object = (sq_node_t *) make_string($6);
					n->label = $8;
					$$ = (sq_node_t *) n;
				}
			| SECURITY LABEL opt_provider ON TYPE_P Typename
			  IS security_label
				{
					sq_sec_label_stmt_t *n = make_node<sq_sec_label_stmt_t>();

					n->provider = $3;
					n->objtype = SQ_OBJECT_TYPE;
					n->object = (sq_node_t *) $6;
					n->label = $8;
					$$ = (sq_node_t *) n;
				}
			| SECURITY LABEL opt_provider ON DOMAIN_P Typename
			  IS security_label
				{
					sq_sec_label_stmt_t *n = make_node<sq_sec_label_stmt_t>();

					n->provider = $3;
					n->objtype = SQ_OBJECT_DOMAIN;
					n->object = (sq_node_t *) $6;
					n->label = $8;
					$$ = (sq_node_t *) n;
				}
			| SECURITY LABEL opt_provider ON AGGREGATE aggregate_with_argtypes
			  IS security_label
				{
					sq_sec_label_stmt_t *n = make_node<sq_sec_label_stmt_t>();

					n->provider = $3;
					n->objtype = SQ_OBJECT_AGGREGATE;
					n->object = (sq_node_t *) $6;
					n->label = $8;
					$$ = (sq_node_t *) n;
				}
			| SECURITY LABEL opt_provider ON FUNCTION function_with_argtypes
			  IS security_label
				{
					sq_sec_label_stmt_t *n = make_node<sq_sec_label_stmt_t>();

					n->provider = $3;
					n->objtype = SQ_OBJECT_FUNCTION;
					n->object = (sq_node_t *) $6;
					n->label = $8;
					$$ = (sq_node_t *) n;
				}
			| SECURITY LABEL opt_provider ON LARGE_P OBJECT_P NumericOnly
			  IS security_label
				{
					sq_sec_label_stmt_t *n = make_node<sq_sec_label_stmt_t>();

					n->provider = $3;
					n->objtype = SQ_OBJECT_LARGEOBJECT;
					n->object = (sq_node_t *) $7;
					n->label = $9;
					$$ = (sq_node_t *) n;
				}
			| SECURITY LABEL opt_provider ON PROCEDURE function_with_argtypes
			  IS security_label
				{
					sq_sec_label_stmt_t *n = make_node<sq_sec_label_stmt_t>();

					n->provider = $3;
					n->objtype = SQ_OBJECT_PROCEDURE;
					n->object = (sq_node_t *) $6;
					n->label = $8;
					$$ = (sq_node_t *) n;
				}
			| SECURITY LABEL opt_provider ON ROUTINE function_with_argtypes
			  IS security_label
				{
					sq_sec_label_stmt_t *n = make_node<sq_sec_label_stmt_t>();

					n->provider = $3;
					n->objtype = SQ_OBJECT_ROUTINE;
					n->object = (sq_node_t *) $6;
					n->label = $8;
					$$ = (sq_node_t *) n;
				}
		;

opt_provider:	FOR NonReservedWord_or_Sconst	{ $$ = $2; }
				| /* EMPTY */					{ $$ = NULL; }
		;

security_label:	Sconst				{ $$ = $1; }
				| NULL_P			{ $$ = NULL; }
		;

/*****************************************************************************
 *
 *		QUERY:
 *			fetch/move
 *
 *****************************************************************************/

FetchStmt:	FETCH fetch_args
				{
					sq_fetch_stmt_t *n = (sq_fetch_stmt_t *) $2;

					n->ismove = false;
					$$ = (sq_node_t *) n;
				}
			| MOVE fetch_args
				{
					sq_fetch_stmt_t *n = (sq_fetch_stmt_t *) $2;

					n->ismove = true;
					$$ = (sq_node_t *) n;
				}
		;

fetch_args:	cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $1;
					n->direction = SQ_FETCH_FORWARD;
					n->how_many = 1;
					$$ = (sq_node_t *) n;
				}
			| from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $2;
					n->direction = SQ_FETCH_FORWARD;
					n->how_many = 1;
					$$ = (sq_node_t *) n;
				}
			| NEXT opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $3;
					n->direction = SQ_FETCH_FORWARD;
					n->how_many = 1;
					$$ = (sq_node_t *) n;
				}
			| PRIOR opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $3;
					n->direction = SQ_FETCH_BACKWARD;
					n->how_many = 1;
					$$ = (sq_node_t *) n;
				}
			| FIRST_P opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $3;
					n->direction = SQ_FETCH_ABSOLUTE;
					n->how_many = 1;
					$$ = (sq_node_t *) n;
				}
			| LAST_P opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $3;
					n->direction = SQ_FETCH_ABSOLUTE;
					n->how_many = -1;
					$$ = (sq_node_t *) n;
				}
			| ABSOLUTE_P SignedIconst opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $4;
					n->direction = SQ_FETCH_ABSOLUTE;
					n->how_many = $2;
					$$ = (sq_node_t *) n;
				}
			| RELATIVE_P SignedIconst opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $4;
					n->direction = SQ_FETCH_RELATIVE;
					n->how_many = $2;
					$$ = (sq_node_t *) n;
				}
			| SignedIconst opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $3;
					n->direction = SQ_FETCH_FORWARD;
					n->how_many = $1;
					$$ = (sq_node_t *) n;
				}
			| ALL opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $3;
					n->direction = SQ_FETCH_FORWARD;
					n->how_many = SQ_FETCH_ALL;
					$$ = (sq_node_t *) n;
				}
			| FORWARD opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $3;
					n->direction = SQ_FETCH_FORWARD;
					n->how_many = 1;
					$$ = (sq_node_t *) n;
				}
			| FORWARD SignedIconst opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $4;
					n->direction = SQ_FETCH_FORWARD;
					n->how_many = $2;
					$$ = (sq_node_t *) n;
				}
			| FORWARD ALL opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $4;
					n->direction = SQ_FETCH_FORWARD;
					n->how_many = SQ_FETCH_ALL;
					$$ = (sq_node_t *) n;
				}
			| BACKWARD opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $3;
					n->direction = SQ_FETCH_BACKWARD;
					n->how_many = 1;
					$$ = (sq_node_t *) n;
				}
			| BACKWARD SignedIconst opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $4;
					n->direction = SQ_FETCH_BACKWARD;
					n->how_many = $2;
					$$ = (sq_node_t *) n;
				}
			| BACKWARD ALL opt_from_in cursor_name
				{
					sq_fetch_stmt_t *n = make_node<sq_fetch_stmt_t>();

					n->portalname = $4;
					n->direction = SQ_FETCH_BACKWARD;
					n->how_many = SQ_FETCH_ALL;
					$$ = (sq_node_t *) n;
				}
		;

from_in:	FROM
			| IN_P
		;

opt_from_in:	from_in
			| /* EMPTY */
		;


/*****************************************************************************
 *
 * GRANT and REVOKE statements
 *
 *****************************************************************************/

GrantStmt:	GRANT privileges ON privilege_target TO grantee_list
			opt_grant_grant_option opt_granted_by
				{
					sq_grant_stmt_t *n = make_node<sq_grant_stmt_t>();

					n->is_grant = true;
					n->privileges = $2;
					n->targtype = ($4)->targtype;
					n->objtype = ($4)->objtype;
					n->objects = ($4)->objs;
					n->grantees = $6;
					n->grant_option = $7;
					n->grantor = $8;
					$$ = (sq_node_t *) n;
				}
		;

RevokeStmt:
			REVOKE privileges ON privilege_target
			FROM grantee_list opt_granted_by opt_drop_behavior
				{
					sq_grant_stmt_t *n = make_node<sq_grant_stmt_t>();

					n->is_grant = false;
					n->grant_option = false;
					n->privileges = $2;
					n->targtype = ($4)->targtype;
					n->objtype = ($4)->objtype;
					n->objects = ($4)->objs;
					n->grantees = $6;
					n->grantor = $7;
					n->behavior = $8;
					$$ = (sq_node_t *) n;
				}
			| REVOKE GRANT OPTION FOR privileges ON privilege_target
			FROM grantee_list opt_granted_by opt_drop_behavior
				{
					sq_grant_stmt_t *n = make_node<sq_grant_stmt_t>();

					n->is_grant = false;
					n->grant_option = true;
					n->privileges = $5;
					n->targtype = ($7)->targtype;
					n->objtype = ($7)->objtype;
					n->objects = ($7)->objs;
					n->grantees = $9;
					n->grantor = $10;
					n->behavior = $11;
					$$ = (sq_node_t *) n;
				}
		;


/*
 * Privilege names are represented as strings; the validity of the privilege
 * names gets checked at execution.  This is a bit annoying but we have little
 * choice because of the syntactic conflict with lists of role names in
 * GRANT/REVOKE.  What's more, we have to call out in the "privilege"
 * production any reserved keywords that need to be usable as privilege names.
 */

/* either ALL [PRIVILEGES] or a list of individual privileges */
privileges: privilege_list
				{ $$ = $1; }
			| ALL
				{ $$ = NIL; }
			| ALL PRIVILEGES
				{ $$ = NIL; }
			| ALL '(' columnList ')'
				{
					sq_access_priv_t *n = make_node<sq_access_priv_t>();

					n->priv_name = NULL;
					n->cols = $3;
					$$ = list_make1(n);
				}
			| ALL PRIVILEGES '(' columnList ')'
				{
					sq_access_priv_t *n = make_node<sq_access_priv_t>();

					n->priv_name = NULL;
					n->cols = $4;
					$$ = list_make1(n);
				}
		;

privilege_list:	privilege							{ $$ = list_make1($1); }
			| privilege_list ',' privilege			{ $$ = lappend($1, $3); }
		;

privilege:	SELECT opt_column_list
			{
				sq_access_priv_t *n = make_node<sq_access_priv_t>();

				n->priv_name = tree_strdup($1);
				n->cols = $2;
				$$ = n;
			}
		| REFERENCES opt_column_list
			{
				sq_access_priv_t *n = make_node<sq_access_priv_t>();

				n->priv_name = tree_strdup($1);
				n->cols = $2;
				$$ = n;
			}
		| CREATE opt_column_list
			{
				sq_access_priv_t *n = make_node<sq_access_priv_t>();

				n->priv_name = tree_strdup($1);
				n->cols = $2;
				$$ = n;
			}
		| ALTER SYSTEM_P
			{
				sq_access_priv_t *n = make_node<sq_access_priv_t>();
				n->priv_name = tree_strdup("alter system");
				n->cols = NIL;
				$$ = n;
			}
		| ColId opt_column_list
			{
				sq_access_priv_t *n = make_node<sq_access_priv_t>();

				n->priv_name = $1;
				n->cols = $2;
				$$ = n;
			}
		;

parameter_name_list:
		parameter_name
			{
				$$ = list_make1(make_string($1));
			}
		| parameter_name_list ',' parameter_name
			{
				$$ = lappend($1, make_string($3));
			}
		;

parameter_name:
		ColId
			{
				$$ = $1;
			}
		| parameter_name '.' ColId
			{
				$$ = tree_format("{}.{}", $1, $3);
			}
		;


/* Don't bother trying to fold the first two rules into one using
 * opt_table.  You're going to get conflicts.
 */
privilege_target:
			qualified_name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_TABLE;
					n->objs = $1;
					$$ = n;
				}
			| TABLE qualified_name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_TABLE;
					n->objs = $2;
					$$ = n;
				}
			| SEQUENCE qualified_name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_SEQUENCE;
					n->objs = $2;
					$$ = n;
				}
			| FOREIGN DATA_P WRAPPER name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_FDW;
					n->objs = $4;
					$$ = n;
				}
			| FOREIGN SERVER name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_FOREIGN_SERVER;
					n->objs = $3;
					$$ = n;
				}
			| FUNCTION function_with_argtypes_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_FUNCTION;
					n->objs = $2;
					$$ = n;
				}
			| PROCEDURE function_with_argtypes_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_PROCEDURE;
					n->objs = $2;
					$$ = n;
				}
			| ROUTINE function_with_argtypes_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_ROUTINE;
					n->objs = $2;
					$$ = n;
				}
			| DATABASE name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_DATABASE;
					n->objs = $2;
					$$ = n;
				}
			| DOMAIN_P any_name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_DOMAIN;
					n->objs = $2;
					$$ = n;
				}
			| LANGUAGE name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_LANGUAGE;
					n->objs = $2;
					$$ = n;
				}
			| LARGE_P OBJECT_P NumericOnly_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_LARGEOBJECT;
					n->objs = $3;
					$$ = n;
				}
			| PARAMETER parameter_name_list
				{
					priv_target_t *n = make_node<priv_target_t>();
					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_PARAMETER_ACL;
					n->objs = $2;
					$$ = n;
				}
			| SCHEMA name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_SCHEMA;
					n->objs = $2;
					$$ = n;
				}
			| TABLESPACE name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_TABLESPACE;
					n->objs = $2;
					$$ = n;
				}
			| TYPE_P any_name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_OBJECT;
					n->objtype = SQ_OBJECT_TYPE;
					n->objs = $2;
					$$ = n;
				}
			| ALL TABLES IN_P SCHEMA name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_ALL_IN_SCHEMA;
					n->objtype = SQ_OBJECT_TABLE;
					n->objs = $5;
					$$ = n;
				}
			| ALL SEQUENCES IN_P SCHEMA name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_ALL_IN_SCHEMA;
					n->objtype = SQ_OBJECT_SEQUENCE;
					n->objs = $5;
					$$ = n;
				}
			| ALL FUNCTIONS IN_P SCHEMA name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_ALL_IN_SCHEMA;
					n->objtype = SQ_OBJECT_FUNCTION;
					n->objs = $5;
					$$ = n;
				}
			| ALL PROCEDURES IN_P SCHEMA name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_ALL_IN_SCHEMA;
					n->objtype = SQ_OBJECT_PROCEDURE;
					n->objs = $5;
					$$ = n;
				}
			| ALL ROUTINES IN_P SCHEMA name_list
				{
					priv_target_t *n = make_node<priv_target_t>();

					n->targtype = SQ_ACL_TARGET_ALL_IN_SCHEMA;
					n->objtype = SQ_OBJECT_ROUTINE;
					n->objs = $5;
					$$ = n;
				}
		;


grantee_list:
			grantee									{ $$ = list_make1($1); }
			| grantee_list ',' grantee				{ $$ = lappend($1, $3); }
		;

grantee:
			RoleSpec								{ $$ = $1; }
			| GROUP_P RoleSpec						{ $$ = $2; }
		;


opt_grant_grant_option:
			WITH GRANT OPTION { $$ = true; }
			| /*EMPTY*/ { $$ = false; }
		;

/*****************************************************************************
 *
 * GRANT and REVOKE ROLE statements
 *
 *****************************************************************************/

GrantRoleStmt:
			GRANT privilege_list TO role_list opt_granted_by
				{
					sq_grant_role_stmt_t *n = make_node<sq_grant_role_stmt_t>();

					n->is_grant = true;
					n->granted_roles = $2;
					n->grantee_roles = $4;
					n->opt = NIL;
					n->grantor = $5;
					$$ = (sq_node_t *) n;
				}
		  | GRANT privilege_list TO role_list WITH grant_role_opt_list opt_granted_by
				{
					sq_grant_role_stmt_t *n = make_node<sq_grant_role_stmt_t>();

					n->is_grant = true;
					n->granted_roles = $2;
					n->grantee_roles = $4;
					n->opt = $6;
					n->grantor = $7;
					$$ = (sq_node_t *) n;
				}
		;

RevokeRoleStmt:
			REVOKE privilege_list FROM role_list opt_granted_by opt_drop_behavior
				{
					sq_grant_role_stmt_t *n = make_node<sq_grant_role_stmt_t>();

					n->is_grant = false;
					n->opt = NIL;
					n->granted_roles = $2;
					n->grantee_roles = $4;
					n->grantor = $5;
					n->behavior = $6;
					$$ = (sq_node_t *) n;
				}
			| REVOKE ColId OPTION FOR privilege_list FROM role_list opt_granted_by opt_drop_behavior
				{
					sq_grant_role_stmt_t *n = make_node<sq_grant_role_stmt_t>();
					sq_def_elem_t *opt;

					opt = make_def_elem(tree_strdup($2),
									  (sq_node_t *) make_boolean(false), @2);
					n->is_grant = false;
					n->opt = list_make1(opt);
					n->granted_roles = $5;
					n->grantee_roles = $7;
					n->grantor = $8;
					n->behavior = $9;
					$$ = (sq_node_t *) n;
				}
		;

grant_role_opt_list:
			grant_role_opt_list ',' grant_role_opt	{ $$ = lappend($1, $3); }
			| grant_role_opt						{ $$ = list_make1($1); }
		;

grant_role_opt:
		ColLabel grant_role_opt_value
			{
				$$ = make_def_elem(tree_strdup($1), $2, @1);
			}
		;

grant_role_opt_value:
		OPTION			{ $$ = (sq_node_t *) make_boolean(true); }
		| TRUE_P		{ $$ = (sq_node_t *) make_boolean(true); }
		| FALSE_P		{ $$ = (sq_node_t *) make_boolean(false); }
		;

opt_granted_by: GRANTED BY RoleSpec						{ $$ = $3; }
			| /*EMPTY*/									{ $$ = NULL; }
		;

/*****************************************************************************
 *
 * ALTER DEFAULT PRIVILEGES statement
 *
 *****************************************************************************/

AlterDefaultPrivilegesStmt:
			ALTER DEFAULT PRIVILEGES DefACLOptionList DefACLAction
				{
					sq_alter_default_privileges_stmt_t *n = make_node<sq_alter_default_privileges_stmt_t>();

					n->options = $4;
					n->action = (sq_grant_stmt_t *) $5;
					$$ = (sq_node_t *) n;
				}
		;

DefACLOptionList:
			DefACLOptionList DefACLOption			{ $$ = lappend($1, $2); }
			| /* EMPTY */							{ $$ = NIL; }
		;

DefACLOption:
			IN_P SCHEMA name_list
				{
					$$ = make_def_elem("schemas", (sq_node_t *) $3, @1);
				}
			| FOR ROLE role_list
				{
					$$ = make_def_elem("roles", (sq_node_t *) $3, @1);
				}
			| FOR USER role_list
				{
					$$ = make_def_elem("roles", (sq_node_t *) $3, @1);
				}
		;

/*
 * This should match GRANT/REVOKE, except that individual target objects
 * are not mentioned and we only allow a subset of object types.
 */
DefACLAction:
			GRANT privileges ON defacl_privilege_target TO grantee_list
			opt_grant_grant_option
				{
					sq_grant_stmt_t *n = make_node<sq_grant_stmt_t>();

					n->is_grant = true;
					n->privileges = $2;
					n->targtype = SQ_ACL_TARGET_DEFAULTS;
					n->objtype = (sq_object_type_t) $4;
					n->objects = NIL;
					n->grantees = $6;
					n->grant_option = $7;
					$$ = (sq_node_t *) n;
				}
			| REVOKE privileges ON defacl_privilege_target
			FROM grantee_list opt_drop_behavior
				{
					sq_grant_stmt_t *n = make_node<sq_grant_stmt_t>();

					n->is_grant = false;
					n->grant_option = false;
					n->privileges = $2;
					n->targtype = SQ_ACL_TARGET_DEFAULTS;
					n->objtype = (sq_object_type_t) $4;
					n->objects = NIL;
					n->grantees = $6;
					n->behavior = $7;
					$$ = (sq_node_t *) n;
				}
			| REVOKE GRANT OPTION FOR privileges ON defacl_privilege_target
			FROM grantee_list opt_drop_behavior
				{
					sq_grant_stmt_t *n = make_node<sq_grant_stmt_t>();

					n->is_grant = false;
					n->grant_option = true;
					n->privileges = $5;
					n->targtype = SQ_ACL_TARGET_DEFAULTS;
					n->objtype = (sq_object_type_t) $7;
					n->objects = NIL;
					n->grantees = $9;
					n->behavior = $10;
					$$ = (sq_node_t *) n;
				}
		;

defacl_privilege_target:
			TABLES			{ $$ = SQ_OBJECT_TABLE; }
			| FUNCTIONS		{ $$ = SQ_OBJECT_FUNCTION; }
			| ROUTINES		{ $$ = SQ_OBJECT_FUNCTION; }
			| SEQUENCES		{ $$ = SQ_OBJECT_SEQUENCE; }
			| TYPES_P		{ $$ = SQ_OBJECT_TYPE; }
			| SCHEMAS		{ $$ = SQ_OBJECT_SCHEMA; }
			| LARGE_P OBJECTS_P	{ $$ = SQ_OBJECT_LARGEOBJECT; }
		;


/*****************************************************************************
 *
 *		QUERY: CREATE INDEX
 *
 * Note: we cannot put TABLESPACE clause after WHERE clause unless we are
 * willing to make TABLESPACE a fully reserved word.
 *****************************************************************************/

IndexStmt:	CREATE opt_unique INDEX opt_concurrently opt_single_name
			ON relation_expr access_method_clause '(' index_params ')'
			opt_include opt_unique_null_treatment opt_reloptions OptTableSpace where_clause
				{
					sq_index_stmt_t *n = make_node<sq_index_stmt_t>();

					n->unique = $2;
					n->concurrent = $4;
					n->idxname = $5;
					n->relation = $7;
					n->access_method = $8;
					n->index_params = $10;
					n->index_including_params = $12;
					n->nulls_not_distinct = !$13;
					n->options = $14;
					n->table_space = $15;
					n->where_clause = $16;
					n->exclude_op_names = NIL;
					n->idxcomment = NULL;
					n->index_oid = invalid_oid;
					n->old_number = invalid_rel_file_number;
					n->old_create_subid = invalid_sub_transaction_id;
					n->old_first_relfilelocator_subid = invalid_sub_transaction_id;
					n->primary = false;
					n->isconstraint = false;
					n->deferrable = false;
					n->initdeferred = false;
					n->transformed = false;
					n->if_not_exists = false;
					n->reset_default_tblspc = false;
					$$ = (sq_node_t *) n;
				}
			| CREATE opt_unique INDEX opt_concurrently IF_P NOT EXISTS name
			ON relation_expr access_method_clause '(' index_params ')'
			opt_include opt_unique_null_treatment opt_reloptions OptTableSpace where_clause
				{
					sq_index_stmt_t *n = make_node<sq_index_stmt_t>();

					n->unique = $2;
					n->concurrent = $4;
					n->idxname = $8;
					n->relation = $10;
					n->access_method = $11;
					n->index_params = $13;
					n->index_including_params = $15;
					n->nulls_not_distinct = !$16;
					n->options = $17;
					n->table_space = $18;
					n->where_clause = $19;
					n->exclude_op_names = NIL;
					n->idxcomment = NULL;
					n->index_oid = invalid_oid;
					n->old_number = invalid_rel_file_number;
					n->old_create_subid = invalid_sub_transaction_id;
					n->old_first_relfilelocator_subid = invalid_sub_transaction_id;
					n->primary = false;
					n->isconstraint = false;
					n->deferrable = false;
					n->initdeferred = false;
					n->transformed = false;
					n->if_not_exists = true;
					n->reset_default_tblspc = false;
					$$ = (sq_node_t *) n;
				}
		;

opt_unique:
			UNIQUE									{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
		;

access_method_clause:
			USING name								{ $$ = $2; }
			| /*EMPTY*/								{ $$ = tree_strdup(DEFAULT_INDEX_TYPE); }
		;

index_params:	index_elem							{ $$ = list_make1($1); }
			| index_params ',' index_elem			{ $$ = lappend($1, $3); }
		;


index_elem_options:
	opt_collate opt_qualified_name opt_asc_desc opt_nulls_order
		{
			$$ = make_node<sq_index_elem_t>();
			$$->name = NULL;
			$$->expr = NULL;
			$$->indexcolname = NULL;
			$$->collation = $1;
			$$->opclass = $2;
			$$->opclassopts = NIL;
			$$->ordering = (sq_sort_by_dir_t) $3;
			$$->nulls_ordering = (sq_sort_by_nulls_t) $4;
		}
	| opt_collate any_name reloptions opt_asc_desc opt_nulls_order
		{
			$$ = make_node<sq_index_elem_t>();
			$$->name = NULL;
			$$->expr = NULL;
			$$->indexcolname = NULL;
			$$->collation = $1;
			$$->opclass = $2;
			$$->opclassopts = $3;
			$$->ordering = (sq_sort_by_dir_t) $4;
			$$->nulls_ordering = (sq_sort_by_nulls_t) $5;
		}
	;

/*
 * Index attributes can be either simple column references, or arbitrary
 * expressions in parens.  For backwards-compatibility reasons, we allow
 * an expression that's just a function call to be written without parens.
 */
index_elem: ColId index_elem_options
				{
					$$ = $2;
					$$->name = $1;
				}
			| func_expr_windowless index_elem_options
				{
					$$ = $2;
					$$->expr = $1;
				}
			| '(' a_expr ')' index_elem_options
				{
					$$ = $4;
					$$->expr = $2;
				}
		;

opt_include:		INCLUDE '(' index_including_params ')'			{ $$ = $3; }
			 |		/* EMPTY */						{ $$ = NIL; }
		;

index_including_params:	index_elem						{ $$ = list_make1($1); }
			| index_including_params ',' index_elem		{ $$ = lappend($1, $3); }
		;

opt_collate: COLLATE any_name						{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;


opt_asc_desc: ASC							{ $$ = SQ_SORTBY_ASC; }
			| DESC							{ $$ = SQ_SORTBY_DESC; }
			| /*EMPTY*/						{ $$ = SQ_SORTBY_DEFAULT; }
		;

opt_nulls_order: NULLS_LA FIRST_P			{ $$ = SQ_SORTBY_NULLS_FIRST; }
			| NULLS_LA LAST_P				{ $$ = SQ_SORTBY_NULLS_LAST; }
			| /*EMPTY*/						{ $$ = SQ_SORTBY_NULLS_DEFAULT; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				create [or replace] function <fname>
 *						[(<type-1> { , <type-n>})]
 *						returns <type-r>
 *						as <filename or code in language as appropriate>
 *						language <lang> [with parameters]
 *
 *****************************************************************************/

CreateFunctionStmt:
			CREATE opt_or_replace FUNCTION func_name func_args_with_defaults
			RETURNS func_return opt_createfunc_opt_list opt_routine_body
				{
					sq_create_function_stmt_t *n = make_node<sq_create_function_stmt_t>();

					n->is_procedure = false;
					n->replace = $2;
					n->funcname = $4;
					n->parameters = $5;
					n->return_type = $7;
					n->options = $8;
					n->sql_body = $9;
					$$ = (sq_node_t *) n;
				}
			| CREATE opt_or_replace FUNCTION func_name func_args_with_defaults
			  RETURNS TABLE '(' table_func_column_list ')' opt_createfunc_opt_list opt_routine_body
				{
					sq_create_function_stmt_t *n = make_node<sq_create_function_stmt_t>();

					n->is_procedure = false;
					n->replace = $2;
					n->funcname = $4;
					n->parameters = merge_table_func_parameters($5, $9, yyscanner);
					n->return_type = table_func_type_name($9);
					n->return_type->location = @7;
					n->options = $11;
					n->sql_body = $12;
					$$ = (sq_node_t *) n;
				}
			| CREATE opt_or_replace FUNCTION func_name func_args_with_defaults
			  opt_createfunc_opt_list opt_routine_body
				{
					sq_create_function_stmt_t *n = make_node<sq_create_function_stmt_t>();

					n->is_procedure = false;
					n->replace = $2;
					n->funcname = $4;
					n->parameters = $5;
					n->return_type = NULL;
					n->options = $6;
					n->sql_body = $7;
					$$ = (sq_node_t *) n;
				}
			| CREATE opt_or_replace PROCEDURE func_name func_args_with_defaults
			  opt_createfunc_opt_list opt_routine_body
				{
					sq_create_function_stmt_t *n = make_node<sq_create_function_stmt_t>();

					n->is_procedure = true;
					n->replace = $2;
					n->funcname = $4;
					n->parameters = $5;
					n->return_type = NULL;
					n->options = $6;
					n->sql_body = $7;
					$$ = (sq_node_t *) n;
				}
		;

opt_or_replace:
			OR REPLACE								{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
		;

func_args:	'(' func_args_list ')'					{ $$ = $2; }
			| '(' ')'								{ $$ = NIL; }
		;

func_args_list:
			func_arg								{ $$ = list_make1($1); }
			| func_args_list ',' func_arg			{ $$ = lappend($1, $3); }
		;

function_with_argtypes_list:
			function_with_argtypes					{ $$ = list_make1($1); }
			| function_with_argtypes_list ',' function_with_argtypes
													{ $$ = lappend($1, $3); }
		;

function_with_argtypes:
			func_name func_args
				{
					sq_object_with_args_t *n = make_node<sq_object_with_args_t>();

					n->objname = $1;
					n->objargs = extract_arg_types($2);
					n->objfuncargs = $2;
					$$ = n;
				}
			/*
			 * Because of reduce/reduce conflicts, we can't use func_name
			 * below, but we can write it out the long way, which actually
			 * allows more cases.
			 */
			| type_func_name_keyword
				{
					sq_object_with_args_t *n = make_node<sq_object_with_args_t>();

					n->objname = list_make1(make_string(tree_strdup($1)));
					n->args_unspecified = true;
					$$ = n;
				}
			| ColId
				{
					sq_object_with_args_t *n = make_node<sq_object_with_args_t>();

					n->objname = list_make1(make_string($1));
					n->args_unspecified = true;
					$$ = n;
				}
			| ColId indirection
				{
					sq_object_with_args_t *n = make_node<sq_object_with_args_t>();

					n->objname = check_func_name(lcons(make_string($1), $2),
												  yyscanner);
					n->args_unspecified = true;
					$$ = n;
				}
		;

/*
 * func_args_with_defaults is separate because we only want to accept
 * defaults in CREATE FUNCTION, not in ALTER etc.
 */
func_args_with_defaults:
		'(' func_args_with_defaults_list ')'		{ $$ = $2; }
		| '(' ')'									{ $$ = NIL; }
		;

func_args_with_defaults_list:
		func_arg_with_default						{ $$ = list_make1($1); }
		| func_args_with_defaults_list ',' func_arg_with_default
													{ $$ = lappend($1, $3); }
		;

/*
 * The style with arg_class first is SQL99 standard, but Oracle puts
 * param_name first; accept both since it's likely people will try both
 * anyway.  Don't bother trying to save productions by letting arg_class
 * have an empty alternative ... you'll get shift/reduce conflicts.
 *
 * We can catch over-specified arguments here if we want to,
 * but for now better to silently swallow typmod, etc.
 * - thomas 2000-03-22
 */
func_arg:
			arg_class param_name func_type
				{
					sq_function_parameter_t *n = make_node<sq_function_parameter_t>();

					n->name = $2;
					n->arg_type = $3;
					n->mode = $1;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
			| param_name arg_class func_type
				{
					sq_function_parameter_t *n = make_node<sq_function_parameter_t>();

					n->name = $1;
					n->arg_type = $3;
					n->mode = $2;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
			| param_name func_type
				{
					sq_function_parameter_t *n = make_node<sq_function_parameter_t>();

					n->name = $1;
					n->arg_type = $2;
					n->mode = SQ_FUNC_PARAM_DEFAULT;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
			| arg_class func_type
				{
					sq_function_parameter_t *n = make_node<sq_function_parameter_t>();

					n->name = NULL;
					n->arg_type = $2;
					n->mode = $1;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
			| func_type
				{
					sq_function_parameter_t *n = make_node<sq_function_parameter_t>();

					n->name = NULL;
					n->arg_type = $1;
					n->mode = SQ_FUNC_PARAM_DEFAULT;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
		;

/* INOUT is SQL99 standard, IN OUT is for Oracle compatibility */
arg_class:	IN_P								{ $$ = SQ_FUNC_PARAM_IN; }
			| OUT_P								{ $$ = SQ_FUNC_PARAM_OUT; }
			| INOUT								{ $$ = SQ_FUNC_PARAM_INOUT; }
			| IN_P OUT_P						{ $$ = SQ_FUNC_PARAM_INOUT; }
			| VARIADIC							{ $$ = SQ_FUNC_PARAM_VARIADIC; }
		;

/*
 * Ideally param_name should be ColId, but that causes too many conflicts.
 */
param_name:	type_function_name
		;

func_return:
			func_type
				{
					/* We can catch over-specified results here if we want to,
					 * but for now better to silently swallow typmod, etc.
					 * - thomas 2000-03-22
					 */
					$$ = $1;
				}
		;

/*
 * We would like to make the %TYPE productions here be ColId attrs etc,
 * but that causes reduce/reduce conflicts.  type_function_name
 * is next best choice.
 */
func_type:	Typename								{ $$ = $1; }
			| type_function_name attrs '%' TYPE_P
				{
					$$ = make_type_name_from_name_list(lcons(make_string($1), $2));
					$$->pct_type = true;
					$$->location = @1;
				}
			| SETOF type_function_name attrs '%' TYPE_P
				{
					$$ = make_type_name_from_name_list(lcons(make_string($2), $3));
					$$->pct_type = true;
					$$->setof = true;
					$$->location = @2;
				}
		;

func_arg_with_default:
		func_arg
				{
					$$ = $1;
				}
		| func_arg DEFAULT a_expr
				{
					$$ = $1;
					$$->defexpr = $3;
				}
		| func_arg '=' a_expr
				{
					$$ = $1;
					$$->defexpr = $3;
				}
		;

/* Aggregate args can be most things that function args can be */
aggr_arg:	func_arg
				{
					if (!($1->mode == SQ_FUNC_PARAM_DEFAULT ||
						  $1->mode == SQ_FUNC_PARAM_IN ||
						  $1->mode == SQ_FUNC_PARAM_VARIADIC))
						raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@1)}, "aggregates cannot have output arguments");
					$$ = $1;
				}
		;

/*
 * The SQL standard offers no guidance on how to declare aggregate argument
 * lists, since it doesn't have CREATE AGGREGATE etc.  We accept these cases:
 *
 * (*)									- normal agg with no args
 * (aggr_arg,...)						- normal agg with args
 * (ORDER BY aggr_arg,...)				- ordered-set agg with no direct args
 * (aggr_arg,... ORDER BY aggr_arg,...)	- ordered-set agg with direct args
 *
 * The zero-argument case is spelled with '*' for consistency with COUNT(*).
 *
 * An additional restriction is that if the direct-args list ends in a
 * VARIADIC item, the ordered-args list must contain exactly one item that
 * is also VARIADIC with the same type.  This allows us to collapse the two
 * VARIADIC items into one, which is necessary to represent the aggregate in
 * pg_proc.  We check this at the grammar stage so that we can return a list
 * in which the second VARIADIC item is already discarded, avoiding extra work
 * in cases such as DROP AGGREGATE.
 *
 * The return value of this production is a two-element list, in which the
 * first item is a sublist of FunctionParameter nodes (with any duplicate
 * VARIADIC item already dropped, as per above) and the second is an Integer
 * node, containing -1 if there was no ORDER BY and otherwise the number
 * of argument declarations before the ORDER BY.  (If this number is equal
 * to the first sublist's length, then we dropped a duplicate VARIADIC item.)
 * This representation is passed as-is to CREATE AGGREGATE; for operations
 * on existing aggregates, we can just apply extractArgTypes to the first
 * sublist.
 */
aggr_args:	'(' '*' ')'
				{
					$$ = list_make2(NIL, make_integer(-1));
				}
			| '(' aggr_args_list ')'
				{
					$$ = list_make2($2, make_integer(-1));
				}
			| '(' ORDER BY aggr_args_list ')'
				{
					$$ = list_make2($4, make_integer(0));
				}
			| '(' aggr_args_list ORDER BY aggr_args_list ')'
				{
					/* this is the only case requiring consistency checking */
					$$ = make_ordered_set_args($2, $5, yyscanner);
				}
		;

aggr_args_list:
			aggr_arg								{ $$ = list_make1($1); }
			| aggr_args_list ',' aggr_arg			{ $$ = lappend($1, $3); }
		;

aggregate_with_argtypes:
			func_name aggr_args
				{
					sq_object_with_args_t *n = make_node<sq_object_with_args_t>();

					n->objname = $1;
					n->objargs = extract_aggr_arg_types($2);
					n->objfuncargs = (list_ref_t ) linitial($2);
					$$ = n;
				}
		;

aggregate_with_argtypes_list:
			aggregate_with_argtypes					{ $$ = list_make1($1); }
			| aggregate_with_argtypes_list ',' aggregate_with_argtypes
													{ $$ = lappend($1, $3); }
		;

opt_createfunc_opt_list:
			createfunc_opt_list
			| /*EMPTY*/ { $$ = NIL; }
	;

createfunc_opt_list:
			/* Must be at least one to prevent conflict */
			createfunc_opt_item						{ $$ = list_make1($1); }
			| createfunc_opt_list createfunc_opt_item { $$ = lappend($1, $2); }
	;

/*
 * Options common to both CREATE FUNCTION and ALTER FUNCTION
 */
common_func_opt_item:
			CALLED ON NULL_P INPUT_P
				{
					$$ = make_def_elem("strict", (sq_node_t *) make_boolean(false), @1);
				}
			| RETURNS NULL_P ON NULL_P INPUT_P
				{
					$$ = make_def_elem("strict", (sq_node_t *) make_boolean(true), @1);
				}
			| STRICT_P
				{
					$$ = make_def_elem("strict", (sq_node_t *) make_boolean(true), @1);
				}
			| IMMUTABLE
				{
					$$ = make_def_elem("volatility", (sq_node_t *) make_string("immutable"), @1);
				}
			| STABLE
				{
					$$ = make_def_elem("volatility", (sq_node_t *) make_string("stable"), @1);
				}
			| VOLATILE
				{
					$$ = make_def_elem("volatility", (sq_node_t *) make_string("volatile"), @1);
				}
			| EXTERNAL SECURITY DEFINER
				{
					$$ = make_def_elem("security", (sq_node_t *) make_boolean(true), @1);
				}
			| EXTERNAL SECURITY INVOKER
				{
					$$ = make_def_elem("security", (sq_node_t *) make_boolean(false), @1);
				}
			| SECURITY DEFINER
				{
					$$ = make_def_elem("security", (sq_node_t *) make_boolean(true), @1);
				}
			| SECURITY INVOKER
				{
					$$ = make_def_elem("security", (sq_node_t *) make_boolean(false), @1);
				}
			| LEAKPROOF
				{
					$$ = make_def_elem("leakproof", (sq_node_t *) make_boolean(true), @1);
				}
			| NOT LEAKPROOF
				{
					$$ = make_def_elem("leakproof", (sq_node_t *) make_boolean(false), @1);
				}
			| COST NumericOnly
				{
					$$ = make_def_elem("cost", (sq_node_t *) $2, @1);
				}
			| ROWS NumericOnly
				{
					$$ = make_def_elem("rows", (sq_node_t *) $2, @1);
				}
			| SUPPORT any_name
				{
					$$ = make_def_elem("support", (sq_node_t *) $2, @1);
				}
			| FunctionSetResetClause
				{
					/* we abuse the normal content of a DefElem here */
					$$ = make_def_elem("set", (sq_node_t *) $1, @1);
				}
			| PARALLEL ColId
				{
					$$ = make_def_elem("parallel", (sq_node_t *) make_string($2), @1);
				}
		;

createfunc_opt_item:
			AS func_as
				{
					$$ = make_def_elem("as", (sq_node_t *) $2, @1);
				}
			| LANGUAGE NonReservedWord_or_Sconst
				{
					$$ = make_def_elem("language", (sq_node_t *) make_string($2), @1);
				}
			| TRANSFORM transform_type_list
				{
					$$ = make_def_elem("transform", (sq_node_t *) $2, @1);
				}
			| WINDOW
				{
					$$ = make_def_elem("window", (sq_node_t *) make_boolean(true), @1);
				}
			| common_func_opt_item
				{
					$$ = $1;
				}
		;

func_as:	Sconst						{ $$ = list_make1(make_string($1)); }
			| Sconst ',' Sconst
				{
					$$ = list_make2(make_string($1), make_string($3));
				}
		;

ReturnStmt:	RETURN a_expr
				{
					sq_return_stmt_t *r = make_node<sq_return_stmt_t>();

					r->returnval = (sq_node_t *) $2;
					$$ = (sq_node_t *) r;
				}
		;

opt_routine_body:
			ReturnStmt
				{
					$$ = $1;
				}
			| BEGIN_P ATOMIC routine_body_stmt_list END_P
				{
					/*
					 * A compound statement is stored as a single-item list
					 * containing the list of statements as its member.  That
					 * way, the parse analysis code can tell apart an empty
					 * body from no body at all.
					 */
					$$ = (sq_node_t *) list_make1($3);
				}
			| /*EMPTY*/
				{
					$$ = NULL;
				}
		;

routine_body_stmt_list:
			routine_body_stmt_list routine_body_stmt ';'
				{
					/* As in stmtmulti, discard empty statements */
					if ($2 != NULL)
						$$ = lappend($1, $2);
					else
						$$ = $1;
				}
			| /*EMPTY*/
				{
					$$ = NIL;
				}
		;

routine_body_stmt:
			stmt
			| ReturnStmt
		;

transform_type_list:
			FOR TYPE_P Typename { $$ = list_make1($3); }
			| transform_type_list ',' FOR TYPE_P Typename { $$ = lappend($1, $5); }
		;

opt_definition:
			WITH definition							{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

table_func_column:	param_name func_type
				{
					sq_function_parameter_t *n = make_node<sq_function_parameter_t>();

					n->name = $1;
					n->arg_type = $2;
					n->mode = SQ_FUNC_PARAM_TABLE;
					n->defexpr = NULL;
					n->location = @1;
					$$ = n;
				}
		;

table_func_column_list:
			table_func_column
				{
					$$ = list_make1($1);
				}
			| table_func_column_list ',' table_func_column
				{
					$$ = lappend($1, $3);
				}
		;

/*****************************************************************************
 * ALTER FUNCTION / ALTER PROCEDURE / ALTER ROUTINE
 *
 * RENAME and OWNER subcommands are already provided by the generic
 * ALTER infrastructure, here we just specify alterations that can
 * only be applied to functions.
 *
 *****************************************************************************/
AlterFunctionStmt:
			ALTER FUNCTION function_with_argtypes alterfunc_opt_list opt_restrict
				{
					sq_alter_function_stmt_t *n = make_node<sq_alter_function_stmt_t>();

					n->objtype = SQ_OBJECT_FUNCTION;
					n->func = $3;
					n->actions = $4;
					$$ = (sq_node_t *) n;
				}
			| ALTER PROCEDURE function_with_argtypes alterfunc_opt_list opt_restrict
				{
					sq_alter_function_stmt_t *n = make_node<sq_alter_function_stmt_t>();

					n->objtype = SQ_OBJECT_PROCEDURE;
					n->func = $3;
					n->actions = $4;
					$$ = (sq_node_t *) n;
				}
			| ALTER ROUTINE function_with_argtypes alterfunc_opt_list opt_restrict
				{
					sq_alter_function_stmt_t *n = make_node<sq_alter_function_stmt_t>();

					n->objtype = SQ_OBJECT_ROUTINE;
					n->func = $3;
					n->actions = $4;
					$$ = (sq_node_t *) n;
				}
		;

alterfunc_opt_list:
			/* At least one option must be specified */
			common_func_opt_item					{ $$ = list_make1($1); }
			| alterfunc_opt_list common_func_opt_item { $$ = lappend($1, $2); }
		;

/* Ignored, merely for SQL compliance */
opt_restrict:
			RESTRICT
			| /* EMPTY */
		;


/*****************************************************************************
 *
 *		QUERY:
 *
 *		DROP FUNCTION funcname (arg1, arg2, ...) [ RESTRICT | CASCADE ]
 *		DROP PROCEDURE procname (arg1, arg2, ...) [ RESTRICT | CASCADE ]
 *		DROP ROUTINE routname (arg1, arg2, ...) [ RESTRICT | CASCADE ]
 *		DROP AGGREGATE aggname (arg1, ...) [ RESTRICT | CASCADE ]
 *		DROP OPERATOR opname (leftoperand_typ, rightoperand_typ) [ RESTRICT | CASCADE ]
 *
 *****************************************************************************/

RemoveFuncStmt:
			DROP FUNCTION function_with_argtypes_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_FUNCTION;
					n->objects = $3;
					n->behavior = $4;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP FUNCTION IF_P EXISTS function_with_argtypes_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_FUNCTION;
					n->objects = $5;
					n->behavior = $6;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP PROCEDURE function_with_argtypes_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_PROCEDURE;
					n->objects = $3;
					n->behavior = $4;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP PROCEDURE IF_P EXISTS function_with_argtypes_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_PROCEDURE;
					n->objects = $5;
					n->behavior = $6;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP ROUTINE function_with_argtypes_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_ROUTINE;
					n->objects = $3;
					n->behavior = $4;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP ROUTINE IF_P EXISTS function_with_argtypes_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_ROUTINE;
					n->objects = $5;
					n->behavior = $6;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
		;

RemoveAggrStmt:
			DROP AGGREGATE aggregate_with_argtypes_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_AGGREGATE;
					n->objects = $3;
					n->behavior = $4;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP AGGREGATE IF_P EXISTS aggregate_with_argtypes_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_AGGREGATE;
					n->objects = $5;
					n->behavior = $6;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
		;

RemoveOperStmt:
			DROP OPERATOR operator_with_argtypes_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_OPERATOR;
					n->objects = $3;
					n->behavior = $4;
					n->missing_ok = false;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
			| DROP OPERATOR IF_P EXISTS operator_with_argtypes_list opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_OPERATOR;
					n->objects = $5;
					n->behavior = $6;
					n->missing_ok = true;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
		;

oper_argtypes:
			'(' Typename ')'
				{
				   raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@3), .hint = "Use NONE to denote the missing argument of a unary operator."}, "missing argument");
				}
			| '(' Typename ',' Typename ')'
					{ $$ = list_make2($2, $4); }
			| '(' NONE ',' Typename ')'					/* left unary */
					{ $$ = list_make2(NULL, $4); }
			| '(' Typename ',' NONE ')'					/* right unary */
					{ $$ = list_make2($2, NULL); }
		;

any_operator:
			all_Op
					{ $$ = list_make1(make_string($1)); }
			| ColId '.' any_operator
					{ $$ = lcons(make_string($1), $3); }
		;

operator_with_argtypes_list:
			operator_with_argtypes					{ $$ = list_make1($1); }
			| operator_with_argtypes_list ',' operator_with_argtypes
													{ $$ = lappend($1, $3); }
		;

operator_with_argtypes:
			any_operator oper_argtypes
				{
					sq_object_with_args_t *n = make_node<sq_object_with_args_t>();

					n->objname = $1;
					n->objargs = $2;
					$$ = n;
				}
		;

/*****************************************************************************
 *
 *		DO <anonymous code block> [ LANGUAGE language ]
 *
 * We use a DefElem list for future extensibility, and to allow flexibility
 * in the clause order.
 *
 *****************************************************************************/

DoStmt: DO dostmt_opt_list
				{
					sq_do_stmt_t *n = make_node<sq_do_stmt_t>();

					n->args = $2;
					$$ = (sq_node_t *) n;
				}
		;

dostmt_opt_list:
			dostmt_opt_item						{ $$ = list_make1($1); }
			| dostmt_opt_list dostmt_opt_item	{ $$ = lappend($1, $2); }
		;

dostmt_opt_item:
			Sconst
				{
					$$ = make_def_elem("as", (sq_node_t *) make_string($1), @1);
				}
			| LANGUAGE NonReservedWord_or_Sconst
				{
					$$ = make_def_elem("language", (sq_node_t *) make_string($2), @1);
				}
		;

/*****************************************************************************
 *
 *		CREATE CAST / DROP CAST
 *
 *****************************************************************************/

CreateCastStmt: CREATE CAST '(' Typename AS Typename ')'
					WITH FUNCTION function_with_argtypes cast_context
				{
					sq_create_cast_stmt_t *n = make_node<sq_create_cast_stmt_t>();

					n->sourcetype = $4;
					n->targettype = $6;
					n->func = $10;
					n->context = (sq_coercion_context_t) $11;
					n->inout = false;
					$$ = (sq_node_t *) n;
				}
			| CREATE CAST '(' Typename AS Typename ')'
					WITHOUT FUNCTION cast_context
				{
					sq_create_cast_stmt_t *n = make_node<sq_create_cast_stmt_t>();

					n->sourcetype = $4;
					n->targettype = $6;
					n->func = NULL;
					n->context = (sq_coercion_context_t) $10;
					n->inout = false;
					$$ = (sq_node_t *) n;
				}
			| CREATE CAST '(' Typename AS Typename ')'
					WITH INOUT cast_context
				{
					sq_create_cast_stmt_t *n = make_node<sq_create_cast_stmt_t>();

					n->sourcetype = $4;
					n->targettype = $6;
					n->func = NULL;
					n->context = (sq_coercion_context_t) $10;
					n->inout = true;
					$$ = (sq_node_t *) n;
				}
		;

cast_context:  AS IMPLICIT_P					{ $$ = SQ_COERCION_IMPLICIT; }
		| AS ASSIGNMENT							{ $$ = SQ_COERCION_ASSIGNMENT; }
		| /*EMPTY*/								{ $$ = SQ_COERCION_EXPLICIT; }
		;


DropCastStmt: DROP CAST opt_if_exists '(' Typename AS Typename ')' opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_CAST;
					n->objects = list_make1(list_make2($5, $7));
					n->behavior = $9;
					n->missing_ok = $3;
					n->concurrent = false;
					$$ = (sq_node_t *) n;
				}
		;

opt_if_exists: IF_P EXISTS						{ $$ = true; }
		| /*EMPTY*/								{ $$ = false; }
		;


/*****************************************************************************
 *
 *		CREATE TRANSFORM / DROP TRANSFORM
 *
 *****************************************************************************/

CreateTransformStmt: CREATE opt_or_replace TRANSFORM FOR Typename LANGUAGE name '(' transform_element_list ')'
				{
					sq_create_transform_stmt_t *n = make_node<sq_create_transform_stmt_t>();

					n->replace = $2;
					n->type_name = $5;
					n->lang = $7;
					n->fromsql = (sq_object_with_args_t *) linitial($9);
					n->tosql = (sq_object_with_args_t *) lsecond($9);
					$$ = (sq_node_t *) n;
				}
		;

transform_element_list: FROM SQL_P WITH FUNCTION function_with_argtypes ',' TO SQL_P WITH FUNCTION function_with_argtypes
				{
					$$ = list_make2($5, $11);
				}
				| TO SQL_P WITH FUNCTION function_with_argtypes ',' FROM SQL_P WITH FUNCTION function_with_argtypes
				{
					$$ = list_make2($11, $5);
				}
				| FROM SQL_P WITH FUNCTION function_with_argtypes
				{
					$$ = list_make2($5, NULL);
				}
				| TO SQL_P WITH FUNCTION function_with_argtypes
				{
					$$ = list_make2(NULL, $5);
				}
		;


DropTransformStmt: DROP TRANSFORM opt_if_exists FOR Typename LANGUAGE name opt_drop_behavior
				{
					sq_drop_stmt_t *n = make_node<sq_drop_stmt_t>();

					n->remove_type = SQ_OBJECT_TRANSFORM;
					n->objects = list_make1(list_make2($5, make_string($7)));
					n->behavior = $8;
					n->missing_ok = $3;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 *		QUERY:
 *
 *		REINDEX [ (options) ] {INDEX | TABLE | SCHEMA} [CONCURRENTLY] <name>
 *		REINDEX [ (options) ] {DATABASE | SYSTEM} [CONCURRENTLY] [<name>]
 *****************************************************************************/

ReindexStmt:
			REINDEX opt_reindex_option_list reindex_target_relation opt_concurrently qualified_name
				{
					sq_reindex_stmt_t *n = make_node<sq_reindex_stmt_t>();

					n->kind = (sq_reindex_object_type_t) $3;
					n->relation = $5;
					n->name = NULL;
					n->params = $2;
					if ($4)
						n->params = lappend(n->params, make_def_elem("concurrently", NULL, @4));
					$$ = (sq_node_t *) n;
				}
			| REINDEX opt_reindex_option_list SCHEMA opt_concurrently name
				{
					sq_reindex_stmt_t *n = make_node<sq_reindex_stmt_t>();

					n->kind = SQ_REINDEX_OBJECT_SCHEMA;
					n->relation = NULL;
					n->name = $5;
					n->params = $2;
					if ($4)
						n->params = lappend(n->params, make_def_elem("concurrently", NULL, @4));
					$$ = (sq_node_t *) n;
				}
			| REINDEX opt_reindex_option_list reindex_target_all opt_concurrently opt_single_name
				{
					sq_reindex_stmt_t *n = make_node<sq_reindex_stmt_t>();

					n->kind = (sq_reindex_object_type_t) $3;
					n->relation = NULL;
					n->name = $5;
					n->params = $2;
					if ($4)
						n->params = lappend(n->params, make_def_elem("concurrently", NULL, @4));
					$$ = (sq_node_t *) n;
				}
		;
reindex_target_relation:
			INDEX					{ $$ = SQ_REINDEX_OBJECT_INDEX; }
			| TABLE					{ $$ = SQ_REINDEX_OBJECT_TABLE; }
		;
reindex_target_all:
			SYSTEM_P				{ $$ = SQ_REINDEX_OBJECT_SYSTEM; }
			| DATABASE				{ $$ = SQ_REINDEX_OBJECT_DATABASE; }
		;
opt_reindex_option_list:
			'(' utility_option_list ')'				{ $$ = $2; }
			| /* EMPTY */							{ $$ = NULL; }
		;

/*****************************************************************************
 *
 * ALTER TABLESPACE
 *
 *****************************************************************************/

AlterTblSpcStmt:
			ALTER TABLESPACE name SET reloptions
				{
					sq_alter_table_space_options_stmt_t *n =
						make_node<sq_alter_table_space_options_stmt_t>();

					n->tablespacename = $3;
					n->options = $5;
					n->is_reset = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TABLESPACE name RESET reloptions
				{
					sq_alter_table_space_options_stmt_t *n =
						make_node<sq_alter_table_space_options_stmt_t>();

					n->tablespacename = $3;
					n->options = $5;
					n->is_reset = true;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 * ALTER THING name RENAME TO newname
 *
 *****************************************************************************/

RenameStmt: ALTER AGGREGATE aggregate_with_argtypes RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_AGGREGATE;
					n->object = (sq_node_t *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER COLLATION any_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_COLLATION;
					n->object = (sq_node_t *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER CONVERSION_P any_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_CONVERSION;
					n->object = (sq_node_t *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER DATABASE name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_DATABASE;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER DOMAIN_P any_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_DOMAIN;
					n->object = (sq_node_t *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER DOMAIN_P any_name RENAME CONSTRAINT name TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_DOMCONSTRAINT;
					n->object = (sq_node_t *) $3;
					n->subname = $6;
					n->newname = $8;
					$$ = (sq_node_t *) n;
				}
			| ALTER FOREIGN DATA_P WRAPPER name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_FDW;
					n->object = (sq_node_t *) make_string($5);
					n->newname = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER FUNCTION function_with_argtypes RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_FUNCTION;
					n->object = (sq_node_t *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER GROUP_P RoleId RENAME TO RoleId
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_ROLE;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER opt_procedural LANGUAGE name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_LANGUAGE;
					n->object = (sq_node_t *) make_string($4);
					n->newname = $7;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER OPERATOR CLASS any_name USING name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_OPCLASS;
					n->object = (sq_node_t *) lcons(make_string($6), $4);
					n->newname = $9;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER OPERATOR FAMILY any_name USING name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_OPFAMILY;
					n->object = (sq_node_t *) lcons(make_string($6), $4);
					n->newname = $9;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER POLICY name ON qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_POLICY;
					n->relation = $5;
					n->subname = $3;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER POLICY IF_P EXISTS name ON qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_POLICY;
					n->relation = $7;
					n->subname = $5;
					n->newname = $10;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER PROCEDURE function_with_argtypes RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_PROCEDURE;
					n->object = (sq_node_t *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER PUBLICATION name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_PUBLICATION;
					n->object = (sq_node_t *) make_string($3);
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER ROUTINE function_with_argtypes RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_ROUTINE;
					n->object = (sq_node_t *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER SCHEMA name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_SCHEMA;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER SERVER name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_FOREIGN_SERVER;
					n->object = (sq_node_t *) make_string($3);
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER SUBSCRIPTION name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_SUBSCRIPTION;
					n->object = (sq_node_t *) make_string($3);
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TABLE relation_expr RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_TABLE;
					n->relation = $3;
					n->subname = NULL;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TABLE IF_P EXISTS relation_expr RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_TABLE;
					n->relation = $5;
					n->subname = NULL;
					n->newname = $8;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER SEQUENCE qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_SEQUENCE;
					n->relation = $3;
					n->subname = NULL;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER SEQUENCE IF_P EXISTS qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_SEQUENCE;
					n->relation = $5;
					n->subname = NULL;
					n->newname = $8;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER VIEW qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_VIEW;
					n->relation = $3;
					n->subname = NULL;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER VIEW IF_P EXISTS qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_VIEW;
					n->relation = $5;
					n->subname = NULL;
					n->newname = $8;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER MATERIALIZED VIEW qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_MATVIEW;
					n->relation = $4;
					n->subname = NULL;
					n->newname = $7;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_MATVIEW;
					n->relation = $6;
					n->subname = NULL;
					n->newname = $9;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER INDEX qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_INDEX;
					n->relation = $3;
					n->subname = NULL;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER INDEX IF_P EXISTS qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_INDEX;
					n->relation = $5;
					n->subname = NULL;
					n->newname = $8;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER FOREIGN TABLE relation_expr RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_FOREIGN_TABLE;
					n->relation = $4;
					n->subname = NULL;
					n->newname = $7;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER FOREIGN TABLE IF_P EXISTS relation_expr RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_FOREIGN_TABLE;
					n->relation = $6;
					n->subname = NULL;
					n->newname = $9;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER TABLE relation_expr RENAME opt_column name TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_COLUMN;
					n->relation_type = SQ_OBJECT_TABLE;
					n->relation = $3;
					n->subname = $6;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TABLE IF_P EXISTS relation_expr RENAME opt_column name TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_COLUMN;
					n->relation_type = SQ_OBJECT_TABLE;
					n->relation = $5;
					n->subname = $8;
					n->newname = $10;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER VIEW qualified_name RENAME opt_column name TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_COLUMN;
					n->relation_type = SQ_OBJECT_VIEW;
					n->relation = $3;
					n->subname = $6;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER VIEW IF_P EXISTS qualified_name RENAME opt_column name TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_COLUMN;
					n->relation_type = SQ_OBJECT_VIEW;
					n->relation = $5;
					n->subname = $8;
					n->newname = $10;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER MATERIALIZED VIEW qualified_name RENAME opt_column name TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_COLUMN;
					n->relation_type = SQ_OBJECT_MATVIEW;
					n->relation = $4;
					n->subname = $7;
					n->newname = $9;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name RENAME opt_column name TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_COLUMN;
					n->relation_type = SQ_OBJECT_MATVIEW;
					n->relation = $6;
					n->subname = $9;
					n->newname = $11;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER TABLE relation_expr RENAME CONSTRAINT name TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_TABCONSTRAINT;
					n->relation = $3;
					n->subname = $6;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TABLE IF_P EXISTS relation_expr RENAME CONSTRAINT name TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_TABCONSTRAINT;
					n->relation = $5;
					n->subname = $8;
					n->newname = $10;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER FOREIGN TABLE relation_expr RENAME opt_column name TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_COLUMN;
					n->relation_type = SQ_OBJECT_FOREIGN_TABLE;
					n->relation = $4;
					n->subname = $7;
					n->newname = $9;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER FOREIGN TABLE IF_P EXISTS relation_expr RENAME opt_column name TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_COLUMN;
					n->relation_type = SQ_OBJECT_FOREIGN_TABLE;
					n->relation = $6;
					n->subname = $9;
					n->newname = $11;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER RULE name ON qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_RULE;
					n->relation = $5;
					n->subname = $3;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TRIGGER name ON qualified_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_TRIGGER;
					n->relation = $5;
					n->subname = $3;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER EVENT TRIGGER name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_EVENT_TRIGGER;
					n->object = (sq_node_t *) make_string($4);
					n->newname = $7;
					$$ = (sq_node_t *) n;
				}
			| ALTER ROLE RoleId RENAME TO RoleId
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_ROLE;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER USER RoleId RENAME TO RoleId
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_ROLE;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TABLESPACE name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_TABLESPACE;
					n->subname = $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER STATISTICS any_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_STATISTIC_EXT;
					n->object = (sq_node_t *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH PARSER any_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_TSPARSER;
					n->object = (sq_node_t *) $5;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH DICTIONARY any_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_TSDICTIONARY;
					n->object = (sq_node_t *) $5;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH TEMPLATE any_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_TSTEMPLATE;
					n->object = (sq_node_t *) $5;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_TSCONFIGURATION;
					n->object = (sq_node_t *) $5;
					n->newname = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TYPE_P any_name RENAME TO name
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_TYPE;
					n->object = (sq_node_t *) $3;
					n->newname = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TYPE_P any_name RENAME ATTRIBUTE name TO name opt_drop_behavior
				{
					sq_rename_stmt_t *n = make_node<sq_rename_stmt_t>();

					n->rename_type = SQ_OBJECT_ATTRIBUTE;
					n->relation_type = SQ_OBJECT_TYPE;
					n->relation = make_range_var_from_any_name($3, @3, yyscanner);
					n->subname = $6;
					n->newname = $8;
					n->behavior = $9;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
		;

opt_column: COLUMN
			| /*EMPTY*/
		;

opt_set_data: SET DATA_P							{ $$ = 1; }
			| /*EMPTY*/								{ $$ = 0; }
		;

/*****************************************************************************
 *
 * ALTER THING name DEPENDS ON EXTENSION name
 *
 *****************************************************************************/

AlterObjectDependsStmt:
			ALTER FUNCTION function_with_argtypes opt_no DEPENDS ON EXTENSION name
				{
					sq_alter_object_depends_stmt_t *n = make_node<sq_alter_object_depends_stmt_t>();

					n->object_type = SQ_OBJECT_FUNCTION;
					n->object = (sq_node_t *) $3;
					n->extname = make_string($8);
					n->remove = $4;
					$$ = (sq_node_t *) n;
				}
			| ALTER PROCEDURE function_with_argtypes opt_no DEPENDS ON EXTENSION name
				{
					sq_alter_object_depends_stmt_t *n = make_node<sq_alter_object_depends_stmt_t>();

					n->object_type = SQ_OBJECT_PROCEDURE;
					n->object = (sq_node_t *) $3;
					n->extname = make_string($8);
					n->remove = $4;
					$$ = (sq_node_t *) n;
				}
			| ALTER ROUTINE function_with_argtypes opt_no DEPENDS ON EXTENSION name
				{
					sq_alter_object_depends_stmt_t *n = make_node<sq_alter_object_depends_stmt_t>();

					n->object_type = SQ_OBJECT_ROUTINE;
					n->object = (sq_node_t *) $3;
					n->extname = make_string($8);
					n->remove = $4;
					$$ = (sq_node_t *) n;
				}
			| ALTER TRIGGER name ON qualified_name opt_no DEPENDS ON EXTENSION name
				{
					sq_alter_object_depends_stmt_t *n = make_node<sq_alter_object_depends_stmt_t>();

					n->object_type = SQ_OBJECT_TRIGGER;
					n->relation = $5;
					n->object = (sq_node_t *) list_make1(make_string($3));
					n->extname = make_string($10);
					n->remove = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER MATERIALIZED VIEW qualified_name opt_no DEPENDS ON EXTENSION name
				{
					sq_alter_object_depends_stmt_t *n = make_node<sq_alter_object_depends_stmt_t>();

					n->object_type = SQ_OBJECT_MATVIEW;
					n->relation = $4;
					n->extname = make_string($9);
					n->remove = $5;
					$$ = (sq_node_t *) n;
				}
			| ALTER INDEX qualified_name opt_no DEPENDS ON EXTENSION name
				{
					sq_alter_object_depends_stmt_t *n = make_node<sq_alter_object_depends_stmt_t>();

					n->object_type = SQ_OBJECT_INDEX;
					n->relation = $3;
					n->extname = make_string($8);
					n->remove = $4;
					$$ = (sq_node_t *) n;
				}
		;

opt_no:		NO				{ $$ = true; }
			| /* EMPTY */	{ $$ = false;	}
		;

/*****************************************************************************
 *
 * ALTER THING name SET SCHEMA name
 *
 *****************************************************************************/

AlterObjectSchemaStmt:
			ALTER AGGREGATE aggregate_with_argtypes SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_AGGREGATE;
					n->object = (sq_node_t *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER COLLATION any_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_COLLATION;
					n->object = (sq_node_t *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER CONVERSION_P any_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_CONVERSION;
					n->object = (sq_node_t *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER DOMAIN_P any_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_DOMAIN;
					n->object = (sq_node_t *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER EXTENSION name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_EXTENSION;
					n->object = (sq_node_t *) make_string($3);
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER FUNCTION function_with_argtypes SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_FUNCTION;
					n->object = (sq_node_t *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER OPERATOR operator_with_argtypes SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_OPERATOR;
					n->object = (sq_node_t *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER OPERATOR CLASS any_name USING name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_OPCLASS;
					n->object = (sq_node_t *) lcons(make_string($6), $4);
					n->newschema = $9;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER OPERATOR FAMILY any_name USING name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_OPFAMILY;
					n->object = (sq_node_t *) lcons(make_string($6), $4);
					n->newschema = $9;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER PROCEDURE function_with_argtypes SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_PROCEDURE;
					n->object = (sq_node_t *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER ROUTINE function_with_argtypes SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_ROUTINE;
					n->object = (sq_node_t *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TABLE relation_expr SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_TABLE;
					n->relation = $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TABLE IF_P EXISTS relation_expr SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_TABLE;
					n->relation = $5;
					n->newschema = $8;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER STATISTICS any_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_STATISTIC_EXT;
					n->object = (sq_node_t *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH PARSER any_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_TSPARSER;
					n->object = (sq_node_t *) $5;
					n->newschema = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH DICTIONARY any_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_TSDICTIONARY;
					n->object = (sq_node_t *) $5;
					n->newschema = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH TEMPLATE any_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_TSTEMPLATE;
					n->object = (sq_node_t *) $5;
					n->newschema = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_TSCONFIGURATION;
					n->object = (sq_node_t *) $5;
					n->newschema = $8;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER SEQUENCE qualified_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_SEQUENCE;
					n->relation = $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER SEQUENCE IF_P EXISTS qualified_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_SEQUENCE;
					n->relation = $5;
					n->newschema = $8;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER VIEW qualified_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_VIEW;
					n->relation = $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER VIEW IF_P EXISTS qualified_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_VIEW;
					n->relation = $5;
					n->newschema = $8;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER MATERIALIZED VIEW qualified_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_MATVIEW;
					n->relation = $4;
					n->newschema = $7;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER MATERIALIZED VIEW IF_P EXISTS qualified_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_MATVIEW;
					n->relation = $6;
					n->newschema = $9;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER FOREIGN TABLE relation_expr SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_FOREIGN_TABLE;
					n->relation = $4;
					n->newschema = $7;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER FOREIGN TABLE IF_P EXISTS relation_expr SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_FOREIGN_TABLE;
					n->relation = $6;
					n->newschema = $9;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER TYPE_P any_name SET SCHEMA name
				{
					sq_alter_object_schema_stmt_t *n = make_node<sq_alter_object_schema_stmt_t>();

					n->object_type = SQ_OBJECT_TYPE;
					n->object = (sq_node_t *) $3;
					n->newschema = $6;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 * ALTER OPERATOR name SET define
 *
 *****************************************************************************/

AlterOperatorStmt:
			ALTER OPERATOR operator_with_argtypes SET '(' operator_def_list ')'
				{
					sq_alter_operator_stmt_t *n = make_node<sq_alter_operator_stmt_t>();

					n->opername = $3;
					n->options = $6;
					$$ = (sq_node_t *) n;
				}
		;

operator_def_list:	operator_def_elem								{ $$ = list_make1($1); }
			| operator_def_list ',' operator_def_elem				{ $$ = lappend($1, $3); }
		;

operator_def_elem: ColLabel '=' NONE
						{ $$ = make_def_elem($1, NULL, @1); }
				   | ColLabel '=' operator_def_arg
						{ $$ = make_def_elem($1, (sq_node_t *) $3, @1); }
				   | ColLabel
						{ $$ = make_def_elem($1, NULL, @1); }
		;

/* must be similar enough to def_arg to avoid reduce/reduce conflicts */
operator_def_arg:
			func_type						{ $$ = (sq_node_t *) $1; }
			| reserved_keyword				{ $$ = (sq_node_t *) make_string(tree_strdup($1)); }
			| qual_all_Op					{ $$ = (sq_node_t *) $1; }
			| NumericOnly					{ $$ = (sq_node_t *) $1; }
			| Sconst						{ $$ = (sq_node_t *) make_string($1); }
		;

/*****************************************************************************
 *
 * ALTER TYPE name SET define
 *
 * We repurpose ALTER OPERATOR's version of "definition" here
 *
 *****************************************************************************/

AlterTypeStmt:
			ALTER TYPE_P any_name SET '(' operator_def_list ')'
				{
					sq_alter_type_stmt_t *n = make_node<sq_alter_type_stmt_t>();

					n->type_name = $3;
					n->options = $6;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 * ALTER THING name OWNER TO newname
 *
 *****************************************************************************/

AlterOwnerStmt: ALTER AGGREGATE aggregate_with_argtypes OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_AGGREGATE;
					n->object = (sq_node_t *) $3;
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER COLLATION any_name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_COLLATION;
					n->object = (sq_node_t *) $3;
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER CONVERSION_P any_name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_CONVERSION;
					n->object = (sq_node_t *) $3;
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER DATABASE name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_DATABASE;
					n->object = (sq_node_t *) make_string($3);
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER DOMAIN_P any_name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_DOMAIN;
					n->object = (sq_node_t *) $3;
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER FUNCTION function_with_argtypes OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_FUNCTION;
					n->object = (sq_node_t *) $3;
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER opt_procedural LANGUAGE name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_LANGUAGE;
					n->object = (sq_node_t *) make_string($4);
					n->newowner = $7;
					$$ = (sq_node_t *) n;
				}
			| ALTER LARGE_P OBJECT_P NumericOnly OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_LARGEOBJECT;
					n->object = (sq_node_t *) $4;
					n->newowner = $7;
					$$ = (sq_node_t *) n;
				}
			| ALTER OPERATOR operator_with_argtypes OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_OPERATOR;
					n->object = (sq_node_t *) $3;
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER OPERATOR CLASS any_name USING name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_OPCLASS;
					n->object = (sq_node_t *) lcons(make_string($6), $4);
					n->newowner = $9;
					$$ = (sq_node_t *) n;
				}
			| ALTER OPERATOR FAMILY any_name USING name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_OPFAMILY;
					n->object = (sq_node_t *) lcons(make_string($6), $4);
					n->newowner = $9;
					$$ = (sq_node_t *) n;
				}
			| ALTER PROCEDURE function_with_argtypes OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_PROCEDURE;
					n->object = (sq_node_t *) $3;
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER ROUTINE function_with_argtypes OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_ROUTINE;
					n->object = (sq_node_t *) $3;
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER SCHEMA name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_SCHEMA;
					n->object = (sq_node_t *) make_string($3);
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER TYPE_P any_name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_TYPE;
					n->object = (sq_node_t *) $3;
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER TABLESPACE name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_TABLESPACE;
					n->object = (sq_node_t *) make_string($3);
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER STATISTICS any_name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_STATISTIC_EXT;
					n->object = (sq_node_t *) $3;
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH DICTIONARY any_name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_TSDICTIONARY;
					n->object = (sq_node_t *) $5;
					n->newowner = $8;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_TSCONFIGURATION;
					n->object = (sq_node_t *) $5;
					n->newowner = $8;
					$$ = (sq_node_t *) n;
				}
			| ALTER FOREIGN DATA_P WRAPPER name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_FDW;
					n->object = (sq_node_t *) make_string($5);
					n->newowner = $8;
					$$ = (sq_node_t *) n;
				}
			| ALTER SERVER name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_FOREIGN_SERVER;
					n->object = (sq_node_t *) make_string($3);
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER EVENT TRIGGER name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_EVENT_TRIGGER;
					n->object = (sq_node_t *) make_string($4);
					n->newowner = $7;
					$$ = (sq_node_t *) n;
				}
			| ALTER PUBLICATION name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_PUBLICATION;
					n->object = (sq_node_t *) make_string($3);
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER SUBSCRIPTION name OWNER TO RoleSpec
				{
					sq_alter_owner_stmt_t *n = make_node<sq_alter_owner_stmt_t>();

					n->object_type = SQ_OBJECT_SUBSCRIPTION;
					n->object = (sq_node_t *) make_string($3);
					n->newowner = $6;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 * CREATE PUBLICATION name [WITH options]
 *
 * CREATE PUBLICATION FOR ALL TABLES [WITH options]
 *
 * CREATE PUBLICATION FOR pub_obj [, ...] [WITH options]
 *
 * pub_obj is one of:
 *
 *		TABLE table [, ...]
 *		TABLES IN SCHEMA schema [, ...]
 *
 *****************************************************************************/

CreatePublicationStmt:
			CREATE PUBLICATION name opt_definition
				{
					sq_create_publication_stmt_t *n = make_node<sq_create_publication_stmt_t>();

					n->pubname = $3;
					n->options = $4;
					$$ = (sq_node_t *) n;
				}
			| CREATE PUBLICATION name FOR ALL TABLES opt_definition
				{
					sq_create_publication_stmt_t *n = make_node<sq_create_publication_stmt_t>();

					n->pubname = $3;
					n->options = $7;
					n->for_all_tables = true;
					$$ = (sq_node_t *) n;
				}
			| CREATE PUBLICATION name FOR pub_obj_list opt_definition
				{
					sq_create_publication_stmt_t *n = make_node<sq_create_publication_stmt_t>();

					n->pubname = $3;
					n->options = $6;
					n->pubobjects = (list_ref_t ) $5;
					preprocess_pubobj_list(n->pubobjects, yyscanner);
					$$ = (sq_node_t *) n;
				}
		;

/*
 * FOR TABLE and FOR TABLES IN SCHEMA specifications
 *
 * This rule parses publication objects with and without keyword prefixes.
 *
 * The actual type of the object without keyword prefix depends on the previous
 * one with keyword prefix. It will be preprocessed in preprocess_pubobj_list().
 *
 * For the object without keyword prefix, we cannot just use relation_expr here,
 * because some extended expressions in relation_expr cannot be used as a
 * schemaname and we cannot differentiate it. So, we extract the rules from
 * relation_expr here.
 */
PublicationObjSpec:
			TABLE relation_expr opt_column_list OptWhereClause
				{
					$$ = make_node<sq_publication_obj_spec_t>();
					$$->pubobjtype = SQ_PUBLICATIONOBJ_TABLE;
					$$->pubtable = make_node<sq_publication_table_t>();
					$$->pubtable->relation = $2;
					$$->pubtable->columns = $3;
					$$->pubtable->where_clause = $4;
				}
			| TABLES IN_P SCHEMA ColId
				{
					$$ = make_node<sq_publication_obj_spec_t>();
					$$->pubobjtype = SQ_PUBLICATIONOBJ_TABLES_IN_SCHEMA;
					$$->name = $4;
					$$->location = @4;
				}
			| TABLES IN_P SCHEMA CURRENT_SCHEMA
				{
					$$ = make_node<sq_publication_obj_spec_t>();
					$$->pubobjtype = SQ_PUBLICATIONOBJ_TABLES_IN_CUR_SCHEMA;
					$$->location = @4;
				}
			| ColId opt_column_list OptWhereClause
				{
					$$ = make_node<sq_publication_obj_spec_t>();
					$$->pubobjtype = SQ_PUBLICATIONOBJ_CONTINUATION;
					/*
					 * If either a row filter or column list is specified, create
					 * a PublicationTable object.
					 */
					if ($2 || $3)
					{
						/*
						 * The OptWhereClause must be stored here but it is
						 * valid only for tables. For non-table objects, an
						 * error will be thrown later via
						 * preprocess_pubobj_list().
						 */
						$$->pubtable = make_node<sq_publication_table_t>();
						$$->pubtable->relation = make_range_var(NULL, $1, @1);
						$$->pubtable->columns = $2;
						$$->pubtable->where_clause = $3;
					}
					else
					{
						$$->name = $1;
					}
					$$->location = @1;
				}
			| ColId indirection opt_column_list OptWhereClause
				{
					$$ = make_node<sq_publication_obj_spec_t>();
					$$->pubobjtype = SQ_PUBLICATIONOBJ_CONTINUATION;
					$$->pubtable = make_node<sq_publication_table_t>();
					$$->pubtable->relation = make_range_var_from_qualified_name($1, $2, @1, yyscanner);
					$$->pubtable->columns = $3;
					$$->pubtable->where_clause = $4;
					$$->location = @1;
				}
			/* grammar like tablename * , ONLY tablename, ONLY ( tablename ) */
			| extended_relation_expr opt_column_list OptWhereClause
				{
					$$ = make_node<sq_publication_obj_spec_t>();
					$$->pubobjtype = SQ_PUBLICATIONOBJ_CONTINUATION;
					$$->pubtable = make_node<sq_publication_table_t>();
					$$->pubtable->relation = $1;
					$$->pubtable->columns = $2;
					$$->pubtable->where_clause = $3;
				}
			| CURRENT_SCHEMA
				{
					$$ = make_node<sq_publication_obj_spec_t>();
					$$->pubobjtype = SQ_PUBLICATIONOBJ_CONTINUATION;
					$$->location = @1;
				}
				;

pub_obj_list:	PublicationObjSpec
					{ $$ = list_make1($1); }
			| pub_obj_list ',' PublicationObjSpec
					{ $$ = lappend($1, $3); }
	;

/*****************************************************************************
 *
 * ALTER PUBLICATION name SET ( options )
 *
 * ALTER PUBLICATION name ADD pub_obj [, ...]
 *
 * ALTER PUBLICATION name DROP pub_obj [, ...]
 *
 * ALTER PUBLICATION name SET pub_obj [, ...]
 *
 * pub_obj is one of:
 *
 *		TABLE table_name [, ...]
 *		TABLES IN SCHEMA schema_name [, ...]
 *
 *****************************************************************************/

AlterPublicationStmt:
			ALTER PUBLICATION name SET definition
				{
					sq_alter_publication_stmt_t *n = make_node<sq_alter_publication_stmt_t>();

					n->pubname = $3;
					n->options = $5;
					$$ = (sq_node_t *) n;
				}
			| ALTER PUBLICATION name ADD_P pub_obj_list
				{
					sq_alter_publication_stmt_t *n = make_node<sq_alter_publication_stmt_t>();

					n->pubname = $3;
					n->pubobjects = $5;
					preprocess_pubobj_list(n->pubobjects, yyscanner);
					n->action = SQ_AP_AddObjects;
					$$ = (sq_node_t *) n;
				}
			| ALTER PUBLICATION name SET pub_obj_list
				{
					sq_alter_publication_stmt_t *n = make_node<sq_alter_publication_stmt_t>();

					n->pubname = $3;
					n->pubobjects = $5;
					preprocess_pubobj_list(n->pubobjects, yyscanner);
					n->action = SQ_AP_SetObjects;
					$$ = (sq_node_t *) n;
				}
			| ALTER PUBLICATION name DROP pub_obj_list
				{
					sq_alter_publication_stmt_t *n = make_node<sq_alter_publication_stmt_t>();

					n->pubname = $3;
					n->pubobjects = $5;
					preprocess_pubobj_list(n->pubobjects, yyscanner);
					n->action = SQ_AP_DropObjects;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 * CREATE SUBSCRIPTION name ...
 *
 *****************************************************************************/

CreateSubscriptionStmt:
			CREATE SUBSCRIPTION name CONNECTION Sconst PUBLICATION name_list opt_definition
				{
					sq_create_subscription_stmt_t *n =
						make_node<sq_create_subscription_stmt_t>();
					n->subname = $3;
					n->conninfo = $5;
					n->publication = $7;
					n->options = $8;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 * ALTER SUBSCRIPTION name ...
 *
 *****************************************************************************/

AlterSubscriptionStmt:
			ALTER SUBSCRIPTION name SET definition
				{
					sq_alter_subscription_stmt_t *n =
						make_node<sq_alter_subscription_stmt_t>();

					n->kind = SQ_ALTER_SUBSCRIPTION_OPTIONS;
					n->subname = $3;
					n->options = $5;
					$$ = (sq_node_t *) n;
				}
			| ALTER SUBSCRIPTION name CONNECTION Sconst
				{
					sq_alter_subscription_stmt_t *n =
						make_node<sq_alter_subscription_stmt_t>();

					n->kind = SQ_ALTER_SUBSCRIPTION_CONNECTION;
					n->subname = $3;
					n->conninfo = $5;
					$$ = (sq_node_t *) n;
				}
			| ALTER SUBSCRIPTION name REFRESH PUBLICATION opt_definition
				{
					sq_alter_subscription_stmt_t *n =
						make_node<sq_alter_subscription_stmt_t>();

					n->kind = SQ_ALTER_SUBSCRIPTION_REFRESH;
					n->subname = $3;
					n->options = $6;
					$$ = (sq_node_t *) n;
				}
			| ALTER SUBSCRIPTION name ADD_P PUBLICATION name_list opt_definition
				{
					sq_alter_subscription_stmt_t *n =
						make_node<sq_alter_subscription_stmt_t>();

					n->kind = SQ_ALTER_SUBSCRIPTION_ADD_PUBLICATION;
					n->subname = $3;
					n->publication = $6;
					n->options = $7;
					$$ = (sq_node_t *) n;
				}
			| ALTER SUBSCRIPTION name DROP PUBLICATION name_list opt_definition
				{
					sq_alter_subscription_stmt_t *n =
						make_node<sq_alter_subscription_stmt_t>();

					n->kind = SQ_ALTER_SUBSCRIPTION_DROP_PUBLICATION;
					n->subname = $3;
					n->publication = $6;
					n->options = $7;
					$$ = (sq_node_t *) n;
				}
			| ALTER SUBSCRIPTION name SET PUBLICATION name_list opt_definition
				{
					sq_alter_subscription_stmt_t *n =
						make_node<sq_alter_subscription_stmt_t>();

					n->kind = SQ_ALTER_SUBSCRIPTION_SET_PUBLICATION;
					n->subname = $3;
					n->publication = $6;
					n->options = $7;
					$$ = (sq_node_t *) n;
				}
			| ALTER SUBSCRIPTION name ENABLE_P
				{
					sq_alter_subscription_stmt_t *n =
						make_node<sq_alter_subscription_stmt_t>();

					n->kind = SQ_ALTER_SUBSCRIPTION_ENABLED;
					n->subname = $3;
					n->options = list_make1(make_def_elem("enabled",
											(sq_node_t *) make_boolean(true), @1));
					$$ = (sq_node_t *) n;
				}
			| ALTER SUBSCRIPTION name DISABLE_P
				{
					sq_alter_subscription_stmt_t *n =
						make_node<sq_alter_subscription_stmt_t>();

					n->kind = SQ_ALTER_SUBSCRIPTION_ENABLED;
					n->subname = $3;
					n->options = list_make1(make_def_elem("enabled",
											(sq_node_t *) make_boolean(false), @1));
					$$ = (sq_node_t *) n;
				}
			| ALTER SUBSCRIPTION name SKIP definition
				{
					sq_alter_subscription_stmt_t *n =
						make_node<sq_alter_subscription_stmt_t>();

					n->kind = SQ_ALTER_SUBSCRIPTION_SKIP;
					n->subname = $3;
					n->options = $5;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 * DROP SUBSCRIPTION [ IF EXISTS ] name
 *
 *****************************************************************************/

DropSubscriptionStmt: DROP SUBSCRIPTION name opt_drop_behavior
				{
					sq_drop_subscription_stmt_t *n = make_node<sq_drop_subscription_stmt_t>();

					n->subname = $3;
					n->missing_ok = false;
					n->behavior = $4;
					$$ = (sq_node_t *) n;
				}
				|  DROP SUBSCRIPTION IF_P EXISTS name opt_drop_behavior
				{
					sq_drop_subscription_stmt_t *n = make_node<sq_drop_subscription_stmt_t>();

					n->subname = $5;
					n->missing_ok = true;
					n->behavior = $6;
					$$ = (sq_node_t *) n;
				}
		;

/*****************************************************************************
 *
 *		QUERY:	Define Rewrite Rule
 *
 *****************************************************************************/

RuleStmt:	CREATE opt_or_replace RULE name AS
			ON event TO qualified_name where_clause
			DO opt_instead RuleActionList
				{
					sq_rule_stmt_t   *n = make_node<sq_rule_stmt_t>();

					n->replace = $2;
					n->relation = $9;
					n->rulename = $4;
					n->where_clause = $10;
					n->event = (sq_cmd_type_t) $7;
					n->instead = $12;
					n->actions = $13;
					$$ = (sq_node_t *) n;
				}
		;

RuleActionList:
			NOTHING									{ $$ = NIL; }
			| RuleActionStmt						{ $$ = list_make1($1); }
			| '(' RuleActionMulti ')'				{ $$ = $2; }
		;

/* the thrashing around here is to discard "empty" statements... */
RuleActionMulti:
			RuleActionMulti ';' RuleActionStmtOrEmpty
				{ if ($3 != NULL)
					$$ = lappend($1, $3);
				  else
					$$ = $1;
				}
			| RuleActionStmtOrEmpty
				{ if ($1 != NULL)
					$$ = list_make1($1);
				  else
					$$ = NIL;
				}
		;

RuleActionStmt:
			SelectStmt
			| InsertStmt
			| UpdateStmt
			| DeleteStmt
			| NotifyStmt
		;

RuleActionStmtOrEmpty:
			RuleActionStmt							{ $$ = $1; }
			|	/*EMPTY*/							{ $$ = NULL; }
		;

event:		SELECT									{ $$ = SQ_CMD_SELECT; }
			| UPDATE								{ $$ = SQ_CMD_UPDATE; }
			| DELETE_P								{ $$ = SQ_CMD_DELETE; }
			| INSERT								{ $$ = SQ_CMD_INSERT; }
		 ;

opt_instead:
			INSTEAD									{ $$ = true; }
			| ALSO									{ $$ = false; }
			| /*EMPTY*/								{ $$ = false; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				NOTIFY <identifier> can appear both in rule bodies and
 *				as a query-level command
 *
 *****************************************************************************/

NotifyStmt: NOTIFY ColId notify_payload
				{
					sq_notify_stmt_t *n = make_node<sq_notify_stmt_t>();

					n->conditionname = $2;
					n->payload = $3;
					$$ = (sq_node_t *) n;
				}
		;

notify_payload:
			',' Sconst							{ $$ = $2; }
			| /*EMPTY*/							{ $$ = NULL; }
		;

ListenStmt: LISTEN ColId
				{
					sq_listen_stmt_t *n = make_node<sq_listen_stmt_t>();

					n->conditionname = $2;
					$$ = (sq_node_t *) n;
				}
		;

UnlistenStmt:
			UNLISTEN ColId
				{
					sq_unlisten_stmt_t *n = make_node<sq_unlisten_stmt_t>();

					n->conditionname = $2;
					$$ = (sq_node_t *) n;
				}
			| UNLISTEN '*'
				{
					sq_unlisten_stmt_t *n = make_node<sq_unlisten_stmt_t>();

					n->conditionname = NULL;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 *		Transactions:
 *
 *		BEGIN / COMMIT / ROLLBACK
 *		(also older versions END / ABORT)
 *
 *****************************************************************************/

TransactionStmt:
			ABORT_P opt_transaction opt_transaction_chain
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_ROLLBACK;
					n->options = NIL;
					n->chain = $3;
					n->location = -1;
					$$ = (sq_node_t *) n;
				}
			| START TRANSACTION transaction_mode_list_or_empty
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_START;
					n->options = $3;
					n->location = -1;
					$$ = (sq_node_t *) n;
				}
			| COMMIT opt_transaction opt_transaction_chain
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_COMMIT;
					n->options = NIL;
					n->chain = $3;
					n->location = -1;
					$$ = (sq_node_t *) n;
				}
			| ROLLBACK opt_transaction opt_transaction_chain
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_ROLLBACK;
					n->options = NIL;
					n->chain = $3;
					n->location = -1;
					$$ = (sq_node_t *) n;
				}
			| SAVEPOINT ColId
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_SAVEPOINT;
					n->savepoint_name = $2;
					n->location = @2;
					$$ = (sq_node_t *) n;
				}
			| RELEASE SAVEPOINT ColId
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_RELEASE;
					n->savepoint_name = $3;
					n->location = @3;
					$$ = (sq_node_t *) n;
				}
			| RELEASE ColId
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_RELEASE;
					n->savepoint_name = $2;
					n->location = @2;
					$$ = (sq_node_t *) n;
				}
			| ROLLBACK opt_transaction TO SAVEPOINT ColId
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_ROLLBACK_TO;
					n->savepoint_name = $5;
					n->location = @5;
					$$ = (sq_node_t *) n;
				}
			| ROLLBACK opt_transaction TO ColId
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_ROLLBACK_TO;
					n->savepoint_name = $4;
					n->location = @4;
					$$ = (sq_node_t *) n;
				}
			| PREPARE TRANSACTION Sconst
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_PREPARE;
					n->gid = $3;
					n->location = @3;
					$$ = (sq_node_t *) n;
				}
			| COMMIT PREPARED Sconst
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_COMMIT_PREPARED;
					n->gid = $3;
					n->location = @3;
					$$ = (sq_node_t *) n;
				}
			| ROLLBACK PREPARED Sconst
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_ROLLBACK_PREPARED;
					n->gid = $3;
					n->location = @3;
					$$ = (sq_node_t *) n;
				}
		;

TransactionStmtLegacy:
			BEGIN_P opt_transaction transaction_mode_list_or_empty
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_BEGIN;
					n->options = $3;
					n->location = -1;
					$$ = (sq_node_t *) n;
				}
			| END_P opt_transaction opt_transaction_chain
				{
					sq_transaction_stmt_t *n = make_node<sq_transaction_stmt_t>();

					n->kind = SQ_TRANS_STMT_COMMIT;
					n->options = NIL;
					n->chain = $3;
					n->location = -1;
					$$ = (sq_node_t *) n;
				}
		;

opt_transaction:	WORK
			| TRANSACTION
			| /*EMPTY*/
		;

transaction_mode_item:
			ISOLATION LEVEL iso_level
					{ $$ = make_def_elem("transaction_isolation",
									   make_string_const($3, @3), @1); }
			| READ ONLY
					{ $$ = make_def_elem("transaction_read_only",
									   make_int_const(true, @1), @1); }
			| READ WRITE
					{ $$ = make_def_elem("transaction_read_only",
									   make_int_const(false, @1), @1); }
			| DEFERRABLE
					{ $$ = make_def_elem("transaction_deferrable",
									   make_int_const(true, @1), @1); }
			| NOT DEFERRABLE
					{ $$ = make_def_elem("transaction_deferrable",
									   make_int_const(false, @1), @1); }
		;

/* Syntax with commas is SQL-spec, without commas is Postgres historical */
transaction_mode_list:
			transaction_mode_item
					{ $$ = list_make1($1); }
			| transaction_mode_list ',' transaction_mode_item
					{ $$ = lappend($1, $3); }
			| transaction_mode_list transaction_mode_item
					{ $$ = lappend($1, $2); }
		;

transaction_mode_list_or_empty:
			transaction_mode_list
			| /* EMPTY */
					{ $$ = NIL; }
		;

opt_transaction_chain:
			AND CHAIN		{ $$ = true; }
			| AND NO CHAIN	{ $$ = false; }
			| /* EMPTY */	{ $$ = false; }
		;


/*****************************************************************************
 *
 *	QUERY:
 *		CREATE [ OR REPLACE ] [ TEMP ] VIEW <viewname> '('target-list ')'
 *			AS <query> [ WITH [ CASCADED | LOCAL ] CHECK OPTION ]
 *
 *****************************************************************************/

ViewStmt: CREATE OptTemp VIEW qualified_name opt_column_list opt_reloptions
				AS SelectStmt opt_check_option
				{
					sq_view_stmt_t   *n = make_node<sq_view_stmt_t>();

					n->view = $4;
					n->view->relpersistence = $2;
					n->aliases = $5;
					n->query = $8;
					n->replace = false;
					n->options = $6;
					n->with_check_option = (sq_view_check_option_t) $9;
					$$ = (sq_node_t *) n;
				}
		| CREATE OR REPLACE OptTemp VIEW qualified_name opt_column_list opt_reloptions
				AS SelectStmt opt_check_option
				{
					sq_view_stmt_t   *n = make_node<sq_view_stmt_t>();

					n->view = $6;
					n->view->relpersistence = $4;
					n->aliases = $7;
					n->query = $10;
					n->replace = true;
					n->options = $8;
					n->with_check_option = (sq_view_check_option_t) $11;
					$$ = (sq_node_t *) n;
				}
		| CREATE OptTemp RECURSIVE VIEW qualified_name '(' columnList ')' opt_reloptions
				AS SelectStmt opt_check_option
				{
					sq_view_stmt_t   *n = make_node<sq_view_stmt_t>();

					n->view = $5;
					n->view->relpersistence = $2;
					n->aliases = $7;
					n->query = make_recursive_view_select(n->view->relname, n->aliases, $11);
					n->replace = false;
					n->options = $9;
					n->with_check_option = (sq_view_check_option_t) $12;
					if (n->with_check_option != SQ_NO_CHECK_OPTION)
						raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@12)}, "WITH CHECK OPTION not supported on recursive views");
					$$ = (sq_node_t *) n;
				}
		| CREATE OR REPLACE OptTemp RECURSIVE VIEW qualified_name '(' columnList ')' opt_reloptions
				AS SelectStmt opt_check_option
				{
					sq_view_stmt_t   *n = make_node<sq_view_stmt_t>();

					n->view = $7;
					n->view->relpersistence = $4;
					n->aliases = $9;
					n->query = make_recursive_view_select(n->view->relname, n->aliases, $13);
					n->replace = true;
					n->options = $11;
					n->with_check_option = (sq_view_check_option_t) $14;
					if (n->with_check_option != SQ_NO_CHECK_OPTION)
						raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@14)}, "WITH CHECK OPTION not supported on recursive views");
					$$ = (sq_node_t *) n;
				}
		;

opt_check_option:
		WITH CHECK OPTION				{ $$ = SQ_CASCADED_CHECK_OPTION; }
		| WITH CASCADED CHECK OPTION	{ $$ = SQ_CASCADED_CHECK_OPTION; }
		| WITH LOCAL CHECK OPTION		{ $$ = SQ_LOCAL_CHECK_OPTION; }
		| /* EMPTY */					{ $$ = SQ_NO_CHECK_OPTION; }
		;

/*****************************************************************************
 *
 *		QUERY:
 *				LOAD "filename"
 *
 *****************************************************************************/

LoadStmt:	LOAD file_name
				{
					sq_load_stmt_t   *n = make_node<sq_load_stmt_t>();

					n->filename = $2;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 *		CREATE DATABASE
 *
 *****************************************************************************/

CreatedbStmt:
			CREATE DATABASE name opt_with createdb_opt_list
				{
					sq_createdb_stmt_t *n = make_node<sq_createdb_stmt_t>();

					n->dbname = $3;
					n->options = $5;
					$$ = (sq_node_t *) n;
				}
		;

createdb_opt_list:
			createdb_opt_items						{ $$ = $1; }
			| /* EMPTY */							{ $$ = NIL; }
		;

createdb_opt_items:
			createdb_opt_item						{ $$ = list_make1($1); }
			| createdb_opt_items createdb_opt_item	{ $$ = lappend($1, $2); }
		;

createdb_opt_item:
			createdb_opt_name opt_equal NumericOnly
				{
					$$ = make_def_elem($1, $3, @1);
				}
			| createdb_opt_name opt_equal opt_boolean_or_string
				{
					$$ = make_def_elem($1, (sq_node_t *) make_string($3), @1);
				}
			| createdb_opt_name opt_equal DEFAULT
				{
					$$ = make_def_elem($1, NULL, @1);
				}
		;

/*
 * Ideally we'd use ColId here, but that causes shift/reduce conflicts against
 * the ALTER DATABASE SET/RESET syntaxes.  Instead call out specific keywords
 * we need, and allow IDENT so that database option names don't have to be
 * parser keywords unless they are already keywords for other reasons.
 *
 * XXX this coding technique is fragile since if someone makes a formerly
 * non-keyword option name into a keyword and forgets to add it here, the
 * option will silently break.  Best defense is to provide a regression test
 * exercising every such option, at least at the syntax level.
 */
createdb_opt_name:
			IDENT							{ $$ = $1; }
			| CONNECTION LIMIT				{ $$ = tree_strdup("connection_limit"); }
			| ENCODING						{ $$ = tree_strdup($1); }
			| LOCATION						{ $$ = tree_strdup($1); }
			| OWNER							{ $$ = tree_strdup($1); }
			| TABLESPACE					{ $$ = tree_strdup($1); }
			| TEMPLATE						{ $$ = tree_strdup($1); }
		;

/*
 *	Though the equals sign doesn't match other WITH options, pg_dump uses
 *	equals for backward compatibility, and it doesn't seem worth removing it.
 */
opt_equal:	'='
			| /*EMPTY*/
		;


/*****************************************************************************
 *
 *		ALTER DATABASE
 *
 *****************************************************************************/

AlterDatabaseStmt:
			ALTER DATABASE name WITH createdb_opt_list
				 {
					sq_alter_database_stmt_t *n = make_node<sq_alter_database_stmt_t>();

					n->dbname = $3;
					n->options = $5;
					$$ = (sq_node_t *) n;
				 }
			| ALTER DATABASE name createdb_opt_list
				 {
					sq_alter_database_stmt_t *n = make_node<sq_alter_database_stmt_t>();

					n->dbname = $3;
					n->options = $4;
					$$ = (sq_node_t *) n;
				 }
			| ALTER DATABASE name SET TABLESPACE name
				 {
					sq_alter_database_stmt_t *n = make_node<sq_alter_database_stmt_t>();

					n->dbname = $3;
					n->options = list_make1(make_def_elem("tablespace",
														(sq_node_t *) make_string($6), @6));
					$$ = (sq_node_t *) n;
				 }
			| ALTER DATABASE name REFRESH COLLATION VERSION_P
				 {
					sq_alter_database_refresh_coll_stmt_t *n = make_node<sq_alter_database_refresh_coll_stmt_t>();

					n->dbname = $3;
					$$ = (sq_node_t *) n;
				 }
		;

AlterDatabaseSetStmt:
			ALTER DATABASE name SetResetClause
				{
					sq_alter_database_set_stmt_t *n = make_node<sq_alter_database_set_stmt_t>();

					n->dbname = $3;
					n->setstmt = $4;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 *		DROP DATABASE [ IF EXISTS ] dbname [ [ WITH ] ( options ) ]
 *
 * This is implicitly CASCADE, no need for drop behavior
 *****************************************************************************/

DropdbStmt: DROP DATABASE name
				{
					sq_dropdb_stmt_t *n = make_node<sq_dropdb_stmt_t>();

					n->dbname = $3;
					n->missing_ok = false;
					n->options = NIL;
					$$ = (sq_node_t *) n;
				}
			| DROP DATABASE IF_P EXISTS name
				{
					sq_dropdb_stmt_t *n = make_node<sq_dropdb_stmt_t>();

					n->dbname = $5;
					n->missing_ok = true;
					n->options = NIL;
					$$ = (sq_node_t *) n;
				}
			| DROP DATABASE name opt_with '(' drop_option_list ')'
				{
					sq_dropdb_stmt_t *n = make_node<sq_dropdb_stmt_t>();

					n->dbname = $3;
					n->missing_ok = false;
					n->options = $6;
					$$ = (sq_node_t *) n;
				}
			| DROP DATABASE IF_P EXISTS name opt_with '(' drop_option_list ')'
				{
					sq_dropdb_stmt_t *n = make_node<sq_dropdb_stmt_t>();

					n->dbname = $5;
					n->missing_ok = true;
					n->options = $8;
					$$ = (sq_node_t *) n;
				}
		;

drop_option_list:
			drop_option
				{
					$$ = list_make1((sq_node_t *) $1);
				}
			| drop_option_list ',' drop_option
				{
					$$ = lappend($1, (sq_node_t *) $3);
				}
		;

/*
 * Currently only the FORCE option is supported, but the syntax is designed
 * to be extensible so that we can add more options in the future if required.
 */
drop_option:
			FORCE
				{
					$$ = make_def_elem("force", NULL, @1);
				}
		;

/*****************************************************************************
 *
 *		ALTER COLLATION
 *
 *****************************************************************************/

AlterCollationStmt: ALTER COLLATION any_name REFRESH VERSION_P
				{
					sq_alter_collation_stmt_t *n = make_node<sq_alter_collation_stmt_t>();

					n->collname = $3;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 *		ALTER SYSTEM
 *
 * This is used to change configuration parameters persistently.
 *****************************************************************************/

AlterSystemStmt:
			ALTER SYSTEM_P SET generic_set
				{
					sq_alter_system_stmt_t *n = make_node<sq_alter_system_stmt_t>();

					n->setstmt = $4;
					$$ = (sq_node_t *) n;
				}
			| ALTER SYSTEM_P RESET generic_reset
				{
					sq_alter_system_stmt_t *n = make_node<sq_alter_system_stmt_t>();

					n->setstmt = $4;
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 * Manipulate a domain
 *
 *****************************************************************************/

CreateDomainStmt:
			CREATE DOMAIN_P any_name opt_as Typename ColQualList
				{
					sq_create_domain_stmt_t *n = make_node<sq_create_domain_stmt_t>();

					n->domainname = $3;
					n->type_name = $5;
					split_col_qual_list($6, &n->constraints, &n->coll_clause,
									 yyscanner);
					$$ = (sq_node_t *) n;
				}
		;

AlterDomainStmt:
			/* ALTER DOMAIN <domain> {SET DEFAULT <expr>|DROP DEFAULT} */
			ALTER DOMAIN_P any_name alter_column_default
				{
					sq_alter_domain_stmt_t *n = make_node<sq_alter_domain_stmt_t>();

					n->subtype = 'T';
					n->type_name = $3;
					n->def = $4;
					$$ = (sq_node_t *) n;
				}
			/* ALTER DOMAIN <domain> DROP NOT NULL */
			| ALTER DOMAIN_P any_name DROP NOT NULL_P
				{
					sq_alter_domain_stmt_t *n = make_node<sq_alter_domain_stmt_t>();

					n->subtype = 'N';
					n->type_name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER DOMAIN <domain> SET NOT NULL */
			| ALTER DOMAIN_P any_name SET NOT NULL_P
				{
					sq_alter_domain_stmt_t *n = make_node<sq_alter_domain_stmt_t>();

					n->subtype = 'O';
					n->type_name = $3;
					$$ = (sq_node_t *) n;
				}
			/* ALTER DOMAIN <domain> ADD CONSTRAINT ... */
			| ALTER DOMAIN_P any_name ADD_P DomainConstraint
				{
					sq_alter_domain_stmt_t *n = make_node<sq_alter_domain_stmt_t>();

					n->subtype = 'C';
					n->type_name = $3;
					n->def = $5;
					$$ = (sq_node_t *) n;
				}
			/* ALTER DOMAIN <domain> DROP CONSTRAINT <name> [RESTRICT|CASCADE] */
			| ALTER DOMAIN_P any_name DROP CONSTRAINT name opt_drop_behavior
				{
					sq_alter_domain_stmt_t *n = make_node<sq_alter_domain_stmt_t>();

					n->subtype = 'X';
					n->type_name = $3;
					n->name = $6;
					n->behavior = $7;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			/* ALTER DOMAIN <domain> DROP CONSTRAINT IF EXISTS <name> [RESTRICT|CASCADE] */
			| ALTER DOMAIN_P any_name DROP CONSTRAINT IF_P EXISTS name opt_drop_behavior
				{
					sq_alter_domain_stmt_t *n = make_node<sq_alter_domain_stmt_t>();

					n->subtype = 'X';
					n->type_name = $3;
					n->name = $8;
					n->behavior = $9;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
			/* ALTER DOMAIN <domain> VALIDATE CONSTRAINT <name> */
			| ALTER DOMAIN_P any_name VALIDATE CONSTRAINT name
				{
					sq_alter_domain_stmt_t *n = make_node<sq_alter_domain_stmt_t>();

					n->subtype = 'V';
					n->type_name = $3;
					n->name = $6;
					$$ = (sq_node_t *) n;
				}
			;

opt_as:		AS
			| /* EMPTY */
		;


/*****************************************************************************
 *
 * Manipulate a text search dictionary or configuration
 *
 *****************************************************************************/

AlterTSDictionaryStmt:
			ALTER TEXT_P SEARCH DICTIONARY any_name definition
				{
					sq_alter_ts_dictionary_stmt_t *n = make_node<sq_alter_ts_dictionary_stmt_t>();

					n->dictname = $5;
					n->options = $6;
					$$ = (sq_node_t *) n;
				}
		;

AlterTSConfigurationStmt:
			ALTER TEXT_P SEARCH CONFIGURATION any_name ADD_P MAPPING FOR name_list any_with any_name_list
				{
					sq_alter_ts_configuration_stmt_t *n = make_node<sq_alter_ts_configuration_stmt_t>();

					n->kind = SQ_ALTER_TSCONFIG_ADD_MAPPING;
					n->cfgname = $5;
					n->tokentype = $9;
					n->dicts = $11;
					n->override = false;
					n->replace = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING FOR name_list any_with any_name_list
				{
					sq_alter_ts_configuration_stmt_t *n = make_node<sq_alter_ts_configuration_stmt_t>();

					n->kind = SQ_ALTER_TSCONFIG_ALTER_MAPPING_FOR_TOKEN;
					n->cfgname = $5;
					n->tokentype = $9;
					n->dicts = $11;
					n->override = true;
					n->replace = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING REPLACE any_name any_with any_name
				{
					sq_alter_ts_configuration_stmt_t *n = make_node<sq_alter_ts_configuration_stmt_t>();

					n->kind = SQ_ALTER_TSCONFIG_REPLACE_DICT;
					n->cfgname = $5;
					n->tokentype = NIL;
					n->dicts = list_make2($9,$11);
					n->override = false;
					n->replace = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name ALTER MAPPING FOR name_list REPLACE any_name any_with any_name
				{
					sq_alter_ts_configuration_stmt_t *n = make_node<sq_alter_ts_configuration_stmt_t>();

					n->kind = SQ_ALTER_TSCONFIG_REPLACE_DICT_FOR_TOKEN;
					n->cfgname = $5;
					n->tokentype = $9;
					n->dicts = list_make2($11,$13);
					n->override = false;
					n->replace = true;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name DROP MAPPING FOR name_list
				{
					sq_alter_ts_configuration_stmt_t *n = make_node<sq_alter_ts_configuration_stmt_t>();

					n->kind = SQ_ALTER_TSCONFIG_DROP_MAPPING;
					n->cfgname = $5;
					n->tokentype = $9;
					n->missing_ok = false;
					$$ = (sq_node_t *) n;
				}
			| ALTER TEXT_P SEARCH CONFIGURATION any_name DROP MAPPING IF_P EXISTS FOR name_list
				{
					sq_alter_ts_configuration_stmt_t *n = make_node<sq_alter_ts_configuration_stmt_t>();

					n->kind = SQ_ALTER_TSCONFIG_DROP_MAPPING;
					n->cfgname = $5;
					n->tokentype = $11;
					n->missing_ok = true;
					$$ = (sq_node_t *) n;
				}
		;

/* Use this if TIME or ORDINALITY after WITH should be taken as an identifier */
any_with:	WITH
			| WITH_LA
		;


/*****************************************************************************
 *
 * Manipulate a conversion
 *
 *		CREATE [DEFAULT] CONVERSION <conversion_name>
 *		FOR <encoding_name> TO <encoding_name> FROM <func_name>
 *
 *****************************************************************************/

CreateConversionStmt:
			CREATE opt_default CONVERSION_P any_name FOR Sconst
			TO Sconst FROM any_name
			{
				sq_create_conversion_stmt_t *n = make_node<sq_create_conversion_stmt_t>();

				n->conversion_name = $4;
				n->for_encoding_name = $6;
				n->to_encoding_name = $8;
				n->func_name = $10;
				n->def = $2;
				$$ = (sq_node_t *) n;
			}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				CLUSTER (options) [ <qualified_name> [ USING <index_name> ] ]
 *				CLUSTER [VERBOSE] [ <qualified_name> [ USING <index_name> ] ]
 *				CLUSTER [VERBOSE] <index_name> ON <qualified_name> (for pre-8.3)
 *
 *****************************************************************************/

ClusterStmt:
			CLUSTER '(' utility_option_list ')' qualified_name cluster_index_specification
				{
					sq_cluster_stmt_t *n = make_node<sq_cluster_stmt_t>();

					n->relation = $5;
					n->indexname = $6;
					n->params = $3;
					$$ = (sq_node_t *) n;
				}
			| CLUSTER '(' utility_option_list ')'
				{
					sq_cluster_stmt_t *n = make_node<sq_cluster_stmt_t>();

					n->relation = NULL;
					n->indexname = NULL;
					n->params = $3;
					$$ = (sq_node_t *) n;
				}
			/* unparenthesized VERBOSE kept for pre-14 compatibility */
			| CLUSTER opt_verbose qualified_name cluster_index_specification
				{
					sq_cluster_stmt_t *n = make_node<sq_cluster_stmt_t>();

					n->relation = $3;
					n->indexname = $4;
					n->params = NIL;
					if ($2)
						n->params = lappend(n->params, make_def_elem("verbose", NULL, @2));
					$$ = (sq_node_t *) n;
				}
			/* unparenthesized VERBOSE kept for pre-17 compatibility */
			| CLUSTER opt_verbose
				{
					sq_cluster_stmt_t *n = make_node<sq_cluster_stmt_t>();

					n->relation = NULL;
					n->indexname = NULL;
					n->params = NIL;
					if ($2)
						n->params = lappend(n->params, make_def_elem("verbose", NULL, @2));
					$$ = (sq_node_t *) n;
				}
			/* kept for pre-8.3 compatibility */
			| CLUSTER opt_verbose name ON qualified_name
				{
					sq_cluster_stmt_t *n = make_node<sq_cluster_stmt_t>();

					n->relation = $5;
					n->indexname = $3;
					n->params = NIL;
					if ($2)
						n->params = lappend(n->params, make_def_elem("verbose", NULL, @2));
					$$ = (sq_node_t *) n;
				}
		;

cluster_index_specification:
			USING name				{ $$ = $2; }
			| /*EMPTY*/				{ $$ = NULL; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				VACUUM
 *				ANALYZE
 *
 *****************************************************************************/

VacuumStmt: VACUUM opt_full opt_freeze opt_verbose opt_analyze opt_vacuum_relation_list
				{
					sq_vacuum_stmt_t *n = make_node<sq_vacuum_stmt_t>();

					n->options = NIL;
					if ($2)
						n->options = lappend(n->options, make_def_elem("full", NULL, @2));
					if ($3)
						n->options = lappend(n->options, make_def_elem("freeze", NULL, @3));
					if ($4)
						n->options = lappend(n->options, make_def_elem("verbose", NULL, @4));
					if ($5)
						n->options = lappend(n->options, make_def_elem("analyze", NULL, @5));
					n->rels = $6;
					n->is_vacuumcmd = true;
					$$ = (sq_node_t *) n;
				}
			| VACUUM '(' utility_option_list ')' opt_vacuum_relation_list
				{
					sq_vacuum_stmt_t *n = make_node<sq_vacuum_stmt_t>();

					n->options = $3;
					n->rels = $5;
					n->is_vacuumcmd = true;
					$$ = (sq_node_t *) n;
				}
		;

AnalyzeStmt: analyze_keyword opt_verbose opt_vacuum_relation_list
				{
					sq_vacuum_stmt_t *n = make_node<sq_vacuum_stmt_t>();

					n->options = NIL;
					if ($2)
						n->options = lappend(n->options, make_def_elem("verbose", NULL, @2));
					n->rels = $3;
					n->is_vacuumcmd = false;
					$$ = (sq_node_t *) n;
				}
			| analyze_keyword '(' utility_option_list ')' opt_vacuum_relation_list
				{
					sq_vacuum_stmt_t *n = make_node<sq_vacuum_stmt_t>();

					n->options = $3;
					n->rels = $5;
					n->is_vacuumcmd = false;
					$$ = (sq_node_t *) n;
				}
		;

utility_option_list:
			utility_option_elem
				{
					$$ = list_make1($1);
				}
			| utility_option_list ',' utility_option_elem
				{
					$$ = lappend($1, $3);
				}
		;

analyze_keyword:
			ANALYZE
			| ANALYSE /* British */
		;

utility_option_elem:
			utility_option_name utility_option_arg
				{
					$$ = make_def_elem($1, $2, @1);
				}
		;

utility_option_name:
			NonReservedWord							{ $$ = $1; }
			| analyze_keyword						{ $$ = "analyze"; }
			| FORMAT_LA								{ $$ = "format"; }
		;

utility_option_arg:
			opt_boolean_or_string					{ $$ = (sq_node_t *) make_string($1); }
			| NumericOnly							{ $$ = (sq_node_t *) $1; }
			| /* EMPTY */							{ $$ = NULL; }
		;

opt_analyze:
			analyze_keyword							{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
		;

opt_verbose:
			VERBOSE									{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
		;

opt_full:	FULL									{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
		;

opt_freeze: FREEZE									{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
		;

opt_name_list:
			'(' name_list ')'						{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

vacuum_relation:
			relation_expr opt_name_list
				{
					$$ = (sq_node_t *) make_vacuum_relation($1, invalid_oid, $2);
				}
		;

vacuum_relation_list:
			vacuum_relation
					{ $$ = list_make1($1); }
			| vacuum_relation_list ',' vacuum_relation
					{ $$ = lappend($1, $3); }
		;

opt_vacuum_relation_list:
			vacuum_relation_list					{ $$ = $1; }
			| /*EMPTY*/								{ $$ = NIL; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				EXPLAIN [ANALYZE] [VERBOSE] query
 *				EXPLAIN ( options ) query
 *
 *****************************************************************************/

ExplainStmt:
		EXPLAIN ExplainableStmt
				{
					sq_explain_stmt_t *n = make_node<sq_explain_stmt_t>();

					n->query = $2;
					n->options = NIL;
					$$ = (sq_node_t *) n;
				}
		| EXPLAIN analyze_keyword opt_verbose ExplainableStmt
				{
					sq_explain_stmt_t *n = make_node<sq_explain_stmt_t>();

					n->query = $4;
					n->options = list_make1(make_def_elem("analyze", NULL, @2));
					if ($3)
						n->options = lappend(n->options, make_def_elem("verbose", NULL, @3));
					$$ = (sq_node_t *) n;
				}
		| EXPLAIN VERBOSE ExplainableStmt
				{
					sq_explain_stmt_t *n = make_node<sq_explain_stmt_t>();

					n->query = $3;
					n->options = list_make1(make_def_elem("verbose", NULL, @2));
					$$ = (sq_node_t *) n;
				}
		| EXPLAIN '(' utility_option_list ')' ExplainableStmt
				{
					sq_explain_stmt_t *n = make_node<sq_explain_stmt_t>();

					n->query = $5;
					n->options = $3;
					$$ = (sq_node_t *) n;
				}
		;

ExplainableStmt:
			SelectStmt
			| InsertStmt
			| UpdateStmt
			| DeleteStmt
			| MergeStmt
			| DeclareCursorStmt
			| CreateAsStmt
			| CreateMatViewStmt
			| RefreshMatViewStmt
			| ExecuteStmt					/* by default all are $$=$1 */
		;

/*****************************************************************************
 *
 *		QUERY:
 *				PREPARE <plan_name> [(args, ...)] AS <query>
 *
 *****************************************************************************/

PrepareStmt: PREPARE name prep_type_clause AS PreparableStmt
				{
					sq_prepare_stmt_t *n = make_node<sq_prepare_stmt_t>();

					n->name = $2;
					n->argtypes = $3;
					n->query = $5;
					$$ = (sq_node_t *) n;
				}
		;

prep_type_clause: '(' type_list ')'			{ $$ = $2; }
				| /* EMPTY */				{ $$ = NIL; }
		;

PreparableStmt:
			SelectStmt
			| InsertStmt
			| UpdateStmt
			| DeleteStmt
			| MergeStmt						/* by default all are $$=$1 */
		;

/*****************************************************************************
 *
 * EXECUTE <plan_name> [(params, ...)]
 * CREATE TABLE <name> AS EXECUTE <plan_name> [(params, ...)]
 *
 *****************************************************************************/

ExecuteStmt: EXECUTE name execute_param_clause
				{
					sq_execute_stmt_t *n = make_node<sq_execute_stmt_t>();

					n->name = $2;
					n->params = $3;
					$$ = (sq_node_t *) n;
				}
			| CREATE OptTemp TABLE create_as_target AS
				EXECUTE name execute_param_clause opt_with_data
				{
					sq_create_table_as_stmt_t *ctas = make_node<sq_create_table_as_stmt_t>();
					sq_execute_stmt_t *n = make_node<sq_execute_stmt_t>();

					n->name = $7;
					n->params = $8;
					ctas->query = (sq_node_t *) n;
					ctas->into = $4;
					ctas->objtype = SQ_OBJECT_TABLE;
					ctas->is_select_into = false;
					ctas->if_not_exists = false;
					/* cram additional flags into the IntoClause */
					$4->rel->relpersistence = $2;
					$4->skip_data = !($9);
					$$ = (sq_node_t *) ctas;
				}
			| CREATE OptTemp TABLE IF_P NOT EXISTS create_as_target AS
				EXECUTE name execute_param_clause opt_with_data
				{
					sq_create_table_as_stmt_t *ctas = make_node<sq_create_table_as_stmt_t>();
					sq_execute_stmt_t *n = make_node<sq_execute_stmt_t>();

					n->name = $10;
					n->params = $11;
					ctas->query = (sq_node_t *) n;
					ctas->into = $7;
					ctas->objtype = SQ_OBJECT_TABLE;
					ctas->is_select_into = false;
					ctas->if_not_exists = true;
					/* cram additional flags into the IntoClause */
					$7->rel->relpersistence = $2;
					$7->skip_data = !($12);
					$$ = (sq_node_t *) ctas;
				}
		;

execute_param_clause: '(' expr_list ')'				{ $$ = $2; }
					| /* EMPTY */					{ $$ = NIL; }
					;

/*****************************************************************************
 *
 *		QUERY:
 *				DEALLOCATE [PREPARE] <plan_name>
 *
 *****************************************************************************/

DeallocateStmt: DEALLOCATE name
					{
						sq_deallocate_stmt_t *n = make_node<sq_deallocate_stmt_t>();

						n->name = $2;
						n->isall = false;
						n->location = @2;
						$$ = (sq_node_t *) n;
					}
				| DEALLOCATE PREPARE name
					{
						sq_deallocate_stmt_t *n = make_node<sq_deallocate_stmt_t>();

						n->name = $3;
						n->isall = false;
						n->location = @3;
						$$ = (sq_node_t *) n;
					}
				| DEALLOCATE ALL
					{
						sq_deallocate_stmt_t *n = make_node<sq_deallocate_stmt_t>();

						n->name = NULL;
						n->isall = true;
						n->location = -1;
						$$ = (sq_node_t *) n;
					}
				| DEALLOCATE PREPARE ALL
					{
						sq_deallocate_stmt_t *n = make_node<sq_deallocate_stmt_t>();

						n->name = NULL;
						n->isall = true;
						n->location = -1;
						$$ = (sq_node_t *) n;
					}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				INSERT STATEMENTS
 *
 *****************************************************************************/

InsertStmt:
			opt_with_clause INSERT INTO insert_target insert_rest
			opt_on_conflict returning_clause
				{
					$5->relation = $4;
					$5->on_conflict_clause = $6;
					$5->returning_clause = $7;
					$5->with_clause = $1;
					$$ = (sq_node_t *) $5;
				}
		;

/*
 * Can't easily make AS optional here, because VALUES in insert_rest would
 * have a shift/reduce conflict with VALUES as an optional alias.  We could
 * easily allow unreserved_keywords as optional aliases, but that'd be an odd
 * divergence from other places.  So just require AS for now.
 */
insert_target:
			qualified_name
				{
					$$ = $1;
				}
			| qualified_name AS ColId
				{
					$1->alias = make_alias($3, NIL);
					$$ = $1;
				}
		;

insert_rest:
			SelectStmt
				{
					$$ = make_node<sq_insert_stmt_t>();
					$$->cols = NIL;
					$$->select_stmt = $1;
				}
			| OVERRIDING override_kind VALUE_P SelectStmt
				{
					$$ = make_node<sq_insert_stmt_t>();
					$$->cols = NIL;
					$$->override = (sq_overriding_kind_t) $2;
					$$->select_stmt = $4;
				}
			| '(' insert_column_list ')' SelectStmt
				{
					$$ = make_node<sq_insert_stmt_t>();
					$$->cols = $2;
					$$->select_stmt = $4;
				}
			| '(' insert_column_list ')' OVERRIDING override_kind VALUE_P SelectStmt
				{
					$$ = make_node<sq_insert_stmt_t>();
					$$->cols = $2;
					$$->override = (sq_overriding_kind_t) $5;
					$$->select_stmt = $7;
				}
			| DEFAULT VALUES
				{
					$$ = make_node<sq_insert_stmt_t>();
					$$->cols = NIL;
					$$->select_stmt = NULL;
				}
		;

override_kind:
			USER		{ $$ = SQ_OVERRIDING_USER_VALUE; }
			| SYSTEM_P	{ $$ = SQ_OVERRIDING_SYSTEM_VALUE; }
		;

insert_column_list:
			insert_column_item
					{ $$ = list_make1($1); }
			| insert_column_list ',' insert_column_item
					{ $$ = lappend($1, $3); }
		;

insert_column_item:
			ColId opt_indirection
				{
					$$ = make_node<sq_res_target_t>();
					$$->name = $1;
					$$->indirection = check_indirection($2, yyscanner);
					$$->val = NULL;
					$$->location = @1;
				}
		;

opt_on_conflict:
			ON CONFLICT opt_conf_expr DO UPDATE SET set_clause_list	where_clause
				{
					$$ = make_node<sq_on_conflict_clause_t>();
					$$->action = SQ_ONCONFLICT_UPDATE;
					$$->infer = $3;
					$$->target_list = $7;
					$$->where_clause = $8;
					$$->location = @1;
				}
			|
			ON CONFLICT opt_conf_expr DO NOTHING
				{
					$$ = make_node<sq_on_conflict_clause_t>();
					$$->action = SQ_ONCONFLICT_NOTHING;
					$$->infer = $3;
					$$->target_list = NIL;
					$$->where_clause = NULL;
					$$->location = @1;
				}
			| /*EMPTY*/
				{
					$$ = NULL;
				}
		;

opt_conf_expr:
			'(' index_params ')' where_clause
				{
					$$ = make_node<sq_infer_clause_t>();
					$$->index_elems = $2;
					$$->where_clause = $4;
					$$->conname = NULL;
					$$->location = @1;
				}
			|
			ON CONSTRAINT name
				{
					$$ = make_node<sq_infer_clause_t>();
					$$->index_elems = NIL;
					$$->where_clause = NULL;
					$$->conname = $3;
					$$->location = @1;
				}
			| /*EMPTY*/
				{
					$$ = NULL;
				}
		;

returning_clause:
			RETURNING returning_with_clause target_list
				{
					sq_returning_clause_t *n = make_node<sq_returning_clause_t>();

					n->options = $2;
					n->exprs = $3;
					$$ = n;
				}
			| /* EMPTY */
				{
					$$ = NULL;
				}
		;

returning_with_clause:
			WITH '(' returning_options ')'		{ $$ = $3; }
			| /* EMPTY */						{ $$ = NIL; }
		;

returning_options:
			returning_option							{ $$ = list_make1($1); }
			| returning_options ',' returning_option	{ $$ = lappend($1, $3); }
		;

returning_option:
			returning_option_kind AS ColId
				{
					sq_returning_option_t *n = make_node<sq_returning_option_t>();

					n->option = $1;
					n->value = $3;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
		;

returning_option_kind:
			OLD			{ $$ = SQ_RETURNING_OPTION_OLD; }
			| NEW		{ $$ = SQ_RETURNING_OPTION_NEW; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				DELETE STATEMENTS
 *
 *****************************************************************************/

DeleteStmt: opt_with_clause DELETE_P FROM relation_expr_opt_alias
			using_clause where_or_current_clause returning_clause
				{
					sq_delete_stmt_t *n = make_node<sq_delete_stmt_t>();

					n->relation = $4;
					n->using_clause = $5;
					n->where_clause = $6;
					n->returning_clause = $7;
					n->with_clause = $1;
					$$ = (sq_node_t *) n;
				}
		;

using_clause:
				USING from_list						{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				LOCK TABLE
 *
 *****************************************************************************/

LockStmt:	LOCK_P opt_table relation_expr_list opt_lock opt_nowait
				{
					sq_lock_stmt_t   *n = make_node<sq_lock_stmt_t>();

					n->relations = $3;
					n->mode = $4;
					n->nowait = $5;
					$$ = (sq_node_t *) n;
				}
		;

opt_lock:	IN_P lock_type MODE				{ $$ = $2; }
			| /*EMPTY*/						{ $$ = access_exclusive_lock; }
		;

lock_type:	ACCESS SHARE					{ $$ = access_share_lock; }
			| ROW SHARE						{ $$ = row_share_lock; }
			| ROW EXCLUSIVE					{ $$ = row_exclusive_lock; }
			| SHARE UPDATE EXCLUSIVE		{ $$ = share_update_exclusive_lock; }
			| SHARE							{ $$ = share_lock; }
			| SHARE ROW EXCLUSIVE			{ $$ = share_row_exclusive_lock; }
			| EXCLUSIVE						{ $$ = exclusive_lock; }
			| ACCESS EXCLUSIVE				{ $$ = access_exclusive_lock; }
		;

opt_nowait:	NOWAIT							{ $$ = true; }
			| /*EMPTY*/						{ $$ = false; }
		;

opt_nowait_or_skip:
			NOWAIT							{ $$ = SQ_LockWaitError; }
			| SKIP LOCKED					{ $$ = SQ_LockWaitSkip; }
			| /*EMPTY*/						{ $$ = SQ_LockWaitBlock; }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				UpdateStmt (UPDATE)
 *
 *****************************************************************************/

UpdateStmt: opt_with_clause UPDATE relation_expr_opt_alias
			SET set_clause_list
			from_clause
			where_or_current_clause
			returning_clause
				{
					sq_update_stmt_t *n = make_node<sq_update_stmt_t>();

					n->relation = $3;
					n->target_list = $5;
					n->from_clause = $6;
					n->where_clause = $7;
					n->returning_clause = $8;
					n->with_clause = $1;
					$$ = (sq_node_t *) n;
				}
		;

set_clause_list:
			set_clause							{ $$ = $1; }
			| set_clause_list ',' set_clause	{ $$ = list_concat($1,$3); }
		;

set_clause:
			set_target '=' a_expr
				{
					$1->val = (sq_node_t *) $3;
					$$ = list_make1($1);
				}
			| '(' set_target_list ')' '=' a_expr
				{
					int			ncolumns = list_length($2);
					int			i = 1;
					list_cell_t   col_cell;

					/* Create a MultiAssignRef source for each target */
					foreach(col_cell, $2)
					{
						sq_res_target_t  *res_col = (sq_res_target_t *) lfirst(col_cell);
						sq_multi_assign_ref_t *r = make_node<sq_multi_assign_ref_t>();

						r->source = (sq_node_t *) $5;
						r->colno = i;
						r->ncolumns = ncolumns;
						res_col->val = (sq_node_t *) r;
						i++;
					}

					$$ = $2;
				}
		;

set_target:
			ColId opt_indirection
				{
					$$ = make_node<sq_res_target_t>();
					$$->name = $1;
					$$->indirection = check_indirection($2, yyscanner);
					$$->val = NULL;	/* upper production sets this */
					$$->location = @1;
				}
		;

set_target_list:
			set_target								{ $$ = list_make1($1); }
			| set_target_list ',' set_target		{ $$ = lappend($1,$3); }
		;


/*****************************************************************************
 *
 *		QUERY:
 *				MERGE
 *
 *****************************************************************************/

MergeStmt:
			opt_with_clause MERGE INTO relation_expr_opt_alias
			USING table_ref
			ON a_expr
			merge_when_list
			returning_clause
				{
					sq_merge_stmt_t  *m = make_node<sq_merge_stmt_t>();

					m->with_clause = $1;
					m->relation = $4;
					m->source_relation = $6;
					m->join_condition = $8;
					m->merge_when_clauses = $9;
					m->returning_clause = $10;

					$$ = (sq_node_t *) m;
				}
		;

merge_when_list:
			merge_when_clause						{ $$ = list_make1($1); }
			| merge_when_list merge_when_clause		{ $$ = lappend($1,$2); }
		;

/*
 * A WHEN clause may be WHEN MATCHED, WHEN NOT MATCHED BY SOURCE, or WHEN NOT
 * MATCHED [BY TARGET]. The first two cases match target tuples, and support
 * UPDATE/DELETE/DO NOTHING actions. The third case does not match target
 * tuples, and only supports INSERT/DO NOTHING actions.
 */
merge_when_clause:
			merge_when_tgt_matched opt_merge_when_condition THEN merge_update
				{
					$4->match_kind = $1;
					$4->condition = $2;

					$$ = (sq_node_t *) $4;
				}
			| merge_when_tgt_matched opt_merge_when_condition THEN merge_delete
				{
					$4->match_kind = $1;
					$4->condition = $2;

					$$ = (sq_node_t *) $4;
				}
			| merge_when_tgt_not_matched opt_merge_when_condition THEN merge_insert
				{
					$4->match_kind = $1;
					$4->condition = $2;

					$$ = (sq_node_t *) $4;
				}
			| merge_when_tgt_matched opt_merge_when_condition THEN DO NOTHING
				{
					sq_merge_when_clause_t *m = make_node<sq_merge_when_clause_t>();

					m->match_kind = $1;
					m->command_type = SQ_CMD_NOTHING;
					m->condition = $2;

					$$ = (sq_node_t *) m;
				}
			| merge_when_tgt_not_matched opt_merge_when_condition THEN DO NOTHING
				{
					sq_merge_when_clause_t *m = make_node<sq_merge_when_clause_t>();

					m->match_kind = $1;
					m->command_type = SQ_CMD_NOTHING;
					m->condition = $2;

					$$ = (sq_node_t *) m;
				}
		;

merge_when_tgt_matched:
			WHEN MATCHED					{ $$ = SQ_MERGE_WHEN_MATCHED; }
			| WHEN NOT MATCHED BY SOURCE	{ $$ = SQ_MERGE_WHEN_NOT_MATCHED_BY_SOURCE; }
		;

merge_when_tgt_not_matched:
			WHEN NOT MATCHED				{ $$ = SQ_MERGE_WHEN_NOT_MATCHED_BY_TARGET; }
			| WHEN NOT MATCHED BY TARGET	{ $$ = SQ_MERGE_WHEN_NOT_MATCHED_BY_TARGET; }
		;

opt_merge_when_condition:
			AND a_expr				{ $$ = $2; }
			|						{ $$ = NULL; }
		;

merge_update:
			UPDATE SET set_clause_list
				{
					sq_merge_when_clause_t *n = make_node<sq_merge_when_clause_t>();
					n->command_type = SQ_CMD_UPDATE;
					n->override = SQ_OVERRIDING_NOT_SET;
					n->target_list = $3;
					n->values = NIL;

					$$ = n;
				}
		;

merge_delete:
			DELETE_P
				{
					sq_merge_when_clause_t *n = make_node<sq_merge_when_clause_t>();
					n->command_type = SQ_CMD_DELETE;
					n->override = SQ_OVERRIDING_NOT_SET;
					n->target_list = NIL;
					n->values = NIL;

					$$ = n;
				}
		;

merge_insert:
			INSERT merge_values_clause
				{
					sq_merge_when_clause_t *n = make_node<sq_merge_when_clause_t>();
					n->command_type = SQ_CMD_INSERT;
					n->override = SQ_OVERRIDING_NOT_SET;
					n->target_list = NIL;
					n->values = $2;
					$$ = n;
				}
			| INSERT OVERRIDING override_kind VALUE_P merge_values_clause
				{
					sq_merge_when_clause_t *n = make_node<sq_merge_when_clause_t>();
					n->command_type = SQ_CMD_INSERT;
					n->override = (sq_overriding_kind_t) $3;
					n->target_list = NIL;
					n->values = $5;
					$$ = n;
				}
			| INSERT '(' insert_column_list ')' merge_values_clause
				{
					sq_merge_when_clause_t *n = make_node<sq_merge_when_clause_t>();
					n->command_type = SQ_CMD_INSERT;
					n->override = SQ_OVERRIDING_NOT_SET;
					n->target_list = $3;
					n->values = $5;
					$$ = n;
				}
			| INSERT '(' insert_column_list ')' OVERRIDING override_kind VALUE_P merge_values_clause
				{
					sq_merge_when_clause_t *n = make_node<sq_merge_when_clause_t>();
					n->command_type = SQ_CMD_INSERT;
					n->override = (sq_overriding_kind_t) $6;
					n->target_list = $3;
					n->values = $8;
					$$ = n;
				}
			| INSERT DEFAULT VALUES
				{
					sq_merge_when_clause_t *n = make_node<sq_merge_when_clause_t>();
					n->command_type = SQ_CMD_INSERT;
					n->override = SQ_OVERRIDING_NOT_SET;
					n->target_list = NIL;
					n->values = NIL;
					$$ = n;
				}
		;

merge_values_clause:
			VALUES '(' expr_list ')'
				{
					$$ = $3;
				}
		;

/*****************************************************************************
 *
 *		QUERY:
 *				CURSOR STATEMENTS
 *
 *****************************************************************************/
DeclareCursorStmt: DECLARE cursor_name cursor_options CURSOR opt_hold FOR SelectStmt
				{
					sq_declare_cursor_stmt_t *n = make_node<sq_declare_cursor_stmt_t>();

					n->portalname = $2;
					/* currently we always set FAST_PLAN option */
					n->options = $3 | $5 | SQ_CURSOR_OPT_FAST_PLAN;
					n->query = $7;
					$$ = (sq_node_t *) n;
				}
		;

cursor_name:	name						{ $$ = $1; }
		;

cursor_options: /*EMPTY*/					{ $$ = 0; }
			| cursor_options NO SCROLL		{ $$ = $1 | SQ_CURSOR_OPT_NO_SCROLL; }
			| cursor_options SCROLL			{ $$ = $1 | SQ_CURSOR_OPT_SCROLL; }
			| cursor_options BINARY			{ $$ = $1 | SQ_CURSOR_OPT_BINARY; }
			| cursor_options ASENSITIVE		{ $$ = $1 | SQ_CURSOR_OPT_ASENSITIVE; }
			| cursor_options INSENSITIVE	{ $$ = $1 | SQ_CURSOR_OPT_INSENSITIVE; }
		;

opt_hold: /* EMPTY */						{ $$ = 0; }
			| WITH HOLD						{ $$ = SQ_CURSOR_OPT_HOLD; }
			| WITHOUT HOLD					{ $$ = 0; }
		;

/*****************************************************************************
 *
 *		QUERY:
 *				SELECT STATEMENTS
 *
 *****************************************************************************/

/* A complete SELECT statement looks like this.
 *
 * The rule returns either a single SelectStmt node or a tree of them,
 * representing a set-operation tree.
 *
 * There is an ambiguity when a sub-SELECT is within an a_expr and there
 * are excess parentheses: do the parentheses belong to the sub-SELECT or
 * to the surrounding a_expr?  We don't really care, but bison wants to know.
 * To resolve the ambiguity, we are careful to define the grammar so that
 * the decision is staved off as long as possible: as long as we can keep
 * absorbing parentheses into the sub-SELECT, we will do so, and only when
 * it's no longer possible to do that will we decide that parens belong to
 * the expression.	For example, in "SELECT (((SELECT 2)) + 3)" the extra
 * parentheses are treated as part of the sub-select.  The necessity of doing
 * it that way is shown by "SELECT (((SELECT 2)) UNION SELECT 2)".	Had we
 * parsed "((SELECT 2))" as an a_expr, it'd be too late to go back to the
 * SELECT viewpoint when we see the UNION.
 *
 * This approach is implemented by defining a nonterminal select_with_parens,
 * which represents a SELECT with at least one outer layer of parentheses,
 * and being careful to use select_with_parens, never '(' SelectStmt ')',
 * in the expression grammar.  We will then have shift-reduce conflicts
 * which we can resolve in favor of always treating '(' <select> ')' as
 * a select_with_parens.  To resolve the conflicts, the productions that
 * conflict with the select_with_parens productions are manually given
 * precedences lower than the precedence of ')', thereby ensuring that we
 * shift ')' (and then reduce to select_with_parens) rather than trying to
 * reduce the inner <select> nonterminal to something else.  We use UMINUS
 * precedence for this, which is a fairly arbitrary choice.
 *
 * To be able to define select_with_parens itself without ambiguity, we need
 * a nonterminal select_no_parens that represents a SELECT structure with no
 * outermost parentheses.  This is a little bit tedious, but it works.
 *
 * In non-expression contexts, we use SelectStmt which can represent a SELECT
 * with or without outer parentheses.
 */

SelectStmt: select_no_parens			%prec UMINUS
			| select_with_parens		%prec UMINUS
		;

select_with_parens:
			'(' select_no_parens ')'				{ $$ = $2; }
			| '(' select_with_parens ')'			{ $$ = $2; }
		;

/*
 * This rule parses the equivalent of the standard's <query expression>.
 * The duplicative productions are annoying, but hard to get rid of without
 * creating shift/reduce conflicts.
 *
 *	The locking clause (FOR UPDATE etc) may be before or after LIMIT/OFFSET.
 *	In <=7.2.X, LIMIT/OFFSET had to be after FOR UPDATE
 *	We now support both orderings, but prefer LIMIT/OFFSET before the locking
 * clause.
 *	2002-08-28 bjm
 */
select_no_parens:
			simple_select						{ $$ = $1; }
			| select_clause sort_clause
				{
					insert_select_options((sq_select_stmt_t *) $1, $2, NIL,
										NULL, NULL,
										yyscanner);
					$$ = $1;
				}
			| select_clause opt_sort_clause for_locking_clause opt_select_limit
				{
					insert_select_options((sq_select_stmt_t *) $1, $2, $3,
										$4,
										NULL,
										yyscanner);
					$$ = $1;
				}
			| select_clause opt_sort_clause select_limit opt_for_locking_clause
				{
					insert_select_options((sq_select_stmt_t *) $1, $2, $4,
										$3,
										NULL,
										yyscanner);
					$$ = $1;
				}
			| with_clause select_clause
				{
					insert_select_options((sq_select_stmt_t *) $2, NULL, NIL,
										NULL,
										$1,
										yyscanner);
					$$ = $2;
				}
			| with_clause select_clause sort_clause
				{
					insert_select_options((sq_select_stmt_t *) $2, $3, NIL,
										NULL,
										$1,
										yyscanner);
					$$ = $2;
				}
			| with_clause select_clause opt_sort_clause for_locking_clause opt_select_limit
				{
					insert_select_options((sq_select_stmt_t *) $2, $3, $4,
										$5,
										$1,
										yyscanner);
					$$ = $2;
				}
			| with_clause select_clause opt_sort_clause select_limit opt_for_locking_clause
				{
					insert_select_options((sq_select_stmt_t *) $2, $3, $5,
										$4,
										$1,
										yyscanner);
					$$ = $2;
				}
		;

select_clause:
			simple_select							{ $$ = $1; }
			| select_with_parens					{ $$ = $1; }
		;

/*
 * This rule parses SELECT statements that can appear within set operations,
 * including UNION, INTERSECT and EXCEPT.  '(' and ')' can be used to specify
 * the ordering of the set operations.	Without '(' and ')' we want the
 * operations to be ordered per the precedence specs at the head of this file.
 *
 * As with select_no_parens, simple_select cannot have outer parentheses,
 * but can have parenthesized subclauses.
 *
 * It might appear that we could fold the first two alternatives into one
 * by using opt_distinct_clause.  However, that causes a shift/reduce conflict
 * against INSERT ... SELECT ... ON CONFLICT.  We avoid the ambiguity by
 * requiring SELECT DISTINCT [ON] to be followed by a non-empty target_list.
 *
 * Note that sort clauses cannot be included at this level --- SQL requires
 *		SELECT foo UNION SELECT bar ORDER BY baz
 * to be parsed as
 *		(SELECT foo UNION SELECT bar) ORDER BY baz
 * not
 *		SELECT foo UNION (SELECT bar ORDER BY baz)
 * Likewise for WITH, FOR UPDATE and LIMIT.  Therefore, those clauses are
 * described as part of the select_no_parens production, not simple_select.
 * This does not limit functionality, because you can reintroduce these
 * clauses inside parentheses.
 *
 * NOTE: only the leftmost component SelectStmt should have INTO.
 * However, this is not checked by the grammar; parse analysis must check it.
 */
simple_select:
			SELECT opt_all_clause opt_target_list
			into_clause from_clause where_clause
			group_clause having_clause window_clause
				{
					sq_select_stmt_t *n = make_node<sq_select_stmt_t>();

					n->target_list = $3;
					n->into_clause = $4;
					n->from_clause = $5;
					n->where_clause = $6;
					n->group_clause = ($7)->list;
					n->group_distinct = ($7)->distinct;
					n->having_clause = $8;
					n->window_clause = $9;
					$$ = (sq_node_t *) n;
				}
			| SELECT distinct_clause target_list
			into_clause from_clause where_clause
			group_clause having_clause window_clause
				{
					sq_select_stmt_t *n = make_node<sq_select_stmt_t>();

					n->distinct_clause = $2;
					n->target_list = $3;
					n->into_clause = $4;
					n->from_clause = $5;
					n->where_clause = $6;
					n->group_clause = ($7)->list;
					n->group_distinct = ($7)->distinct;
					n->having_clause = $8;
					n->window_clause = $9;
					$$ = (sq_node_t *) n;
				}
			| values_clause							{ $$ = $1; }
			| TABLE relation_expr
				{
					/* same as SELECT * FROM relation_expr */
					sq_column_ref_t  *cr = make_node<sq_column_ref_t>();
					sq_res_target_t  *rt = make_node<sq_res_target_t>();
					sq_select_stmt_t *n = make_node<sq_select_stmt_t>();

					cr->fields = list_make1(make_node<sq_a_star_t>());
					cr->location = -1;

					rt->name = NULL;
					rt->indirection = NIL;
					rt->val = (sq_node_t *) cr;
					rt->location = -1;

					n->target_list = list_make1(rt);
					n->from_clause = list_make1($2);
					$$ = (sq_node_t *) n;
				}
			| select_clause UNION set_quantifier select_clause
				{
					$$ = make_set_op(SQ_SETOP_UNION, $3 == SQ_SET_QUANTIFIER_ALL, $1, $4);
				}
			| select_clause INTERSECT set_quantifier select_clause
				{
					$$ = make_set_op(SQ_SETOP_INTERSECT, $3 == SQ_SET_QUANTIFIER_ALL, $1, $4);
				}
			| select_clause EXCEPT set_quantifier select_clause
				{
					$$ = make_set_op(SQ_SETOP_EXCEPT, $3 == SQ_SET_QUANTIFIER_ALL, $1, $4);
				}
		;

/*
 * SQL standard WITH clause looks like:
 *
 * WITH [ RECURSIVE ] <query name> [ (<column>,...) ]
 *		AS (query) [ SEARCH or CYCLE clause ]
 *
 * Recognizing WITH_LA here allows a CTE to be named TIME or ORDINALITY.
 */
with_clause:
		WITH cte_list
			{
				$$ = make_node<sq_with_clause_t>();
				$$->ctes = $2;
				$$->recursive = false;
				$$->location = @1;
			}
		| WITH_LA cte_list
			{
				$$ = make_node<sq_with_clause_t>();
				$$->ctes = $2;
				$$->recursive = false;
				$$->location = @1;
			}
		| WITH RECURSIVE cte_list
			{
				$$ = make_node<sq_with_clause_t>();
				$$->ctes = $3;
				$$->recursive = true;
				$$->location = @1;
			}
		;

cte_list:
		common_table_expr						{ $$ = list_make1($1); }
		| cte_list ',' common_table_expr		{ $$ = lappend($1, $3); }
		;

common_table_expr:  name opt_name_list AS opt_materialized '(' PreparableStmt ')' opt_search_clause opt_cycle_clause
			{
				sq_common_table_expr_t *n = make_node<sq_common_table_expr_t>();

				n->ctename = $1;
				n->aliascolnames = $2;
				n->ctematerialized = (sq_cte_materialize_t) $4;
				n->ctequery = $6;
				n->search_clause = cast_node(sq_cte_search_clause_t, $8);
				n->cycle_clause = cast_node(sq_cte_cycle_clause_t, $9);
				n->location = @1;
				$$ = (sq_node_t *) n;
			}
		;

opt_materialized:
		MATERIALIZED							{ $$ = SQ_CTEMaterializeAlways; }
		| NOT MATERIALIZED						{ $$ = SQ_CTEMaterializeNever; }
		| /*EMPTY*/								{ $$ = SQ_CTEMaterializeDefault; }
		;

opt_search_clause:
		SEARCH DEPTH FIRST_P BY columnList SET ColId
			{
				sq_cte_search_clause_t *n = make_node<sq_cte_search_clause_t>();

				n->search_col_list = $5;
				n->search_breadth_first = false;
				n->search_seq_column = $7;
				n->location = @1;
				$$ = (sq_node_t *) n;
			}
		| SEARCH BREADTH FIRST_P BY columnList SET ColId
			{
				sq_cte_search_clause_t *n = make_node<sq_cte_search_clause_t>();

				n->search_col_list = $5;
				n->search_breadth_first = true;
				n->search_seq_column = $7;
				n->location = @1;
				$$ = (sq_node_t *) n;
			}
		| /*EMPTY*/
			{
				$$ = NULL;
			}
		;

opt_cycle_clause:
		CYCLE columnList SET ColId TO AexprConst DEFAULT AexprConst USING ColId
			{
				sq_cte_cycle_clause_t *n = make_node<sq_cte_cycle_clause_t>();

				n->cycle_col_list = $2;
				n->cycle_mark_column = $4;
				n->cycle_mark_value = $6;
				n->cycle_mark_default = $8;
				n->cycle_path_column = $10;
				n->location = @1;
				$$ = (sq_node_t *) n;
			}
		| CYCLE columnList SET ColId USING ColId
			{
				sq_cte_cycle_clause_t *n = make_node<sq_cte_cycle_clause_t>();

				n->cycle_col_list = $2;
				n->cycle_mark_column = $4;
				n->cycle_mark_value = make_bool_a_const(true, -1);
				n->cycle_mark_default = make_bool_a_const(false, -1);
				n->cycle_path_column = $6;
				n->location = @1;
				$$ = (sq_node_t *) n;
			}
		| /*EMPTY*/
			{
				$$ = NULL;
			}
		;

opt_with_clause:
		with_clause								{ $$ = $1; }
		| /*EMPTY*/								{ $$ = NULL; }
		;

into_clause:
			INTO OptTempTableName
				{
					$$ = make_node<sq_into_clause_t>();
					$$->rel = $2;
					$$->col_names = NIL;
					$$->options = NIL;
					$$->on_commit = SQ_ONCOMMIT_NOOP;
					$$->table_space_name = NULL;
					$$->view_query = NULL;
					$$->skip_data = false;
				}
			| /*EMPTY*/
				{ $$ = NULL; }
		;

/*
 * Redundancy here is needed to avoid shift/reduce conflicts,
 * since TEMP is not a reserved word.  See also OptTemp.
 */
OptTempTableName:
			TEMPORARY opt_table qualified_name
				{
					$$ = $3;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| TEMP opt_table qualified_name
				{
					$$ = $3;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| LOCAL TEMPORARY opt_table qualified_name
				{
					$$ = $4;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| LOCAL TEMP opt_table qualified_name
				{
					$$ = $4;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| GLOBAL TEMPORARY opt_table qualified_name
				{
					notify(*scanner_extra(yyscanner), SQ_WARNING, sqlstate_warning,
						   parser_errposition(@1), "GLOBAL is deprecated in temporary table creation", nullptr);
					$$ = $4;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| GLOBAL TEMP opt_table qualified_name
				{
					notify(*scanner_extra(yyscanner), SQ_WARNING, sqlstate_warning,
						   parser_errposition(@1), "GLOBAL is deprecated in temporary table creation", nullptr);
					$$ = $4;
					$$->relpersistence = RELPERSISTENCE_TEMP;
				}
			| UNLOGGED opt_table qualified_name
				{
					$$ = $3;
					$$->relpersistence = RELPERSISTENCE_UNLOGGED;
				}
			| TABLE qualified_name
				{
					$$ = $2;
					$$->relpersistence = RELPERSISTENCE_PERMANENT;
				}
			| qualified_name
				{
					$$ = $1;
					$$->relpersistence = RELPERSISTENCE_PERMANENT;
				}
		;

opt_table:	TABLE
			| /*EMPTY*/
		;

set_quantifier:
			ALL										{ $$ = SQ_SET_QUANTIFIER_ALL; }
			| DISTINCT								{ $$ = SQ_SET_QUANTIFIER_DISTINCT; }
			| /*EMPTY*/								{ $$ = SQ_SET_QUANTIFIER_DEFAULT; }
		;

/* We use (NIL) as a placeholder to indicate that all target expressions
 * should be placed in the DISTINCT list during parsetree analysis.
 */
distinct_clause:
			DISTINCT								{ $$ = list_make1(NIL); }
			| DISTINCT ON '(' expr_list ')'			{ $$ = $4; }
		;

opt_all_clause:
			ALL
			| /*EMPTY*/
		;

opt_distinct_clause:
			distinct_clause							{ $$ = $1; }
			| opt_all_clause						{ $$ = NIL; }
		;

opt_sort_clause:
			sort_clause								{ $$ = $1; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

sort_clause:
			ORDER BY sortby_list					{ $$ = $3; }
		;

sortby_list:
			sortby									{ $$ = list_make1($1); }
			| sortby_list ',' sortby				{ $$ = lappend($1, $3); }
		;

sortby:		a_expr USING qual_all_Op opt_nulls_order
				{
					$$ = make_node<sq_sort_by_t>();
					$$->node = $1;
					$$->sortby_dir = SQ_SORTBY_USING;
					$$->sortby_nulls = (sq_sort_by_nulls_t) $4;
					$$->use_op = $3;
					$$->location = @3;
				}
			| a_expr opt_asc_desc opt_nulls_order
				{
					$$ = make_node<sq_sort_by_t>();
					$$->node = $1;
					$$->sortby_dir = (sq_sort_by_dir_t) $2;
					$$->sortby_nulls = (sq_sort_by_nulls_t) $3;
					$$->use_op = NIL;
					$$->location = -1;		/* no operator */
				}
		;


select_limit:
			limit_clause offset_clause
				{
					$$ = $1;
					($$)->limit_offset = $2;
					($$)->offset_loc = @2;
				}
			| offset_clause limit_clause
				{
					$$ = $2;
					($$)->limit_offset = $1;
					($$)->offset_loc = @1;
				}
			| limit_clause
				{
					$$ = $1;
				}
			| offset_clause
				{
					select_limit_t *n = make_node<select_limit_t>();

					n->limit_offset = $1;
					n->limit_count = NULL;
					n->limit_option = SQ_LIMIT_OPTION_COUNT;
					n->offset_loc = @1;
					n->count_loc = -1;
					n->option_loc = -1;
					$$ = n;
				}
		;

opt_select_limit:
			select_limit						{ $$ = $1; }
			| /* EMPTY */						{ $$ = NULL; }
		;

limit_clause:
			LIMIT select_limit_value
				{
					select_limit_t *n = make_node<select_limit_t>();

					n->limit_offset = NULL;
					n->limit_count = $2;
					n->limit_option = SQ_LIMIT_OPTION_COUNT;
					n->offset_loc = -1;
					n->count_loc = @1;
					n->option_loc = -1;
					$$ = n;
				}
			| LIMIT select_limit_value ',' select_offset_value
				{
					/* Disabled because it was too confusing, bjm 2002-02-18 */
					raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@1), .hint = "Use separate LIMIT and OFFSET clauses."}, "LIMIT #,# syntax is not supported");
				}
			/* SQL:2008 syntax */
			/* to avoid shift/reduce conflicts, handle the optional value with
			 * a separate production rather than an opt_ expression.  The fact
			 * that ONLY is fully reserved means that this way, we defer any
			 * decision about what rule reduces ROW or ROWS to the point where
			 * we can see the ONLY token in the lookahead slot.
			 */
			| FETCH first_or_next select_fetch_first_value row_or_rows ONLY
				{
					select_limit_t *n = make_node<select_limit_t>();

					n->limit_offset = NULL;
					n->limit_count = $3;
					n->limit_option = SQ_LIMIT_OPTION_COUNT;
					n->offset_loc = -1;
					n->count_loc = @1;
					n->option_loc = -1;
					$$ = n;
				}
			| FETCH first_or_next select_fetch_first_value row_or_rows WITH TIES
				{
					select_limit_t *n = make_node<select_limit_t>();

					n->limit_offset = NULL;
					n->limit_count = $3;
					n->limit_option = SQ_LIMIT_OPTION_WITH_TIES;
					n->offset_loc = -1;
					n->count_loc = @1;
					n->option_loc = @5;
					$$ = n;
				}
			| FETCH first_or_next row_or_rows ONLY
				{
					select_limit_t *n = make_node<select_limit_t>();

					n->limit_offset = NULL;
					n->limit_count = make_int_const(1, -1);
					n->limit_option = SQ_LIMIT_OPTION_COUNT;
					n->offset_loc = -1;
					n->count_loc = @1;
					n->option_loc = -1;
					$$ = n;
				}
			| FETCH first_or_next row_or_rows WITH TIES
				{
					select_limit_t *n = make_node<select_limit_t>();

					n->limit_offset = NULL;
					n->limit_count = make_int_const(1, -1);
					n->limit_option = SQ_LIMIT_OPTION_WITH_TIES;
					n->offset_loc = -1;
					n->count_loc = @1;
					n->option_loc = @4;
					$$ = n;
				}
		;

offset_clause:
			OFFSET select_offset_value
				{ $$ = $2; }
			/* SQL:2008 syntax */
			| OFFSET select_fetch_first_value row_or_rows
				{ $$ = $2; }
		;

select_limit_value:
			a_expr									{ $$ = $1; }
			| ALL
				{
					/* LIMIT ALL is represented as a NULL constant */
					$$ = make_null_a_const(@1);
				}
		;

select_offset_value:
			a_expr									{ $$ = $1; }
		;

/*
 * Allowing full expressions without parentheses causes various parsing
 * problems with the trailing ROW/ROWS key words.  SQL spec only calls for
 * <simple value specification>, which is either a literal or a parameter (but
 * an <SQL parameter reference> could be an identifier, bringing up conflicts
 * with ROW/ROWS). We solve this by leveraging the presence of ONLY (see above)
 * to determine whether the expression is missing rather than trying to make it
 * optional in this rule.
 *
 * c_expr covers almost all the spec-required cases (and more), but it doesn't
 * cover signed numeric literals, which are allowed by the spec. So we include
 * those here explicitly. We need FCONST as well as ICONST because values that
 * don't fit in the platform's "long", but do fit in bigint, should still be
 * accepted here. (This is possible in 64-bit Windows as well as all 32-bit
 * builds.)
 */
select_fetch_first_value:
			c_expr									{ $$ = $1; }
			| '+' I_or_F_const
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "+", NULL, $2, @1); }
			| '-' I_or_F_const
				{ $$ = do_negate($2, @1); }
		;

I_or_F_const:
			Iconst									{ $$ = make_int_const($1,@1); }
			| FCONST								{ $$ = make_float_const($1,@1); }
		;

/* noise words */
row_or_rows: ROW									{ $$ = 0; }
			| ROWS									{ $$ = 0; }
		;

first_or_next: FIRST_P								{ $$ = 0; }
			| NEXT									{ $$ = 0; }
		;


/*
 * This syntax for group_clause tries to follow the spec quite closely.
 * However, the spec allows only column references, not expressions,
 * which introduces an ambiguity between implicit row constructors
 * (a,b) and lists of column references.
 *
 * We handle this by using the a_expr production for what the spec calls
 * <ordinary grouping set>, which in the spec represents either one column
 * reference or a parenthesized list of column references. Then, we check the
 * top node of the a_expr to see if it's an implicit RowExpr, and if so, just
 * grab and use the list, discarding the node. (this is done in parse analysis,
 * not here)
 *
 * (we abuse the row_format field of RowExpr to distinguish implicit and
 * explicit row constructors; it's debatable if anyone sanely wants to use them
 * in a group clause, but if they have a reason to, we make it possible.)
 *
 * Each item in the group_clause list is either an expression tree or a
 * GroupingSet node of some type.
 */
group_clause:
			GROUP_P BY set_quantifier group_by_list
				{
					group_clause_t *n = make_node<group_clause_t>();

					n->distinct = $3 == SQ_SET_QUANTIFIER_DISTINCT;
					n->list = $4;
					$$ = n;
				}
			| /*EMPTY*/
				{
					group_clause_t *n = make_node<group_clause_t>();

					n->distinct = false;
					n->list = NIL;
					$$ = n;
				}
		;

group_by_list:
			group_by_item							{ $$ = list_make1($1); }
			| group_by_list ',' group_by_item		{ $$ = lappend($1,$3); }
		;

group_by_item:
			a_expr									{ $$ = $1; }
			| empty_grouping_set					{ $$ = $1; }
			| cube_clause							{ $$ = $1; }
			| rollup_clause							{ $$ = $1; }
			| grouping_sets_clause					{ $$ = $1; }
		;

empty_grouping_set:
			'(' ')'
				{
					$$ = (sq_node_t *) make_grouping_set(SQ_GROUPING_SET_EMPTY, NIL, @1);
				}
		;

/*
 * These hacks rely on setting precedence of CUBE and ROLLUP below that of '(',
 * so that they shift in these rules rather than reducing the conflicting
 * unreserved_keyword rule.
 */

rollup_clause:
			ROLLUP '(' expr_list ')'
				{
					$$ = (sq_node_t *) make_grouping_set(SQ_GROUPING_SET_ROLLUP, $3, @1);
				}
		;

cube_clause:
			CUBE '(' expr_list ')'
				{
					$$ = (sq_node_t *) make_grouping_set(SQ_GROUPING_SET_CUBE, $3, @1);
				}
		;

grouping_sets_clause:
			GROUPING SETS '(' group_by_list ')'
				{
					$$ = (sq_node_t *) make_grouping_set(SQ_GROUPING_SET_SETS, $4, @1);
				}
		;

having_clause:
			HAVING a_expr							{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

for_locking_clause:
			for_locking_items						{ $$ = $1; }
			| FOR READ ONLY							{ $$ = NIL; }
		;

opt_for_locking_clause:
			for_locking_clause						{ $$ = $1; }
			| /* EMPTY */							{ $$ = NIL; }
		;

for_locking_items:
			for_locking_item						{ $$ = list_make1($1); }
			| for_locking_items for_locking_item	{ $$ = lappend($1, $2); }
		;

for_locking_item:
			for_locking_strength locked_rels_list opt_nowait_or_skip
				{
					sq_locking_clause_t *n = make_node<sq_locking_clause_t>();

					n->locked_rels = $2;
					n->strength = (sq_lock_clause_strength_t) $1;
					n->wait_policy = (sq_lock_wait_policy_t) $3;
					$$ = (sq_node_t *) n;
				}
		;

for_locking_strength:
			FOR UPDATE							{ $$ = SQ_LCS_FORUPDATE; }
			| FOR NO KEY UPDATE					{ $$ = SQ_LCS_FORNOKEYUPDATE; }
			| FOR SHARE							{ $$ = SQ_LCS_FORSHARE; }
			| FOR KEY SHARE						{ $$ = SQ_LCS_FORKEYSHARE; }
		;

locked_rels_list:
			OF qualified_name_list					{ $$ = $2; }
			| /* EMPTY */							{ $$ = NIL; }
		;


/*
 * We should allow ROW '(' expr_list ')' too, but that seems to require
 * making VALUES a fully reserved word, which will probably break more apps
 * than allowing the noise-word is worth.
 */
values_clause:
			VALUES '(' expr_list ')'
				{
					sq_select_stmt_t *n = make_node<sq_select_stmt_t>();

					n->values_lists = list_make1($3);
					$$ = (sq_node_t *) n;
				}
			| values_clause ',' '(' expr_list ')'
				{
					sq_select_stmt_t *n = (sq_select_stmt_t *) $1;

					n->values_lists = lappend(n->values_lists, $4);
					$$ = (sq_node_t *) n;
				}
		;


/*****************************************************************************
 *
 *	clauses common to all Optimizable Stmts:
 *		from_clause		- allow list of both JOIN expressions and table names
 *		where_clause	- qualifications for joins or restrictions
 *
 *****************************************************************************/

from_clause:
			FROM from_list							{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

from_list:
			table_ref								{ $$ = list_make1($1); }
			| from_list ',' table_ref				{ $$ = lappend($1, $3); }
		;

/*
 * table_ref is where an alias clause can be attached.
 */
table_ref:	relation_expr opt_alias_clause
				{
					$1->alias = $2;
					$$ = (sq_node_t *) $1;
				}
			| relation_expr opt_alias_clause tablesample_clause
				{
					sq_range_table_sample_t *n = (sq_range_table_sample_t *) $3;

					$1->alias = $2;
					/* relation_expr goes inside the RangeTableSample node */
					n->relation = (sq_node_t *) $1;
					$$ = (sq_node_t *) n;
				}
			| func_table func_alias_clause
				{
					sq_range_function_t *n = (sq_range_function_t *) $1;

					n->alias = (sq_alias_t *) linitial($2);
					n->coldeflist = (list_ref_t ) lsecond($2);
					$$ = (sq_node_t *) n;
				}
			| LATERAL_P func_table func_alias_clause
				{
					sq_range_function_t *n = (sq_range_function_t *) $2;

					n->lateral = true;
					n->alias = (sq_alias_t *) linitial($3);
					n->coldeflist = (list_ref_t ) lsecond($3);
					$$ = (sq_node_t *) n;
				}
			| xmltable opt_alias_clause
				{
					sq_range_table_func_t *n = (sq_range_table_func_t *) $1;

					n->alias = $2;
					$$ = (sq_node_t *) n;
				}
			| LATERAL_P xmltable opt_alias_clause
				{
					sq_range_table_func_t *n = (sq_range_table_func_t *) $2;

					n->lateral = true;
					n->alias = $3;
					$$ = (sq_node_t *) n;
				}
			| select_with_parens opt_alias_clause
				{
					sq_range_subselect_t *n = make_node<sq_range_subselect_t>();

					n->lateral = false;
					n->subquery = $1;
					n->alias = $2;
					$$ = (sq_node_t *) n;
				}
			| LATERAL_P select_with_parens opt_alias_clause
				{
					sq_range_subselect_t *n = make_node<sq_range_subselect_t>();

					n->lateral = true;
					n->subquery = $2;
					n->alias = $3;
					$$ = (sq_node_t *) n;
				}
			| joined_table
				{
					$$ = (sq_node_t *) $1;
				}
			| '(' joined_table ')' alias_clause
				{
					$2->alias = $4;
					$$ = (sq_node_t *) $2;
				}
			| json_table opt_alias_clause
				{
					sq_json_table_t  *jt = cast_node(sq_json_table_t, $1);

					jt->alias = $2;
					$$ = (sq_node_t *) jt;
				}
			| LATERAL_P json_table opt_alias_clause
				{
					sq_json_table_t  *jt = cast_node(sq_json_table_t, $2);

					jt->alias = $3;
					jt->lateral = true;
					$$ = (sq_node_t *) jt;
				}
		;


/*
 * It may seem silly to separate joined_table from table_ref, but there is
 * method in SQL's madness: if you don't do it this way you get reduce-
 * reduce conflicts, because it's not clear to the parser generator whether
 * to expect alias_clause after ')' or not.  For the same reason we must
 * treat 'JOIN' and 'join_type JOIN' separately, rather than allowing
 * join_type to expand to empty; if we try it, the parser generator can't
 * figure out when to reduce an empty join_type right after table_ref.
 *
 * Note that a CROSS JOIN is the same as an unqualified
 * INNER JOIN, and an INNER JOIN/ON has the same shape
 * but a qualification expression to limit membership.
 * A NATURAL JOIN implicitly matches column names between
 * tables and the shape is determined by which columns are
 * in common. We'll collect columns during the later transformations.
 */

joined_table:
			'(' joined_table ')'
				{
					$$ = $2;
				}
			| table_ref CROSS JOIN table_ref
				{
					/* CROSS JOIN is same as unqualified inner join */
					sq_join_expr_t   *n = make_node<sq_join_expr_t>();

					n->jointype = SQ_JOIN_INNER;
					n->is_natural = false;
					n->larg = $1;
					n->rarg = $4;
					n->using_clause = NIL;
					n->join_using_alias = NULL;
					n->quals = NULL;
					$$ = n;
				}
			| table_ref join_type JOIN table_ref join_qual
				{
					sq_join_expr_t   *n = make_node<sq_join_expr_t>();

					n->jointype = $2;
					n->is_natural = false;
					n->larg = $1;
					n->rarg = $4;
					if ($5 != NULL && is_a($5, SQ_T_LIST))
					{
						 /* USING clause */
						n->using_clause = list_ref_t(linitial(list_ref_t($5)));
						n->join_using_alias = lsecond_node(sq_alias_t, list_ref_t($5));
					}
					else
					{
						/* ON clause */
						n->quals = $5;
					}
					$$ = n;
				}
			| table_ref JOIN table_ref join_qual
				{
					/* letting join_type reduce to empty doesn't work */
					sq_join_expr_t   *n = make_node<sq_join_expr_t>();

					n->jointype = SQ_JOIN_INNER;
					n->is_natural = false;
					n->larg = $1;
					n->rarg = $3;
					if ($4 != NULL && is_a($4, SQ_T_LIST))
					{
						/* USING clause */
						n->using_clause = list_ref_t(linitial(list_ref_t($4)));
						n->join_using_alias = lsecond_node(sq_alias_t, list_ref_t($4));
					}
					else
					{
						/* ON clause */
						n->quals = $4;
					}
					$$ = n;
				}
			| table_ref NATURAL join_type JOIN table_ref
				{
					sq_join_expr_t   *n = make_node<sq_join_expr_t>();

					n->jointype = $3;
					n->is_natural = true;
					n->larg = $1;
					n->rarg = $5;
					n->using_clause = NIL; /* figure out which columns later... */
					n->join_using_alias = NULL;
					n->quals = NULL; /* fill later */
					$$ = n;
				}
			| table_ref NATURAL JOIN table_ref
				{
					/* letting join_type reduce to empty doesn't work */
					sq_join_expr_t   *n = make_node<sq_join_expr_t>();

					n->jointype = SQ_JOIN_INNER;
					n->is_natural = true;
					n->larg = $1;
					n->rarg = $4;
					n->using_clause = NIL; /* figure out which columns later... */
					n->join_using_alias = NULL;
					n->quals = NULL; /* fill later */
					$$ = n;
				}
		;

alias_clause:
			AS ColId '(' name_list ')'
				{
					$$ = make_node<sq_alias_t>();
					$$->aliasname = $2;
					$$->colnames = $4;
				}
			| AS ColId
				{
					$$ = make_node<sq_alias_t>();
					$$->aliasname = $2;
				}
			| ColId '(' name_list ')'
				{
					$$ = make_node<sq_alias_t>();
					$$->aliasname = $1;
					$$->colnames = $3;
				}
			| ColId
				{
					$$ = make_node<sq_alias_t>();
					$$->aliasname = $1;
				}
		;

opt_alias_clause: alias_clause						{ $$ = $1; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

/*
 * The alias clause after JOIN ... USING only accepts the AS ColId spelling,
 * per SQL standard.  (The grammar could parse the other variants, but they
 * don't seem to be useful, and it might lead to parser problems in the
 * future.)
 */
opt_alias_clause_for_join_using:
			AS ColId
				{
					$$ = make_node<sq_alias_t>();
					$$->aliasname = $2;
					/* the column name list will be inserted later */
				}
			| /*EMPTY*/								{ $$ = NULL; }
		;

/*
 * func_alias_clause can include both an Alias and a coldeflist, so we make it
 * return a 2-element list that gets disassembled by calling production.
 */
func_alias_clause:
			alias_clause
				{
					$$ = list_make2($1, NIL);
				}
			| AS '(' TableFuncElementList ')'
				{
					$$ = list_make2(NULL, $3);
				}
			| AS ColId '(' TableFuncElementList ')'
				{
					sq_alias_t	   *a = make_node<sq_alias_t>();

					a->aliasname = $2;
					$$ = list_make2(a, $4);
				}
			| ColId '(' TableFuncElementList ')'
				{
					sq_alias_t	   *a = make_node<sq_alias_t>();

					a->aliasname = $1;
					$$ = list_make2(a, $3);
				}
			| /*EMPTY*/
				{
					$$ = list_make2(NULL, NIL);
				}
		;

join_type:	FULL opt_outer							{ $$ = SQ_JOIN_FULL; }
			| LEFT opt_outer						{ $$ = SQ_JOIN_LEFT; }
			| RIGHT opt_outer						{ $$ = SQ_JOIN_RIGHT; }
			| INNER_P								{ $$ = SQ_JOIN_INNER; }
		;

/* OUTER is just noise... */
opt_outer: OUTER_P
			| /*EMPTY*/
		;

/* JOIN qualification clauses
 * Possibilities are:
 *	USING ( column list ) [ AS alias ]
 *						  allows only unqualified column names,
 *						  which must match between tables.
 *	ON expr allows more general qualifications.
 *
 * We return USING as a two-element List (the first item being a sub-List
 * of the common column names, and the second either an Alias item or NULL).
 * An ON-expr will not be a List, so it can be told apart that way.
 */

join_qual: USING '(' name_list ')' opt_alias_clause_for_join_using
				{
					$$ = (sq_node_t *) list_make2($3, $5);
				}
			| ON a_expr
				{
					$$ = $2;
				}
		;


relation_expr:
			qualified_name
				{
					/* inheritance query, implicitly */
					$$ = $1;
					$$->inh = true;
					$$->alias = NULL;
				}
			| extended_relation_expr
				{
					$$ = $1;
				}
		;

extended_relation_expr:
			qualified_name '*'
				{
					/* inheritance query, explicitly */
					$$ = $1;
					$$->inh = true;
					$$->alias = NULL;
				}
			| ONLY qualified_name
				{
					/* no inheritance */
					$$ = $2;
					$$->inh = false;
					$$->alias = NULL;
				}
			| ONLY '(' qualified_name ')'
				{
					/* no inheritance, SQL99-style syntax */
					$$ = $3;
					$$->inh = false;
					$$->alias = NULL;
				}
		;


relation_expr_list:
			relation_expr							{ $$ = list_make1($1); }
			| relation_expr_list ',' relation_expr	{ $$ = lappend($1, $3); }
		;


/*
 * Given "UPDATE foo set set ...", we have to decide without looking any
 * further ahead whether the first "set" is an alias or the UPDATE's SET
 * keyword.  Since "set" is allowed as a column name both interpretations
 * are feasible.  We resolve the shift/reduce conflict by giving the first
 * relation_expr_opt_alias production a higher precedence than the SET token
 * has, causing the parser to prefer to reduce, in effect assuming that the
 * SET is not an alias.
 */
relation_expr_opt_alias: relation_expr					%prec UMINUS
				{
					$$ = $1;
				}
			| relation_expr ColId
				{
					sq_alias_t	   *alias = make_node<sq_alias_t>();

					alias->aliasname = $2;
					$1->alias = alias;
					$$ = $1;
				}
			| relation_expr AS ColId
				{
					sq_alias_t	   *alias = make_node<sq_alias_t>();

					alias->aliasname = $3;
					$1->alias = alias;
					$$ = $1;
				}
		;

/*
 * TABLESAMPLE decoration in a FROM item
 */
tablesample_clause:
			TABLESAMPLE func_name '(' expr_list ')' opt_repeatable_clause
				{
					sq_range_table_sample_t *n = make_node<sq_range_table_sample_t>();

					/* n->relation will be filled in later */
					n->method = $2;
					n->args = $4;
					n->repeatable = $6;
					n->location = @2;
					$$ = (sq_node_t *) n;
				}
		;

opt_repeatable_clause:
			REPEATABLE '(' a_expr ')'	{ $$ = (sq_node_t *) $3; }
			| /*EMPTY*/					{ $$ = NULL; }
		;

/*
 * func_table represents a function invocation in a FROM list. It can be
 * a plain function call, like "foo(...)", or a ROWS FROM expression with
 * one or more function calls, "ROWS FROM (foo(...), bar(...))",
 * optionally with WITH ORDINALITY attached.
 * In the ROWS FROM syntax, a column definition list can be given for each
 * function, for example:
 *     ROWS FROM (foo() AS (foo_res_a text, foo_res_b text),
 *                bar() AS (bar_res_a text, bar_res_b text))
 * It's also possible to attach a column definition list to the RangeFunction
 * as a whole, but that's handled by the table_ref production.
 */
func_table: func_expr_windowless opt_ordinality
				{
					sq_range_function_t *n = make_node<sq_range_function_t>();

					n->lateral = false;
					n->ordinality = $2;
					n->is_rowsfrom = false;
					n->functions = list_make1(list_make2($1, NIL));
					/* alias and coldeflist are set by table_ref production */
					$$ = (sq_node_t *) n;
				}
			| ROWS FROM '(' rowsfrom_list ')' opt_ordinality
				{
					sq_range_function_t *n = make_node<sq_range_function_t>();

					n->lateral = false;
					n->ordinality = $6;
					n->is_rowsfrom = true;
					n->functions = $4;
					/* alias and coldeflist are set by table_ref production */
					$$ = (sq_node_t *) n;
				}
		;

rowsfrom_item: func_expr_windowless opt_col_def_list
				{ $$ = list_make2($1, $2); }
		;

rowsfrom_list:
			rowsfrom_item						{ $$ = list_make1($1); }
			| rowsfrom_list ',' rowsfrom_item	{ $$ = lappend($1, $3); }
		;

opt_col_def_list: AS '(' TableFuncElementList ')'	{ $$ = $3; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

opt_ordinality: WITH_LA ORDINALITY					{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
		;


where_clause:
			WHERE a_expr							{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

/* variant for UPDATE and DELETE */
where_or_current_clause:
			WHERE a_expr							{ $$ = $2; }
			| WHERE CURRENT_P OF cursor_name
				{
					sq_current_of_expr_t *n = make_node<sq_current_of_expr_t>();

					/* cvarno is filled in by parse analysis */
					n->cursor_name = $4;
					n->cursor_param = 0;
					$$ = (sq_node_t *) n;
				}
			| /*EMPTY*/								{ $$ = NULL; }
		;


OptTableFuncElementList:
			TableFuncElementList				{ $$ = $1; }
			| /*EMPTY*/							{ $$ = NIL; }
		;

TableFuncElementList:
			TableFuncElement
				{
					$$ = list_make1($1);
				}
			| TableFuncElementList ',' TableFuncElement
				{
					$$ = lappend($1, $3);
				}
		;

TableFuncElement:	ColId Typename opt_collate_clause
				{
					sq_column_def_t *n = make_node<sq_column_def_t>();

					n->colname = $1;
					n->type_name = $2;
					n->inhcount = 0;
					n->is_local = true;
					n->is_not_null = false;
					n->is_from_type = false;
					n->storage = 0;
					n->raw_default = NULL;
					n->cooked_default = NULL;
					n->coll_clause = (sq_collate_clause_t *) $3;
					n->coll_oid = invalid_oid;
					n->constraints = NIL;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
		;

/*
 * XMLTABLE
 */
xmltable:
			XMLTABLE '(' c_expr xmlexists_argument COLUMNS xmltable_column_list ')'
				{
					sq_range_table_func_t *n = make_node<sq_range_table_func_t>();

					n->rowexpr = $3;
					n->docexpr = $4;
					n->columns = $6;
					n->namespaces = NIL;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| XMLTABLE '(' XMLNAMESPACES '(' xml_namespace_list ')' ','
				c_expr xmlexists_argument COLUMNS xmltable_column_list ')'
				{
					sq_range_table_func_t *n = make_node<sq_range_table_func_t>();

					n->rowexpr = $8;
					n->docexpr = $9;
					n->columns = $11;
					n->namespaces = $5;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
		;

xmltable_column_list: xmltable_column_el					{ $$ = list_make1($1); }
			| xmltable_column_list ',' xmltable_column_el	{ $$ = lappend($1, $3); }
		;

xmltable_column_el:
			ColId Typename
				{
					sq_range_table_func_col_t *fc = make_node<sq_range_table_func_col_t>();

					fc->colname = $1;
					fc->for_ordinality = false;
					fc->type_name = $2;
					fc->is_not_null = false;
					fc->colexpr = NULL;
					fc->coldefexpr = NULL;
					fc->location = @1;

					$$ = (sq_node_t *) fc;
				}
			| ColId Typename xmltable_column_option_list
				{
					sq_range_table_func_col_t *fc = make_node<sq_range_table_func_col_t>();
					list_cell_t   option;
					bool		nullability_seen = false;

					fc->colname = $1;
					fc->type_name = $2;
					fc->for_ordinality = false;
					fc->is_not_null = false;
					fc->colexpr = NULL;
					fc->coldefexpr = NULL;
					fc->location = @1;

					foreach(option, $3)
					{
						sq_def_elem_t   *defel = (sq_def_elem_t *) lfirst(option);

						if (strcmp(defel->defname, "default") == 0)
						{
							if (fc->coldefexpr != NULL)
								raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(defel->location)}, "only one DEFAULT value is allowed");
							fc->coldefexpr = defel->arg;
						}
						else if (strcmp(defel->defname, "path") == 0)
						{
							if (fc->colexpr != NULL)
								raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(defel->location)}, "only one PATH value per column is allowed");
							fc->colexpr = defel->arg;
						}
						else if (strcmp(defel->defname, "__pg__is_not_null") == 0)
						{
							if (nullability_seen)
								raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(defel->location)}, "conflicting or redundant NULL / NOT NULL declarations for column \"{}\"", fc->colname);
							fc->is_not_null = bool_val(defel->arg);
							nullability_seen = true;
						}
						else
						{
							raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(defel->location)}, "unrecognized column option \"{}\"",
											defel->defname);
						}
					}
					$$ = (sq_node_t *) fc;
				}
			| ColId FOR ORDINALITY
				{
					sq_range_table_func_col_t *fc = make_node<sq_range_table_func_col_t>();

					fc->colname = $1;
					fc->for_ordinality = true;
					/* other fields are ignored, initialized by makeNode */
					fc->location = @1;

					$$ = (sq_node_t *) fc;
				}
		;

xmltable_column_option_list:
			xmltable_column_option_el
				{ $$ = list_make1($1); }
			| xmltable_column_option_list xmltable_column_option_el
				{ $$ = lappend($1, $2); }
		;

xmltable_column_option_el:
			IDENT b_expr
				{
					if (strcmp($1, "__pg__is_not_null") == 0)
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@1)}, "option name \"{}\" cannot be used in XMLTABLE", $1);
					$$ = make_def_elem($1, $2, @1);
				}
			| DEFAULT b_expr
				{ $$ = make_def_elem("default", $2, @1); }
			| NOT NULL_P
				{ $$ = make_def_elem("__pg__is_not_null", (sq_node_t *) make_boolean(true), @1); }
			| NULL_P
				{ $$ = make_def_elem("__pg__is_not_null", (sq_node_t *) make_boolean(false), @1); }
			| PATH b_expr
				{ $$ = make_def_elem("path", $2, @1); }
		;

xml_namespace_list:
			xml_namespace_el
				{ $$ = list_make1($1); }
			| xml_namespace_list ',' xml_namespace_el
				{ $$ = lappend($1, $3); }
		;

xml_namespace_el:
			b_expr AS ColLabel
				{
					$$ = make_node<sq_res_target_t>();
					$$->name = $3;
					$$->indirection = NIL;
					$$->val = $1;
					$$->location = @1;
				}
			| DEFAULT b_expr
				{
					$$ = make_node<sq_res_target_t>();
					$$->name = NULL;
					$$->indirection = NIL;
					$$->val = $2;
					$$->location = @1;
				}
		;

json_table:
			JSON_TABLE '('
				json_value_expr ',' a_expr json_table_path_name_opt
				json_passing_clause_opt
				COLUMNS '(' json_table_column_definition_list ')'
				json_on_error_clause_opt
			')'
				{
					sq_json_table_t *n = make_node<sq_json_table_t>();
					const char *pathstring;

					n->context_item = (sq_json_value_expr_t *) $3;
					if (!is_a($5, SQ_T_A_CONST) ||
						cast_node(sq_a_const_t, $5)->isnull ||
						!is_a(&cast_node(sq_a_const_t, $5)->val.node, SQ_T_STRING))
						raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@5)}, "only string constants are supported in JSON_TABLE path specification");
					pathstring = str_val(&cast_node(sq_a_const_t, $5)->val.node);
					n->pathspec = make_json_table_path_spec(pathstring, $6, @5, @6);
					n->passing = $7;
					n->columns = $10;
					n->on_error = (sq_json_behavior_t *) $12;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
		;

json_table_path_name_opt:
			AS name			{ $$ = $2; }
			| /* empty */	{ $$ = NULL; }
		;

json_table_column_definition_list:
			json_table_column_definition
				{ $$ = list_make1($1); }
			| json_table_column_definition_list ',' json_table_column_definition
				{ $$ = lappend($1, $3); }
		;

json_table_column_definition:
			ColId FOR ORDINALITY
				{
					sq_json_table_column_t *n = make_node<sq_json_table_column_t>();

					n->coltype = SQ_JTC_FOR_ORDINALITY;
					n->name = $1;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| ColId Typename
				json_table_column_path_clause_opt
				json_wrapper_behavior
				json_quotes_clause_opt
				json_behavior_clause_opt
				{
					sq_json_table_column_t *n = make_node<sq_json_table_column_t>();

					n->coltype = SQ_JTC_REGULAR;
					n->name = $1;
					n->type_name = $2;
					n->format = make_json_format(SQ_JS_FORMAT_DEFAULT, SQ_JS_ENC_DEFAULT, -1);
					n->pathspec = (sq_json_table_path_spec_t *) $3;
					n->wrapper = (sq_json_wrapper_t) $4;
					n->quotes = (sq_json_quotes_t) $5;
					n->on_empty = (sq_json_behavior_t *) linitial($6);
					n->on_error = (sq_json_behavior_t *) lsecond($6);
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| ColId Typename json_format_clause
				json_table_column_path_clause_opt
				json_wrapper_behavior
				json_quotes_clause_opt
				json_behavior_clause_opt
				{
					sq_json_table_column_t *n = make_node<sq_json_table_column_t>();

					n->coltype = SQ_JTC_FORMATTED;
					n->name = $1;
					n->type_name = $2;
					n->format = (sq_json_format_t *) $3;
					n->pathspec = (sq_json_table_path_spec_t *) $4;
					n->wrapper = (sq_json_wrapper_t) $5;
					n->quotes = (sq_json_quotes_t) $6;
					n->on_empty = (sq_json_behavior_t *) linitial($7);
					n->on_error = (sq_json_behavior_t *) lsecond($7);
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| ColId Typename
				EXISTS json_table_column_path_clause_opt
				json_on_error_clause_opt
				{
					sq_json_table_column_t *n = make_node<sq_json_table_column_t>();

					n->coltype = SQ_JTC_EXISTS;
					n->name = $1;
					n->type_name = $2;
					n->format = make_json_format(SQ_JS_FORMAT_DEFAULT, SQ_JS_ENC_DEFAULT, -1);
					n->wrapper = SQ_JSW_NONE;
					n->quotes = SQ_JS_QUOTES_UNSPEC;
					n->pathspec = (sq_json_table_path_spec_t *) $4;
					n->on_empty = NULL;
					n->on_error = (sq_json_behavior_t *) $5;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| NESTED path_opt Sconst
				COLUMNS '(' json_table_column_definition_list ')'
				{
					sq_json_table_column_t *n = make_node<sq_json_table_column_t>();

					n->coltype = SQ_JTC_NESTED;
					n->pathspec = (sq_json_table_path_spec_t *)
						make_json_table_path_spec($3, NULL, @3, -1);
					n->columns = $6;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| NESTED path_opt Sconst AS name
				COLUMNS '(' json_table_column_definition_list ')'
				{
					sq_json_table_column_t *n = make_node<sq_json_table_column_t>();

					n->coltype = SQ_JTC_NESTED;
					n->pathspec = (sq_json_table_path_spec_t *)
						make_json_table_path_spec($3, $5, @3, @5);
					n->columns = $8;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
		;

path_opt:
			PATH
			| /* EMPTY */
		;

json_table_column_path_clause_opt:
			PATH Sconst
				{ $$ = (sq_node_t *) make_json_table_path_spec($2, NULL, @2, -1); }
			| /* EMPTY */
				{ $$ = NULL; }
		;

/*****************************************************************************
 *
 *	Type syntax
 *		SQL introduces a large amount of type-specific syntax.
 *		Define individual clauses to handle these cases, and use
 *		 the generic case to handle regular type-extensible Postgres syntax.
 *		- thomas 1997-10-10
 *
 *****************************************************************************/

Typename:	SimpleTypename opt_array_bounds
				{
					$$ = $1;
					$$->array_bounds = $2;
				}
			| SETOF SimpleTypename opt_array_bounds
				{
					$$ = $2;
					$$->array_bounds = $3;
					$$->setof = true;
				}
			/* SQL standard syntax, currently only one-dimensional */
			| SimpleTypename ARRAY '[' Iconst ']'
				{
					$$ = $1;
					$$->array_bounds = list_make1(make_integer($4));
				}
			| SETOF SimpleTypename ARRAY '[' Iconst ']'
				{
					$$ = $2;
					$$->array_bounds = list_make1(make_integer($5));
					$$->setof = true;
				}
			| SimpleTypename ARRAY
				{
					$$ = $1;
					$$->array_bounds = list_make1(make_integer(-1));
				}
			| SETOF SimpleTypename ARRAY
				{
					$$ = $2;
					$$->array_bounds = list_make1(make_integer(-1));
					$$->setof = true;
				}
		;

opt_array_bounds:
			opt_array_bounds '[' ']'
					{  $$ = lappend($1, make_integer(-1)); }
			| opt_array_bounds '[' Iconst ']'
					{  $$ = lappend($1, make_integer($3)); }
			| /*EMPTY*/
					{  $$ = NIL; }
		;

SimpleTypename:
			GenericType								{ $$ = $1; }
			| Numeric								{ $$ = $1; }
			| Bit									{ $$ = $1; }
			| Character								{ $$ = $1; }
			| ConstDatetime							{ $$ = $1; }
			| ConstInterval opt_interval
				{
					$$ = $1;
					$$->typmods = $2;
				}
			| ConstInterval '(' Iconst ')'
				{
					$$ = $1;
					$$->typmods = list_make2(make_int_const(INTERVAL_FULL_RANGE, -1),
											 make_int_const($3, @3));
				}
			| JsonType								{ $$ = $1; }
		;

/* We have a separate ConstTypename to allow defaulting fixed-length
 * types such as CHAR() and BIT() to an unspecified length.
 * SQL9x requires that these default to a length of one, but this
 * makes no sense for constructs like CHAR 'hi' and BIT '0101',
 * where there is an obvious better choice to make.
 * Note that ConstInterval is not included here since it must
 * be pushed up higher in the rules to accommodate the postfix
 * options (e.g. INTERVAL '1' YEAR). Likewise, we have to handle
 * the generic-type-name case in AexprConst to avoid premature
 * reduce/reduce conflicts against function names.
 */
ConstTypename:
			Numeric									{ $$ = $1; }
			| ConstBit								{ $$ = $1; }
			| ConstCharacter						{ $$ = $1; }
			| ConstDatetime							{ $$ = $1; }
			| JsonType								{ $$ = $1; }
		;

/*
 * GenericType covers all type names that don't have special syntax mandated
 * by the standard, including qualified names.  We also allow type modifiers.
 * To avoid parsing conflicts against function invocations, the modifiers
 * have to be shown as expr_list here, but parse analysis will only accept
 * constants for them.
 */
GenericType:
			type_function_name opt_type_modifiers
				{
					$$ = make_type_name($1);
					$$->typmods = $2;
					$$->location = @1;
				}
			| type_function_name attrs opt_type_modifiers
				{
					$$ = make_type_name_from_name_list(lcons(make_string($1), $2));
					$$->typmods = $3;
					$$->location = @1;
				}
		;

opt_type_modifiers: '(' expr_list ')'				{ $$ = $2; }
					| /* EMPTY */					{ $$ = NIL; }
		;

/*
 * SQL numeric data types
 */
Numeric:	INT_P
				{
					$$ = system_type_name("int4");
					$$->location = @1;
				}
			| INTEGER
				{
					$$ = system_type_name("int4");
					$$->location = @1;
				}
			| SMALLINT
				{
					$$ = system_type_name("int2");
					$$->location = @1;
				}
			| BIGINT
				{
					$$ = system_type_name("int8");
					$$->location = @1;
				}
			| REAL
				{
					$$ = system_type_name("float4");
					$$->location = @1;
				}
			| FLOAT_P opt_float
				{
					$$ = $2;
					$$->location = @1;
				}
			| DOUBLE_P PRECISION
				{
					$$ = system_type_name("float8");
					$$->location = @1;
				}
			| DECIMAL_P opt_type_modifiers
				{
					$$ = system_type_name("numeric");
					$$->typmods = $2;
					$$->location = @1;
				}
			| DEC opt_type_modifiers
				{
					$$ = system_type_name("numeric");
					$$->typmods = $2;
					$$->location = @1;
				}
			| NUMERIC opt_type_modifiers
				{
					$$ = system_type_name("numeric");
					$$->typmods = $2;
					$$->location = @1;
				}
			| BOOLEAN_P
				{
					$$ = system_type_name("bool");
					$$->location = @1;
				}
		;

opt_float:	'(' Iconst ')'
				{
					/*
					 * Check FLOAT() precision limits assuming IEEE floating
					 * types - thomas 1997-09-18
					 */
					if ($2 < 1)
						raise({.sqlstate = sqlstate_invalid_parameter_value, .pos = parser_errposition(@2)}, "precision for type float must be at least 1 bit");
					else if ($2 <= 24)
						$$ = system_type_name("float4");
					else if ($2 <= 53)
						$$ = system_type_name("float8");
					else
						raise({.sqlstate = sqlstate_invalid_parameter_value, .pos = parser_errposition(@2)}, "precision for type float must be less than 54 bits");
				}
			| /*EMPTY*/
				{
					$$ = system_type_name("float8");
				}
		;

/*
 * SQL bit-field data types
 * The following implements BIT() and BIT VARYING().
 */
Bit:		BitWithLength
				{
					$$ = $1;
				}
			| BitWithoutLength
				{
					$$ = $1;
				}
		;

/* ConstBit is like Bit except "BIT" defaults to unspecified length */
/* See notes for ConstCharacter, which addresses same issue for "CHAR" */
ConstBit:	BitWithLength
				{
					$$ = $1;
				}
			| BitWithoutLength
				{
					$$ = $1;
					$$->typmods = NIL;
				}
		;

BitWithLength:
			BIT opt_varying '(' expr_list ')'
				{
					char *typname;

					typname = (char *) ($2 ? "varbit" : "bit");
					$$ = system_type_name(typname);
					$$->typmods = $4;
					$$->location = @1;
				}
		;

BitWithoutLength:
			BIT opt_varying
				{
					/* bit defaults to bit(1), varbit to no limit */
					if ($2)
					{
						$$ = system_type_name("varbit");
					}
					else
					{
						$$ = system_type_name("bit");
						$$->typmods = list_make1(make_int_const(1, -1));
					}
					$$->location = @1;
				}
		;


/*
 * SQL character data types
 * The following implements CHAR() and VARCHAR().
 */
Character:  CharacterWithLength
				{
					$$ = $1;
				}
			| CharacterWithoutLength
				{
					$$ = $1;
				}
		;

ConstCharacter:  CharacterWithLength
				{
					$$ = $1;
				}
			| CharacterWithoutLength
				{
					/* Length was not specified so allow to be unrestricted.
					 * This handles problems with fixed-length (bpchar) strings
					 * which in column definitions must default to a length
					 * of one, but should not be constrained if the length
					 * was not specified.
					 */
					$$ = $1;
					$$->typmods = NIL;
				}
		;

CharacterWithLength:  character '(' Iconst ')'
				{
					$$ = system_type_name($1);
					$$->typmods = list_make1(make_int_const($3, @3));
					$$->location = @1;
				}
		;

CharacterWithoutLength:	 character
				{
					$$ = system_type_name($1);
					/* char defaults to char(1), varchar to no limit */
					if (strcmp($1, "bpchar") == 0)
						$$->typmods = list_make1(make_int_const(1, -1));
					$$->location = @1;
				}
		;

character:	CHARACTER opt_varying
										{ $$ = (char *) ($2 ? "varchar": "bpchar"); }
			| CHAR_P opt_varying
										{ $$ = (char *) ($2 ? "varchar": "bpchar"); }
			| VARCHAR
										{ $$ = "varchar"; }
			| NATIONAL CHARACTER opt_varying
										{ $$ = (char *) ($3 ? "varchar": "bpchar"); }
			| NATIONAL CHAR_P opt_varying
										{ $$ = (char *) ($3 ? "varchar": "bpchar"); }
			| NCHAR opt_varying
										{ $$ = (char *) ($2 ? "varchar": "bpchar"); }
		;

opt_varying:
			VARYING									{ $$ = true; }
			| /*EMPTY*/								{ $$ = false; }
		;

/*
 * SQL date/time types
 */
ConstDatetime:
			TIMESTAMP '(' Iconst ')' opt_timezone
				{
					if ($5)
						$$ = system_type_name("timestamptz");
					else
						$$ = system_type_name("timestamp");
					$$->typmods = list_make1(make_int_const($3, @3));
					$$->location = @1;
				}
			| TIMESTAMP opt_timezone
				{
					if ($2)
						$$ = system_type_name("timestamptz");
					else
						$$ = system_type_name("timestamp");
					$$->location = @1;
				}
			| TIME '(' Iconst ')' opt_timezone
				{
					if ($5)
						$$ = system_type_name("timetz");
					else
						$$ = system_type_name("time");
					$$->typmods = list_make1(make_int_const($3, @3));
					$$->location = @1;
				}
			| TIME opt_timezone
				{
					if ($2)
						$$ = system_type_name("timetz");
					else
						$$ = system_type_name("time");
					$$->location = @1;
				}
		;

ConstInterval:
			INTERVAL
				{
					$$ = system_type_name("interval");
					$$->location = @1;
				}
		;

opt_timezone:
			WITH_LA TIME ZONE						{ $$ = true; }
			| WITHOUT_LA TIME ZONE					{ $$ = false; }
			| /*EMPTY*/								{ $$ = false; }
		;

opt_interval:
			YEAR_P
				{ $$ = list_make1(make_int_const(INTERVAL_MASK(YEAR), @1)); }
			| MONTH_P
				{ $$ = list_make1(make_int_const(INTERVAL_MASK(MONTH), @1)); }
			| DAY_P
				{ $$ = list_make1(make_int_const(INTERVAL_MASK(DAY), @1)); }
			| HOUR_P
				{ $$ = list_make1(make_int_const(INTERVAL_MASK(HOUR), @1)); }
			| MINUTE_P
				{ $$ = list_make1(make_int_const(INTERVAL_MASK(MINUTE), @1)); }
			| interval_second
				{ $$ = $1; }
			| YEAR_P TO MONTH_P
				{
					$$ = list_make1(make_int_const(INTERVAL_MASK(YEAR) |
												 INTERVAL_MASK(MONTH), @1));
				}
			| DAY_P TO HOUR_P
				{
					$$ = list_make1(make_int_const(INTERVAL_MASK(DAY) |
												 INTERVAL_MASK(HOUR), @1));
				}
			| DAY_P TO MINUTE_P
				{
					$$ = list_make1(make_int_const(INTERVAL_MASK(DAY) |
												 INTERVAL_MASK(HOUR) |
												 INTERVAL_MASK(MINUTE), @1));
				}
			| DAY_P TO interval_second
				{
					$$ = $3;
					linitial($$) = make_int_const(INTERVAL_MASK(DAY) |
												INTERVAL_MASK(HOUR) |
												INTERVAL_MASK(MINUTE) |
												INTERVAL_MASK(SECOND), @1);
				}
			| HOUR_P TO MINUTE_P
				{
					$$ = list_make1(make_int_const(INTERVAL_MASK(HOUR) |
												 INTERVAL_MASK(MINUTE), @1));
				}
			| HOUR_P TO interval_second
				{
					$$ = $3;
					linitial($$) = make_int_const(INTERVAL_MASK(HOUR) |
												INTERVAL_MASK(MINUTE) |
												INTERVAL_MASK(SECOND), @1);
				}
			| MINUTE_P TO interval_second
				{
					$$ = $3;
					linitial($$) = make_int_const(INTERVAL_MASK(MINUTE) |
												INTERVAL_MASK(SECOND), @1);
				}
			| /*EMPTY*/
				{ $$ = NIL; }
		;

interval_second:
			SECOND_P
				{
					$$ = list_make1(make_int_const(INTERVAL_MASK(SECOND), @1));
				}
			| SECOND_P '(' Iconst ')'
				{
					$$ = list_make2(make_int_const(INTERVAL_MASK(SECOND), @1),
									make_int_const($3, @3));
				}
		;

JsonType:
			JSON
				{
					$$ = system_type_name("json");
					$$->location = @1;
				}
		;

/*****************************************************************************
 *
 *	expression grammar
 *
 *****************************************************************************/

/*
 * General expressions
 * This is the heart of the expression syntax.
 *
 * We have two expression types: a_expr is the unrestricted kind, and
 * b_expr is a subset that must be used in some places to avoid shift/reduce
 * conflicts.  For example, we can't do BETWEEN as "BETWEEN a_expr AND a_expr"
 * because that use of AND conflicts with AND as a boolean operator.  So,
 * b_expr is used in BETWEEN and we remove boolean keywords from b_expr.
 *
 * Note that '(' a_expr ')' is a b_expr, so an unrestricted expression can
 * always be used by surrounding it with parens.
 *
 * c_expr is all the productions that are common to a_expr and b_expr;
 * it's factored out just to eliminate redundant coding.
 *
 * Be careful of productions involving more than one terminal token.
 * By default, bison will assign such productions the precedence of their
 * last terminal, but in nearly all cases you want it to be the precedence
 * of the first terminal instead; otherwise you will not get the behavior
 * you expect!  So we use %prec annotations freely to set precedences.
 */
a_expr:		c_expr									{ $$ = $1; }
			| a_expr TYPECAST Typename
					{ $$ = make_type_cast($1, $3, @2); }
			| a_expr COLLATE any_name
				{
					sq_collate_clause_t *n = make_node<sq_collate_clause_t>();

					n->arg = $1;
					n->collname = $3;
					n->location = @2;
					$$ = (sq_node_t *) n;
				}
			| a_expr AT TIME ZONE a_expr			%prec AT
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("timezone"),
											   list_make2($5, $1),
											   SQ_COERCE_SQL_SYNTAX,
											   @2);
				}
			| a_expr AT LOCAL						%prec AT
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("timezone"),
											   list_make1($1),
											   SQ_COERCE_SQL_SYNTAX,
											   -1);
				}
		/*
		 * These operators must be called out explicitly in order to make use
		 * of bison's automatic operator-precedence handling.  All other
		 * operator names are handled by the generic productions using "Op",
		 * below; and all those operators will have the same precedence.
		 *
		 * If you add more explicitly-known operators, be sure to add them
		 * also to b_expr and to the MathOp list below.
		 */
			| '+' a_expr					%prec UMINUS
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "+", NULL, $2, @1); }
			| '-' a_expr					%prec UMINUS
				{ $$ = do_negate($2, @1); }
			| a_expr '+' a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "+", $1, $3, @2); }
			| a_expr '-' a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "-", $1, $3, @2); }
			| a_expr '*' a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "*", $1, $3, @2); }
			| a_expr '/' a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "/", $1, $3, @2); }
			| a_expr '%' a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "%", $1, $3, @2); }
			| a_expr '^' a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "^", $1, $3, @2); }
			| a_expr '<' a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "<", $1, $3, @2); }
			| a_expr '>' a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, ">", $1, $3, @2); }
			| a_expr '=' a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "=", $1, $3, @2); }
			| a_expr LESS_EQUALS a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "<=", $1, $3, @2); }
			| a_expr GREATER_EQUALS a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, ">=", $1, $3, @2); }
			| a_expr NOT_EQUALS a_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "<>", $1, $3, @2); }

			| a_expr qual_Op a_expr				%prec Op
				{ $$ = (sq_node_t *) make_a_expr(SQ_AEXPR_OP, $2, $1, $3, @2); }
			| qual_Op a_expr					%prec Op
				{ $$ = (sq_node_t *) make_a_expr(SQ_AEXPR_OP, $1, NULL, $2, @1); }

			| a_expr AND a_expr
				{ $$ = make_and_expr($1, $3, @2); }
			| a_expr OR a_expr
				{ $$ = make_or_expr($1, $3, @2); }
			| NOT a_expr
				{ $$ = make_not_expr($2, @1); }
			| NOT_LA a_expr						%prec NOT
				{ $$ = make_not_expr($2, @1); }

			| a_expr LIKE a_expr
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_LIKE, "~~",
												   $1, $3, @2);
				}
			| a_expr LIKE a_expr ESCAPE a_expr					%prec LIKE
				{
					sq_func_call_t   *n = make_func_call(system_func_name("like_escape"),
												 list_make2($3, $5),
												 SQ_COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_LIKE, "~~",
												   $1, (sq_node_t *) n, @2);
				}
			| a_expr NOT_LA LIKE a_expr							%prec NOT_LA
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_LIKE, "!~~",
												   $1, $4, @2);
				}
			| a_expr NOT_LA LIKE a_expr ESCAPE a_expr			%prec NOT_LA
				{
					sq_func_call_t   *n = make_func_call(system_func_name("like_escape"),
												 list_make2($4, $6),
												 SQ_COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_LIKE, "!~~",
												   $1, (sq_node_t *) n, @2);
				}
			| a_expr ILIKE a_expr
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_ILIKE, "~~*",
												   $1, $3, @2);
				}
			| a_expr ILIKE a_expr ESCAPE a_expr					%prec ILIKE
				{
					sq_func_call_t   *n = make_func_call(system_func_name("like_escape"),
												 list_make2($3, $5),
												 SQ_COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_ILIKE, "~~*",
												   $1, (sq_node_t *) n, @2);
				}
			| a_expr NOT_LA ILIKE a_expr						%prec NOT_LA
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_ILIKE, "!~~*",
												   $1, $4, @2);
				}
			| a_expr NOT_LA ILIKE a_expr ESCAPE a_expr			%prec NOT_LA
				{
					sq_func_call_t   *n = make_func_call(system_func_name("like_escape"),
												 list_make2($4, $6),
												 SQ_COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_ILIKE, "!~~*",
												   $1, (sq_node_t *) n, @2);
				}

			| a_expr SIMILAR TO a_expr							%prec SIMILAR
				{
					sq_func_call_t   *n = make_func_call(system_func_name("similar_to_escape"),
												 list_make1($4),
												 SQ_COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_SIMILAR, "~",
												   $1, (sq_node_t *) n, @2);
				}
			| a_expr SIMILAR TO a_expr ESCAPE a_expr			%prec SIMILAR
				{
					sq_func_call_t   *n = make_func_call(system_func_name("similar_to_escape"),
												 list_make2($4, $6),
												 SQ_COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_SIMILAR, "~",
												   $1, (sq_node_t *) n, @2);
				}
			| a_expr NOT_LA SIMILAR TO a_expr					%prec NOT_LA
				{
					sq_func_call_t   *n = make_func_call(system_func_name("similar_to_escape"),
												 list_make1($5),
												 SQ_COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_SIMILAR, "!~",
												   $1, (sq_node_t *) n, @2);
				}
			| a_expr NOT_LA SIMILAR TO a_expr ESCAPE a_expr		%prec NOT_LA
				{
					sq_func_call_t   *n = make_func_call(system_func_name("similar_to_escape"),
												 list_make2($5, $7),
												 SQ_COERCE_EXPLICIT_CALL,
												 @2);
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_SIMILAR, "!~",
												   $1, (sq_node_t *) n, @2);
				}

			/* NullTest clause
			 * Define SQL-style Null test clause.
			 * Allow two forms described in the standard:
			 *	a IS NULL
			 *	a IS NOT NULL
			 * Allow two SQL extensions
			 *	a ISNULL
			 *	a NOTNULL
			 */
			| a_expr IS NULL_P							%prec IS
				{
					sq_null_test_t   *n = make_node<sq_null_test_t>();

					n->arg = (sq_node_t *) $1;
					n->nulltesttype = SQ_IS_NULL;
					n->location = @2;
					$$ = (sq_node_t *) n;
				}
			| a_expr ISNULL
				{
					sq_null_test_t   *n = make_node<sq_null_test_t>();

					n->arg = (sq_node_t *) $1;
					n->nulltesttype = SQ_IS_NULL;
					n->location = @2;
					$$ = (sq_node_t *) n;
				}
			| a_expr IS NOT NULL_P						%prec IS
				{
					sq_null_test_t   *n = make_node<sq_null_test_t>();

					n->arg = (sq_node_t *) $1;
					n->nulltesttype = SQ_IS_NOT_NULL;
					n->location = @2;
					$$ = (sq_node_t *) n;
				}
			| a_expr NOTNULL
				{
					sq_null_test_t   *n = make_node<sq_null_test_t>();

					n->arg = (sq_node_t *) $1;
					n->nulltesttype = SQ_IS_NOT_NULL;
					n->location = @2;
					$$ = (sq_node_t *) n;
				}
			| row OVERLAPS row
				{
					if (list_length($1) != 2)
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@1)}, "wrong number of parameters on left side of OVERLAPS expression");
					if (list_length($3) != 2)
						raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@3)}, "wrong number of parameters on right side of OVERLAPS expression");
					$$ = (sq_node_t *) make_func_call(system_func_name("overlaps"),
											   list_concat($1, $3),
											   SQ_COERCE_SQL_SYNTAX,
											   @2);
				}
			| a_expr IS TRUE_P							%prec IS
				{
					sq_boolean_test_t *b = make_node<sq_boolean_test_t>();

					b->arg = (sq_node_t *) $1;
					b->booltesttype = SQ_IS_TRUE;
					b->location = @2;
					$$ = (sq_node_t *) b;
				}
			| a_expr IS NOT TRUE_P						%prec IS
				{
					sq_boolean_test_t *b = make_node<sq_boolean_test_t>();

					b->arg = (sq_node_t *) $1;
					b->booltesttype = SQ_IS_NOT_TRUE;
					b->location = @2;
					$$ = (sq_node_t *) b;
				}
			| a_expr IS FALSE_P							%prec IS
				{
					sq_boolean_test_t *b = make_node<sq_boolean_test_t>();

					b->arg = (sq_node_t *) $1;
					b->booltesttype = SQ_IS_FALSE;
					b->location = @2;
					$$ = (sq_node_t *) b;
				}
			| a_expr IS NOT FALSE_P						%prec IS
				{
					sq_boolean_test_t *b = make_node<sq_boolean_test_t>();

					b->arg = (sq_node_t *) $1;
					b->booltesttype = SQ_IS_NOT_FALSE;
					b->location = @2;
					$$ = (sq_node_t *) b;
				}
			| a_expr IS UNKNOWN							%prec IS
				{
					sq_boolean_test_t *b = make_node<sq_boolean_test_t>();

					b->arg = (sq_node_t *) $1;
					b->booltesttype = SQ_IS_UNKNOWN;
					b->location = @2;
					$$ = (sq_node_t *) b;
				}
			| a_expr IS NOT UNKNOWN						%prec IS
				{
					sq_boolean_test_t *b = make_node<sq_boolean_test_t>();

					b->arg = (sq_node_t *) $1;
					b->booltesttype = SQ_IS_NOT_UNKNOWN;
					b->location = @2;
					$$ = (sq_node_t *) b;
				}
			| a_expr IS DISTINCT FROM a_expr			%prec IS
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_DISTINCT, "=", $1, $5, @2);
				}
			| a_expr IS NOT DISTINCT FROM a_expr		%prec IS
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_NOT_DISTINCT, "=", $1, $6, @2);
				}
			| a_expr BETWEEN opt_asymmetric b_expr AND a_expr		%prec BETWEEN
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_BETWEEN,
												   "BETWEEN",
												   $1,
												   (sq_node_t *) list_make2($4, $6),
												   @2);
				}
			| a_expr NOT_LA BETWEEN opt_asymmetric b_expr AND a_expr %prec NOT_LA
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_NOT_BETWEEN,
												   "NOT BETWEEN",
												   $1,
												   (sq_node_t *) list_make2($5, $7),
												   @2);
				}
			| a_expr BETWEEN SYMMETRIC b_expr AND a_expr			%prec BETWEEN
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_BETWEEN_SYM,
												   "BETWEEN SYMMETRIC",
												   $1,
												   (sq_node_t *) list_make2($4, $6),
												   @2);
				}
			| a_expr NOT_LA BETWEEN SYMMETRIC b_expr AND a_expr		%prec NOT_LA
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_NOT_BETWEEN_SYM,
												   "NOT BETWEEN SYMMETRIC",
												   $1,
												   (sq_node_t *) list_make2($5, $7),
												   @2);
				}
			| a_expr IN_P select_with_parens
				{
					/* generate foo = ANY (subquery) */
					sq_sub_link_t	   *n = make_node<sq_sub_link_t>();

					n->subselect = $3;
					n->sub_link_type = SQ_ANY_SUBLINK;
					n->sub_link_id = 0;
					n->testexpr = $1;
					n->oper_name = NIL;		/* show it's IN not = ANY */
					n->location = @2;
					$$ = (sq_node_t *) n;
				}
			| a_expr IN_P '(' expr_list ')'
				{
					/* generate scalar IN expression */
					sq_a_expr_t *n = make_simple_a_expr(SQ_AEXPR_IN, "=", $1, (sq_node_t *) $4, @2);

					n->rexpr_list_start = @3;
					n->rexpr_list_end = @5;
					$$ = (sq_node_t *) n;
				}
			| a_expr NOT_LA IN_P select_with_parens			%prec NOT_LA
				{
					/* generate NOT (foo = ANY (subquery)) */
					sq_sub_link_t	   *n = make_node<sq_sub_link_t>();

					n->subselect = $4;
					n->sub_link_type = SQ_ANY_SUBLINK;
					n->sub_link_id = 0;
					n->testexpr = $1;
					n->oper_name = NIL;		/* show it's IN not = ANY */
					n->location = @2;
					/* Stick a NOT on top; must have same parse location */
					$$ = make_not_expr((sq_node_t *) n, @2);
				}
			| a_expr NOT_LA IN_P '(' expr_list ')'
				{
					/* generate scalar NOT IN expression */
					sq_a_expr_t *n = make_simple_a_expr(SQ_AEXPR_IN, "<>", $1, (sq_node_t *) $5, @2);

					n->rexpr_list_start = @4;
					n->rexpr_list_end = @6;
					$$ = (sq_node_t *) n;
				}
			| a_expr subquery_Op sub_type select_with_parens	%prec Op
				{
					sq_sub_link_t	   *n = make_node<sq_sub_link_t>();

					n->sub_link_type = (sq_sub_link_type_t) $3;
					n->sub_link_id = 0;
					n->testexpr = $1;
					n->oper_name = $2;
					n->subselect = $4;
					n->location = @2;
					$$ = (sq_node_t *) n;
				}
			| a_expr subquery_Op sub_type '(' a_expr ')'		%prec Op
				{
					if ($3 == SQ_ANY_SUBLINK)
						$$ = (sq_node_t *) make_a_expr(SQ_AEXPR_OP_ANY, $2, $1, $5, @2);
					else
						$$ = (sq_node_t *) make_a_expr(SQ_AEXPR_OP_ALL, $2, $1, $5, @2);
				}
			| UNIQUE opt_unique_null_treatment select_with_parens
				{
					/* Not sure how to get rid of the parentheses
					 * but there are lots of shift/reduce errors without them.
					 *
					 * Should be able to implement this by plopping the entire
					 * select into a node, then transforming the target expressions
					 * from whatever they are into count(*), and testing the
					 * entire result equal to one.
					 * But, will probably implement a separate node in the executor.
					 */
					raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(@1)}, "UNIQUE predicate is not yet implemented");
				}
			| a_expr IS DOCUMENT_P					%prec IS
				{
					$$ = make_xml_expr(SQ_IS_DOCUMENT, NULL, NIL,
									 list_make1($1), @2);
				}
			| a_expr IS NOT DOCUMENT_P				%prec IS
				{
					$$ = make_not_expr(make_xml_expr(SQ_IS_DOCUMENT, NULL, NIL,
												 list_make1($1), @2),
									 @2);
				}
			| a_expr IS NORMALIZED								%prec IS
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("is_normalized"),
											   list_make1($1),
											   SQ_COERCE_SQL_SYNTAX,
											   @2);
				}
			| a_expr IS unicode_normal_form NORMALIZED			%prec IS
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("is_normalized"),
											   list_make2($1, make_string_const($3, @3)),
											   SQ_COERCE_SQL_SYNTAX,
											   @2);
				}
			| a_expr IS NOT NORMALIZED							%prec IS
				{
					$$ = make_not_expr((sq_node_t *) make_func_call(system_func_name("is_normalized"),
														   list_make1($1),
														   SQ_COERCE_SQL_SYNTAX,
														   @2),
									 @2);
				}
			| a_expr IS NOT unicode_normal_form NORMALIZED		%prec IS
				{
					$$ = make_not_expr((sq_node_t *) make_func_call(system_func_name("is_normalized"),
														   list_make2($1, make_string_const($4, @4)),
														   SQ_COERCE_SQL_SYNTAX,
														   @2),
									 @2);
				}
			| a_expr IS json_predicate_type_constraint
					json_key_uniqueness_constraint_opt		%prec IS
				{
					sq_json_format_t *format = make_json_format(SQ_JS_FORMAT_DEFAULT, SQ_JS_ENC_DEFAULT, -1);

					$$ = make_json_is_predicate($1, format, (sq_json_value_type_t) $3, $4, @1);
				}
			/*
			 * Required by SQL/JSON, but there are conflicts
			| a_expr
				json_format_clause
				IS  json_predicate_type_constraint
					json_key_uniqueness_constraint_opt		%prec IS
				{
					$$ = makeJsonIsPredicate($1, $2, $4, $5, @1);
				}
			*/
			| a_expr IS NOT
					json_predicate_type_constraint
					json_key_uniqueness_constraint_opt		%prec IS
				{
					sq_json_format_t *format = make_json_format(SQ_JS_FORMAT_DEFAULT, SQ_JS_ENC_DEFAULT, -1);

					$$ = make_not_expr(make_json_is_predicate($1, format, (sq_json_value_type_t) $4, $5, @1), @1);
				}
			/*
			 * Required by SQL/JSON, but there are conflicts
			| a_expr
				json_format_clause
				IS NOT
					json_predicate_type_constraint
					json_key_uniqueness_constraint_opt		%prec IS
				{
					$$ = makeNotExpr(makeJsonIsPredicate($1, $2, $5, $6, @1), @1);
				}
			*/
			| DEFAULT
				{
					/*
					 * The SQL spec only allows DEFAULT in "contextually typed
					 * expressions", but for us, it's easier to allow it in
					 * any a_expr and then throw error during parse analysis
					 * if it's in an inappropriate context.  This way also
					 * lets us say something smarter than "syntax error".
					 */
					sq_set_to_default_t *n = make_node<sq_set_to_default_t>();

					/* parse analysis will fill in the rest */
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
		;

/*
 * Restricted expressions
 *
 * b_expr is a subset of the complete expression syntax defined by a_expr.
 *
 * Presently, AND, NOT, IS, and IN are the a_expr keywords that would
 * cause trouble in the places where b_expr is used.  For simplicity, we
 * just eliminate all the boolean-keyword-operator productions from b_expr.
 */
b_expr:		c_expr
				{ $$ = $1; }
			| b_expr TYPECAST Typename
				{ $$ = make_type_cast($1, $3, @2); }
			| '+' b_expr					%prec UMINUS
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "+", NULL, $2, @1); }
			| '-' b_expr					%prec UMINUS
				{ $$ = do_negate($2, @1); }
			| b_expr '+' b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "+", $1, $3, @2); }
			| b_expr '-' b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "-", $1, $3, @2); }
			| b_expr '*' b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "*", $1, $3, @2); }
			| b_expr '/' b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "/", $1, $3, @2); }
			| b_expr '%' b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "%", $1, $3, @2); }
			| b_expr '^' b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "^", $1, $3, @2); }
			| b_expr '<' b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "<", $1, $3, @2); }
			| b_expr '>' b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, ">", $1, $3, @2); }
			| b_expr '=' b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "=", $1, $3, @2); }
			| b_expr LESS_EQUALS b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "<=", $1, $3, @2); }
			| b_expr GREATER_EQUALS b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, ">=", $1, $3, @2); }
			| b_expr NOT_EQUALS b_expr
				{ $$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "<>", $1, $3, @2); }
			| b_expr qual_Op b_expr				%prec Op
				{ $$ = (sq_node_t *) make_a_expr(SQ_AEXPR_OP, $2, $1, $3, @2); }
			| qual_Op b_expr					%prec Op
				{ $$ = (sq_node_t *) make_a_expr(SQ_AEXPR_OP, $1, NULL, $2, @1); }
			| b_expr IS DISTINCT FROM b_expr		%prec IS
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_DISTINCT, "=", $1, $5, @2);
				}
			| b_expr IS NOT DISTINCT FROM b_expr	%prec IS
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_NOT_DISTINCT, "=", $1, $6, @2);
				}
			| b_expr IS DOCUMENT_P					%prec IS
				{
					$$ = make_xml_expr(SQ_IS_DOCUMENT, NULL, NIL,
									 list_make1($1), @2);
				}
			| b_expr IS NOT DOCUMENT_P				%prec IS
				{
					$$ = make_not_expr(make_xml_expr(SQ_IS_DOCUMENT, NULL, NIL,
												 list_make1($1), @2),
									 @2);
				}
		;

/*
 * Productions that can be used in both a_expr and b_expr.
 *
 * Note: productions that refer recursively to a_expr or b_expr mostly
 * cannot appear here.	However, it's OK to refer to a_exprs that occur
 * inside parentheses, such as function arguments; that cannot introduce
 * ambiguity to the b_expr syntax.
 */
c_expr:		columnref								{ $$ = $1; }
			| AexprConst							{ $$ = $1; }
			| PARAM opt_indirection
				{
					sq_param_ref_t   *p = make_node<sq_param_ref_t>();

					p->number = $1;
					p->location = @1;
					if ($2)
					{
						sq_a_indirection_t *n = make_node<sq_a_indirection_t>();

						n->arg = (sq_node_t *) p;
						n->indirection = check_indirection($2, yyscanner);
						$$ = (sq_node_t *) n;
					}
					else
						$$ = (sq_node_t *) p;
				}
			| '(' a_expr ')' opt_indirection
				{
					if ($4)
					{
						sq_a_indirection_t *n = make_node<sq_a_indirection_t>();

						n->arg = $2;
						n->indirection = check_indirection($4, yyscanner);
						$$ = (sq_node_t *) n;
					}
					else
						$$ = $2;
				}
			| case_expr
				{ $$ = $1; }
			| func_expr
				{ $$ = $1; }
			| select_with_parens			%prec UMINUS
				{
					sq_sub_link_t	   *n = make_node<sq_sub_link_t>();

					n->sub_link_type = SQ_EXPR_SUBLINK;
					n->sub_link_id = 0;
					n->testexpr = NULL;
					n->oper_name = NIL;
					n->subselect = $1;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| select_with_parens indirection
				{
					/*
					 * Because the select_with_parens nonterminal is designed
					 * to "eat" as many levels of parens as possible, the
					 * '(' a_expr ')' opt_indirection production above will
					 * fail to match a sub-SELECT with indirection decoration;
					 * the sub-SELECT won't be regarded as an a_expr as long
					 * as there are parens around it.  To support applying
					 * subscripting or field selection to a sub-SELECT result,
					 * we need this redundant-looking production.
					 */
					sq_sub_link_t	   *n = make_node<sq_sub_link_t>();
					sq_a_indirection_t *a = make_node<sq_a_indirection_t>();

					n->sub_link_type = SQ_EXPR_SUBLINK;
					n->sub_link_id = 0;
					n->testexpr = NULL;
					n->oper_name = NIL;
					n->subselect = $1;
					n->location = @1;
					a->arg = (sq_node_t *) n;
					a->indirection = check_indirection($2, yyscanner);
					$$ = (sq_node_t *) a;
				}
			| EXISTS select_with_parens
				{
					sq_sub_link_t	   *n = make_node<sq_sub_link_t>();

					n->sub_link_type = SQ_EXISTS_SUBLINK;
					n->sub_link_id = 0;
					n->testexpr = NULL;
					n->oper_name = NIL;
					n->subselect = $2;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| ARRAY select_with_parens
				{
					sq_sub_link_t	   *n = make_node<sq_sub_link_t>();

					n->sub_link_type = SQ_ARRAY_SUBLINK;
					n->sub_link_id = 0;
					n->testexpr = NULL;
					n->oper_name = NIL;
					n->subselect = $2;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| ARRAY array_expr
				{
					sq_a_array_expr_t *n = cast_node(sq_a_array_expr_t, $2);

					/* point outermost A_ArrayExpr to the ARRAY keyword */
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| explicit_row
				{
					sq_row_expr_t	   *r = make_node<sq_row_expr_t>();

					r->args = $1;
					r->row_typeid = invalid_oid;	/* not analyzed yet */
					r->colnames = NIL;	/* to be filled in during analysis */
					r->row_format = SQ_COERCE_EXPLICIT_CALL; /* abuse */
					r->location = @1;
					$$ = (sq_node_t *) r;
				}
			| implicit_row
				{
					sq_row_expr_t	   *r = make_node<sq_row_expr_t>();

					r->args = $1;
					r->row_typeid = invalid_oid;	/* not analyzed yet */
					r->colnames = NIL;	/* to be filled in during analysis */
					r->row_format = SQ_COERCE_IMPLICIT_CAST; /* abuse */
					r->location = @1;
					$$ = (sq_node_t *) r;
				}
			| GROUPING '(' expr_list ')'
			  {
				  sq_grouping_func_t *g = make_node<sq_grouping_func_t>();

				  g->args = $3;
				  g->location = @1;
				  $$ = (sq_node_t *) g;
			  }
		;

func_application: func_name '(' ')'
				{
					$$ = (sq_node_t *) make_func_call($1, NIL,
											   SQ_COERCE_EXPLICIT_CALL,
											   @1);
				}
			| func_name '(' func_arg_list opt_sort_clause ')'
				{
					sq_func_call_t   *n = make_func_call($1, $3,
												 SQ_COERCE_EXPLICIT_CALL,
												 @1);

					n->agg_order = $4;
					$$ = (sq_node_t *) n;
				}
			| func_name '(' VARIADIC func_arg_expr opt_sort_clause ')'
				{
					sq_func_call_t   *n = make_func_call($1, list_make1($4),
												 SQ_COERCE_EXPLICIT_CALL,
												 @1);

					n->func_variadic = true;
					n->agg_order = $5;
					$$ = (sq_node_t *) n;
				}
			| func_name '(' func_arg_list ',' VARIADIC func_arg_expr opt_sort_clause ')'
				{
					sq_func_call_t   *n = make_func_call($1, lappend($3, $6),
												 SQ_COERCE_EXPLICIT_CALL,
												 @1);

					n->func_variadic = true;
					n->agg_order = $7;
					$$ = (sq_node_t *) n;
				}
			| func_name '(' ALL func_arg_list opt_sort_clause ')'
				{
					sq_func_call_t   *n = make_func_call($1, $4,
												 SQ_COERCE_EXPLICIT_CALL,
												 @1);

					n->agg_order = $5;
					/* Ideally we'd mark the FuncCall node to indicate
					 * "must be an aggregate", but there's no provision
					 * for that in FuncCall at the moment.
					 */
					$$ = (sq_node_t *) n;
				}
			| func_name '(' DISTINCT func_arg_list opt_sort_clause ')'
				{
					sq_func_call_t   *n = make_func_call($1, $4,
												 SQ_COERCE_EXPLICIT_CALL,
												 @1);

					n->agg_order = $5;
					n->agg_distinct = true;
					$$ = (sq_node_t *) n;
				}
			| func_name '(' '*' ')'
				{
					/*
					 * We consider AGGREGATE(*) to invoke a parameterless
					 * aggregate.  This does the right thing for COUNT(*),
					 * and there are no other aggregates in SQL that accept
					 * '*' as parameter.
					 *
					 * The FuncCall node is also marked agg_star = true,
					 * so that later processing can detect what the argument
					 * really was.
					 */
					sq_func_call_t   *n = make_func_call($1, NIL,
												 SQ_COERCE_EXPLICIT_CALL,
												 @1);

					n->agg_star = true;
					$$ = (sq_node_t *) n;
				}
		;


/*
 * func_expr and its cousin func_expr_windowless are split out from c_expr just
 * so that we have classifications for "everything that is a function call or
 * looks like one".  This isn't very important, but it saves us having to
 * document which variants are legal in places like "FROM function()" or the
 * backwards-compatible functional-index syntax for CREATE INDEX.
 * (Note that many of the special SQL functions wouldn't actually make any
 * sense as functional index entries, but we ignore that consideration here.)
 */
func_expr: func_application within_group_clause filter_clause over_clause
				{
					sq_func_call_t   *n = (sq_func_call_t *) $1;

					/*
					 * The order clause for WITHIN GROUP and the one for
					 * plain-aggregate ORDER BY share a field, so we have to
					 * check here that at most one is present.  We also check
					 * for DISTINCT and VARIADIC here to give a better error
					 * location.  Other consistency checks are deferred to
					 * parse analysis.
					 */
					if ($2 != NIL)
					{
						if (n->agg_order != NIL)
							raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@2)}, "cannot use multiple ORDER BY clauses with WITHIN GROUP");
						if (n->agg_distinct)
							raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@2)}, "cannot use DISTINCT with WITHIN GROUP");
						if (n->func_variadic)
							raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@2)}, "cannot use VARIADIC with WITHIN GROUP");
						n->agg_order = $2;
						n->agg_within_group = true;
					}
					n->agg_filter = $3;
					n->over = $4;
					$$ = (sq_node_t *) n;
				}
			| json_aggregate_func filter_clause over_clause
				{
					sq_json_agg_constructor_t *n = is_a($1, SQ_T_JSON_OBJECT_AGG) ?
						((sq_json_object_agg_t *) $1)->constructor :
						((sq_json_array_agg_t *) $1)->constructor;

					n->agg_filter = $2;
					n->over = $3;
					$$ = (sq_node_t *) $1;
				}
			| func_expr_common_subexpr
				{ $$ = $1; }
		;

/*
 * Like func_expr but does not accept WINDOW functions directly
 * (but they can still be contained in arguments for functions etc).
 * Use this when window expressions are not allowed, where needed to
 * disambiguate the grammar (e.g. in CREATE INDEX).
 */
func_expr_windowless:
			func_application						{ $$ = $1; }
			| func_expr_common_subexpr				{ $$ = $1; }
			| json_aggregate_func					{ $$ = $1; }
		;

/*
 * Special expressions that are considered to be functions.
 */
func_expr_common_subexpr:
			COLLATION FOR '(' a_expr ')'
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("pg_collation_for"),
											   list_make1($4),
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| CURRENT_DATE
				{
					$$ = make_sql_value_function(SQ_SVFOP_CURRENT_DATE, -1, @1);
				}
			| CURRENT_TIME
				{
					$$ = make_sql_value_function(SQ_SVFOP_CURRENT_TIME, -1, @1);
				}
			| CURRENT_TIME '(' Iconst ')'
				{
					$$ = make_sql_value_function(SQ_SVFOP_CURRENT_TIME_N, $3, @1);
				}
			| CURRENT_TIMESTAMP
				{
					$$ = make_sql_value_function(SQ_SVFOP_CURRENT_TIMESTAMP, -1, @1);
				}
			| CURRENT_TIMESTAMP '(' Iconst ')'
				{
					$$ = make_sql_value_function(SQ_SVFOP_CURRENT_TIMESTAMP_N, $3, @1);
				}
			| LOCALTIME
				{
					$$ = make_sql_value_function(SQ_SVFOP_LOCALTIME, -1, @1);
				}
			| LOCALTIME '(' Iconst ')'
				{
					$$ = make_sql_value_function(SQ_SVFOP_LOCALTIME_N, $3, @1);
				}
			| LOCALTIMESTAMP
				{
					$$ = make_sql_value_function(SQ_SVFOP_LOCALTIMESTAMP, -1, @1);
				}
			| LOCALTIMESTAMP '(' Iconst ')'
				{
					$$ = make_sql_value_function(SQ_SVFOP_LOCALTIMESTAMP_N, $3, @1);
				}
			| CURRENT_ROLE
				{
					$$ = make_sql_value_function(SQ_SVFOP_CURRENT_ROLE, -1, @1);
				}
			| CURRENT_USER
				{
					$$ = make_sql_value_function(SQ_SVFOP_CURRENT_USER, -1, @1);
				}
			| SESSION_USER
				{
					$$ = make_sql_value_function(SQ_SVFOP_SESSION_USER, -1, @1);
				}
			| SYSTEM_USER
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("system_user"),
											   NIL,
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| USER
				{
					$$ = make_sql_value_function(SQ_SVFOP_USER, -1, @1);
				}
			| CURRENT_CATALOG
				{
					$$ = make_sql_value_function(SQ_SVFOP_CURRENT_CATALOG, -1, @1);
				}
			| CURRENT_SCHEMA
				{
					$$ = make_sql_value_function(SQ_SVFOP_CURRENT_SCHEMA, -1, @1);
				}
			| CAST '(' a_expr AS Typename ')'
				{ $$ = make_type_cast($3, $5, @1); }
			| EXTRACT '(' extract_list ')'
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("extract"),
											   $3,
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| NORMALIZE '(' a_expr ')'
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("normalize"),
											   list_make1($3),
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| NORMALIZE '(' a_expr ',' unicode_normal_form ')'
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("normalize"),
											   list_make2($3, make_string_const($5, @5)),
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| OVERLAY '(' overlay_list ')'
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("overlay"),
											   $3,
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| OVERLAY '(' func_arg_list_opt ')'
				{
					/*
					 * allow functions named overlay() to be called without
					 * special syntax
					 */
					$$ = (sq_node_t *) make_func_call(list_make1(make_string("overlay")),
											   $3,
											   SQ_COERCE_EXPLICIT_CALL,
											   @1);
				}
			| POSITION '(' position_list ')'
				{
					/*
					 * position(A in B) is converted to position(B, A)
					 *
					 * We deliberately don't offer a "plain syntax" option
					 * for position(), because the reversal of the arguments
					 * creates too much risk of confusion.
					 */
					$$ = (sq_node_t *) make_func_call(system_func_name("position"),
											   $3,
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| SUBSTRING '(' substr_list ')'
				{
					/* substring(A from B for C) is converted to
					 * substring(A, B, C) - thomas 2000-11-28
					 */
					$$ = (sq_node_t *) make_func_call(system_func_name("substring"),
											   $3,
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| SUBSTRING '(' func_arg_list_opt ')'
				{
					/*
					 * allow functions named substring() to be called without
					 * special syntax
					 */
					$$ = (sq_node_t *) make_func_call(list_make1(make_string("substring")),
											   $3,
											   SQ_COERCE_EXPLICIT_CALL,
											   @1);
				}
			| TREAT '(' a_expr AS Typename ')'
				{
					/* TREAT(expr AS target) converts expr of a particular type to target,
					 * which is defined to be a subtype of the original expression.
					 * In SQL99, this is intended for use with structured UDTs,
					 * but let's make this a generally useful form allowing stronger
					 * coercions than are handled by implicit casting.
					 *
					 * Convert SystemTypeName() to SystemFuncName() even though
					 * at the moment they result in the same thing.
					 */
					$$ = (sq_node_t *) make_func_call(system_func_name(str_val(llast($5->names))),
											   list_make1($3),
											   SQ_COERCE_EXPLICIT_CALL,
											   @1);
				}
			| TRIM '(' BOTH trim_list ')'
				{
					/* various trim expressions are defined in SQL
					 * - thomas 1997-07-19
					 */
					$$ = (sq_node_t *) make_func_call(system_func_name("btrim"),
											   $4,
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| TRIM '(' LEADING trim_list ')'
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("ltrim"),
											   $4,
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| TRIM '(' TRAILING trim_list ')'
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("rtrim"),
											   $4,
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| TRIM '(' trim_list ')'
				{
					$$ = (sq_node_t *) make_func_call(system_func_name("btrim"),
											   $3,
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| NULLIF '(' a_expr ',' a_expr ')'
				{
					$$ = (sq_node_t *) make_simple_a_expr(SQ_AEXPR_NULLIF, "=", $3, $5, @1);
				}
			| COALESCE '(' expr_list ')'
				{
					sq_coalesce_expr_t *c = make_node<sq_coalesce_expr_t>();

					c->args = $3;
					c->location = @1;
					$$ = (sq_node_t *) c;
				}
			| GREATEST '(' expr_list ')'
				{
					sq_min_max_expr_t *v = make_node<sq_min_max_expr_t>();

					v->args = $3;
					v->op = SQ_IS_GREATEST;
					v->location = @1;
					$$ = (sq_node_t *) v;
				}
			| LEAST '(' expr_list ')'
				{
					sq_min_max_expr_t *v = make_node<sq_min_max_expr_t>();

					v->args = $3;
					v->op = SQ_IS_LEAST;
					v->location = @1;
					$$ = (sq_node_t *) v;
				}
			| XMLCONCAT '(' expr_list ')'
				{
					$$ = make_xml_expr(SQ_IS_XMLCONCAT, NULL, NIL, $3, @1);
				}
			| XMLELEMENT '(' NAME_P ColLabel ')'
				{
					$$ = make_xml_expr(SQ_IS_XMLELEMENT, $4, NIL, NIL, @1);
				}
			| XMLELEMENT '(' NAME_P ColLabel ',' xml_attributes ')'
				{
					$$ = make_xml_expr(SQ_IS_XMLELEMENT, $4, $6, NIL, @1);
				}
			| XMLELEMENT '(' NAME_P ColLabel ',' expr_list ')'
				{
					$$ = make_xml_expr(SQ_IS_XMLELEMENT, $4, NIL, $6, @1);
				}
			| XMLELEMENT '(' NAME_P ColLabel ',' xml_attributes ',' expr_list ')'
				{
					$$ = make_xml_expr(SQ_IS_XMLELEMENT, $4, $6, $8, @1);
				}
			| XMLEXISTS '(' c_expr xmlexists_argument ')'
				{
					/* xmlexists(A PASSING [BY REF] B [BY REF]) is
					 * converted to xmlexists(A, B)*/
					$$ = (sq_node_t *) make_func_call(system_func_name("xmlexists"),
											   list_make2($3, $4),
											   SQ_COERCE_SQL_SYNTAX,
											   @1);
				}
			| XMLFOREST '(' xml_attribute_list ')'
				{
					$$ = make_xml_expr(SQ_IS_XMLFOREST, NULL, $3, NIL, @1);
				}
			| XMLPARSE '(' document_or_content a_expr xml_whitespace_option ')'
				{
					sq_xml_expr_t *x = (sq_xml_expr_t *)
						make_xml_expr(SQ_IS_XMLPARSE, NULL, NIL,
									list_make2($4, make_bool_a_const($5, -1)),
									@1);

					x->xmloption = (sq_xml_option_type_t) $3;
					$$ = (sq_node_t *) x;
				}
			| XMLPI '(' NAME_P ColLabel ')'
				{
					$$ = make_xml_expr(SQ_IS_XMLPI, $4, NULL, NIL, @1);
				}
			| XMLPI '(' NAME_P ColLabel ',' a_expr ')'
				{
					$$ = make_xml_expr(SQ_IS_XMLPI, $4, NULL, list_make1($6), @1);
				}
			| XMLROOT '(' a_expr ',' xml_root_version opt_xml_root_standalone ')'
				{
					$$ = make_xml_expr(SQ_IS_XMLROOT, NULL, NIL,
									 list_make3($3, $5, $6), @1);
				}
			| XMLSERIALIZE '(' document_or_content a_expr AS SimpleTypename xml_indent_option ')'
				{
					sq_xml_serialize_t *n = make_node<sq_xml_serialize_t>();

					n->xmloption = (sq_xml_option_type_t) $3;
					n->expr = $4;
					n->type_name = $6;
					n->indent = $7;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| JSON_OBJECT '(' func_arg_list ')'
				{
					/* Support for legacy (non-standard) json_object() */
					$$ = (sq_node_t *) make_func_call(system_func_name("json_object"),
											   $3, SQ_COERCE_EXPLICIT_CALL, @1);
				}
			| JSON_OBJECT '(' json_name_and_value_list
				json_object_constructor_null_clause_opt
				json_key_uniqueness_constraint_opt
				json_returning_clause_opt ')'
				{
					sq_json_object_constructor_t *n = make_node<sq_json_object_constructor_t>();

					n->exprs = $3;
					n->absent_on_null = $4;
					n->unique = $5;
					n->output = (sq_json_output_t *) $6;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| JSON_OBJECT '(' json_returning_clause_opt ')'
				{
					sq_json_object_constructor_t *n = make_node<sq_json_object_constructor_t>();

					n->exprs = NIL;
					n->absent_on_null = false;
					n->unique = false;
					n->output = (sq_json_output_t *) $3;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| JSON_ARRAY '('
				json_value_expr_list
				json_array_constructor_null_clause_opt
				json_returning_clause_opt
			')'
				{
					sq_json_array_constructor_t *n = make_node<sq_json_array_constructor_t>();

					n->exprs = $3;
					n->absent_on_null = $4;
					n->output = (sq_json_output_t *) $5;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| JSON_ARRAY '('
				select_no_parens
				json_format_clause_opt
				/* json_array_constructor_null_clause_opt */
				json_returning_clause_opt
			')'
				{
					sq_json_array_query_constructor_t *n = make_node<sq_json_array_query_constructor_t>();

					n->query = $3;
					n->format = (sq_json_format_t *) $4;
					n->absent_on_null = true;	/* XXX */
					n->output = (sq_json_output_t *) $5;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| JSON_ARRAY '('
				json_returning_clause_opt
			')'
				{
					sq_json_array_constructor_t *n = make_node<sq_json_array_constructor_t>();

					n->exprs = NIL;
					n->absent_on_null = true;
					n->output = (sq_json_output_t *) $3;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| JSON '(' json_value_expr json_key_uniqueness_constraint_opt ')'
				{
					sq_json_parse_expr_t *n = make_node<sq_json_parse_expr_t>();

					n->expr = (sq_json_value_expr_t *) $3;
					n->unique_keys = $4;
					n->output = NULL;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| JSON_SCALAR '(' a_expr ')'
				{
					sq_json_scalar_expr_t *n = make_node<sq_json_scalar_expr_t>();

					n->expr = (sq_node_t *) $3;
					n->output = NULL;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| JSON_SERIALIZE '(' json_value_expr json_returning_clause_opt ')'
				{
					sq_json_serialize_expr_t *n = make_node<sq_json_serialize_expr_t>();

					n->expr = (sq_json_value_expr_t *) $3;
					n->output = (sq_json_output_t *) $4;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| MERGE_ACTION '(' ')'
				{
					sq_merge_support_func_t *m = make_node<sq_merge_support_func_t>();

					m->msftype = TEXTOID;
					m->location = @1;
					$$ = (sq_node_t *) m;
				}
			| JSON_QUERY '('
				json_value_expr ',' a_expr json_passing_clause_opt
				json_returning_clause_opt
				json_wrapper_behavior
				json_quotes_clause_opt
				json_behavior_clause_opt
			')'
				{
					sq_json_func_expr_t *n = make_node<sq_json_func_expr_t>();

					n->op = SQ_JSON_QUERY_OP;
					n->context_item = (sq_json_value_expr_t *) $3;
					n->pathspec = $5;
					n->passing = $6;
					n->output = (sq_json_output_t *) $7;
					n->wrapper = (sq_json_wrapper_t) $8;
					n->quotes = (sq_json_quotes_t) $9;
					n->on_empty = (sq_json_behavior_t *) linitial($10);
					n->on_error = (sq_json_behavior_t *) lsecond($10);
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| JSON_EXISTS '('
				json_value_expr ',' a_expr json_passing_clause_opt
				json_on_error_clause_opt
			')'
				{
					sq_json_func_expr_t *n = make_node<sq_json_func_expr_t>();

					n->op = SQ_JSON_EXISTS_OP;
					n->context_item = (sq_json_value_expr_t *) $3;
					n->pathspec = $5;
					n->passing = $6;
					n->output = NULL;
					n->on_error = (sq_json_behavior_t *) $7;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			| JSON_VALUE '('
				json_value_expr ',' a_expr json_passing_clause_opt
				json_returning_clause_opt
				json_behavior_clause_opt
			')'
				{
					sq_json_func_expr_t *n = make_node<sq_json_func_expr_t>();

					n->op = SQ_JSON_VALUE_OP;
					n->context_item = (sq_json_value_expr_t *) $3;
					n->pathspec = $5;
					n->passing = $6;
					n->output = (sq_json_output_t *) $7;
					n->on_empty = (sq_json_behavior_t *) linitial($8);
					n->on_error = (sq_json_behavior_t *) lsecond($8);
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
			;


/*
 * SQL/XML support
 */
xml_root_version: VERSION_P a_expr
				{ $$ = $2; }
			| VERSION_P NO VALUE_P
				{ $$ = make_null_a_const(-1); }
		;

opt_xml_root_standalone: ',' STANDALONE_P YES_P
				{ $$ = make_int_const(XML_STANDALONE_YES, -1); }
			| ',' STANDALONE_P NO
				{ $$ = make_int_const(XML_STANDALONE_NO, -1); }
			| ',' STANDALONE_P NO VALUE_P
				{ $$ = make_int_const(XML_STANDALONE_NO_VALUE, -1); }
			| /*EMPTY*/
				{ $$ = make_int_const(XML_STANDALONE_OMITTED, -1); }
		;

xml_attributes: XMLATTRIBUTES '(' xml_attribute_list ')'	{ $$ = $3; }
		;

xml_attribute_list:	xml_attribute_el					{ $$ = list_make1($1); }
			| xml_attribute_list ',' xml_attribute_el	{ $$ = lappend($1, $3); }
		;

xml_attribute_el: a_expr AS ColLabel
				{
					$$ = make_node<sq_res_target_t>();
					$$->name = $3;
					$$->indirection = NIL;
					$$->val = (sq_node_t *) $1;
					$$->location = @1;
				}
			| a_expr
				{
					$$ = make_node<sq_res_target_t>();
					$$->name = NULL;
					$$->indirection = NIL;
					$$->val = (sq_node_t *) $1;
					$$->location = @1;
				}
		;

document_or_content: DOCUMENT_P						{ $$ = SQ_XMLOPTION_DOCUMENT; }
			| CONTENT_P								{ $$ = SQ_XMLOPTION_CONTENT; }
		;

xml_indent_option: INDENT							{ $$ = true; }
			| NO INDENT								{ $$ = false; }
			| /*EMPTY*/								{ $$ = false; }
		;

xml_whitespace_option: PRESERVE WHITESPACE_P		{ $$ = true; }
			| STRIP_P WHITESPACE_P					{ $$ = false; }
			| /*EMPTY*/								{ $$ = false; }
		;

/* We allow several variants for SQL and other compatibility. */
xmlexists_argument:
			PASSING c_expr
				{
					$$ = $2;
				}
			| PASSING c_expr xml_passing_mech
				{
					$$ = $2;
				}
			| PASSING xml_passing_mech c_expr
				{
					$$ = $3;
				}
			| PASSING xml_passing_mech c_expr xml_passing_mech
				{
					$$ = $3;
				}
		;

xml_passing_mech:
			BY REF_P
			| BY VALUE_P
		;


/*
 * Aggregate decoration clauses
 */
within_group_clause:
			WITHIN GROUP_P '(' sort_clause ')'		{ $$ = $4; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

filter_clause:
			FILTER '(' WHERE a_expr ')'				{ $$ = $4; }
			| /*EMPTY*/								{ $$ = NULL; }
		;


/*
 * Window Definitions
 */
window_clause:
			WINDOW window_definition_list			{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

window_definition_list:
			window_definition						{ $$ = list_make1($1); }
			| window_definition_list ',' window_definition
													{ $$ = lappend($1, $3); }
		;

window_definition:
			ColId AS window_specification
				{
					sq_window_def_t  *n = $3;

					n->name = $1;
					$$ = n;
				}
		;

over_clause: OVER window_specification
				{ $$ = $2; }
			| OVER ColId
				{
					sq_window_def_t  *n = make_node<sq_window_def_t>();

					n->name = $2;
					n->refname = NULL;
					n->partition_clause = NIL;
					n->order_clause = NIL;
					n->frame_options = SQ_FRAMEOPTION_DEFAULTS;
					n->start_offset = NULL;
					n->end_offset = NULL;
					n->location = @2;
					$$ = n;
				}
			| /*EMPTY*/
				{ $$ = NULL; }
		;

window_specification: '(' opt_existing_window_name opt_partition_clause
						opt_sort_clause opt_frame_clause ')'
				{
					sq_window_def_t  *n = make_node<sq_window_def_t>();

					n->name = NULL;
					n->refname = $2;
					n->partition_clause = $3;
					n->order_clause = $4;
					/* copy relevant fields of opt_frame_clause */
					n->frame_options = $5->frame_options;
					n->start_offset = $5->start_offset;
					n->end_offset = $5->end_offset;
					n->location = @1;
					$$ = n;
				}
		;

/*
 * If we see PARTITION, RANGE, ROWS or GROUPS as the first token after the '('
 * of a window_specification, we want the assumption to be that there is
 * no existing_window_name; but those keywords are unreserved and so could
 * be ColIds.  We fix this by making them have the same precedence as IDENT
 * and giving the empty production here a slightly higher precedence, so
 * that the shift/reduce conflict is resolved in favor of reducing the rule.
 * These keywords are thus precluded from being an existing_window_name but
 * are not reserved for any other purpose.
 */
opt_existing_window_name: ColId						{ $$ = $1; }
			| /*EMPTY*/				%prec Op		{ $$ = NULL; }
		;

opt_partition_clause: PARTITION BY expr_list		{ $$ = $3; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

/*
 * For frame clauses, we return a WindowDef, but only some fields are used:
 * frameOptions, startOffset, and endOffset.
 */
opt_frame_clause:
			RANGE frame_extent opt_window_exclusion_clause
				{
					sq_window_def_t  *n = $2;

					n->frame_options |= SQ_FRAMEOPTION_NONDEFAULT | SQ_FRAMEOPTION_RANGE;
					n->frame_options |= $3;
					$$ = n;
				}
			| ROWS frame_extent opt_window_exclusion_clause
				{
					sq_window_def_t  *n = $2;

					n->frame_options |= SQ_FRAMEOPTION_NONDEFAULT | SQ_FRAMEOPTION_ROWS;
					n->frame_options |= $3;
					$$ = n;
				}
			| GROUPS frame_extent opt_window_exclusion_clause
				{
					sq_window_def_t  *n = $2;

					n->frame_options |= SQ_FRAMEOPTION_NONDEFAULT | SQ_FRAMEOPTION_GROUPS;
					n->frame_options |= $3;
					$$ = n;
				}
			| /*EMPTY*/
				{
					sq_window_def_t  *n = make_node<sq_window_def_t>();

					n->frame_options = SQ_FRAMEOPTION_DEFAULTS;
					n->start_offset = NULL;
					n->end_offset = NULL;
					$$ = n;
				}
		;

frame_extent: frame_bound
				{
					sq_window_def_t  *n = $1;

					/* reject invalid cases */
					if (n->frame_options & SQ_FRAMEOPTION_START_UNBOUNDED_FOLLOWING)
						raise({.sqlstate = sqlstate_windowing_error, .pos = parser_errposition(@1)}, "frame start cannot be UNBOUNDED FOLLOWING");
					if (n->frame_options & SQ_FRAMEOPTION_START_OFFSET_FOLLOWING)
						raise({.sqlstate = sqlstate_windowing_error, .pos = parser_errposition(@1)}, "frame starting from following row cannot end with current row");
					n->frame_options |= SQ_FRAMEOPTION_END_CURRENT_ROW;
					$$ = n;
				}
			| BETWEEN frame_bound AND frame_bound
				{
					sq_window_def_t  *n1 = $2;
					sq_window_def_t  *n2 = $4;

					/* form merged options */
					int		frame_options = n1->frame_options;
					/* shift converts START_ options to END_ options */
					frame_options |= n2->frame_options << 1;
					frame_options |= SQ_FRAMEOPTION_BETWEEN;
					/* reject invalid cases */
					if (frame_options & SQ_FRAMEOPTION_START_UNBOUNDED_FOLLOWING)
						raise({.sqlstate = sqlstate_windowing_error, .pos = parser_errposition(@2)}, "frame start cannot be UNBOUNDED FOLLOWING");
					if (frame_options & SQ_FRAMEOPTION_END_UNBOUNDED_PRECEDING)
						raise({.sqlstate = sqlstate_windowing_error, .pos = parser_errposition(@4)}, "frame end cannot be UNBOUNDED PRECEDING");
					if ((frame_options & SQ_FRAMEOPTION_START_CURRENT_ROW) &&
						(frame_options & SQ_FRAMEOPTION_END_OFFSET_PRECEDING))
						raise({.sqlstate = sqlstate_windowing_error, .pos = parser_errposition(@4)}, "frame starting from current row cannot have preceding rows");
					if ((frame_options & SQ_FRAMEOPTION_START_OFFSET_FOLLOWING) &&
						(frame_options & (SQ_FRAMEOPTION_END_OFFSET_PRECEDING |
										 SQ_FRAMEOPTION_END_CURRENT_ROW)))
						raise({.sqlstate = sqlstate_windowing_error, .pos = parser_errposition(@4)}, "frame starting from following row cannot have preceding rows");
					n1->frame_options = frame_options;
					n1->end_offset = n2->start_offset;
					$$ = n1;
				}
		;

/*
 * This is used for both frame start and frame end, with output set up on
 * the assumption it's frame start; the frame_extent productions must reject
 * invalid cases.
 */
frame_bound:
			UNBOUNDED PRECEDING
				{
					sq_window_def_t  *n = make_node<sq_window_def_t>();

					n->frame_options = SQ_FRAMEOPTION_START_UNBOUNDED_PRECEDING;
					n->start_offset = NULL;
					n->end_offset = NULL;
					$$ = n;
				}
			| UNBOUNDED FOLLOWING
				{
					sq_window_def_t  *n = make_node<sq_window_def_t>();

					n->frame_options = SQ_FRAMEOPTION_START_UNBOUNDED_FOLLOWING;
					n->start_offset = NULL;
					n->end_offset = NULL;
					$$ = n;
				}
			| CURRENT_P ROW
				{
					sq_window_def_t  *n = make_node<sq_window_def_t>();

					n->frame_options = SQ_FRAMEOPTION_START_CURRENT_ROW;
					n->start_offset = NULL;
					n->end_offset = NULL;
					$$ = n;
				}
			| a_expr PRECEDING
				{
					sq_window_def_t  *n = make_node<sq_window_def_t>();

					n->frame_options = SQ_FRAMEOPTION_START_OFFSET_PRECEDING;
					n->start_offset = $1;
					n->end_offset = NULL;
					$$ = n;
				}
			| a_expr FOLLOWING
				{
					sq_window_def_t  *n = make_node<sq_window_def_t>();

					n->frame_options = SQ_FRAMEOPTION_START_OFFSET_FOLLOWING;
					n->start_offset = $1;
					n->end_offset = NULL;
					$$ = n;
				}
		;

opt_window_exclusion_clause:
			EXCLUDE CURRENT_P ROW	{ $$ = SQ_FRAMEOPTION_EXCLUDE_CURRENT_ROW; }
			| EXCLUDE GROUP_P		{ $$ = SQ_FRAMEOPTION_EXCLUDE_GROUP; }
			| EXCLUDE TIES			{ $$ = SQ_FRAMEOPTION_EXCLUDE_TIES; }
			| EXCLUDE NO OTHERS		{ $$ = 0; }
			| /*EMPTY*/				{ $$ = 0; }
		;


/*
 * Supporting nonterminals for expressions.
 */

/* Explicit row production.
 *
 * SQL99 allows an optional ROW keyword, so we can now do single-element rows
 * without conflicting with the parenthesized a_expr production.  Without the
 * ROW keyword, there must be more than one a_expr inside the parens.
 */
row:		ROW '(' expr_list ')'					{ $$ = $3; }
			| ROW '(' ')'							{ $$ = NIL; }
			| '(' expr_list ',' a_expr ')'			{ $$ = lappend($2, $4); }
		;

explicit_row:	ROW '(' expr_list ')'				{ $$ = $3; }
			| ROW '(' ')'							{ $$ = NIL; }
		;

implicit_row:	'(' expr_list ',' a_expr ')'		{ $$ = lappend($2, $4); }
		;

sub_type:	ANY										{ $$ = SQ_ANY_SUBLINK; }
			| SOME									{ $$ = SQ_ANY_SUBLINK; }
			| ALL									{ $$ = SQ_ALL_SUBLINK; }
		;

all_Op:		Op										{ $$ = $1; }
			| MathOp								{ $$ = $1; }
		;

MathOp:		 '+'									{ $$ = "+"; }
			| '-'									{ $$ = "-"; }
			| '*'									{ $$ = "*"; }
			| '/'									{ $$ = "/"; }
			| '%'									{ $$ = "%"; }
			| '^'									{ $$ = "^"; }
			| '<'									{ $$ = "<"; }
			| '>'									{ $$ = ">"; }
			| '='									{ $$ = "="; }
			| LESS_EQUALS							{ $$ = "<="; }
			| GREATER_EQUALS						{ $$ = ">="; }
			| NOT_EQUALS							{ $$ = "<>"; }
		;

qual_Op:	Op
					{ $$ = list_make1(make_string($1)); }
			| OPERATOR '(' any_operator ')'
					{ $$ = $3; }
		;

qual_all_Op:
			all_Op
					{ $$ = list_make1(make_string($1)); }
			| OPERATOR '(' any_operator ')'
					{ $$ = $3; }
		;

subquery_Op:
			all_Op
					{ $$ = list_make1(make_string($1)); }
			| OPERATOR '(' any_operator ')'
					{ $$ = $3; }
			| LIKE
					{ $$ = list_make1(make_string("~~")); }
			| NOT_LA LIKE
					{ $$ = list_make1(make_string("!~~")); }
			| ILIKE
					{ $$ = list_make1(make_string("~~*")); }
			| NOT_LA ILIKE
					{ $$ = list_make1(make_string("!~~*")); }
/* cannot put SIMILAR TO here, because SIMILAR TO is a hack.
 * the regular expression is preprocessed by a function (similar_to_escape),
 * and the ~ operator for posix regular expressions is used.
 *        x SIMILAR TO y     ->    x ~ similar_to_escape(y)
 * this transformation is made on the fly by the parser upwards.
 * however the SubLink structure which handles any/some/all stuff
 * is not ready for such a thing.
 */
			;

expr_list:	a_expr
				{
					$$ = list_make1($1);
				}
			| expr_list ',' a_expr
				{
					$$ = lappend($1, $3);
				}
		;

/* function arguments can have names */
func_arg_list:  func_arg_expr
				{
					$$ = list_make1($1);
				}
			| func_arg_list ',' func_arg_expr
				{
					$$ = lappend($1, $3);
				}
		;

func_arg_expr:  a_expr
				{
					$$ = $1;
				}
			| param_name COLON_EQUALS a_expr
				{
					sq_named_arg_expr_t *na = make_node<sq_named_arg_expr_t>();

					na->name = $1;
					na->arg = (sq_node_t *) $3;
					na->argnumber = -1;		/* until determined */
					na->location = @1;
					$$ = (sq_node_t *) na;
				}
			| param_name EQUALS_GREATER a_expr
				{
					sq_named_arg_expr_t *na = make_node<sq_named_arg_expr_t>();

					na->name = $1;
					na->arg = (sq_node_t *) $3;
					na->argnumber = -1;		/* until determined */
					na->location = @1;
					$$ = (sq_node_t *) na;
				}
		;

func_arg_list_opt:	func_arg_list					{ $$ = $1; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

type_list:	Typename								{ $$ = list_make1($1); }
			| type_list ',' Typename				{ $$ = lappend($1, $3); }
		;

array_expr: '[' expr_list ']'
				{
					$$ = make_a_array_expr($2, @1, @3);
				}
			| '[' array_expr_list ']'
				{
					$$ = make_a_array_expr($2, @1, @3);
				}
			| '[' ']'
				{
					$$ = make_a_array_expr(NIL, @1, @2);
				}
		;

array_expr_list: array_expr							{ $$ = list_make1($1); }
			| array_expr_list ',' array_expr		{ $$ = lappend($1, $3); }
		;


extract_list:
			extract_arg FROM a_expr
				{
					$$ = list_make2(make_string_const($1, @1), $3);
				}
		;

/* Allow delimited string Sconst in extract_arg as an SQL extension.
 * - thomas 2001-04-12
 */
extract_arg:
			IDENT									{ $$ = $1; }
			| YEAR_P								{ $$ = "year"; }
			| MONTH_P								{ $$ = "month"; }
			| DAY_P									{ $$ = "day"; }
			| HOUR_P								{ $$ = "hour"; }
			| MINUTE_P								{ $$ = "minute"; }
			| SECOND_P								{ $$ = "second"; }
			| Sconst								{ $$ = $1; }
		;

unicode_normal_form:
			NFC										{ $$ = "NFC"; }
			| NFD									{ $$ = "NFD"; }
			| NFKC									{ $$ = "NFKC"; }
			| NFKD									{ $$ = "NFKD"; }
		;

/* OVERLAY() arguments */
overlay_list:
			a_expr PLACING a_expr FROM a_expr FOR a_expr
				{
					/* overlay(A PLACING B FROM C FOR D) is converted to overlay(A, B, C, D) */
					$$ = list_make4($1, $3, $5, $7);
				}
			| a_expr PLACING a_expr FROM a_expr
				{
					/* overlay(A PLACING B FROM C) is converted to overlay(A, B, C) */
					$$ = list_make3($1, $3, $5);
				}
		;

/* position_list uses b_expr not a_expr to avoid conflict with general IN */
position_list:
			b_expr IN_P b_expr						{ $$ = list_make2($3, $1); }
		;

/*
 * SUBSTRING() arguments
 *
 * Note that SQL:1999 has both
 *     text FROM int FOR int
 * and
 *     text FROM pattern FOR escape
 *
 * In the parser we map them both to a call to the substring() function and
 * rely on type resolution to pick the right one.
 *
 * In SQL:2003, the second variant was changed to
 *     text SIMILAR pattern ESCAPE escape
 * We could in theory map that to a different function internally, but
 * since we still support the SQL:1999 version, we don't.  However,
 * ruleutils.c will reverse-list the call in the newer style.
 */
substr_list:
			a_expr FROM a_expr FOR a_expr
				{
					$$ = list_make3($1, $3, $5);
				}
			| a_expr FOR a_expr FROM a_expr
				{
					/* not legal per SQL, but might as well allow it */
					$$ = list_make3($1, $5, $3);
				}
			| a_expr FROM a_expr
				{
					/*
					 * Because we aren't restricting data types here, this
					 * syntax can end up resolving to textregexsubstr().
					 * We've historically allowed that to happen, so continue
					 * to accept it.  However, ruleutils.c will reverse-list
					 * such a call in regular function call syntax.
					 */
					$$ = list_make2($1, $3);
				}
			| a_expr FOR a_expr
				{
					/* not legal per SQL */

					/*
					 * Since there are no cases where this syntax allows
					 * a textual FOR value, we forcibly cast the argument
					 * to int4.  The possible matches in pg_proc are
					 * substring(text,int4) and substring(text,text),
					 * and we don't want the parser to choose the latter,
					 * which it is likely to do if the second argument
					 * is unknown or doesn't have an implicit cast to int4.
					 */
					$$ = list_make3($1, make_int_const(1, -1),
									make_type_cast($3,
												 system_type_name("int4"), -1));
				}
			| a_expr SIMILAR a_expr ESCAPE a_expr
				{
					$$ = list_make3($1, $3, $5);
				}
		;

trim_list:	a_expr FROM expr_list					{ $$ = lappend($3, $1); }
			| FROM expr_list						{ $$ = $2; }
			| expr_list								{ $$ = $1; }
		;

/*
 * Define SQL-style CASE clause.
 * - Full specification
 *	CASE WHEN a = b THEN c ... ELSE d END
 * - Implicit argument
 *	CASE a WHEN b THEN c ... ELSE d END
 */
case_expr:	CASE case_arg when_clause_list case_default END_P
				{
					sq_case_expr_t   *c = make_node<sq_case_expr_t>();

					c->casetype = invalid_oid; /* not analyzed yet */
					c->arg = (sq_node_t *) $2;
					c->args = $3;
					c->defresult = (sq_node_t *) $4;
					c->location = @1;
					$$ = (sq_node_t *) c;
				}
		;

when_clause_list:
			/* There must be at least one */
			when_clause								{ $$ = list_make1($1); }
			| when_clause_list when_clause			{ $$ = lappend($1, $2); }
		;

when_clause:
			WHEN a_expr THEN a_expr
				{
					sq_case_when_t   *w = make_node<sq_case_when_t>();

					w->expr = (sq_node_t *) $2;
					w->result = (sq_node_t *) $4;
					w->location = @1;
					$$ = (sq_node_t *) w;
				}
		;

case_default:
			ELSE a_expr								{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

case_arg:	a_expr									{ $$ = $1; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

columnref:	ColId
				{
					$$ = make_column_ref($1, NIL, @1, yyscanner);
				}
			| ColId indirection
				{
					$$ = make_column_ref($1, $2, @1, yyscanner);
				}
		;

indirection_el:
			'.' attr_name
				{
					$$ = (sq_node_t *) make_string($2);
				}
			| '.' '*'
				{
					$$ = (sq_node_t *) make_node<sq_a_star_t>();
				}
			| '[' a_expr ']'
				{
					sq_a_indices_t *ai = make_node<sq_a_indices_t>();

					ai->is_slice = false;
					ai->lidx = NULL;
					ai->uidx = $2;
					$$ = (sq_node_t *) ai;
				}
			| '[' opt_slice_bound ':' opt_slice_bound ']'
				{
					sq_a_indices_t *ai = make_node<sq_a_indices_t>();

					ai->is_slice = true;
					ai->lidx = $2;
					ai->uidx = $4;
					$$ = (sq_node_t *) ai;
				}
		;

opt_slice_bound:
			a_expr									{ $$ = $1; }
			| /*EMPTY*/								{ $$ = NULL; }
		;

indirection:
			indirection_el							{ $$ = list_make1($1); }
			| indirection indirection_el			{ $$ = lappend($1, $2); }
		;

opt_indirection:
			/*EMPTY*/								{ $$ = NIL; }
			| opt_indirection indirection_el		{ $$ = lappend($1, $2); }
		;

opt_asymmetric: ASYMMETRIC
			| /*EMPTY*/
		;

/* SQL/JSON support */
json_passing_clause_opt:
			PASSING json_arguments					{ $$ = $2; }
			| /*EMPTY*/								{ $$ = NIL; }
		;

json_arguments:
			json_argument							{ $$ = list_make1($1); }
			| json_arguments ',' json_argument		{ $$ = lappend($1, $3); }
		;

json_argument:
			json_value_expr AS ColLabel
			{
				sq_json_argument_t *n = make_node<sq_json_argument_t>();

				n->val = (sq_json_value_expr_t *) $1;
				n->name = $3;
				$$ = (sq_node_t *) n;
			}
		;

/* ARRAY is a noise word */
json_wrapper_behavior:
			  WITHOUT WRAPPER					{ $$ = SQ_JSW_NONE; }
			| WITHOUT ARRAY	WRAPPER				{ $$ = SQ_JSW_NONE; }
			| WITH WRAPPER						{ $$ = SQ_JSW_UNCONDITIONAL; }
			| WITH ARRAY WRAPPER				{ $$ = SQ_JSW_UNCONDITIONAL; }
			| WITH CONDITIONAL ARRAY WRAPPER	{ $$ = SQ_JSW_CONDITIONAL; }
			| WITH UNCONDITIONAL ARRAY WRAPPER	{ $$ = SQ_JSW_UNCONDITIONAL; }
			| WITH CONDITIONAL WRAPPER			{ $$ = SQ_JSW_CONDITIONAL; }
			| WITH UNCONDITIONAL WRAPPER		{ $$ = SQ_JSW_UNCONDITIONAL; }
			| /* empty */						{ $$ = SQ_JSW_UNSPEC; }
		;

json_behavior:
			DEFAULT a_expr
				{ $$ = (sq_node_t *) make_json_behavior(SQ_JSON_BEHAVIOR_DEFAULT, $2, @1); }
			| json_behavior_type
				{ $$ = (sq_node_t *) make_json_behavior((sq_json_behavior_type_t) $1, NULL, @1); }
		;

json_behavior_type:
			ERROR_P		{ $$ = SQ_JSON_BEHAVIOR_ERROR; }
			| NULL_P	{ $$ = SQ_JSON_BEHAVIOR_NULL; }
			| TRUE_P	{ $$ = SQ_JSON_BEHAVIOR_TRUE; }
			| FALSE_P	{ $$ = SQ_JSON_BEHAVIOR_FALSE; }
			| UNKNOWN	{ $$ = SQ_JSON_BEHAVIOR_UNKNOWN; }
			| EMPTY_P ARRAY	{ $$ = SQ_JSON_BEHAVIOR_EMPTY_ARRAY; }
			| EMPTY_P OBJECT_P	{ $$ = SQ_JSON_BEHAVIOR_EMPTY_OBJECT; }
			/* non-standard, for Oracle compatibility only */
			| EMPTY_P	{ $$ = SQ_JSON_BEHAVIOR_EMPTY_ARRAY; }
		;

json_behavior_clause_opt:
			json_behavior ON EMPTY_P
				{ $$ = list_make2($1, NULL); }
			| json_behavior ON ERROR_P
				{ $$ = list_make2(NULL, $1); }
			| json_behavior ON EMPTY_P json_behavior ON ERROR_P
				{ $$ = list_make2($1, $4); }
			| /* EMPTY */
				{ $$ = list_make2(NULL, NULL); }
		;

json_on_error_clause_opt:
			json_behavior ON ERROR_P
				{ $$ = $1; }
			| /* EMPTY */
				{ $$ = NULL; }
		;

json_value_expr:
			a_expr json_format_clause_opt
			{
				/* formatted_expr will be set during parse-analysis. */
				$$ = (sq_node_t *) make_json_value_expr((sq_node_t *) $1, NULL,
												cast_node(sq_json_format_t, $2));
			}
		;

json_format_clause:
			FORMAT_LA JSON ENCODING name
				{
					int		encoding;

					if (!strcasecmp($4, "utf8"))
						encoding = SQ_JS_ENC_UTF8;
					else if (!strcasecmp($4, "utf16"))
						encoding = SQ_JS_ENC_UTF16;
					else if (!strcasecmp($4, "utf32"))
						encoding = SQ_JS_ENC_UTF32;
					else
						raise({.sqlstate = sqlstate_invalid_parameter_value, .pos = parser_errposition(@4)}, "unrecognized JSON encoding: {}", $4);

					$$ = (sq_node_t *) make_json_format(SQ_JS_FORMAT_JSON, (sq_json_encoding_t) encoding, @1);
				}
			| FORMAT_LA JSON
				{
					$$ = (sq_node_t *) make_json_format(SQ_JS_FORMAT_JSON, SQ_JS_ENC_DEFAULT, @1);
				}
		;

json_format_clause_opt:
			json_format_clause
				{
					$$ = $1;
				}
			| /* EMPTY */
				{
					$$ = (sq_node_t *) make_json_format(SQ_JS_FORMAT_DEFAULT, SQ_JS_ENC_DEFAULT, -1);
				}
		;

json_quotes_clause_opt:
			KEEP QUOTES ON SCALAR STRING_P		{ $$ = SQ_JS_QUOTES_KEEP; }
			| KEEP QUOTES						{ $$ = SQ_JS_QUOTES_KEEP; }
			| OMIT QUOTES ON SCALAR STRING_P	{ $$ = SQ_JS_QUOTES_OMIT; }
			| OMIT QUOTES						{ $$ = SQ_JS_QUOTES_OMIT; }
			| /* EMPTY */						{ $$ = SQ_JS_QUOTES_UNSPEC; }
		;

json_returning_clause_opt:
			RETURNING Typename json_format_clause_opt
				{
					sq_json_output_t *n = make_node<sq_json_output_t>();

					n->type_name = $2;
					n->returning = make_node<sq_json_returning_t>();
					n->returning->format = (sq_json_format_t *) $3;
					$$ = (sq_node_t *) n;
				}
			| /* EMPTY */							{ $$ = NULL; }
		;

/*
 * We must assign the only-JSON production a precedence less than IDENT in
 * order to favor shifting over reduction when JSON is followed by VALUE_P,
 * OBJECT_P, or SCALAR.  (ARRAY doesn't need that treatment, because it's a
 * fully reserved word.)  Because json_predicate_type_constraint is always
 * followed by json_key_uniqueness_constraint_opt, we also need the only-JSON
 * production to have precedence less than WITH and WITHOUT.  UNBOUNDED isn't
 * really related to this syntax, but it's a convenient choice because it
 * already has a precedence less than IDENT for other reasons.
 */
json_predicate_type_constraint:
			JSON					%prec UNBOUNDED	{ $$ = SQ_JS_TYPE_ANY; }
			| JSON VALUE_P							{ $$ = SQ_JS_TYPE_ANY; }
			| JSON ARRAY							{ $$ = SQ_JS_TYPE_ARRAY; }
			| JSON OBJECT_P							{ $$ = SQ_JS_TYPE_OBJECT; }
			| JSON SCALAR							{ $$ = SQ_JS_TYPE_SCALAR; }
		;

/*
 * KEYS is a noise word here.  To avoid shift/reduce conflicts, assign the
 * KEYS-less productions a precedence less than IDENT (i.e., less than KEYS).
 * This prevents reducing them when the next token is KEYS.
 */
json_key_uniqueness_constraint_opt:
			WITH UNIQUE KEYS							{ $$ = true; }
			| WITH UNIQUE				%prec UNBOUNDED	{ $$ = true; }
			| WITHOUT UNIQUE KEYS						{ $$ = false; }
			| WITHOUT UNIQUE			%prec UNBOUNDED	{ $$ = false; }
			| /* EMPTY */ 				%prec UNBOUNDED	{ $$ = false; }
		;

json_name_and_value_list:
			json_name_and_value
				{ $$ = list_make1($1); }
			| json_name_and_value_list ',' json_name_and_value
				{ $$ = lappend($1, $3); }
		;

json_name_and_value:
/* Supporting this syntax seems to require major surgery
			KEY c_expr VALUE_P json_value_expr
				{ $$ = makeJsonKeyValue($2, $4); }
			|
*/
			c_expr VALUE_P json_value_expr
				{ $$ = make_json_key_value($1, $3); }
			|
			a_expr ':' json_value_expr
				{ $$ = make_json_key_value($1, $3); }
		;

/* empty means false for objects, true for arrays */
json_object_constructor_null_clause_opt:
			NULL_P ON NULL_P					{ $$ = false; }
			| ABSENT ON NULL_P					{ $$ = true; }
			| /* EMPTY */						{ $$ = false; }
		;

json_array_constructor_null_clause_opt:
			NULL_P ON NULL_P						{ $$ = false; }
			| ABSENT ON NULL_P						{ $$ = true; }
			| /* EMPTY */							{ $$ = true; }
		;

json_value_expr_list:
			json_value_expr								{ $$ = list_make1($1); }
			| json_value_expr_list ',' json_value_expr	{ $$ = lappend($1, $3);}
		;

json_aggregate_func:
			JSON_OBJECTAGG '('
				json_name_and_value
				json_object_constructor_null_clause_opt
				json_key_uniqueness_constraint_opt
				json_returning_clause_opt
			')'
				{
					sq_json_object_agg_t *n = make_node<sq_json_object_agg_t>();

					n->arg = (sq_json_key_value_t *) $3;
					n->absent_on_null = $4;
					n->unique = $5;
					n->constructor = make_node<sq_json_agg_constructor_t>();
					n->constructor->output = (sq_json_output_t *) $6;
					n->constructor->agg_order = NIL;
					n->constructor->location = @1;
					$$ = (sq_node_t *) n;
				}
			| JSON_ARRAYAGG '('
				json_value_expr
				json_array_aggregate_order_by_clause_opt
				json_array_constructor_null_clause_opt
				json_returning_clause_opt
			')'
				{
					sq_json_array_agg_t *n = make_node<sq_json_array_agg_t>();

					n->arg = (sq_json_value_expr_t *) $3;
					n->absent_on_null = $5;
					n->constructor = make_node<sq_json_agg_constructor_t>();
					n->constructor->agg_order = $4;
					n->constructor->output = (sq_json_output_t *) $6;
					n->constructor->location = @1;
					$$ = (sq_node_t *) n;
				}
		;

json_array_aggregate_order_by_clause_opt:
			ORDER BY sortby_list					{ $$ = $3; }
			| /* EMPTY */							{ $$ = NIL; }
		;

/*****************************************************************************
 *
 *	target list for SELECT
 *
 *****************************************************************************/

opt_target_list: target_list						{ $$ = $1; }
			| /* EMPTY */							{ $$ = NIL; }
		;

target_list:
			target_el								{ $$ = list_make1($1); }
			| target_list ',' target_el				{ $$ = lappend($1, $3); }
		;

target_el:	a_expr AS ColLabel
				{
					$$ = make_node<sq_res_target_t>();
					$$->name = $3;
					$$->indirection = NIL;
					$$->val = (sq_node_t *) $1;
					$$->location = @1;
				}
			| a_expr BareColLabel
				{
					$$ = make_node<sq_res_target_t>();
					$$->name = $2;
					$$->indirection = NIL;
					$$->val = (sq_node_t *) $1;
					$$->location = @1;
				}
			| a_expr
				{
					$$ = make_node<sq_res_target_t>();
					$$->name = NULL;
					$$->indirection = NIL;
					$$->val = (sq_node_t *) $1;
					$$->location = @1;
				}
			| '*'
				{
					sq_column_ref_t  *n = make_node<sq_column_ref_t>();

					n->fields = list_make1(make_node<sq_a_star_t>());
					n->location = @1;

					$$ = make_node<sq_res_target_t>();
					$$->name = NULL;
					$$->indirection = NIL;
					$$->val = (sq_node_t *) n;
					$$->location = @1;
				}
		;


/*****************************************************************************
 *
 *	Names and constants
 *
 *****************************************************************************/

qualified_name_list:
			qualified_name							{ $$ = list_make1($1); }
			| qualified_name_list ',' qualified_name { $$ = lappend($1, $3); }
		;

/*
 * The production for a qualified relation name has to exactly match the
 * production for a qualified func_name, because in a FROM clause we cannot
 * tell which we are parsing until we see what comes after it ('(' for a
 * func_name, something else for a relation). Therefore we allow 'indirection'
 * which may contain subscripts, and reject that case in the C code.
 */
qualified_name:
			ColId
				{
					$$ = make_range_var(NULL, $1, @1);
				}
			| ColId indirection
				{
					$$ = make_range_var_from_qualified_name($1, $2, @1, yyscanner);
				}
		;

name_list:	name
					{ $$ = list_make1(make_string($1)); }
			| name_list ',' name
					{ $$ = lappend($1, make_string($3)); }
		;


name:		ColId									{ $$ = $1; };

attr_name:	ColLabel								{ $$ = $1; };

file_name:	Sconst									{ $$ = $1; };

/*
 * The production for a qualified func_name has to exactly match the
 * production for a qualified columnref, because we cannot tell which we
 * are parsing until we see what comes after it ('(' or Sconst for a func_name,
 * anything else for a columnref).  Therefore we allow 'indirection' which
 * may contain subscripts, and reject that case in the C code.  (If we
 * ever implement SQL99-like methods, such syntax may actually become legal!)
 */
func_name:	type_function_name
					{ $$ = list_make1(make_string($1)); }
			| ColId indirection
					{
						$$ = check_func_name(lcons(make_string($1), $2),
											 yyscanner);
					}
		;


/*
 * Constants
 */
AexprConst: Iconst
				{
					$$ = make_int_const($1, @1);
				}
			| FCONST
				{
					$$ = make_float_const($1, @1);
				}
			| Sconst
				{
					$$ = make_string_const($1, @1);
				}
			| BCONST
				{
					$$ = make_bit_string_const($1, @1);
				}
			| XCONST
				{
					/* This is a bit constant per SQL99:
					 * Without Feature F511, "BIT data type",
					 * a <general literal> shall not be a
					 * <bit string literal> or a <hex string literal>.
					 */
					$$ = make_bit_string_const($1, @1);
				}
			| func_name Sconst
				{
					/* generic type 'literal' syntax */
					sq_type_name_t   *t = make_type_name_from_name_list($1);

					t->location = @1;
					$$ = make_string_const_cast($2, @2, t);
				}
			| func_name '(' func_arg_list opt_sort_clause ')' Sconst
				{
					/* generic syntax with a type modifier */
					sq_type_name_t   *t = make_type_name_from_name_list($1);
					list_cell_t   lc;

					/*
					 * We must use func_arg_list and opt_sort_clause in the
					 * production to avoid reduce/reduce conflicts, but we
					 * don't actually wish to allow NamedArgExpr in this
					 * context, nor ORDER BY.
					 */
					foreach(lc, $3)
					{
						sq_named_arg_expr_t *arg = (sq_named_arg_expr_t *) lfirst(lc);

						if (is_a(arg, SQ_T_NAMED_ARG_EXPR))
							raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(arg->location)}, "type modifier cannot have parameter name");
					}
					if ($4 != NIL)
							raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(@4)}, "type modifier cannot have ORDER BY");

					t->typmods = $3;
					t->location = @1;
					$$ = make_string_const_cast($6, @6, t);
				}
			| ConstTypename Sconst
				{
					$$ = make_string_const_cast($2, @2, $1);
				}
			| ConstInterval Sconst opt_interval
				{
					sq_type_name_t   *t = $1;

					t->typmods = $3;
					$$ = make_string_const_cast($2, @2, t);
				}
			| ConstInterval '(' Iconst ')' Sconst
				{
					sq_type_name_t   *t = $1;

					t->typmods = list_make2(make_int_const(INTERVAL_FULL_RANGE, -1),
											make_int_const($3, @3));
					$$ = make_string_const_cast($5, @5, t);
				}
			| TRUE_P
				{
					$$ = make_bool_a_const(true, @1);
				}
			| FALSE_P
				{
					$$ = make_bool_a_const(false, @1);
				}
			| NULL_P
				{
					$$ = make_null_a_const(@1);
				}
		;

Iconst:		ICONST									{ $$ = $1; };
Sconst:		SCONST									{ $$ = $1; };

SignedIconst: Iconst								{ $$ = $1; }
			| '+' Iconst							{ $$ = + $2; }
			| '-' Iconst							{ $$ = - $2; }
		;

/* Role specifications */
RoleId:		RoleSpec
				{
					sq_role_spec_t   *spc = (sq_role_spec_t *) $1;

					switch (spc->roletype)
					{
						case SQ_ROLESPEC_CSTRING:
							$$ = const_cast<char *>(spc->rolename);
							break;
						case SQ_ROLESPEC_PUBLIC:
							raise({.sqlstate = sqlstate_reserved_name, .pos = parser_errposition(@1)}, "role name \"{}\" is reserved",
											"public");
							break;
						case SQ_ROLESPEC_SESSION_USER:
							raise({.sqlstate = sqlstate_reserved_name, .pos = parser_errposition(@1)}, "{} cannot be used as a role name here",
											"SESSION_USER");
							break;
						case SQ_ROLESPEC_CURRENT_USER:
							raise({.sqlstate = sqlstate_reserved_name, .pos = parser_errposition(@1)}, "{} cannot be used as a role name here",
											"CURRENT_USER");
							break;
						case SQ_ROLESPEC_CURRENT_ROLE:
							raise({.sqlstate = sqlstate_reserved_name, .pos = parser_errposition(@1)}, "{} cannot be used as a role name here",
											"CURRENT_ROLE");
							break;
					}
				}
			;

RoleSpec:	NonReservedWord
				{
					/*
					 * "public" and "none" are not keywords, but they must
					 * be treated specially here.
					 */
					sq_role_spec_t   *n;

					if (strcmp($1, "public") == 0)
					{
						n = (sq_role_spec_t *) make_role_spec(SQ_ROLESPEC_PUBLIC, @1);
						n->roletype = SQ_ROLESPEC_PUBLIC;
					}
					else if (strcmp($1, "none") == 0)
					{
						raise({.sqlstate = sqlstate_reserved_name, .pos = parser_errposition(@1)}, "role name \"{}\" is reserved",
										"none");
					}
					else
					{
						n = make_role_spec(SQ_ROLESPEC_CSTRING, @1);
						n->rolename = tree_strdup($1);
					}
					$$ = n;
				}
			| CURRENT_ROLE
				{
					$$ = make_role_spec(SQ_ROLESPEC_CURRENT_ROLE, @1);
				}
			| CURRENT_USER
				{
					$$ = make_role_spec(SQ_ROLESPEC_CURRENT_USER, @1);
				}
			| SESSION_USER
				{
					$$ = make_role_spec(SQ_ROLESPEC_SESSION_USER, @1);
				}
		;

role_list:	RoleSpec
				{ $$ = list_make1($1); }
			| role_list ',' RoleSpec
				{ $$ = lappend($1, $3); }
		;


/*****************************************************************************
 *
 * PL/pgSQL extensions
 *
 * You'd think a PL/pgSQL "expression" should be just an a_expr, but
 * historically it can include just about anything that can follow SELECT.
 * Therefore the returned struct is a SelectStmt.
 *****************************************************************************/

PLpgSQL_Expr: opt_distinct_clause opt_target_list
			from_clause where_clause
			group_clause having_clause window_clause
			opt_sort_clause opt_select_limit opt_for_locking_clause
				{
					sq_select_stmt_t *n = make_node<sq_select_stmt_t>();

					n->distinct_clause = $1;
					n->target_list = $2;
					n->from_clause = $3;
					n->where_clause = $4;
					n->group_clause = ($5)->list;
					n->group_distinct = ($5)->distinct;
					n->having_clause = $6;
					n->window_clause = $7;
					n->sort_clause = $8;
					if ($9)
					{
						n->limit_offset = $9->limit_offset;
						n->limit_count = $9->limit_count;
						if (n->sort_clause == NIL &&
							$9->limit_option == SQ_LIMIT_OPTION_WITH_TIES)
							raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition($9->option_loc)}, "WITH TIES cannot be specified without ORDER BY clause");
						n->limit_option = $9->limit_option;
					}
					n->locking_clause = $10;
					$$ = (sq_node_t *) n;
				}
		;

/*
 * PL/pgSQL Assignment statement: name opt_indirection := PLpgSQL_Expr
 */

PLAssignStmt: plassign_target opt_indirection plassign_equals PLpgSQL_Expr
				{
					sq_pl_assign_stmt_t *n = make_node<sq_pl_assign_stmt_t>();

					n->name = $1;
					n->indirection = check_indirection($2, yyscanner);
					/* nnames will be filled by calling production */
					n->val = (sq_select_stmt_t *) $4;
					n->location = @1;
					$$ = (sq_node_t *) n;
				}
		;

plassign_target: ColId							{ $$ = $1; }
			| PARAM								{ $$ = tree_format("${}", $1); }
		;

plassign_equals: COLON_EQUALS
			| '='
		;


/*
 * Name classification hierarchy.
 *
 * IDENT is the lexeme returned by the lexer for identifiers that match
 * no known keyword.  In most cases, we can accept certain keywords as
 * names, not only IDENTs.	We prefer to accept as many such keywords
 * as possible to minimize the impact of "reserved words" on programmers.
 * So, we divide names into several possible classes.  The classification
 * is chosen in part to make keywords acceptable as names wherever possible.
 */

/* Column identifier --- names that can be column, table, etc names.
 */
ColId:		IDENT									{ $$ = $1; }
			| unreserved_keyword					{ $$ = tree_strdup($1); }
			| col_name_keyword						{ $$ = tree_strdup($1); }
		;

/* Type/function identifier --- names that can be type or function names.
 */
type_function_name:	IDENT							{ $$ = $1; }
			| unreserved_keyword					{ $$ = tree_strdup($1); }
			| type_func_name_keyword				{ $$ = tree_strdup($1); }
		;

/* Any not-fully-reserved word --- these names can be, eg, role names.
 */
NonReservedWord:	IDENT							{ $$ = $1; }
			| unreserved_keyword					{ $$ = tree_strdup($1); }
			| col_name_keyword						{ $$ = tree_strdup($1); }
			| type_func_name_keyword				{ $$ = tree_strdup($1); }
		;

/* Column label --- allowed labels in "AS" clauses.
 * This presently includes *all* Postgres keywords.
 */
ColLabel:	IDENT									{ $$ = $1; }
			| unreserved_keyword					{ $$ = tree_strdup($1); }
			| col_name_keyword						{ $$ = tree_strdup($1); }
			| type_func_name_keyword				{ $$ = tree_strdup($1); }
			| reserved_keyword						{ $$ = tree_strdup($1); }
		;

/* Bare column label --- names that can be column labels without writing "AS".
 * This classification is orthogonal to the other keyword categories.
 */
BareColLabel:	IDENT								{ $$ = $1; }
			| bare_label_keyword					{ $$ = tree_strdup($1); }
		;


/*
 * Keyword category lists.  Generally, every keyword present in
 * the Postgres grammar should appear in exactly one of these lists.
 *
 * Put a new keyword into the first list that it can go into without causing
 * shift or reduce conflicts.  The earlier lists define "less reserved"
 * categories of keywords.
 *
 * Make sure that each keyword's category in kwlist.h matches where
 * it is listed here.  (Someday we may be able to generate these lists and
 * kwlist.h's table from one source of truth.)
 */

/* "Unreserved" keywords --- available for use as any kind of name.
 */
unreserved_keyword:
			  ABORT_P
			| ABSENT
			| ABSOLUTE_P
			| ACCESS
			| ACTION
			| ADD_P
			| ADMIN
			| AFTER
			| AGGREGATE
			| ALSO
			| ALTER
			| ALWAYS
			| ASENSITIVE
			| ASSERTION
			| ASSIGNMENT
			| AT
			| ATOMIC
			| ATTACH
			| ATTRIBUTE
			| BACKWARD
			| BEFORE
			| BEGIN_P
			| BREADTH
			| BY
			| CACHE
			| CALL
			| CALLED
			| CASCADE
			| CASCADED
			| CATALOG_P
			| CHAIN
			| CHARACTERISTICS
			| CHECKPOINT
			| CLASS
			| CLOSE
			| CLUSTER
			| COLUMNS
			| COMMENT
			| COMMENTS
			| COMMIT
			| COMMITTED
			| COMPRESSION
			| CONDITIONAL
			| CONFIGURATION
			| CONFLICT
			| CONNECTION
			| CONSTRAINTS
			| CONTENT_P
			| CONTINUE_P
			| CONVERSION_P
			| COPY
			| COST
			| CSV
			| CUBE
			| CURRENT_P
			| CURSOR
			| CYCLE
			| DATA_P
			| DATABASE
			| DAY_P
			| DEALLOCATE
			| DECLARE
			| DEFAULTS
			| DEFERRED
			| DEFINER
			| DELETE_P
			| DELIMITER
			| DELIMITERS
			| DEPENDS
			| DEPTH
			| DETACH
			| DICTIONARY
			| DISABLE_P
			| DISCARD
			| DOCUMENT_P
			| DOMAIN_P
			| DOUBLE_P
			| DROP
			| EACH
			| EMPTY_P
			| ENABLE_P
			| ENCODING
			| ENCRYPTED
			| ENFORCED
			| ENUM_P
			| ERROR_P
			| ESCAPE
			| EVENT
			| EXCLUDE
			| EXCLUDING
			| EXCLUSIVE
			| EXECUTE
			| EXPLAIN
			| EXPRESSION
			| EXTENSION
			| EXTERNAL
			| FAMILY
			| FILTER
			| FINALIZE
			| FIRST_P
			| FOLLOWING
			| FORCE
			| FORMAT
			| FORWARD
			| FUNCTION
			| FUNCTIONS
			| GENERATED
			| GLOBAL
			| GRANTED
			| GROUPS
			| HANDLER
			| HEADER_P
			| HOLD
			| HOUR_P
			| IDENTITY_P
			| IF_P
			| IMMEDIATE
			| IMMUTABLE
			| IMPLICIT_P
			| IMPORT_P
			| INCLUDE
			| INCLUDING
			| INCREMENT
			| INDENT
			| INDEX
			| INDEXES
			| INHERIT
			| INHERITS
			| INLINE_P
			| INPUT_P
			| INSENSITIVE
			| INSERT
			| INSTEAD
			| INVOKER
			| ISOLATION
			| KEEP
			| KEY
			| KEYS
			| LABEL
			| LANGUAGE
			| LARGE_P
			| LAST_P
			| LEAKPROOF
			| LEVEL
			| LISTEN
			| LOAD
			| LOCAL
			| LOCATION
			| LOCK_P
			| LOCKED
			| LOGGED
			| MAPPING
			| MATCH
			| MATCHED
			| MATERIALIZED
			| MAXVALUE
			| MERGE
			| METHOD
			| MINUTE_P
			| MINVALUE
			| MODE
			| MONTH_P
			| MOVE
			| NAME_P
			| NAMES
			| NESTED
			| NEW
			| NEXT
			| NFC
			| NFD
			| NFKC
			| NFKD
			| NO
			| NORMALIZED
			| NOTHING
			| NOTIFY
			| NOWAIT
			| NULLS_P
			| OBJECT_P
			| OBJECTS_P
			| OF
			| OFF
			| OIDS
			| OLD
			| OMIT
			| OPERATOR
			| OPTION
			| OPTIONS
			| ORDINALITY
			| OTHERS
			| OVER
			| OVERRIDING
			| OWNED
			| OWNER
			| PARALLEL
			| PARAMETER
			| PARSER
			| PARTIAL
			| PARTITION
			| PASSING
			| PASSWORD
			| PATH
			| PERIOD
			| PLAN
			| PLANS
			| POLICY
			| PRECEDING
			| PREPARE
			| PREPARED
			| PRESERVE
			| PRIOR
			| PRIVILEGES
			| PROCEDURAL
			| PROCEDURE
			| PROCEDURES
			| PROGRAM
			| PUBLICATION
			| QUOTE
			| QUOTES
			| RANGE
			| READ
			| REASSIGN
			| RECURSIVE
			| REF_P
			| REFERENCING
			| REFRESH
			| REINDEX
			| RELATIVE_P
			| RELEASE
			| RENAME
			| REPEATABLE
			| REPLACE
			| REPLICA
			| RESET
			| RESTART
			| RESTRICT
			| RETURN
			| RETURNS
			| REVOKE
			| ROLE
			| ROLLBACK
			| ROLLUP
			| ROUTINE
			| ROUTINES
			| ROWS
			| RULE
			| SAVEPOINT
			| SCALAR
			| SCHEMA
			| SCHEMAS
			| SCROLL
			| SEARCH
			| SECOND_P
			| SECURITY
			| SEQUENCE
			| SEQUENCES
			| SERIALIZABLE
			| SERVER
			| SESSION
			| SET
			| SETS
			| SHARE
			| SHOW
			| SIMPLE
			| SKIP
			| SNAPSHOT
			| SOURCE
			| SQL_P
			| STABLE
			| STANDALONE_P
			| START
			| STATEMENT
			| STATISTICS
			| STDIN
			| STDOUT
			| STORAGE
			| STORED
			| STRICT_P
			| STRING_P
			| STRIP_P
			| SUBSCRIPTION
			| SUPPORT
			| SYSID
			| SYSTEM_P
			| TABLES
			| TABLESPACE
			| TARGET
			| TEMP
			| TEMPLATE
			| TEMPORARY
			| TEXT_P
			| TIES
			| TRANSACTION
			| TRANSFORM
			| TRIGGER
			| TRUNCATE
			| TRUSTED
			| TYPE_P
			| TYPES_P
			| UESCAPE
			| UNBOUNDED
			| UNCOMMITTED
			| UNCONDITIONAL
			| UNENCRYPTED
			| UNKNOWN
			| UNLISTEN
			| UNLOGGED
			| UNTIL
			| UPDATE
			| VACUUM
			| VALID
			| VALIDATE
			| VALIDATOR
			| VALUE_P
			| VARYING
			| VERSION_P
			| VIEW
			| VIEWS
			| VIRTUAL
			| VOLATILE
			| WHITESPACE_P
			| WITHIN
			| WITHOUT
			| WORK
			| WRAPPER
			| WRITE
			| XML_P
			| YEAR_P
			| YES_P
			| ZONE
		;

/* Column identifier --- keywords that can be column, table, etc names.
 *
 * Many of these keywords will in fact be recognized as type or function
 * names too; but they have special productions for the purpose, and so
 * can't be treated as "generic" type or function names.
 *
 * The type names appearing here are not usable as function names
 * because they can be followed by '(' in typename productions, which
 * looks too much like a function call for an LR(1) parser.
 */
col_name_keyword:
			  BETWEEN
			| BIGINT
			| BIT
			| BOOLEAN_P
			| CHAR_P
			| CHARACTER
			| COALESCE
			| DEC
			| DECIMAL_P
			| EXISTS
			| EXTRACT
			| FLOAT_P
			| GREATEST
			| GROUPING
			| INOUT
			| INT_P
			| INTEGER
			| INTERVAL
			| JSON
			| JSON_ARRAY
			| JSON_ARRAYAGG
			| JSON_EXISTS
			| JSON_OBJECT
			| JSON_OBJECTAGG
			| JSON_QUERY
			| JSON_SCALAR
			| JSON_SERIALIZE
			| JSON_TABLE
			| JSON_VALUE
			| LEAST
			| MERGE_ACTION
			| NATIONAL
			| NCHAR
			| NONE
			| NORMALIZE
			| NULLIF
			| NUMERIC
			| OUT_P
			| OVERLAY
			| POSITION
			| PRECISION
			| REAL
			| ROW
			| SETOF
			| SMALLINT
			| SUBSTRING
			| TIME
			| TIMESTAMP
			| TREAT
			| TRIM
			| VALUES
			| VARCHAR
			| XMLATTRIBUTES
			| XMLCONCAT
			| XMLELEMENT
			| XMLEXISTS
			| XMLFOREST
			| XMLNAMESPACES
			| XMLPARSE
			| XMLPI
			| XMLROOT
			| XMLSERIALIZE
			| XMLTABLE
		;

/* Type/function identifier --- keywords that can be type or function names.
 *
 * Most of these are keywords that are used as operators in expressions;
 * in general such keywords can't be column names because they would be
 * ambiguous with variables, but they are unambiguous as function identifiers.
 *
 * Do not include POSITION, SUBSTRING, etc here since they have explicit
 * productions in a_expr to support the goofy SQL9x argument syntax.
 * - thomas 2000-11-28
 */
type_func_name_keyword:
			  AUTHORIZATION
			| BINARY
			| COLLATION
			| CONCURRENTLY
			| CROSS
			| CURRENT_SCHEMA
			| FREEZE
			| FULL
			| ILIKE
			| INNER_P
			| IS
			| ISNULL
			| JOIN
			| LEFT
			| LIKE
			| NATURAL
			| NOTNULL
			| OUTER_P
			| OVERLAPS
			| RIGHT
			| SIMILAR
			| TABLESAMPLE
			| VERBOSE
		;

/* Reserved keyword --- these keywords are usable only as a ColLabel.
 *
 * Keywords appear here if they could not be distinguished from variable,
 * type, or function names in some contexts.  Don't put things here unless
 * forced to.
 */
reserved_keyword:
			  ALL
			| ANALYSE
			| ANALYZE
			| AND
			| ANY
			| ARRAY
			| AS
			| ASC
			| ASYMMETRIC
			| BOTH
			| CASE
			| CAST
			| CHECK
			| COLLATE
			| COLUMN
			| CONSTRAINT
			| CREATE
			| CURRENT_CATALOG
			| CURRENT_DATE
			| CURRENT_ROLE
			| CURRENT_TIME
			| CURRENT_TIMESTAMP
			| CURRENT_USER
			| DEFAULT
			| DEFERRABLE
			| DESC
			| DISTINCT
			| DO
			| ELSE
			| END_P
			| EXCEPT
			| FALSE_P
			| FETCH
			| FOR
			| FOREIGN
			| FROM
			| GRANT
			| GROUP_P
			| HAVING
			| IN_P
			| INITIALLY
			| INTERSECT
			| INTO
			| LATERAL_P
			| LEADING
			| LIMIT
			| LOCALTIME
			| LOCALTIMESTAMP
			| NOT
			| NULL_P
			| OFFSET
			| ON
			| ONLY
			| OR
			| ORDER
			| PLACING
			| PRIMARY
			| REFERENCES
			| RETURNING
			| SELECT
			| SESSION_USER
			| SOME
			| SYMMETRIC
			| SYSTEM_USER
			| TABLE
			| THEN
			| TO
			| TRAILING
			| TRUE_P
			| UNION
			| UNIQUE
			| USER
			| USING
			| VARIADIC
			| WHEN
			| WHERE
			| WINDOW
			| WITH
		;

/*
 * While all keywords can be used as column labels when preceded by AS,
 * not all of them can be used as a "bare" column label without AS.
 * Those that can be used as a bare label must be listed here,
 * in addition to appearing in one of the category lists above.
 *
 * Always add a new keyword to this list if possible.  Mark it BARE_LABEL
 * in kwlist.h if it is included here, or AS_LABEL if it is not.
 */
bare_label_keyword:
			  ABORT_P
			| ABSENT
			| ABSOLUTE_P
			| ACCESS
			| ACTION
			| ADD_P
			| ADMIN
			| AFTER
			| AGGREGATE
			| ALL
			| ALSO
			| ALTER
			| ALWAYS
			| ANALYSE
			| ANALYZE
			| AND
			| ANY
			| ASC
			| ASENSITIVE
			| ASSERTION
			| ASSIGNMENT
			| ASYMMETRIC
			| AT
			| ATOMIC
			| ATTACH
			| ATTRIBUTE
			| AUTHORIZATION
			| BACKWARD
			| BEFORE
			| BEGIN_P
			| BETWEEN
			| BIGINT
			| BINARY
			| BIT
			| BOOLEAN_P
			| BOTH
			| BREADTH
			| BY
			| CACHE
			| CALL
			| CALLED
			| CASCADE
			| CASCADED
			| CASE
			| CAST
			| CATALOG_P
			| CHAIN
			| CHARACTERISTICS
			| CHECK
			| CHECKPOINT
			| CLASS
			| CLOSE
			| CLUSTER
			| COALESCE
			| COLLATE
			| COLLATION
			| COLUMN
			| COLUMNS
			| COMMENT
			| COMMENTS
			| COMMIT
			| COMMITTED
			| COMPRESSION
			| CONCURRENTLY
			| CONDITIONAL
			| CONFIGURATION
			| CONFLICT
			| CONNECTION
			| CONSTRAINT
			| CONSTRAINTS
			| CONTENT_P
			| CONTINUE_P
			| CONVERSION_P
			| COPY
			| COST
			| CROSS
			| CSV
			| CUBE
			| CURRENT_P
			| CURRENT_CATALOG
			| CURRENT_DATE
			| CURRENT_ROLE
			| CURRENT_SCHEMA
			| CURRENT_TIME
			| CURRENT_TIMESTAMP
			| CURRENT_USER
			| CURSOR
			| CYCLE
			| DATA_P
			| DATABASE
			| DEALLOCATE
			| DEC
			| DECIMAL_P
			| DECLARE
			| DEFAULT
			| DEFAULTS
			| DEFERRABLE
			| DEFERRED
			| DEFINER
			| DELETE_P
			| DELIMITER
			| DELIMITERS
			| DEPENDS
			| DEPTH
			| DESC
			| DETACH
			| DICTIONARY
			| DISABLE_P
			| DISCARD
			| DISTINCT
			| DO
			| DOCUMENT_P
			| DOMAIN_P
			| DOUBLE_P
			| DROP
			| EACH
			| ELSE
			| EMPTY_P
			| ENABLE_P
			| ENCODING
			| ENCRYPTED
			| END_P
			| ENFORCED
			| ENUM_P
			| ERROR_P
			| ESCAPE
			| EVENT
			| EXCLUDE
			| EXCLUDING
			| EXCLUSIVE
			| EXECUTE
			| EXISTS
			| EXPLAIN
			| EXPRESSION
			| EXTENSION
			| EXTERNAL
			| EXTRACT
			| FALSE_P
			| FAMILY
			| FINALIZE
			| FIRST_P
			| FLOAT_P
			| FOLLOWING
			| FORCE
			| FOREIGN
			| FORMAT
			| FORWARD
			| FREEZE
			| FULL
			| FUNCTION
			| FUNCTIONS
			| GENERATED
			| GLOBAL
			| GRANTED
			| GREATEST
			| GROUPING
			| GROUPS
			| HANDLER
			| HEADER_P
			| HOLD
			| IDENTITY_P
			| IF_P
			| ILIKE
			| IMMEDIATE
			| IMMUTABLE
			| IMPLICIT_P
			| IMPORT_P
			| IN_P
			| INCLUDE
			| INCLUDING
			| INCREMENT
			| INDENT
			| INDEX
			| INDEXES
			| INHERIT
			| INHERITS
			| INITIALLY
			| INLINE_P
			| INNER_P
			| INOUT
			| INPUT_P
			| INSENSITIVE
			| INSERT
			| INSTEAD
			| INT_P
			| INTEGER
			| INTERVAL
			| INVOKER
			| IS
			| ISOLATION
			| JOIN
			| JSON
			| JSON_ARRAY
			| JSON_ARRAYAGG
			| JSON_EXISTS
			| JSON_OBJECT
			| JSON_OBJECTAGG
			| JSON_QUERY
			| JSON_SCALAR
			| JSON_SERIALIZE
			| JSON_TABLE
			| JSON_VALUE
			| KEEP
			| KEY
			| KEYS
			| LABEL
			| LANGUAGE
			| LARGE_P
			| LAST_P
			| LATERAL_P
			| LEADING
			| LEAKPROOF
			| LEAST
			| LEFT
			| LEVEL
			| LIKE
			| LISTEN
			| LOAD
			| LOCAL
			| LOCALTIME
			| LOCALTIMESTAMP
			| LOCATION
			| LOCK_P
			| LOCKED
			| LOGGED
			| MAPPING
			| MATCH
			| MATCHED
			| MATERIALIZED
			| MAXVALUE
			| MERGE
			| MERGE_ACTION
			| METHOD
			| MINVALUE
			| MODE
			| MOVE
			| NAME_P
			| NAMES
			| NATIONAL
			| NATURAL
			| NCHAR
			| NESTED
			| NEW
			| NEXT
			| NFC
			| NFD
			| NFKC
			| NFKD
			| NO
			| NONE
			| NORMALIZE
			| NORMALIZED
			| NOT
			| NOTHING
			| NOTIFY
			| NOWAIT
			| NULL_P
			| NULLIF
			| NULLS_P
			| NUMERIC
			| OBJECT_P
			| OBJECTS_P
			| OF
			| OFF
			| OIDS
			| OLD
			| OMIT
			| ONLY
			| OPERATOR
			| OPTION
			| OPTIONS
			| OR
			| ORDINALITY
			| OTHERS
			| OUT_P
			| OUTER_P
			| OVERLAY
			| OVERRIDING
			| OWNED
			| OWNER
			| PARALLEL
			| PARAMETER
			| PARSER
			| PARTIAL
			| PARTITION
			| PASSING
			| PASSWORD
			| PATH
			| PERIOD
			| PLACING
			| PLAN
			| PLANS
			| POLICY
			| POSITION
			| PRECEDING
			| PREPARE
			| PREPARED
			| PRESERVE
			| PRIMARY
			| PRIOR
			| PRIVILEGES
			| PROCEDURAL
			| PROCEDURE
			| PROCEDURES
			| PROGRAM
			| PUBLICATION
			| QUOTE
			| QUOTES
			| RANGE
			| READ
			| REAL
			| REASSIGN
			| RECURSIVE
			| REF_P
			| REFERENCES
			| REFERENCING
			| REFRESH
			| REINDEX
			| RELATIVE_P
			| RELEASE
			| RENAME
			| REPEATABLE
			| REPLACE
			| REPLICA
			| RESET
			| RESTART
			| RESTRICT
			| RETURN
			| RETURNS
			| REVOKE
			| RIGHT
			| ROLE
			| ROLLBACK
			| ROLLUP
			| ROUTINE
			| ROUTINES
			| ROW
			| ROWS
			| RULE
			| SAVEPOINT
			| SCALAR
			| SCHEMA
			| SCHEMAS
			| SCROLL
			| SEARCH
			| SECURITY
			| SELECT
			| SEQUENCE
			| SEQUENCES
			| SERIALIZABLE
			| SERVER
			| SESSION
			| SESSION_USER
			| SET
			| SETOF
			| SETS
			| SHARE
			| SHOW
			| SIMILAR
			| SIMPLE
			| SKIP
			| SMALLINT
			| SNAPSHOT
			| SOME
			| SOURCE
			| SQL_P
			| STABLE
			| STANDALONE_P
			| START
			| STATEMENT
			| STATISTICS
			| STDIN
			| STDOUT
			| STORAGE
			| STORED
			| STRICT_P
			| STRING_P
			| STRIP_P
			| SUBSCRIPTION
			| SUBSTRING
			| SUPPORT
			| SYMMETRIC
			| SYSID
			| SYSTEM_P
			| SYSTEM_USER
			| TABLE
			| TABLES
			| TABLESAMPLE
			| TABLESPACE
			| TARGET
			| TEMP
			| TEMPLATE
			| TEMPORARY
			| TEXT_P
			| THEN
			| TIES
			| TIME
			| TIMESTAMP
			| TRAILING
			| TRANSACTION
			| TRANSFORM
			| TREAT
			| TRIGGER
			| TRIM
			| TRUE_P
			| TRUNCATE
			| TRUSTED
			| TYPE_P
			| TYPES_P
			| UESCAPE
			| UNBOUNDED
			| UNCOMMITTED
			| UNCONDITIONAL
			| UNENCRYPTED
			| UNIQUE
			| UNKNOWN
			| UNLISTEN
			| UNLOGGED
			| UNTIL
			| UPDATE
			| USER
			| USING
			| VACUUM
			| VALID
			| VALIDATE
			| VALIDATOR
			| VALUE_P
			| VALUES
			| VARCHAR
			| VARIADIC
			| VERBOSE
			| VERSION_P
			| VIEW
			| VIEWS
			| VIRTUAL
			| VOLATILE
			| WHEN
			| WHITESPACE_P
			| WORK
			| WRAPPER
			| WRITE
			| XML_P
			| XMLATTRIBUTES
			| XMLCONCAT
			| XMLELEMENT
			| XMLEXISTS
			| XMLFOREST
			| XMLNAMESPACES
			| XMLPARSE
			| XMLPI
			| XMLROOT
			| XMLSERIALIZE
			| XMLTABLE
			| YES_P
			| ZONE
		;

%%

/*
 * The signature of this function is required by bison.  However, we
 * ignore the passed yylloc and instead use the last token position
 * available from the scanner.
 */
static void
sq_base_yyerror(YYLTYPE *yylloc, core_yyscan_t yyscanner, const char *msg)
{
	parser_yyerror(msg);
}

static sq_raw_stmt_t *
make_raw_stmt(sq_node_t *stmt, int stmt_location)
{
	sq_raw_stmt_t    *rs = make_node<sq_raw_stmt_t>();

	rs->stmt = stmt;
	rs->stmt_location = stmt_location;
	rs->stmt_len = 0;			/* might get changed later */
	return rs;
}

/* Adjust a RawStmt to reflect that it doesn't run to the end of the string */
static void
update_raw_stmt_end(sq_raw_stmt_t *rs, int end_location)
{
	/*
	 * If we already set the length, don't change it.  This is for situations
	 * like "select foo ;; select bar" where the same statement will be last
	 * in the string for more than one semicolon.
	 */
	if (rs->stmt_len > 0)
		return;

	/* OK, update length of RawStmt */
	rs->stmt_len = end_location - rs->stmt_location;
}

static sq_node_t *
make_column_ref(const char *colname, list_ref_t indirection,
			  int location, core_yyscan_t yyscanner)
{
	/*
	 * Generate a ColumnRef node, with an A_Indirection node added if there is
	 * any subscripting in the specified indirection list.  However, any field
	 * selection at the start of the indirection list must be transposed into
	 * the "fields" part of the ColumnRef node.
	 */
	sq_column_ref_t  *c = make_node<sq_column_ref_t>();
	int			nfields = 0;
	list_cell_t   l;

	c->location = location;
	foreach(l, indirection)
	{
		if (is_a(lfirst(l), SQ_T_A_INDICES))
		{
			sq_a_indirection_t *i = make_node<sq_a_indirection_t>();

			if (nfields == 0)
			{
				/* easy case - all indirection goes to A_Indirection */
				c->fields = list_make1(make_string(colname));
				i->indirection = check_indirection(indirection, yyscanner);
			}
			else
			{
				/* got to split the list in two */
				i->indirection = check_indirection(list_copy_tail(indirection,
																  nfields),
												   yyscanner);
				indirection = list_truncate(indirection, nfields);
				c->fields = lcons(make_string(colname), indirection);
			}
			i->arg = (sq_node_t *) c;
			return (sq_node_t *) i;
		}
		else if (is_a(lfirst(l), SQ_T_A_STAR))
		{
			/* We only allow '*' at the end of a ColumnRef */
			if (lnext(indirection, l) != NULL)
				parser_yyerror("improper use of \"*\"");
		}
		nfields++;
	}
	/* No subscripting, so all indirection gets added to field list */
	c->fields = lcons(make_string(colname), indirection);
	return (sq_node_t *) c;
}

static sq_node_t *
make_type_cast(sq_node_t *arg, sq_type_name_t *typename_, int location)
{
	sq_type_cast_t   *n = make_node<sq_type_cast_t>();

	n->arg = arg;
	n->type_name = typename_;
	n->location = location;
	return (sq_node_t *) n;
}

static sq_node_t *
make_string_const_cast(char *str, int location, sq_type_name_t *typename_)
{
	sq_node_t	   *s = make_string_const(str, location);

	return make_type_cast(s, typename_, -1);
}

static sq_node_t *
make_int_const(int val, int location)
{
	sq_a_const_t    *n = make_node<sq_a_const_t>();

	n->val.ival.tag = SQ_T_INTEGER;
	n->val.ival.ival = val;
	n->location = location;

	return (sq_node_t *) n;
}

static sq_node_t *
make_float_const(const char *str, int location)
{
	sq_a_const_t    *n = make_node<sq_a_const_t>();

	n->val.fval.tag = SQ_T_FLOAT;
	n->val.fval.fval = str;
	n->location = location;

	return (sq_node_t *) n;
}

static sq_node_t *
make_bool_a_const(bool state, int location)
{
	sq_a_const_t    *n = make_node<sq_a_const_t>();

	n->val.boolval.tag = SQ_T_BOOLEAN;
	n->val.boolval.boolval = state;
	n->location = location;

	return (sq_node_t *) n;
}

static sq_node_t *
make_bit_string_const(char *str, int location)
{
	sq_a_const_t    *n = make_node<sq_a_const_t>();

	n->val.bsval.tag = SQ_T_BIT_STRING;
	n->val.bsval.bsval = str;
	n->location = location;

	return (sq_node_t *) n;
}

static sq_node_t *
make_null_a_const(int location)
{
	sq_a_const_t    *n = make_node<sq_a_const_t>();

	n->isnull = true;
	n->location = location;

	return (sq_node_t *) n;
}

static sq_node_t *
make_a_const(sq_node_t *v, int location)
{
	sq_node_t	   *n;

	switch (v->tag)
	{
		case SQ_T_FLOAT:
			n = make_float_const(cast_node(sq_float_t, v)->fval, location);
			break;

		case SQ_T_INTEGER:
			n = make_int_const(cast_node(sq_integer_t, v)->ival, location);
			break;

		default:
			/* currently not used */
			assert(false);
			n = NULL;
	}

	return n;
}

/* makeRoleSpec
 * Create a RoleSpec with the given type
 */
static sq_role_spec_t *
make_role_spec(sq_role_spec_type_t type, int location)
{
	sq_role_spec_t   *spec = make_node<sq_role_spec_t>();

	spec->roletype = type;
	spec->location = location;

	return spec;
}

/* check_qualified_name --- check the result of qualified_name production
 *
 * It's easiest to let the grammar production for qualified_name allow
 * subscripts and '*', which we then must reject here.
 */
static void
check_qualified_name(list_ref_t names, core_yyscan_t yyscanner)
{
	list_cell_t   i;

	foreach(i, names)
	{
		if (!is_a(lfirst(i), SQ_T_STRING))
			parser_yyerror("syntax error");
	}
}

/* check_func_name --- check the result of func_name production
 *
 * It's easiest to let the grammar production for func_name allow subscripts
 * and '*', which we then must reject here.
 */
static list_ref_t 
check_func_name(list_ref_t names, core_yyscan_t yyscanner)
{
	list_cell_t   i;

	foreach(i, names)
	{
		if (!is_a(lfirst(i), SQ_T_STRING))
			parser_yyerror("syntax error");
	}
	return names;
}

/* check_indirection --- check the result of indirection production
 *
 * We only allow '*' at the end of the list, but it's hard to enforce that
 * in the grammar, so do it here.
 */
static list_ref_t 
check_indirection(list_ref_t indirection, core_yyscan_t yyscanner)
{
	list_cell_t   l;

	foreach(l, indirection)
	{
		if (is_a(lfirst(l), SQ_T_A_STAR))
		{
			if (lnext(indirection, l) != NULL)
				parser_yyerror("improper use of \"*\"");
		}
	}
	return indirection;
}

/* extractArgTypes()
 * Given a list of FunctionParameter nodes, extract a list of just the
 * argument types (TypeNames) for input parameters only.  This is what
 * is needed to look up an existing function, which is what is wanted by
 * the productions that use this call.
 */
static list_ref_t 
extract_arg_types(list_ref_t parameters)
{
	list_ref_t	   result = NIL;
	list_cell_t   i;

	foreach(i, parameters)
	{
		sq_function_parameter_t *p = (sq_function_parameter_t *) lfirst(i);

		if (p->mode != SQ_FUNC_PARAM_OUT && p->mode != SQ_FUNC_PARAM_TABLE)
			result = lappend(result, p->arg_type);
	}
	return result;
}

/* extractAggrArgTypes()
 * As above, but work from the output of the aggr_args production.
 */
static list_ref_t 
extract_aggr_arg_types(list_ref_t aggrargs)
{
	assert(list_length(aggrargs) == 2);
	return extract_arg_types((list_ref_t ) linitial(aggrargs));
}

/* makeOrderedSetArgs()
 * Build the result of the aggr_args production (which see the comments for).
 * This handles only the case where both given lists are nonempty, so that
 * we have to deal with multiple VARIADIC arguments.
 */
static list_ref_t 
make_ordered_set_args(list_ref_t directargs, list_ref_t orderedargs,
				   core_yyscan_t yyscanner)
{
	sq_function_parameter_t *lastd = (sq_function_parameter_t *) llast(directargs);
	sq_integer_t    *ndirectargs;

	/* No restriction unless last direct arg is VARIADIC */
	if (lastd->mode == SQ_FUNC_PARAM_VARIADIC)
	{
		sq_function_parameter_t *firsto = (sq_function_parameter_t *) linitial(orderedargs);

		/*
		 * We ignore the names, though the aggr_arg production allows them; it
		 * doesn't allow default values, so those need not be checked.
		 */
		if (list_length(orderedargs) != 1 ||
			firsto->mode != SQ_FUNC_PARAM_VARIADIC ||
			!equal_nodes(lastd->arg_type, firsto->arg_type))
			raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(firsto->location)}, "an ordered-set aggregate with a VARIADIC direct argument must have one VARIADIC aggregated argument of the same data type");

		/* OK, drop the duplicate VARIADIC argument from the internal form */
		orderedargs = NIL;
	}

	/* don't merge into the next line, as list_concat changes directargs */
	ndirectargs = make_integer(list_length(directargs));

	return list_make2(list_concat(directargs, orderedargs),
					  ndirectargs);
}

/* insertSelectOptions()
 * Insert ORDER BY, etc into an already-constructed SelectStmt.
 *
 * This routine is just to avoid duplicating code in SelectStmt productions.
 */
static void
insert_select_options(sq_select_stmt_t *stmt,
					list_ref_t sort_clause, list_ref_t locking_clause,
					select_limit_t *limit_clause,
					sq_with_clause_t *with_clause,
					core_yyscan_t yyscanner)
{
	assert(is_a(stmt, SQ_T_SELECT_STMT));

	/*
	 * Tests here are to reject constructs like
	 *	(SELECT foo ORDER BY bar) ORDER BY baz
	 */
	if (sort_clause)
	{
		if (stmt->sort_clause != NIL)
			raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(expr_location((sq_node_t *) sort_clause))}, "multiple ORDER BY clauses not allowed");
		stmt->sort_clause = sort_clause;
	}
	/* We can handle multiple locking clauses, though */
	stmt->locking_clause = list_concat(stmt->locking_clause, locking_clause);
	if (limit_clause && limit_clause->limit_offset)
	{
		if (stmt->limit_offset)
			raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(limit_clause->offset_loc)}, "multiple OFFSET clauses not allowed");
		stmt->limit_offset = limit_clause->limit_offset;
	}
	if (limit_clause && limit_clause->limit_count)
	{
		if (stmt->limit_count)
			raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(limit_clause->count_loc)}, "multiple LIMIT clauses not allowed");
		stmt->limit_count = limit_clause->limit_count;
	}
	if (limit_clause)
	{
		/* If there was a conflict, we must have detected it above */
		assert(!stmt->limit_option);
		if (stmt->sort_clause == NIL && limit_clause->limit_option == SQ_LIMIT_OPTION_WITH_TIES)
			raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(limit_clause->option_loc)}, "WITH TIES cannot be specified without ORDER BY clause");
		if (limit_clause->limit_option == SQ_LIMIT_OPTION_WITH_TIES && stmt->locking_clause != NIL)
		{
			list_cell_t lc;

			foreach(lc, stmt->locking_clause)
			{
				sq_locking_clause_t *lock = lfirst_node(sq_locking_clause_t, lc);

				if (lock->wait_policy == SQ_LockWaitSkip)
					raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(limit_clause->option_loc)}, "{} and {} options cannot be used together",
									"SKIP LOCKED", "WITH TIES");
			}
		}
		stmt->limit_option = limit_clause->limit_option;
	}
	if (with_clause)
	{
		if (stmt->with_clause)
			raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(expr_location((sq_node_t *) with_clause))}, "multiple WITH clauses not allowed");
		stmt->with_clause = with_clause;
	}
}

static sq_node_t *
make_set_op(sq_set_operation_t op, bool all, sq_node_t *larg, sq_node_t *rarg)
{
	sq_select_stmt_t *n = make_node<sq_select_stmt_t>();

	n->op = op;
	n->all = all;
	n->larg = (sq_select_stmt_t *) larg;
	n->rarg = (sq_select_stmt_t *) rarg;
	return (sq_node_t *) n;
}

/* SystemFuncName()
 * Build a properly-qualified reference to a built-in function.
 */
list_ref_t 
system_func_name(const char *name)
{
	return list_make2(make_string("pg_catalog"), make_string(name));
}

/* SystemTypeName()
 * Build a properly-qualified reference to a built-in type.
 *
 * typmod is defaulted, but may be changed afterwards by caller.
 * Likewise for the location.
 */
static sq_type_name_t *
system_type_name(const char *name)
{
	return make_type_name_from_name_list(list_make2(make_string("pg_catalog"),
											   make_string(name)));
}

/* doNegate()
 * Handle negation of a numeric constant.
 *
 * Formerly, we did this here because the optimizer couldn't cope with
 * indexquals that looked like "var = -4" --- it wants "var = const"
 * and a unary minus operator applied to a constant didn't qualify.
 * As of Postgres 7.0, that problem doesn't exist anymore because there
 * is a constant-subexpression simplifier in the optimizer.  However,
 * there's still a good reason for doing this here, which is that we can
 * postpone committing to a particular internal representation for simple
 * negative constants.	It's better to leave "-123.456" in string form
 * until we know what the desired type is.
 */
static sq_node_t *
do_negate(sq_node_t *n, int location)
{
	if (is_a(n, SQ_T_A_CONST))
	{
		sq_a_const_t    *con = (sq_a_const_t *) n;

		/* report the constant's location as that of the '-' sign */
		con->location = location;

		if (!con->isnull && is_a(&con->val.node, SQ_T_INTEGER))
		{
			con->val.ival.ival = -con->val.ival.ival;
			return n;
		}
		if (!con->isnull && is_a(&con->val.node, SQ_T_FLOAT))
		{
			do_negate_float(&con->val.fval);
			return n;
		}
	}

	return (sq_node_t *) make_simple_a_expr(SQ_AEXPR_OP, "-", NULL, n, location);
}

static void
do_negate_float(sq_float_t *v)
{
	const char *oldval = v->fval;

	if (*oldval == '+')
		oldval++;
	if (*oldval == '-')
		v->fval = oldval + 1;	/* just strip the '-' */
	else
		v->fval = tree_format("-{}", oldval);
}

static sq_node_t *
make_and_expr(sq_node_t *lexpr, sq_node_t *rexpr, int location)
{
	/* Flatten "a AND b AND c ..." to a single BoolExpr on sight */
	if (is_a(lexpr, SQ_T_BOOL_EXPR))
	{
		sq_bool_expr_t   *blexpr = (sq_bool_expr_t *) lexpr;

		if (blexpr->boolop == SQ_AND_EXPR)
		{
			blexpr->args = lappend(blexpr->args, rexpr);
			return (sq_node_t *) blexpr;
		}
	}
	return (sq_node_t *) make_bool_expr(SQ_AND_EXPR, list_make2(lexpr, rexpr), location);
}

static sq_node_t *
make_or_expr(sq_node_t *lexpr, sq_node_t *rexpr, int location)
{
	/* Flatten "a OR b OR c ..." to a single BoolExpr on sight */
	if (is_a(lexpr, SQ_T_BOOL_EXPR))
	{
		sq_bool_expr_t   *blexpr = (sq_bool_expr_t *) lexpr;

		if (blexpr->boolop == SQ_OR_EXPR)
		{
			blexpr->args = lappend(blexpr->args, rexpr);
			return (sq_node_t *) blexpr;
		}
	}
	return (sq_node_t *) make_bool_expr(SQ_OR_EXPR, list_make2(lexpr, rexpr), location);
}

static sq_node_t *
make_not_expr(sq_node_t *expr, int location)
{
	return (sq_node_t *) make_bool_expr(SQ_NOT_EXPR, list_make1(expr), location);
}

static sq_node_t *
make_a_array_expr(list_ref_t elements, int location, int location_end)
{
	sq_a_array_expr_t *n = make_node<sq_a_array_expr_t>();

	n->elements = elements;
	n->location = location;
	n->list_start = location;
	n->list_end = location_end;
	return (sq_node_t *) n;
}

static sq_node_t *
make_sql_value_function(sq_sql_value_function_op_t op, int32_t typmod, int location)
{
	sq_sql_value_function_t *svf = make_node<sq_sql_value_function_t>();

	svf->op = op;
	/* svf->type will be filled during parse analysis */
	svf->typmod = typmod;
	svf->location = location;
	return (sq_node_t *) svf;
}

static sq_node_t *
make_xml_expr(sq_xml_expr_op_t op, char *name, list_ref_t named_args, list_ref_t args,
			int location)
{
	sq_xml_expr_t    *x = make_node<sq_xml_expr_t>();

	x->op = op;
	x->name = name;

	/*
	 * named_args is a list of ResTarget; it'll be split apart into separate
	 * expression and name lists in transformXmlExpr().
	 */
	x->named_args = named_args;
	x->arg_names = NIL;
	x->args = args;
	/* xmloption, if relevant, must be filled in by caller */
	/* type and typmod will be filled in during parse analysis */
	x->type = invalid_oid;		/* marks the node as not analyzed */
	x->location = location;
	return (sq_node_t *) x;
}

/*
 * Merge the input and output parameters of a table function.
 */
static list_ref_t 
merge_table_func_parameters(list_ref_t func_args, list_ref_t columns, core_yyscan_t yyscanner)
{
	list_cell_t   lc;

	/* Explicit OUT and INOUT parameters shouldn't be used in this syntax */
	foreach(lc, func_args)
	{
		sq_function_parameter_t *p = (sq_function_parameter_t *) lfirst(lc);

		if (p->mode != SQ_FUNC_PARAM_DEFAULT &&
			p->mode != SQ_FUNC_PARAM_IN &&
			p->mode != SQ_FUNC_PARAM_VARIADIC)
			raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(p->location)}, "OUT and INOUT arguments aren't allowed in TABLE functions");
	}

	return list_concat(func_args, columns);
}

/*
 * Determine return type of a TABLE function.  A single result column
 * returns setof that column's type; otherwise return setof record.
 */
static sq_type_name_t *
table_func_type_name(list_ref_t columns)
{
	sq_type_name_t   *result;

	if (list_length(columns) == 1)
	{
		sq_function_parameter_t *p = (sq_function_parameter_t *) linitial(columns);

		result = cast_node(sq_type_name_t, copy_node(p->arg_type));
	}
	else
		result = system_type_name("record");

	result->setof = true;

	return result;
}

/*
 * Convert a list of (dotted) names to a RangeVar (like
 * makeRangeVarFromNameList, but with position support).  The
 * "AnyName" refers to the any_name production in the grammar.
 */
static sq_range_var_t *
make_range_var_from_any_name(list_ref_t names, int position, core_yyscan_t yyscanner)
{
	sq_range_var_t   *r = make_node<sq_range_var_t>();

	switch (list_length(names))
	{
		case 1:
			r->catalogname = NULL;
			r->schemaname = NULL;
			r->relname = str_val(linitial(names));
			break;
		case 2:
			r->catalogname = NULL;
			r->schemaname = str_val(linitial(names));
			r->relname = str_val(lsecond(names));
			break;
		case 3:
			r->catalogname = str_val(linitial(names));
			r->schemaname = str_val(lsecond(names));
			r->relname = str_val(lthird(names));
			break;
		default:
			raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(position)}, "improper qualified name (too many dotted names): {}",
							name_list_to_string(names));
			break;
	}

	r->relpersistence = RELPERSISTENCE_PERMANENT;
	r->location = position;

	return r;
}

/*
 * Convert a relation_name with name and namelist to a RangeVar using
 * makeRangeVar.
 */
static sq_range_var_t *
make_range_var_from_qualified_name(char *name, list_ref_t namelist, int location,
							  core_yyscan_t yyscanner)
{
	sq_range_var_t   *r;

	check_qualified_name(namelist, yyscanner);
	r = make_range_var(NULL, NULL, location);

	switch (list_length(namelist))
	{
		case 1:
			r->catalogname = NULL;
			r->schemaname = name;
			r->relname = str_val(linitial(namelist));
			break;
		case 2:
			r->catalogname = name;
			r->schemaname = str_val(linitial(namelist));
			r->relname = str_val(lsecond(namelist));
			break;
		default:
			raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(location)}, "improper qualified name (too many dotted names): {}",
						   name_list_to_string(lcons(make_string(name), namelist)));
			break;
	}

	return r;
}

/* Separate Constraint nodes from COLLATE clauses in a ColQualList */
static void
split_col_qual_list(list_ref_t qual_list,
				 sq_list_t **constraint_list, sq_collate_clause_t **coll_clause,
				 core_yyscan_t yyscanner)
{
	list_cell_t   cell;

	*coll_clause = NULL;
	*constraint_list = NIL;
	foreach(cell, qual_list)
	{
		sq_node_t	   *n = (sq_node_t *) lfirst(cell);

		if (is_a(n, SQ_T_CONSTRAINT))
		{
			/* keep it in list */
			*constraint_list = lappend(*constraint_list, n);
			continue;
		}
		if (is_a(n, SQ_T_COLLATE_CLAUSE))
		{
			sq_collate_clause_t *c = (sq_collate_clause_t *) n;

			if (*coll_clause)
				raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(c->location)}, "multiple COLLATE clauses not allowed");
			*coll_clause = c;
		}
		else
			raise({.sqlstate = sqlstate_internal_error}, "unexpected node type {}", (int) n->tag);
		/* non-Constraint nodes are left out of the constraint list */
	}
}

/*
 * Process result of ConstraintAttributeSpec, and set appropriate bool flags
 * in the output command node.  Pass NULL for any flags the particular
 * command doesn't support.
 */
static void
process_ca_sbits(int cas_bits, int location, const char *constr_type,
			   bool *deferrable, bool *initdeferred, bool *is_enforced,
			   bool *not_valid, bool *no_inherit, core_yyscan_t yyscanner)
{
	/* defaults */
	if (deferrable)
		*deferrable = false;
	if (initdeferred)
		*initdeferred = false;
	if (not_valid)
		*not_valid = false;
	if (is_enforced)
		*is_enforced = true;

	if (cas_bits & (CAS_DEFERRABLE | CAS_INITIALLY_DEFERRED))
	{
		if (deferrable)
			*deferrable = true;
		else
			raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(location)}, "{} constraints cannot be marked DEFERRABLE",
							constr_type);
	}

	if (cas_bits & CAS_INITIALLY_DEFERRED)
	{
		if (initdeferred)
			*initdeferred = true;
		else
			raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(location)}, "{} constraints cannot be marked DEFERRABLE",
							constr_type);
	}

	if (cas_bits & CAS_NOT_VALID)
	{
		if (not_valid)
			*not_valid = true;
		else
			raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(location)}, "{} constraints cannot be marked NOT VALID",
							constr_type);
	}

	if (cas_bits & CAS_NO_INHERIT)
	{
		if (no_inherit)
			*no_inherit = true;
		else
			raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(location)}, "{} constraints cannot be marked NO INHERIT",
							constr_type);
	}

	if (cas_bits & CAS_NOT_ENFORCED)
	{
		if (is_enforced)
			*is_enforced = false;
		else
			raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(location)}, "{} constraints cannot be marked NOT ENFORCED",
							constr_type);

		/*
		 * NB: The validated status is irrelevant when the constraint is set to
		 * NOT ENFORCED, but for consistency, it should be set accordingly.
		 * This ensures that if the constraint is later changed to ENFORCED, it
		 * will automatically be in the correct NOT VALIDATED state.
		 */
		if (not_valid)
			*not_valid = true;
	}

	if (cas_bits & CAS_ENFORCED)
	{
		if (is_enforced)
			*is_enforced = true;
		else
			raise({.sqlstate = sqlstate_feature_not_supported, .pos = parser_errposition(location)}, "{} constraints cannot be marked ENFORCED",
							constr_type);
	}
}

/*
 * Parse a user-supplied partition strategy string into parse node
 * PartitionStrategy representation, or die trying.
 */
static sq_partition_strategy_t
parse_partition_strategy(char *strategy, int location, core_yyscan_t yyscanner)
{
	if (strcasecmp(strategy, "list") == 0)
		return SQ_PARTITION_STRATEGY_LIST;
	else if (strcasecmp(strategy, "range") == 0)
		return SQ_PARTITION_STRATEGY_RANGE;
	else if (strcasecmp(strategy, "hash") == 0)
		return SQ_PARTITION_STRATEGY_HASH;

	raise({.sqlstate = sqlstate_invalid_parameter_value, .pos = parser_errposition(location)}, "unrecognized partitioning strategy \"{}\"", strategy);
	return SQ_PARTITION_STRATEGY_LIST; /* keep compiler quiet */

}

/*
 * Process pubobjspec_list to check for errors in any of the objects and
 * convert PUBLICATIONOBJ_CONTINUATION into appropriate PublicationObjSpecType.
 */
static void
preprocess_pubobj_list(list_ref_t pubobjspec_list, core_yyscan_t yyscanner)
{
	list_cell_t   cell;
	sq_publication_obj_spec_t *pubobj;
	sq_publication_obj_spec_type_t prevobjtype = SQ_PUBLICATIONOBJ_CONTINUATION;

	if (!pubobjspec_list)
		return;

	pubobj = (sq_publication_obj_spec_t *) linitial(pubobjspec_list);
	if (pubobj->pubobjtype == SQ_PUBLICATIONOBJ_CONTINUATION)
		raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(pubobj->location), .detail = "One of TABLE or TABLES IN SCHEMA must be specified before a standalone table or schema name."}, "invalid publication object list");

	foreach(cell, pubobjspec_list)
	{
		pubobj = (sq_publication_obj_spec_t *) lfirst(cell);

		if (pubobj->pubobjtype == SQ_PUBLICATIONOBJ_CONTINUATION)
			pubobj->pubobjtype = prevobjtype;

		if (pubobj->pubobjtype == SQ_PUBLICATIONOBJ_TABLE)
		{
			/* relation name or pubtable must be set for this type of object */
			if (!pubobj->name && !pubobj->pubtable)
				raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(pubobj->location)}, "invalid table name");

			if (pubobj->name)
			{
				/* convert it to PublicationTable */
				sq_publication_table_t *pubtable = make_node<sq_publication_table_t>();

				pubtable->relation =
					make_range_var(NULL, pubobj->name, pubobj->location);
				pubobj->pubtable = pubtable;
				pubobj->name = NULL;
			}
		}
		else if (pubobj->pubobjtype == SQ_PUBLICATIONOBJ_TABLES_IN_SCHEMA ||
				 pubobj->pubobjtype == SQ_PUBLICATIONOBJ_TABLES_IN_CUR_SCHEMA)
		{
			/* WHERE clause is not allowed on a schema object */
			if (pubobj->pubtable && pubobj->pubtable->where_clause)
				raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(pubobj->location)}, "WHERE clause not allowed for schema");

			/* Column list is not allowed on a schema object */
			if (pubobj->pubtable && pubobj->pubtable->columns != NIL)
				raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(pubobj->location)}, "column specification not allowed for schema");

			/*
			 * We can distinguish between the different type of schema objects
			 * based on whether name and pubtable is set.
			 */
			if (pubobj->name)
				pubobj->pubobjtype = SQ_PUBLICATIONOBJ_TABLES_IN_SCHEMA;
			else if (!pubobj->name && !pubobj->pubtable)
				pubobj->pubobjtype = SQ_PUBLICATIONOBJ_TABLES_IN_CUR_SCHEMA;
			else
				raise({.sqlstate = sqlstate_syntax_error, .pos = parser_errposition(pubobj->location)}, "invalid schema name");
		}

		prevobjtype = pubobj->pubobjtype;
	}
}

/*----------
 * Recursive view transformation
 *
 * Convert
 *
 *     CREATE RECURSIVE VIEW relname (aliases) AS query
 *
 * to
 *
 *     CREATE VIEW relname (aliases) AS
 *         WITH RECURSIVE relname (aliases) AS (query)
 *         SELECT aliases FROM relname
 *
 * Actually, just the WITH ... part, which is then inserted into the original
 * view definition as the query.
 * ----------
 */
static sq_node_t *
make_recursive_view_select(const char *relname, list_ref_t aliases, sq_node_t *query)
{
	sq_select_stmt_t *s = make_node<sq_select_stmt_t>();
	sq_with_clause_t *w = make_node<sq_with_clause_t>();
	sq_common_table_expr_t *cte = make_node<sq_common_table_expr_t>();
	list_ref_t	   tl = NIL;
	list_cell_t   lc;

	/* create common table expression */
	cte->ctename = relname;
	cte->aliascolnames = aliases;
	cte->ctematerialized = SQ_CTEMaterializeDefault;
	cte->ctequery = query;
	cte->location = -1;

	/* create WITH clause and attach CTE */
	w->recursive = true;
	w->ctes = list_make1(cte);
	w->location = -1;

	/*
	 * create target list for the new SELECT from the alias list of the
	 * recursive view specification
	 */
	foreach(lc, aliases)
	{
		sq_res_target_t  *rt = make_node<sq_res_target_t>();

		rt->name = NULL;
		rt->indirection = NIL;
		rt->val = make_column_ref(str_val(lfirst(lc)), NIL, -1, 0);
		rt->location = -1;

		tl = lappend(tl, rt);
	}

	/*
	 * create new SELECT combining WITH clause, target list, and fake FROM
	 * clause
	 */
	s->with_clause = w;
	s->target_list = tl;
	s->from_clause = list_make1(make_range_var(NULL, relname, -1));

	return (sq_node_t *) s;
}

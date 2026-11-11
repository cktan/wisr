/*-------------------------------------------------------------------------
 *
 * guc_hooks.h
 *		Declarations of per-variable callback functions used by GUC.
 *
 * These functions are scattered throughout the system, but we
 * declare them all here to avoid having to propagate guc.h into
 * a lot of unrelated header files.
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 2026, CK Tan: the changes for digres,
 * under the MIT License (https://github.com/cktan/wisr/blob/main/LICENSE)
 *
 *	  src/include/utils/guc_hooks.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef GUC_HOOKS_H
#define GUC_HOOKS_H 1

#include "utils/guc.h"

/*
 * See guc.h for the typedefs that these hook functions should match
 * (GucBoolCheckHook and so on).
 *
 * Please keep the declarations in order by GUC variable name.
 */

extern bool check_application_name(std::string &newval, void **extra,
                                   GucSource source);
extern void assign_application_name(const std::string &newval, void *extra);
extern bool check_backtrace_functions(std::string &newval, void **extra,
                                      GucSource source);
extern void assign_backtrace_functions(const std::string &newval, void *extra);
extern bool check_bonjour(bool *newval, void **extra, GucSource source);
extern bool check_canonical_path(std::string &newval, void **extra,
                                 GucSource source);
extern bool check_client_connection_check_interval(int *newval, void **extra,
                                                   GucSource source);
extern bool check_client_encoding(std::string &newval, void **extra,
                                  GucSource source);
extern void assign_client_encoding(const std::string &newval, void *extra);
extern bool check_cluster_name(std::string &newval, void **extra,
                               GucSource source);
extern bool check_datestyle(std::string &newval, void **extra,
                            GucSource source);
extern void assign_datestyle(const std::string &newval, void *extra);
extern bool check_log_connections(std::string &newval, void **extra,
                                  GucSource source);
extern void assign_log_connections(const std::string &newval, void *extra);
extern bool check_default_table_access_method(std::string &newval, void **extra,
                                              GucSource source);
extern bool check_default_text_search_config(std::string &newval, void **extra,
                                             GucSource source);
extern void assign_default_text_search_config(const std::string &newval,
                                              void *extra);
extern bool check_default_with_oids(bool *newval, void **extra,
                                    GucSource source);
extern bool check_locale_messages(std::string &newval, void **extra,
                                  GucSource source);
extern void assign_locale_messages(const std::string &newval, void *extra);
extern bool check_locale_monetary(std::string &newval, void **extra,
                                  GucSource source);
extern void assign_locale_monetary(const std::string &newval, void *extra);
extern bool check_locale_numeric(std::string &newval, void **extra,
                                 GucSource source);
extern void assign_locale_numeric(const std::string &newval, void *extra);
extern bool check_locale_time(std::string &newval, void **extra,
                              GucSource source);
extern void assign_locale_time(const std::string &newval, void *extra);
extern bool check_log_destination(std::string &newval, void **extra,
                                  GucSource source);
extern void assign_log_destination(const std::string &newval, void *extra);
extern bool check_log_stats(bool *newval, void **extra, GucSource source);
extern bool check_log_timezone(std::string &newval, void **extra,
                               GucSource source);
extern void assign_log_timezone(const std::string &newval, void *extra);
extern const char *show_log_timezone(void);
extern bool check_max_stack_depth(int *newval, void **extra, GucSource source);
extern void assign_max_stack_depth(int newval, void *extra);
extern bool check_random_seed(double *newval, void **extra, GucSource source);
extern void assign_random_seed(double newval, void *extra);
extern const char *show_random_seed(void);
extern bool check_role(std::string &newval, void **extra, GucSource source);
extern void assign_role(const std::string &newval, void *extra);
extern const char *show_role(void);
extern bool check_restrict_nonsystem_relation_kind(std::string &newval,
                                                   void **extra,
                                                   GucSource source);
extern void assign_restrict_nonsystem_relation_kind(const std::string &newval,
                                                    void *extra);
extern bool check_search_path(std::string &newval, void **extra,
                              GucSource source);
extern void assign_search_path(const std::string &newval, void *extra);
extern bool check_session_authorization(std::string &newval, void **extra,
                                        GucSource source);
extern void assign_session_authorization(const std::string &newval,
                                         void *extra);
extern bool check_ssl(bool *newval, void **extra, GucSource source);
extern bool check_stage_log_stats(bool *newval, void **extra, GucSource source);
extern void assign_tcp_keepalives_count(int newval, void *extra);
extern const char *show_tcp_keepalives_count(void);
extern void assign_tcp_keepalives_idle(int newval, void *extra);
extern const char *show_tcp_keepalives_idle(void);
extern void assign_tcp_keepalives_interval(int newval, void *extra);
extern const char *show_tcp_keepalives_interval(void);
extern void assign_tcp_user_timeout(int newval, void *extra);
extern const char *show_tcp_user_timeout(void);
extern bool check_timezone(std::string &newval, void **extra, GucSource source);
extern void assign_timezone(const std::string &newval, void *extra);
extern const char *show_timezone(void);
extern bool check_timezone_abbreviations(std::string &newval, void **extra,
                                         GucSource source);
extern void assign_timezone_abbreviations(const std::string &newval,
                                          void *extra);
extern bool check_transaction_deferrable(bool *newval, void **extra,
                                         GucSource source);
extern bool check_transaction_isolation(int *newval, void **extra,
                                        GucSource source);
extern bool check_transaction_read_only(bool *newval, void **extra,
                                        GucSource source);
extern void assign_transaction_timeout(int newval, void *extra);
extern void assign_deadlock_timeout(int newval, void *extra);
extern const char *show_unix_socket_permissions(void);

#endif /* GUC_HOOKS_H */

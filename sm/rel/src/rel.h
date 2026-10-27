/*
 * rel.h -- internal definitions and structures for the rel test driver.
 */
#ifndef REL_H
#define REL_H

#include "sm.h"
#include <janet.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace rel {

/* One column definition in a table's schema. */
struct col_def_t {
  std::string name;
  uint8_t type_code;
  int32_t typmod;
  bool not_null;
};

/* One key column in an index definition. */
struct key_col_def_t {
  size_t col_idx;
  uint8_t type_code;
  int32_t typmod;
  std::string collation_name;
  sm_collation_t *collation;
  bool desc;
  bool nulls_first;
  std::string expr; /* "lower" or "upper" of the column; empty: the column */
};

/* One INCLUDE column in an index definition. */
struct include_col_def_t {
  std::string name;
  size_t col_idx;
  uint8_t type_code;
  int32_t typmod;
};

/* An index of a table a statement opened, as sm3's catalog defines it. */
struct index_def_t {
  std::string name;
  bool unique = false;
  std::vector<key_col_def_t> keys;
  std::vector<include_col_def_t> includes;
  const sm_index_def_t *def = nullptr;
};

/*
 * A table a statement opened, under its table-oid lock: sm3's
 * definition, rel's view of its columns and indexes, and its
 * handle once opened. The session keeps it until its next
 * statement begins.
 */
struct table_def_t {
  std::string name;
  sm_tbl_t *tbl = nullptr;             /* owned; closed with this */
  const sm_table_def_t *def = nullptr; /* tbl's, valid while it is open */
  std::vector<col_def_t> columns;
  std::map<std::string, index_def_t> indexes;

  table_def_t() = default;
  table_def_t(const table_def_t &) = delete;
  table_def_t &operator=(const table_def_t &) = delete;
  ~table_def_t();
};

/*
 * A row as PostgreSQL-style values and null flags, with the
 * buffers its by-reference values point into.
 */
struct row_values_t {
  std::vector<std::vector<uint8_t>> bufs;
  std::vector<sm_datum_t> values;
  std::unique_ptr<bool[]> isnull;

  explicit row_values_t(size_t n) : bufs(n), values(n), isnull(new bool[n]()) {}
};

/*
 * An index cursor a script opened with (idx-open), and the table
 * handle it reads through, which it keeps past its statement. It
 * is ended before the handle is closed, and before its
 * transaction ends, which would close it under rel.
 */
struct cursor_t {
  std::unique_ptr<table_def_t> table;
  index_def_t *idef = nullptr;   /* table's */
  sm_idx_scan_t *scan = nullptr; /* owned; ended with this */

  cursor_t() = default;
  cursor_t(const cursor_t &) = delete;
  cursor_t &operator=(const cursor_t &) = delete;
  ~cursor_t();
};

/* Context for one active database session. */
struct session_ctx_t {
  std::string name;
  std::string db_name;
  sm_session_t *session = nullptr;
  std::vector<std::unique_ptr<table_def_t>> stmt_tables; /* this statement's */
  std::vector<sm_stmt_t *> outer_stmts; /* put aside by (stmt-inner) */
  std::map<int32_t, std::unique_ptr<cursor_t>> cursors; /* by handle */
  int32_t next_cursor = 0; /* the last handle given out */
};

/* End every cursor s has open, before its transaction ends. */
void end_cursors(session_ctx_t *s);

/*
 * Forget every cursor s has open without ending it: the engine
 * aborted their transaction, which closed them.
 */
void forget_cursors(session_ctx_t *s);

/* The global catalog tracking environment, sessions and active state. */
class catalog_t {
public:
  catalog_t();
  ~catalog_t();

  /* Delete copy and move semantics. */
  catalog_t(const catalog_t &) = delete;
  catalog_t &operator=(const catalog_t &) = delete;

  /* Environment lifecycle. */
  int env_init(const char *path);
  int env_attach(const char *path, bool toast = false,
                 const sm_env_options_t *opt = nullptr);
  int env_detach(int force);
  sm_env_t *env() const { return m_env; }
  const std::string &env_path() const { return m_env_path; }

  /* Session management. */
  int session_open(const std::string &name, const std::string &db_name,
                   const sm_options_t *opt);
  int session_use(const std::string &name);
  int session_close(const std::string &name);
  session_ctx_t *active_session();
  session_ctx_t *get_session(const std::string &name);

  /*
   * Tables, from sm3's catalog in the session's transaction: the
   * table name in public, or nullptr if there is none, locked in
   * mode and kept by the session for this statement; and the
   * index name with its table, as {nullptr, nullptr} if none.
   */
  table_def_t *open_table(session_ctx_t *s, const std::string &name,
                          sm_lockmode_t mode);
  std::pair<table_def_t *, index_def_t *>
  open_index(session_ctx_t *s, const std::string &name, sm_lockmode_t mode);

  /* Reset all state (closing sessions and detaching env). */
  void reset();

private:
  sm_env_t *m_env = nullptr;
  std::string m_env_path;
  std::map<std::string, session_ctx_t> m_sessions;
  std::string m_active_session;
};

/* Global singleton catalog instance. */
catalog_t &get_catalog();

/* ---- Type Conversions ---- */

/* Parse a Janet keyword or symbol into an sm2 type code (e.g. :int4 -> 3). */
int parse_type_code(Janet val, uint8_t *code_out);

/* Return type name for a type code. */
const char *type_name_of(uint8_t code);

/*
 * Convert a Janet value to a value and null flag; a by-reference
 * value points into buf. A type rel does not convert natively is
 * given as its canonical text, parsed by sm_type_in.
 */
int janet_to_value(Janet val, const col_def_t &col, std::vector<uint8_t> &buf,
                   sm_datum_t *val_out, bool *isnull_out);

/*
 * Convert a value back to a Janet value: a number, boolean or
 * string for the types rel converts natively, and canonical text
 * (sm_type_out) for any other. A value out of line (TOAST) is
 * fetched through tbl; with no tbl -- the old row a write hands
 * back, whose values out of line that write erased -- it is the
 * keyword :toast.
 */
Janet value_to_janet(const sm_datum_t &val, bool isnull, uint8_t type_code,
                     int32_t typmod, sm_tbl_t *tbl = nullptr);

/*
 * A column's typmod from the numbers after its type, as
 * PostgreSQL encodes them: (p[, s]) for a decimal, (p) for a
 * time or timestamp precision, (n) for varchar; -1 with none.
 * Fails if the type takes no such numbers.
 */
int typmod_of(uint8_t code, const std::vector<int32_t> &mods,
              int32_t *typmod_out);

/* The numbers after a column's type that give typmod: typmod_of's inverse. */
std::vector<int32_t> mods_of(uint8_t code, int32_t typmod);

/*
 * What rel's evaluator makes of an index (REL_DESIGN.md §4.3): the
 * predicate of a partial one, column where_att equal to where_text
 * as its type prints it, and for each key column that is an
 * expression the function, "lower" or "upper", and the text column
 * it is of. It travels as the index's blob, which the storage
 * manager keeps and does not read.
 */
struct index_eval_t {
  bool has_where = false;
  int32_t where_att = 0;
  uint8_t where_code = 0;
  int32_t where_typmod = -1;
  std::string where_text;
  std::vector<std::pair<std::string, int32_t>> exprs; /* function, attnum */
};

/* e as an index's blob, and back; parsing fails on a blob not rel's. */
std::string format_index_eval(const index_eval_t &e);
bool parse_index_eval(const std::string &blob, index_eval_t &e);

/*
 * rel's evaluator of partial and expression indexes, given to the
 * storage manager when a home is attached. It runs on the index
 * build's worker threads too, so it touches nothing of Janet's.
 */
int index_eval(void *ctx, uint64_t table, uint64_t index,
               const std::string &eval, const sm_datum_t *values,
               const bool *isnull, size_t n, bool *belongs,
               std::vector<std::string> &out, bool *out_isnull) noexcept;

/* Construct a varlena buffer from raw string bytes. */
void make_varlena(const char *data, size_t len, std::vector<uint8_t> &out);

/* The payload of the varlena at p, its length from its header. */
void varlena_payload(const void *p, const char **out_data, size_t *out_len);

/*
 * The table name's oid, as a search path of the session's temporary
 * tables and then public finds it: 0, 1 if there is none, or -1.
 */
int rel_table_oid(session_ctx_t *s, const std::string &name, uint64_t *oid);

/* The index name and its table, found as rel_table_oid finds a table. */
int rel_index_oid(session_ctx_t *s, const std::string &name, uint64_t *table,
                  uint64_t *index);

/* Register all rel native functions and macros into env. */
void register_rel_cfuns(JanetTable *env);

} // namespace rel

#endif /* REL_H */

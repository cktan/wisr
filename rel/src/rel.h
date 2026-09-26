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
};

/* One INCLUDE column in an index definition. */
struct include_col_def_t {
  std::string name;
  size_t col_idx;
  uint8_t type_code;
  int32_t typmod;
};

/* Definition of an index. */
struct index_def_t {
  std::string name;
  std::string table_name;
  uint16_t prefix = 0;
  bool unique = false;
  std::vector<key_col_def_t> keys;
  std::vector<include_col_def_t> includes;
};

/* Definition of a table. */
struct table_def_t {
  std::string name;
  std::string db_name;
  uint64_t storeid = 0;
  std::string seq_name;
  uint16_t next_prefix = 1;
  std::vector<col_def_t> columns;
  std::map<std::string, index_def_t> indexes;
};

/*
 * A row as PostgreSQL-style values and null flags, with the
 * buffers its by-reference values point into.
 */
struct row_values_t {
  std::vector<std::vector<uint8_t>> bufs;
  std::vector<sm_datum_val_t> values;
  std::unique_ptr<bool[]> isnull;

  explicit row_values_t(size_t n) : bufs(n), values(n), isnull(new bool[n]()) {}
};

/* Context for one active database session. */
struct session_ctx_t {
  std::string name;
  std::string db_name;
  sm_session_t *session = nullptr;
  std::map<std::string, sm_rel_t *> open_tables;
  std::map<std::string, sm_idx_t *> open_indexes;
};

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
  int env_attach(const char *path);
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

  /* Database-level Table & Index Catalog. */
  table_def_t *find_table(const std::string &db_name,
                          const std::string &table_name);
  void add_table(const std::string &db_name, table_def_t tdef);
  void remove_table(const std::string &db_name, const std::string &table_name);
  std::pair<table_def_t *, index_def_t *>
  find_index(const std::string &db_name, const std::string &idx_name);
  void remove_index(const std::string &db_name, const std::string &idx_name);

  /* Per-session handles. */
  sm_rel_t *get_or_open_rel(session_ctx_t *s, const table_def_t &tdef);
  sm_idx_t *get_or_open_idx(session_ctx_t *s, const index_def_t &idef,
                            const table_def_t &tdef);

  /* Reset all state (closing sessions and detaching env). */
  void reset();

private:
  sm_env_t *m_env = nullptr;
  std::string m_env_path;
  std::map<std::string, session_ctx_t> m_sessions;
  std::string m_active_session;
  std::map<std::string, std::map<std::string, table_def_t>> m_db_tables;
};

/* The sm_attribute_t schema of a table's columns. */
std::vector<sm_attribute_t> attributes_of(const table_def_t &tdef);

/* Global singleton catalog instance. */
catalog_t &get_catalog();

/* ---- Type Conversions ---- */

/* Parse a Janet keyword or symbol into an sm2 type code (e.g. :int4 -> 3). */
int parse_type_code(Janet val, uint8_t *code_out);

/* Return type name for a type code. */
const char *type_name_of(uint8_t code);

/* Convert a Janet value to a native sm_datum_t buffer. */
int janet_to_datum(Janet val, const col_def_t &col, std::vector<uint8_t> &buf,
                   sm_datum_t *datum_out);

/* Convert a native sm_datum_t back to a Janet value. */
Janet datum_to_janet(const sm_datum_t &datum, uint8_t type_code);

/*
 * Convert a Janet value to a value and null flag; a by-reference
 * value points into buf. Types rel cannot convert are refused.
 */
int janet_to_value(Janet val, const col_def_t &col, std::vector<uint8_t> &buf,
                   sm_datum_val_t *val_out, bool *isnull_out);

/* View a value as an sm_datum_t; a by-value one views val's bytes. */
sm_datum_t value_to_datum(const sm_datum_val_t &val, bool isnull,
                          uint8_t type_code);

/* Convert a value back to a Janet value. */
Janet value_to_janet(const sm_datum_val_t &val, bool isnull, uint8_t type_code);

/* Construct a varlena buffer from raw string bytes. */
void make_varlena(const char *data, size_t len, std::vector<uint8_t> &out);

/* Extract payload from a varlena. */
bool varlena_extract(const void *data, size_t len, const char **out_data,
                     size_t *out_len);

/* Register all rel native functions and macros into env. */
void register_rel_cfuns(JanetTable *env);

} // namespace rel

#endif /* REL_H */

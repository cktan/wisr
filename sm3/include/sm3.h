/*
 * sm3.h -- level 3: schema and index maintenance (SM3_DESIGN.md).
 *
 * So far, the catalog (CATALOG.md): made in a database with
 * cat_init, loaded into memory with cat_open, and added to with
 * cat_create_table. Every call reports failure as sm1 does
 * (DESIGN.md §5): -1, with sm1::errnum() and sm1::errmsg() set.
 */
#ifndef SM3_H
#define SM3_H

#include "sm1.h"
#include "sm2.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sm3 {

/* The built-in namespaces' fixed oids; oids from 1000 are handed out. */
constexpr uint64_t nsp_pg_catalog = 1;
constexpr uint64_t nsp_public = 2;

/* A column, as a table is declared with it. */
struct column_spec_t {
    std::string name;
    uint8_t     type_code;  /* sm2's frozen code (sm2/TYPE.md §3) */
    int32_t     typmod;
    bool        not_null;
};

/* A column of a loaded table. */
struct column_def_t {
    std::string                name;
    const sm2::type_t*         type;
    int32_t                    typmod;
    bool                       not_null;
    bool                       dropped;
    std::optional<std::string> missing;   /* its bytes, as sm2 holds it */
    const sm2::collation_t*    collation; /* nullptr: none */
};

/* An index of a loaded table. */
struct index_def_t {
    uint64_t                          oid;
    std::string                       name;
    uint16_t                          prefix;
    bool                              unique;
    std::vector<int32_t>              key_attnums;     /* 0-based */
    std::vector<int32_t>              include_attnums; /* 0-based */
    std::vector<sm2::key_column_t>    key;             /* for sm2 */
    std::vector<sm2::attribute_t>     include;         /* for sm2 */
};

/*
 * A loaded table. Built whole, never changed: a definition is
 * replaced, not edited. schema and desc are what sm2 lays its rows
 * out by; desc views schema, so a definition is never copied.
 */
struct table_def_t {
    table_def_t() = default;
    table_def_t(const table_def_t&) = delete;
    table_def_t& operator=(const table_def_t&) = delete;

    uint64_t                      oid;
    uint64_t                      nspoid;
    std::string                   name;
    uint64_t                      storeid;
    std::string                   rowno_seq;
    uint16_t                      next_prefix;
    std::vector<column_def_t>     columns;
    std::vector<sm2::attribute_t> schema;
    sm2::tup_desc_t               desc;    /* over schema */
    std::vector<index_def_t>      indexes;
};

/* A database's catalog, loaded into memory. */
struct catalog_t;

/*
 * Make the catalog of a new database, in session's transaction:
 * its format version, the oid sequence, and the pg_catalog and
 * public namespaces. A database that has one already fails with
 * SM_ERR_EXISTS.
 */
int cat_init(sm1::session_t* session);

/*
 * Load the catalog of session's database, in session's
 * transaction, with one scan of each kind, and open every
 * collation it names at its recorded ICU version. A database
 * without one fails with SM_ERR_NOTFOUND; one whose format this
 * code does not read, with SM_ERR_VERSION.
 */
int cat_open(sm1::session_t* session, catalog_t*& out);

/* Close a catalog and its collations; nullptr is a no-op. */
void cat_close(catalog_t* cat);

/* The table with oid, or nullptr. */
std::shared_ptr<const table_def_t> cat_table(const catalog_t* cat,
                                             uint64_t oid);

/* The table named name in namespace nspoid, or nullptr. */
std::shared_ptr<const table_def_t> cat_table(const catalog_t* cat,
                                             uint64_t nspoid,
                                             std::string_view name);

/*
 * Create a table named name in namespace nspoid, with columns,
 * in session's transaction: its store, its rowno sequence, its
 * name and its definition. Its oid is set in oid. A name already
 * taken fails with SM_ERR_DUPKEY; an unknown namespace with
 * SM_ERR_NOTFOUND; a type code sm2 does not have, or a typmod
 * its type refuses, with SM_ERR_INVALID.
 *
 * It writes the catalog only: a catalog already loaded does not
 * see the table until it is loaded again.
 */
int cat_create_table(sm1::session_t* session, uint64_t nspoid,
                     std::string_view name,
                     std::span<const column_spec_t> columns, uint64_t& oid);

} // namespace sm3

#endif /* SM3_H */

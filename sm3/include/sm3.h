/*
 * sm3.h -- level 3: schema and index maintenance (SM3_DESIGN.md).
 *
 * So far, the catalog (CATALOG.md): made in a database with
 * cat_init, loaded into memory with cat_open, and changed by DDL
 * through a session, whose transaction sees its own changes and
 * publishes them at commit. Every call reports failure as sm1
 * does (DESIGN.md §5): -1, with sm1::errnum() and sm1::errmsg()
 * set.
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

/* A key column, as an index is declared with it. */
struct index_key_spec_t {
    int32_t                 attnum;      /* the column's 0-based place */
    bool                    desc;
    bool                    nulls_first;
    std::optional<uint64_t> collation;   /* text, varchar only */
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

/* A database's catalog, loaded into memory, shared by sessions. */
struct catalog_t;

/*
 * A session: an sm1 session reading a shared catalog, with its
 * open transaction's overlay -- the DDL it has made, which it sees
 * and no other session does until it commits.
 */
struct session_t;

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

/* The committed table with oid, or nullptr. */
std::shared_ptr<const table_def_t> cat_table(const catalog_t* cat,
                                             uint64_t oid);

/* The committed table named name in namespace nspoid, or nullptr. */
std::shared_ptr<const table_def_t> cat_table(const catalog_t* cat,
                                             uint64_t nspoid,
                                             std::string_view name);

/*
 * Open a session over sm1's session s, reading the catalog cat;
 * both must outlive it.
 */
int session_open(catalog_t* cat, sm1::session_t* s, session_t*& out);

/* Close a session; nullptr is a no-op. Its overlay is dropped. */
void session_close(session_t* s);

/* The sm1 session under s. */
sm1::session_t* session_sm1(const session_t* s);

/*
 * The transaction, as sm1's (sm1.h), with the overlay kept in
 * step: rollback drops the DDL made since the savepoint, forget
 * keeps it, abort drops it all, and commit publishes it into the
 * shared catalog once sm1 has committed. savepoint returns the
 * depth, as sm1::savepoint does.
 */
int begin(session_t* s, sm1::xctlevel_t level);
int savepoint(session_t* s);
int rollback(session_t* s, int depth);
int forget(session_t* s, int depth);
int commit(session_t* s);
int abort(session_t* s);

/* The table with oid, or nullptr, as s's transaction sees it. */
std::shared_ptr<const table_def_t> cat_table(const session_t* s,
                                             uint64_t oid);

/* The table named name in nspoid, as s's transaction sees it. */
std::shared_ptr<const table_def_t> cat_table(const session_t* s,
                                             uint64_t nspoid,
                                             std::string_view name);

/*
 * Take the schema lock of the table oid in mode, held to the end
 * of s's transaction, then set out to its definition as s sees
 * it (SM3_DESIGN.md §2). A transaction reading the table takes
 * it intent_shared, one writing it intent_exclusive; DDL takes it
 * exclusive itself, so it waits for them, and they for it. Once
 * held, the definition cannot change under s. A table dropped
 * while s waited fails with SM_ERR_NOTFOUND; a deadlock as
 * sm1::lock does.
 */
int cat_lock_table(session_t* s, uint64_t oid, sm1::lockmode_t mode,
                   std::shared_ptr<const table_def_t>& out);

/*
 * Create a table named name in namespace nspoid, with columns,
 * in session's transaction: its store, its rowno sequence, its
 * name and its definition. Its oid is set in oid. A name already
 * taken fails with SM_ERR_DUPKEY; an unknown namespace with
 * SM_ERR_NOTFOUND; a type code sm2 does not have, or a typmod
 * its type refuses, with SM_ERR_INVALID.
 *
 * The session's transaction sees it at once; others once it
 * commits.
 */
int cat_create_table(session_t* session, uint64_t nspoid,
                     std::string_view name,
                     std::span<const column_spec_t> columns, uint64_t& oid);

/*
 * The rest of the DDL, each in session's transaction and, as
 * cat_create_table, seen by it at once and by others at commit. A table, index or
 * collation that is not there fails with SM_ERR_NOTFOUND; a name
 * already taken in its namespace with SM_ERR_DUPKEY; anything the
 * definition cannot hold with SM_ERR_INVALID.
 */

/*
 * Drop the table oid: its name, its indexes' names, its
 * definition, its store with its rows and index entries, and its
 * rowno sequence.
 */
int cat_drop_table(session_t* session, uint64_t oid);

/* Rename the table oid to name, in its namespace. */
int cat_rename_table(session_t* session, uint64_t oid,
                     std::string_view name);

/*
 * Append col to the table oid. A row written before it reads it as
 * missing -- its bytes as sm2 holds them, a varlena for a
 * variable-width type -- or as null without one, so a NOT NULL
 * column needs one.
 */
int cat_add_column(session_t* session, uint64_t oid,
                   const column_spec_t& col,
                   std::optional<std::string_view> missing);

/*
 * Drop the column name of the table oid. It keeps its place, read
 * as null from then on (SM2_DESIGN.md §4); one an index uses is
 * refused.
 */
int cat_drop_column(session_t* session, uint64_t oid,
                    std::string_view name);

/*
 * Create an index named name on the table, keyed by keys, with the
 * INCLUDE columns includes, taking the table's next prefix; its
 * oid is set in oid. It records the index only: filling it from
 * the table's rows is the build's (SM3_DESIGN.md §3).
 */
int cat_create_index(session_t* session, uint64_t table,
                     std::string_view name,
                     std::span<const index_key_spec_t> keys,
                     std::span<const int32_t> includes, bool unique,
                     uint64_t& oid);

/*
 * Drop the index oid of the table: its name, its place in the
 * table's definition, and its entries.
 */
int cat_drop_index(session_t* session, uint64_t table, uint64_t oid);

/*
 * Create a collation named name in namespace nspoid for locale,
 * recording the ICU version it opens at; its oid is set in oid. A
 * locale ICU has no data for fails as sm2::collation_open does.
 */
int cat_create_collation(session_t* session, uint64_t nspoid,
                         std::string_view name, std::string_view locale,
                         bool deterministic, uint64_t& oid);

} // namespace sm3

#endif /* SM3_H */

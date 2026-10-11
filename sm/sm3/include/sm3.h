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
#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace sm3 {

/*
 * The built-in namespaces' fixed oids, which are PostgreSQL's for
 * the two, and the first oid handed out, which is PostgreSQL's
 * FirstNormalObjectId: the oids below it are for what is built in,
 * and digres compiles those in with PostgreSQL's own numbers
 * (PART4.md §10).
 */
constexpr uint64_t nsp_pg_catalog = 11;
constexpr uint64_t nsp_public = 2200;
constexpr uint64_t first_oid = 16384;

/*
 * The relids of a table's rels in its store (SM3_DESIGN.md §4):
 * the heap takes 1 and its indexes the ones above, 0xFFFF being
 * TOAST's. 0 is left unused, so an unset relid is never a live
 * rel.
 */
constexpr uint16_t heap_relid = 1;
constexpr uint16_t first_index_relid = 2;

/* A column, as a table is declared with it. */
struct column_spec_t {
    std::string name;
    uint8_t     type_code;  /* sm2's frozen code (sm2/TYPE.md §3) */
    int32_t     typmod;
    bool        not_null;
};

/*
 * A key column, as an index is declared with it: a column of the
 * table, or with attnum -1 an expression, which has no column
 * behind it and so gives its own type and typmod.
 */
struct index_key_spec_t {
    int32_t                 attnum;      /* 0-based place; -1: expression */
    bool                    desc;
    bool                    nulls_first;
    std::optional<uint64_t> collation;   /* text, varchar only */
    uint8_t                 type_code = 0;  /* an expression's (TYPE.md §3) */
    int32_t                 typmod = -1;    /* an expression's */
};

/*
 * Where an index's build stands (SM3_DESIGN.md §3). A pending
 * index is maintained by every write but not yet filled from the
 * rows that were there before it: it waits for the builder, or is
 * being built by it (builder_building). A failed one is not
 * maintained and can only be dropped.
 */
enum class index_state_t : uint8_t { pending = 0, valid = 1, failed = 2 };

/*
 * An index's state, with why its build failed: the error's code
 * and message, SM_OK and empty unless failed. It is no part of the
 * table's record or of its definition: it is kept by index, and
 * changes without the table being changed (cat_put_index_state).
 */
struct index_status_t {
  index_state_t state = index_state_t::pending;
  sm1::errnum_t error = sm1::SM_OK;
  std::string reason;
};

/*
 * ---- records: what the catalog stores of a table ----
 *
 * A table's record is the table as the catalog keeps it: plain
 * values, with types and collations by code and oid. A caller that
 * changes a table reads its record, changes it, and puts it back
 * (cat_get_table_rec, cat_put_table). A definition, table_def_t
 * below, is the same table resolved for use, and is only read.
 *
 * A record is stored as a key and a value for the table's own
 * fields, one for each column, by its place, and one for each
 * index, by its oid (CATALOG.md §2); a put writes only the ones
 * that changed. A column keeps its place for good, and the
 * indexes are in the order of their oids, which is the order they
 * were made in.
 */

/* A column, as the catalog stores it. */
struct column_rec_t {
  std::string name;
  uint8_t type_code;
  int32_t typmod;
  bool not_null;
  bool dropped;
  std::optional<std::string> missing; /* its bytes; a varlena if variable */
  std::optional<uint64_t> collation;
  bool operator==(const column_rec_t&) const = default;
};

/* One key column of an index; attnum is the column's 0-based place. */
struct index_key_rec_t {
  int32_t attnum;
  bool desc;
  bool nulls_first;
  std::optional<uint64_t> collation;
  uint8_t type_code = 0; /* an expression's, attnum -1 (TYPE.md §3) */
  int32_t typmod = -1;   /* an expression's */
  bool operator==(const index_key_rec_t&) const = default;
};

/* An index, as the catalog stores it. */
struct index_rec_t {
  uint64_t oid;
  std::string name;
  uint16_t relid;
  bool unique;
  std::vector<index_key_rec_t> keys;
  std::vector<int32_t> includes; /* 0-based attnums */
  std::string eval;                 /* the host's blob; empty: plain */
  std::vector<int32_t> reads;       /* the columns it reads, 0-based */
  bool operator==(const index_rec_t&) const = default;
};

/* A table, its columns and its indexes: one blob. */
struct table_rec_t {
  uint64_t nspoid;
  std::string name;
  uint64_t storeid;
  std::string rowno_seq;
  uint16_t next_relid;
  std::vector<column_rec_t> columns;
  std::vector<index_rec_t> indexes;
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
    uint16_t                          relid;
    bool                              unique;
    std::vector<int32_t>              key_attnums;     /* 0-based; -1: expr */
    std::vector<int32_t>              include_attnums; /* 0-based */
    std::vector<sm2::key_column_t>    key;             /* for sm2 */
    std::vector<sm2::attribute_t>     include;         /* for sm2 */
    /*
     * A partial or an expression index (SM3_DESIGN.md §3): eval is
     * its predicate and its expressions as the host serialized
     * them, bytes sm3 does not read, empty for a plain index; reads
     * is the columns they read, 0-based.
     */
    std::string                       eval;
    std::vector<int32_t>              reads;
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
    uint16_t                      next_relid;
    std::vector<column_def_t>     columns;
    std::vector<sm2::attribute_t> schema;
    sm2::tup_desc_t               desc;    /* over schema */
    std::vector<index_def_t>      indexes;
    table_rec_t                   rec;     /* the record it was built from */
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
 * Make the catalog of the new database dbname of env: its format
 * version, the oid sequence, and the pg_catalog and public
 * namespaces. It needs no session: it opens one of its own, with
 * opt as sm1::opendb takes it, and works in a transaction of its
 * own, so the catalog is made whole or not at all. A database
 * that has one already fails with SM_ERR_EXISTS.
 */
int cat_init(sm1::env_t* env, std::string_view dbname,
             const sm1::options_t* opt = nullptr);

/*
 * Load the catalog of the database dbname of env, on a session
 * and in a transaction of its own, with one scan of each kind,
 * and open every collation it names at its recorded ICU version.
 * A database without one fails with SM_ERR_NOTFOUND; one whose
 * format this code does not read, with SM_ERR_VERSION.
 */
int cat_open(sm1::env_t* env, std::string_view dbname, catalog_t*& out);

/* Close a catalog and its collations; nullptr is a no-op. */
void cat_close(catalog_t* cat);

/* The committed table with oid, or nullptr. */
std::shared_ptr<const table_def_t> cat_get_table(const catalog_t* cat,
                                             uint64_t oid);

/* The committed table named name in namespace nspoid, or nullptr. */
std::shared_ptr<const table_def_t> cat_get_table(const catalog_t* cat,
                                             uint64_t nspoid,
                                             std::string_view name);

/* The names of the committed tables in namespace nspoid, sorted. */
std::vector<std::string> cat_list_tables(const catalog_t* cat,
                                         uint64_t nspoid);

/*
 * Whether there is a committed namespace named name; its oid is
 * set in oid.
 */
bool cat_get_namespace(const catalog_t* cat, std::string_view name,
                       uint64_t& oid);

/* The names of the committed namespaces, sorted. */
std::vector<std::string> cat_list_namespaces(const catalog_t* cat);

/*
 * Open a session over sm1's session s, reading the catalog cat;
 * both must outlive it.
 */
int session_open(catalog_t* cat, sm1::session_t* s, session_t*& out);

/* Close a session; nullptr is a no-op. Its overlay is dropped. */
void session_close(session_t* s);

/* The sm1 session under s. */
sm1::session_t* session_sm1(const session_t* s);

/* The catalog s reads. */
catalog_t* session_cat(const session_t* s);

/*
 * The transaction, as sm1's (sm1.h), with the overlay kept in
 * step: rollback drops the DDL made since the savepoint, forget
 * keeps it, abort drops it all, and commit publishes it into the
 * shared catalog once sm1 has committed. savepoint returns the
 * depth, as sm1::savepoint does. read_only is sm1::begin's.
 */
int begin(session_t* s, sm1::xctlevel_t level, bool read_only = false);

/*
 * Begin a statement, as sm1::stmt_begin does. A session's
 * statements start here and not in sm1, because this is where a
 * READ COMMITTED session notes how far the catalog had got before
 * the statement's snapshot, which decides the indexes the statement
 * may scan (tbl_index_usable).
 */
int stmt_begin(session_t* s);

/* What a statement replaces, for stmt_end to put back. */
struct stmt_t {
    sm1::stmt_t sm1;
    uint64_t snap_seq = 0;
    bool pinned = false;
};

/*
 * Begin a statement inside another, as a trigger's commands run
 * inside the statement that fired it, saving the outer one in
 * outer; stmt_end puts it back (sm1::stmt_begin, sm1::stmt_end).
 */
int stmt_begin(session_t* s, stmt_t& outer);

/* End a statement begun inside another, resuming the outer one. */
int stmt_end(session_t* s, const stmt_t& outer);
int savepoint(session_t* s);
int rollback(session_t* s, int depth);
int forget(session_t* s, int depth);
int commit(session_t* s);
int abort(session_t* s);

/*
 * Prepare s's transaction as gid, as sm1::prepare does. One that
 * made DDL is refused with SM_ERR_INVALID: its overlay would have
 * to outlive the session to be published at commit_prepared.
 */
int prepare(session_t* s, std::string_view gid);

/* The table with oid, or nullptr, as s's transaction sees it. */
std::shared_ptr<const table_def_t> cat_get_table(const session_t* s,
                                             uint64_t oid);

/* The table named name in nspoid, as s's transaction sees it. */
std::shared_ptr<const table_def_t> cat_get_table(const session_t* s,
                                             uint64_t nspoid,
                                             std::string_view name);

/* The names of the tables in nspoid, sorted, as s's transaction sees them. */
std::vector<std::string> cat_list_tables(const session_t* s, uint64_t nspoid);

/*
 * Whether there is a namespace named name, as s's transaction
 * sees it; its oid is set in oid.
 */
bool cat_get_namespace(const session_t* s, std::string_view name,
                       uint64_t& oid);

/* The names of the namespaces s's transaction sees, sorted. */
std::vector<std::string> cat_list_namespaces(const session_t* s);

/*
 * The index named name in nspoid, as s's transaction sees it:
 * true with its table's oid and its own set. A scan of every
 * table's indexes, since the catalog keeps no map of their names.
 */
bool cat_get_index(const session_t* s, uint64_t nspoid,
                   std::string_view name, uint64_t& table, uint64_t& index);

/*
 * Take the table-oid lock of the table oid in mode, held to the end
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
 * ---- the catalog's writes: put and del ----
 *
 * The catalog is a repository: it stores what it is given, finds
 * it again, and forgets it. It makes no store and no sequence,
 * fills no index, and checks no more than that its own records
 * agree with each other: a name is taken once, a table's
 * namespace is there, a namespace that is dropped is empty. What
 * a CREATE or a DROP has to do besides is the DDL's, below, which
 * calls these. Each works in s's transaction, which sees the
 * change at once; others see it once it commits.
 */

/* Take the next oid; one is never handed out twice. */
int cat_new_oid(session_t* s, uint64_t& oid);

/*
 * Read the record of the table table_oid in order to change it:
 * take its table-oid lock exclusive, which waits out its readers and
 * writers and holds to the end of the transaction, and set rec to
 * its newest committed record, or the one s's transaction last
 * put. No such table fails with SM_ERR_NOTFOUND.
 */
int cat_get_table_rec(session_t* s, uint64_t table_oid, table_rec_t& rec);

/*
 * Store rec as the record of the table table_oid. A table that is
 * not there is added: its namespace must be, or SM_ERR_NOTFOUND,
 * and is held against a drop to the end of the transaction. One
 * that is there is replaced, under its table-oid lock exclusive; it
 * does not change namespace. Either way the names in rec -- the
 * table's, and each index's -- are made to be the names taken,
 * the new ones first, so that one another object has fails with
 * SM_ERR_DUPKEY and leaves the table as it was. A record that
 * names a type, a collation or a column that is not there fails
 * with SM_ERR_CORRUPT.
 */
int cat_put_table(session_t* s, uint64_t table_oid, const table_rec_t& rec);

/*
 * The state of the index index_oid: as committed, or as s's
 * transaction sees it, its own change included. An index that has
 * none recorded, or is not there, is pending.
 */
index_status_t cat_get_index_state(const catalog_t* cat, uint64_t index_oid);
index_status_t cat_get_index_state(const session_t* s, uint64_t index_oid);

/*
 * Store the state of the index index_oid of the table table_oid,
 * which must be pending: a state is set once, to valid or to
 * failed, and one that is no longer pending fails with
 * SM_ERR_INVALID. Setting a pending index pending changes nothing.
 * It takes the table-oid lock IX, as a writer of the table does,
 * and not exclusive: no reader or writer of the table waits for
 * it or it for them, and only DDL on the table is kept out. A
 * transaction that sees the state late sees pending, which is
 * always safe -- it maintains the index and does not scan it. The
 * state is published with the commit, so the catalog in memory and
 * the store never differ. No such table or index fails with
 * SM_ERR_NOTFOUND.
 */
int cat_put_index_state(session_t* s, uint64_t table_oid, uint64_t index_oid,
                        const index_status_t& status);

/*
 * Erase the record of the table table_oid and the names it took,
 * under its table-oid lock exclusive. Its store and its sequence are
 * the caller's to drop. No such table fails with SM_ERR_NOTFOUND.
 */
int cat_del_table(session_t* s, uint64_t table_oid);

/*
 * Store a namespace named name as namespace_oid, which must be
 * new. A name another namespace has fails with SM_ERR_DUPKEY.
 */
int cat_put_namespace(session_t* s, uint64_t namespace_oid,
                      std::string_view name);

/*
 * Erase the namespace namespace_oid. One that still holds a table
 * or a collation is refused with SM_ERR_INUSE, and the two built
 * in, pg_catalog and public, with SM_ERR_INVALID. It waits for
 * every transaction that is putting something in the namespace,
 * and one that tries after it has committed finds no namespace
 * (CATALOG.md §5).
 */
int cat_del_namespace(session_t* s, uint64_t namespace_oid);

/*
 * Store a collation named name in namespace_oid as collation_oid,
 * which must be new, for locale, recording the ICU version it
 * opens at. The catalog opens it and keeps it open, for a table's
 * definition to point at. A locale ICU has no data for fails as
 * sm2::collation_open does; a taken name with SM_ERR_DUPKEY; no
 * such namespace with SM_ERR_NOTFOUND.
 */
int cat_put_collation(session_t* s, uint64_t collation_oid,
                      uint64_t namespace_oid, std::string_view name,
                      std::string_view locale, bool deterministic);

/*
 * ---- DDL: what a CREATE, a DROP or an ALTER does ----
 *
 * Each call below is one statement's work: it makes or drops what
 * the object needs outside the catalog -- a store, a sequence, an
 * index's entries -- and puts the object's record in the catalog
 * or takes it out. They are callers of the catalog, not part of
 * it.
 */

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
int create_table(session_t* session, uint64_t nspoid, std::string_view name,
                 std::span<const column_spec_t> columns, uint64_t& oid);

/*
 * The rest of the DDL, each in session's transaction and, as
 * create_table, seen by it at once and by others at commit. A table, index or
 * collation that is not there fails with SM_ERR_NOTFOUND; a name
 * already taken in its namespace with SM_ERR_DUPKEY; anything the
 * definition cannot hold with SM_ERR_INVALID.
 */

/*
 * Drop the table oid: its name, its indexes' names, its
 * definition, its store with its rows and index entries, and its
 * rowno sequence.
 */
int drop_table(session_t* session, uint64_t oid);

/* Rename the table oid to name, in its namespace. */
/*
 * Empty the table oid: it gets a new, empty store, and the old one
 * is dropped, rows and index entries with it. The columns, the
 * indexes and their states, and the rowno sequence are kept, so
 * new rows go on from the last rowno handed out. Transactional,
 * and the same cost whatever the table holds. It takes the schema
 * lock exclusive, as other DDL does. A transaction that began
 * before it committed and first reads the table after finds it
 * empty, as in PostgreSQL.
 */
int truncate_table(session_t* session, uint64_t oid);

int rename_table(session_t* session, uint64_t oid,
                 std::string_view name);

/*
 * Append col to the table oid. A row written before it reads it as
 * missing -- its bytes as sm2 holds them, a varlena for a
 * variable-width type -- or as null without one, so a NOT NULL
 * column needs one.
 */
int add_column(session_t* session, uint64_t oid,
               const column_spec_t& col,
               std::optional<std::string_view> missing);

/*
 * Drop the column name of the table oid. It keeps its place, read
 * as null from then on (SM2_DESIGN.md §4); one an index uses is
 * refused.
 */
int drop_column(session_t* session, uint64_t oid,
                std::string_view name);

/*
 * Create a namespace named name, SQL's schema: a container that
 * scopes the names of the tables and collations made in it. Its
 * oid is set in oid. Like any DDL it is made in session's
 * transaction, which sees it at once and no other does until the
 * commit. A name another namespace has fails with SM_ERR_DUPKEY;
 * no name, with SM_ERR_INVALID.
 */
int create_namespace(session_t* session, std::string_view name,
                     uint64_t& oid);

/*
 * Drop the namespace oid. One that still holds a table or a
 * collation is refused with SM_ERR_INUSE, and the two built in,
 * pg_catalog and public, with SM_ERR_INVALID. It waits for every
 * transaction that is creating something in the namespace, and
 * one that tries after it has committed finds no namespace
 * (CATALOG.md §5).
 */
int drop_namespace(session_t* session, uint64_t oid);

/*
 * Create an index named name on the table, keyed by keys, with the
 * INCLUDE columns includes, taking the table's next relid; its
 * oid is set in oid. It records the index as pending, for the
 * build to fill from the table's rows and then mark valid
 * (SM3_DESIGN.md §3). On a table this transaction created, which
 * no other can see, it fills the index itself, from the rows the
 * transaction has written, and records it valid; two of those
 * rows with one key fail a unique index with SM_ERR_DUPKEY.
 */
int create_index(session_t* session, uint64_t table,
                 std::string_view name,
                 std::span<const index_key_spec_t> keys,
                 std::span<const int32_t> includes, bool unique,
                 uint64_t& oid);

/*
 * As create_index, for a partial or an expression index
 * (SM3_DESIGN.md §3). eval is its predicate and its expressions,
 * serialized as the host likes: sm3 keeps the bytes and hands them
 * to the catalog's evaluator, which decides whether a row belongs
 * in the index and computes each key column that is an expression,
 * one with attnum -1. reads lists the columns the predicate and
 * the expressions read, so an update that changes none of them can
 * leave the index alone. An empty eval is a plain index, which
 * then may have neither. Fails with SM_ERR_INVALID if the catalog
 * has no evaluator.
 */
int create_index(session_t* session, uint64_t table,
                 std::string_view name,
                 std::span<const index_key_spec_t> keys,
                 std::span<const int32_t> includes, bool unique,
                 std::string_view eval, std::span<const int32_t> reads,
                 uint64_t& oid);

/*
 * The host's evaluator of partial and expression indexes
 * (SM3_DESIGN.md §3). Given a table, one of its indexes that has
 * an eval, and a row image as its values and null flags, one per
 * attribute of the table's schema, it sets belongs to whether the
 * row is in the index, and for each key column that is an
 * expression, in key order, sets the value in out to the value's
 * bytes as sm2::datum_bytes gives them -- a varlena for a
 * variable-width type -- or its flag in out_isnull. out and
 * out_isnull come sized to the count of expression columns. A
 * by-reference value of the row points at memory good only for
 * the call. It returns 0, or -1 with the error set (sm1::seterr).
 *
 * It must not throw: it fails by returning -1. A std::function's
 * type cannot say noexcept, as sm.h's evaluator does, so sm3 calls
 * it from a function that is, and one that throws ends the
 * process. It must give the same answer
 * for the same row image every time; it may be called from any
 * thread, the build's workers among them; and it must not call the
 * storage manager on the session that is writing.
 */
using index_eval_fn = std::function<int(
    const table_def_t& table, const index_def_t& index,
    std::span<const sm2::datum_t> values, std::span<const bool> isnull,
    bool& belongs, std::vector<std::string>& out,
    std::span<bool> out_isnull)>;

/*
 * Give the catalog its evaluator: once, before any session uses
 * the catalog and before its builder is started.
 */
int cat_set_evaluator(catalog_t* cat, index_eval_fn eval);

/*
 * Drop the index oid of the table: its name, its place in the
 * table's definition, and its entries.
 */
int drop_index(session_t* session, uint64_t table, uint64_t oid);

/*
 * Queue every index of cat that is pending, for the builder. An
 * index is queued by its create_index once that has committed, in
 * a function that a crash loses (sm1::on_commit); this finds what
 * was lost, and is called when a database is opened, after its
 * catalog is attached to the builder. One queued already is not
 * queued twice.
 */
int enqueue_pending_indexes(catalog_t* cat);

/*
 * Set the state of the table's index oid, which must be pending,
 * as the build does when it ends: valid, or failed. A failed index
 * keeps why, error and reason. It is cat_put_index_state,
 * and like it does not wait for the table's readers or writers.
 */
int set_index_state(session_t* session, uint64_t table, uint64_t oid,
                    index_state_t state,
                    sm1::errnum_t error = sm1::SM_OK,
                    std::string_view reason = {});

/*
 * Create a collation named name in namespace nspoid for locale,
 * recording the ICU version it opens at; its oid is set in oid. A
 * locale ICU has no data for fails as sm2::collation_open does.
 */
int create_collation(session_t* session, uint64_t nspoid,
                     std::string_view name, std::string_view locale,
                     bool deterministic, uint64_t& oid);

/*
 * ---- tables: a row and its index entries (SM3_DESIGN.md §3) ----
 *
 * An open table: its definition under the table-oid lock, its heap,
 * and its indexes. It is the one way to write a row, and each
 * write keeps every index of the table in step with the heap.
 */
struct table_t;

/*
 * Open the table oid in session's transaction, taking its schema
 * lock in mode as cat_lock_table does: intent_shared to read its
 * rows, intent_exclusive to write them. The handle is good for
 * this transaction only, the lock ending with it; close it with
 * tbl_close, before or after the transaction ends.
 */
int tbl_open(session_t* session, uint64_t oid, sm1::lockmode_t mode,
             table_t*& out);

/* Close a table from tbl_open; nullptr is a no-op. */
void tbl_close(table_t* table);

/* The definition the table was opened with. */
const table_def_t& tbl_def(const table_t* table);

/* The session the table was opened in. */
session_t* tbl_session(const table_t* table);

/*
 * Whether the table's index oid may be scanned now by the table's
 * session (SM3_DESIGN.md §3). A valid index may not be, if it
 * became valid after the session's snapshot: its entries are
 * versioned as rows are, and that snapshot would see it partly
 * filled. A pending or a failed index never may. An index the
 * session's own transaction made valid always may. Writes maintain
 * an index whether it is usable or not.
 */
bool tbl_index_usable(const table_t* table, uint64_t index);

/*
 * The state of the table's index oid as the handle has it: as its
 * session saw it when the table was opened, which is what its
 * writes go by. No such index is failed.
 */
index_state_t tbl_index_state(const table_t* table, uint64_t index);

/*
 * The writes. Each is the heap write, then a write or two to every
 * index it changes, and sets no savepoint of its own: one that
 * fails after its first step leaves the earlier steps done, the
 * heap and the indexes disagreeing inside the transaction, and the
 * caller must roll back to a savepoint of its own, or abort,
 * before going on. SM_ERR_UPDATED is the exception: it comes from
 * the heap write, which is the first step, so nothing is half done
 * and the transaction goes on as sm1 says (SM1_DESIGN.md §5.4).
 * A table opened in a mode that does not cover intent_exclusive
 * fails with SM_ERR_INVALID, and a row with a NULL in a NOT NULL
 * column with SM_ERR_NOTNULL; both before anything is written.
 */

/*
 * Insert a row of values and null flags, one per attribute of the
 * table's schema; rowno is its id. Every index gets its entry.
 */
int tbl_insert(table_t* table, std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, uint64_t& rowno);

/*
 * Replace the row at rowno. An index is left alone if the row's
 * key columns compare equal before and after, by their types' cmp,
 * and its INCLUDE columns hold the same bytes; otherwise its old
 * entry is removed and a new one added. While an index is pending
 * an old entry that is not there is no error: the build has not
 * reached the row. A failed index is left alone by every write. The old entry is computed
 * from the image the heap write replaced, which at read committed
 * is the newest, not the one the caller read. old, if not null,
 * gets that image.
 */
int tbl_update(table_t* table, uint64_t rowno,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, std::string* old = nullptr);

/*
 * Delete the row at rowno, and its entry in every index. old, if
 * not null, gets the image deleted.
 */
int tbl_delete(table_t* table, uint64_t rowno, std::string* old = nullptr);

/* Read the row at rowno, as sm2::rel_read does. */
int tbl_read(table_t* table, uint64_t rowno, std::string_view& tuple);

/*
 * Read the newest committed version of the row at rowno, as
 * sm2::rel_read_latest does: what a read committed writer that met
 * SM_ERR_UPDATED checks its filter against, and what lets sm1 take
 * its write again.
 */
int tbl_read_latest(table_t* table, uint64_t rowno, std::string_view& tuple);

/*
 * Lock the row at rowno in mode without writing it, as
 * sm2::rel_lock does, to the end of the transaction: shared, as a
 * foreign key check locks the parent row and SELECT ... FOR SHARE
 * a row it read, which other readers of the key may share and a
 * writer waits for; or exclusive, as SELECT ... FOR UPDATE. The
 * row need not exist. It waits as a write does. A mode other than
 * shared needs the table opened to write, SM_ERR_INVALID if not.
 * Above read committed, a row committed since the snapshot fails
 * with SM_ERR_CONFLICT, as sm1::lock says.
 */
int tbl_lock(table_t* table, uint64_t rowno, sm1::lockmode_t mode);

/*
 * Empty the table: its rows and every index's entries, in the
 * table's transaction (truncate_table). The table must be
 * opened exclusive, SM_ERR_INVALID if not. The handle stays open
 * on the emptied table.
 */
int tbl_truncate(table_t* table);

/* Open a cursor over the rows first..last, as sm2::rel_scan does. */
int tbl_scan(table_t* table, uint64_t first, uint64_t last,
             sm2::rel_scan_t*& out);

/*
 * Open a cursor over the entries of the table's index oid between
 * lo and hi, as sm2::idx_scan does; SM_ERR_NOTFOUND if the table
 * has no such index, SM_ERR_INVALID if it is not usable
 * (tbl_index_usable).
 */
int tbl_idx_scan(table_t* table, uint64_t index, const sm2::bound_t* lo,
                 const sm2::bound_t* hi, sm2::index_scan_t*& out,
                 bool* widened = nullptr);

/*
 * What the table's LSM looks like now (SM3_DESIGN.md §6.2, §6.3),
 * read live, through whichever store the handle's definition
 * names, so a transaction that truncated the table sizes the new
 * one. None reads a row.
 *
 * tbl_size sets the heap's bytes, and tbl_idx_size the index
 * oid's, as sm2::relid_size gives them: in sorted files and in
 * memtables, apart (SM_ERR_NOTFOUND if the table has no such
 * index). tbl_rowno_limit sets N, the last rowno handed out, 0
 * if none, as sm1::lastseq gives it. tbl_probes sets the sorted
 * runs a lookup in the table's store may visit, as sm1::probes.
 */
int tbl_size(table_t* table, uint64_t& disk, uint64_t& mem);
int tbl_idx_size(table_t* table, uint64_t index, uint64_t& disk,
                 uint64_t& mem);
int tbl_rowno_limit(table_t* table, uint64_t& n);
int tbl_probes(table_t* table, uint64_t& n);

/*
 * One batch of the build of the table's index oid, which must be
 * pending (SM3_DESIGN.md §3): cover up to max_rows rows from
 * rowno from on. For each it takes the row's lock shared, reads
 * the row's newest committed version, and adds the row's entry if
 * it is not there. next is set to the rowno to go on from, and
 * done once the end of the heap was reached. The caller commits
 * the batch, and marks the index valid after the last.
 *
 * The table is opened to write, in a READ COMMITTED transaction.
 * At another level only a table this transaction made, which no
 * other can see, may be built; no lock is taken on its rows.
 *
 * Errors: SM_ERR_DUPKEY when a unique index finds two rows with
 * one key, which fails the build; SM_ERR_INVALID if the index is
 * not pending or the level does not allow it; SM_ERR_NOTFOUND if
 * the table has no such index. Like a write, a batch that fails
 * partway is the caller's to roll back.
 */
int tbl_index_backfill(table_t* table, uint64_t index, uint64_t from,
                       size_t max_rows, uint64_t& next, bool& done);

/*
 * The schema of an entry's value in the table's index oid, as
 * sm2::idx_value_schema gives it; empty if the table has no such
 * index.
 */
std::span<const sm2::attribute_t> tbl_idx_value_schema(const table_t* table,
                                                       uint64_t index);

/*
 * ---- the builder: pending indexes, filled (SM3_DESIGN.md §3) ----
 *
 * One per process: a queue of pending indexes and the workers
 * that fill them. It is no part of the catalog, which it reads
 * and changes as any caller does. create_index puts an index
 * it leaves pending on the queue once its transaction has
 * committed (sm1::on_commit).
 */

/*
 * Attach cat to the builder: from now on the queue takes cat's
 * indexes. The builder does not look for work. It is given a
 * table and an index, by builder_enqueue; a worker takes them
 * from the queue, checks the index is still pending, and fills
 * it in batches of batch_rows rows, each a
 * transaction, on an sm1 session of its own from open, which
 * returns one on the catalog's database or nullptr, and which the
 * worker closes with sm1::closedb. Then it marks the index valid;
 * or failed, with the error kept in its record, if a batch met an
 * error that is not retryable. Fails with SM_ERR_INVALID if cat is
 * attached already.
 */
int builder_attach(catalog_t* cat, std::function<sm1::session_t*()> open,
                   size_t batch_rows = 1000);

/*
 * Detach cat from the builder: what is queued for it is dropped,
 * and a worker building one of its indexes ends after the batch
 * it is in and closes its session. A build cut short leaves its
 * index pending, for the next builder_attach to queue. A worker
 * waiting on another transaction is interrupted (sm_sys.h), so the
 * detach does not wait for that transaction. It must be
 * called before cat_close, and before the database is closed
 * under the workers' sessions; a catalog not attached is a no-op.
 */
void builder_detach(catalog_t* cat);

/*
 * Put the table's pending index on the queue, starting a worker
 * if fewer than the most allowed are alive. One queued already,
 * or of a catalog not attached, is left alone, and so is one that
 * cannot be queued for want of memory: it stays pending, for the
 * next builder_attach to find. It does not throw.
 */
void builder_enqueue(catalog_t* cat, uint64_t table, uint64_t index);

/*
 * The most builds that run at a time, over every catalog; 4
 * unless set. A worker is a thread, started when a build is
 * queued and ended when it finds the queue empty.
 */
void builder_set_max_workers(size_t max_workers);

/* Whether a worker is filling cat's index oid now. */
bool builder_building(const catalog_t* cat, uint64_t oid);

/*
 * Wait until the build of the table's index oid has ended, for a
 * caller that wants to use the index at once: 0 once it is valid.
 * It waits on the index's state, so any session may wait, for a
 * build any other began. Called outside a transaction, so that
 * the caller's next snapshot is new enough to scan the index.
 *
 * Errors: if the build failed, the error it failed with, code and
 * message, as the index's record keeps them -- SM_ERR_DUPKEY on
 * two rows with one key in a unique index, SM_ERR_KEYTOOLONG on a
 * key too long for one; SM_ERR_INVALID if the index is pending
 * and no build of it is queued or running; SM_ERR_INTERRUPTED if
 * interrupted; SM_ERR_INTRANS inside a transaction;
 * SM_ERR_NOTFOUND if there is no such table or index.
 *
 * An interrupt to the calling thread, a bit in its wait mask
 * (sm_sys.h), ends the wait; the bit stays pending, for the caller
 * to take.
 */
int builder_wait(session_t* session, uint64_t table, uint64_t oid);

} // namespace sm3

#endif /* SM3_H */

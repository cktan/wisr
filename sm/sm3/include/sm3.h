/*
 * sm3.h -- level 3: schema and index maintenance (SM3_DESIGN.md).
 *
 * So far, the catalog (CATALOG_INMEM.md): made in a database with
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
#include "sm_stats.h"

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
 * The last oid handed out: PostgreSQL's Oid is 32 bits, and digres
 * takes an oid as one, so a larger one would wrap onto a built-in
 * oid. cat_new_oid fails past it.
 */
constexpr uint64_t last_oid = UINT32_MAX;

/*
 * The namespace oid of a session's temporary tables (TEMPTABLE.md
 * §4): no namespace at all. sm3 keeps no temporary namespace, and
 * its host names its own. A temporary table's definition has this
 * for its namespace, and the lookups by name take it to mean the
 * session's own temporary tables and their indexes.
 */
constexpr uint64_t nsp_temp = 0;

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
    /* The host's default for the column, opaque to sm: kept, not run. */
    std::optional<std::string> default_expr = std::nullopt;
    /*
     * An identity column's kind, the host's, 'a' (ALWAYS) or 'd' (BY
     * DEFAULT), or 0 for none; and the sequence its values come from.
     */
    char        identity = 0;
    uint64_t    identity_seq = 0;
    /*
     * The host's update default, opaque to sm: what an UPDATE that
     * does not assign the column writes in it. Kept, not run.
     */
    std::optional<std::string> on_update_expr = std::nullopt;
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
    uint8_t                 type_code = 0;  /* expression's (sm2/TYPE.md §3) */
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
 * and message, SM_OK and empty unless failed, and for a duplicate
 * key a row that holds it (sm1::errdup()). It is no part of the
 * table's record or of its definition: it is kept by index, and
 * changes without the table being changed (cat_put_index_state).
 */
struct index_status_t {
  index_state_t state = index_state_t::pending;
  sm1::errnum_t error = sm1::SM_OK;
  std::string reason;
  sm1::dupkey_t dup;
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
 * index, by its oid (CATALOG_ONDISK.md §2); a put writes only the ones
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
  std::optional<std::string> default_expr; /* the host's, opaque */
  char identity = 0;         /* 'a', 'd', or 0: column_spec_t's */
  uint64_t identity_seq = 0; /* its sequence, with an identity */
  std::optional<std::string> on_update_expr; /* the host's, opaque */
  bool operator==(const column_rec_t&) const = default;
};

/* One key column of an index; attnum is the column's 0-based place. */
struct index_key_rec_t {
  int32_t attnum;
  bool desc;
  bool nulls_first;
  std::optional<uint64_t> collation;
  uint8_t type_code = 0; /* an expression's, attnum -1 (sm2/TYPE.md §3) */
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
  bool primary = false;             /* the table's primary key */
  bool constraint = false;          /* a UNIQUE or PRIMARY KEY's */
  bool tree = false;                /* a B+tree of its own pages */
  bool operator==(const index_rec_t&) const = default;
};

/*
 * A foreign key of a table, its child (FOREIGNKEY.md at the top
 * level): the parent table, the unique index of the parent whose
 * keys are ref_attnums, and the column pairs. The actions, the
 * match type and deferrable are kept for the host, which checks the
 * rows, and acted on by no level; host is the host's blob.
 */
struct fkey_rec_t {
  uint64_t oid;
  std::string name;
  uint64_t ref_table;
  uint64_t ref_index;
  std::vector<int32_t> attnums;     /* the child's, 0-based */
  std::vector<int32_t> ref_attnums; /* the parent's, pair by pair */
  char on_update = 'a';             /* 'a','r','c','n','d' */
  char on_delete = 'a';
  char match = 's'; /* 's' simple, 'f' full */
  bool deferrable = false;
  bool deferred = false;
  bool validated = true; /* the rows there were checked */
  std::string host;
  bool operator==(const fkey_rec_t &) const = default;
};

/* A foreign key that references a table: its child, and its oid. */
struct fkey_ref_t {
  uint64_t table;
  uint64_t fkey;
  bool operator==(const fkey_ref_t &) const = default;
};

/*
 * A table, its columns, its indexes and its foreign keys: an entry
 * each. referenced_by is derived, never stored: the foreign keys of
 * the tables, this one among them, that reference it, which loading
 * the catalog and the DDL on a foreign key keep up (FOREIGNKEY.md
 * §4).
 */
struct table_rec_t {
  uint64_t nspoid;
  std::string name;
  uint64_t storeid;
  std::string id_seq;
  uint16_t next_relid;
  std::vector<column_rec_t> columns;
  std::vector<index_rec_t> indexes;
  std::vector<fkey_rec_t> fkeys;
  std::vector<fkey_ref_t> referenced_by;
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
    std::optional<std::string> default_expr; /* the host's, opaque */
    char                       identity = 0;     /* column_spec_t's */
    uint64_t                   identity_seq = 0; /* its sequence */
    std::optional<std::string> on_update_expr; /* the host's, opaque */
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
    /* Its entries' value laid out, once, for every open of it. */
    std::shared_ptr<const sm2::idx_layout_t> layout;
    /*
     * A partial or an expression index (SM3_DESIGN.md §3): eval is
     * its predicate and its expressions as the host serialized
     * them, bytes sm3 does not read, empty for a plain index; reads
     * is the columns they read, 0-based.
     */
    std::string                       eval;
    std::vector<int32_t>              reads;
    bool                              primary = false; /* the table's primary key */
    bool                              constraint = false; /* a UNIQUE or PRIMARY KEY's */
    /* A B+tree of its own pages (../BTREE.md), not sm1's entries. */
    bool                              tree = false;
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
    std::string                   id_seq;
    std::string                   toast_seq; /* the sequence of its TOAST ids */
    uint16_t                      next_relid;
    std::vector<column_def_t>     columns;
    std::vector<sm2::attribute_t> schema;
    sm2::tup_desc_t               desc;    /* over schema */
    std::vector<index_def_t>      indexes;
    table_rec_t                   rec;     /* the record it was built from */
};

/*
 * A relation a view reads (VIEW.md §3): a table's column, by its
 * place, or -1 for the relation as a whole -- a view, or a table
 * read with no column named.
 */
struct view_read_t {
    uint64_t relation;
    int32_t  attnum;

    bool operator==(const view_read_t&) const = default;
};

/*
 * A view (VIEW.md §3): its namespace and name, the host's query and
 * options, kept and never read, and what the query reads.
 */
struct view_def_t {
    uint64_t                 nspoid = 0;
    std::string              name;
    std::string              query;
    std::string              options;
    std::vector<view_read_t> reads;
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

/*
 * The committed table whose rows are in store storeid, or nullptr:
 * a store a TRUNCATE not yet committed made, or a temporary
 * table's, is no committed table's. For a lock's table
 * (sm1::lock_status).
 */
std::shared_ptr<const table_def_t> cat_get_table_by_store(const catalog_t* cat,
                                                      uint64_t storeid);

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

/*
 * Advance the running statement's mark, as sm1::stmt_advance
 * does. Its snapshot stays, and so does its view of the catalog.
 */
int stmt_advance(session_t* s);
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
 * Whether there is a namespace oid, as s's transaction sees it; its
 * name is set in name.
 */
bool cat_get_namespace_name(const session_t* s, uint64_t oid,
                            std::string& name);

/*
 * The index named name in nspoid, as s's transaction sees it:
 * true with its table's oid and its own set. The catalog keeps
 * the committed indexes' names, so it reads no definition but
 * those of the tables s's transaction changed.
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
 * sm1::lock_oid does. With fresh, it is set to whether the shared
 * catalog has no definition of it: s's transaction made it, or it
 * is s's temporary table, and no other transaction sees its rows.
 */
int cat_lock_table(session_t* s, uint64_t oid, sm1::lockmode_t mode,
                   std::shared_ptr<const table_def_t>& out,
                   bool* fresh = nullptr);

/* cat_table_version while s's transaction has changed the table. */
constexpr uint64_t table_version_own = UINT64_MAX;

/*
 * The version of the table oid as s sees it: the count of the
 * catalog's publish that last changed its definition or its
 * statistics, or made one of its indexes valid, 0 if none has since
 * the catalog was loaded; table_version_own while s's transaction
 * has changed its definition or its statistics and not committed. A
 * host that caches what it learns of a table, under the table-oid
 * lock, knows it is still good while the version is the same; one
 * read under the lock IS or IX changes only by an analyze or an
 * index made valid, which DDL's X does not guard.
 */
uint64_t cat_table_version(const session_t* s, uint64_t oid);

/*
 * The namespaces' epoch: the count of the catalog's publish that last
 * made or dropped a namespace, 0 if none has since it was loaded. No
 * lock guards a namespace's lookup by name, so a host that caches
 * namespaces asks this.
 */
uint64_t cat_namespace_epoch(const session_t* s);

/* cat_epoch while s's transaction has DDL of its own. */
constexpr uint64_t epoch_own = UINT64_MAX;

/*
 * The catalog's epoch as s sees it: the count of the catalog's
 * publishes -- every commit of shared DDL, an analyze's statistics,
 * an index made valid -- plus the count of changes to s's temporary
 * catalog, so it moves whenever what s can look up does; epoch_own
 * while s's transaction has DDL of its own, which no epoch dates. A
 * host that caches what it looks up reads it before each lookup, and
 * keeps an answer while it reads the same (../dg/SCHEMACACHE.md).
 */
uint64_t cat_epoch(const session_t* s);

/*
 * As cat_lock_table, but never waiting (sm1::trylock_oid): 0 with the
 * lock held and out set; 1, with nothing taken, if another
 * transaction holds the lock in a mode that conflicts; or -1.
 */
int cat_trylock_table(session_t* s, uint64_t oid, sm1::lockmode_t mode,
                      std::shared_ptr<const table_def_t>& out,
                      bool* fresh = nullptr);

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

/*
 * Take the next oid; one is never handed out twice. Past last_oid
 * it fails with SM_ERR_INVALID: the oids are used up.
 */
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
 * Erase the namespace namespace_oid. One that still holds a table,
 * a collation, a sequence or a view is refused with SM_ERR_INUSE, and the two built
 * in, pg_catalog and public, with SM_ERR_INVALID. It waits for
 * every transaction that is putting something in the namespace,
 * and one that tries after it has committed finds no namespace
 * (CATALOG_INMEM.md §2).
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
 * Store a sequence named name in namespace_oid as sequence_oid,
 * which must be new, and make its counter, the sm1 sequence that
 * hands out start, start + increment, ... A taken name, a table's
 * or an index's among them, fails with SM_ERR_DUPKEY; no such
 * namespace with SM_ERR_NOTFOUND.
 */
int cat_put_sequence(session_t* s, uint64_t sequence_oid,
                     uint64_t namespace_oid, std::string_view name,
                     uint64_t start, uint64_t increment);

/*
 * Erase the sequence sequence_oid, with its name and its counter.
 * One that is not there fails with SM_ERR_NOTFOUND.
 */
int cat_del_sequence(session_t* s, uint64_t sequence_oid);

/*
 * The sequence named name in nspoid, as a table is found by its
 * name: s's own creates and drops, then every committed one, from
 * memory, not as of the transaction's snapshot -- 0 with its oid
 * set in oid, 1 if there is none, or -1.
 */
int cat_get_sequence(session_t* s, uint64_t nspoid, std::string_view name,
                     uint64_t& oid);

/*
 * The namespace and name of the sequence oid, as cat_get_sequence
 * finds a sequence by them: 0 with them set, 1 if there is no such
 * sequence -- an oid of another kind among them -- or -1.
 */
int cat_get_sequence_name(session_t* s, uint64_t oid, uint64_t& nspoid,
                          std::string& name);

/* A sequence as CREATE SEQUENCE and OWNED BY left it. */
struct sequence_def_t {
    uint64_t                nspoid;
    std::string             name;
    uint64_t                start;
    uint64_t                increment;
    std::optional<uint64_t> owner;            /* the table owning it */
    int32_t                 owner_attnum = -1; /* its column, 0-based */
};

/* As cat_get_sequence_name, the whole of the sequence oid, into def. */
int cat_get_sequence_def(session_t* s, uint64_t oid, sequence_def_t& def);

/*
 * The names of the sequences in nspoid, sorted, into names, which it
 * replaces: those cat_get_sequence finds, the committed ones with the
 * transaction's own creates and drops. s needs a transaction open.
 */
int cat_list_sequences(session_t* s, uint64_t nspoid,
                       std::vector<std::string>& names);

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
 * in session's transaction: its store, its _id sequence, its
 * name and its definition. Its oid is set in oid. A name already
 * taken fails with SM_ERR_DUPKEY; an unknown namespace with
 * SM_ERR_NOTFOUND; a type code sm2 does not have, or a typmod
 * its type refuses, with SM_ERR_INVALID.
 *
 * An identity column is a NOT NULL smallint, integer or bigint
 * with no default, whose sequence is in nspoid and owned by no
 * column; the column then owns it (own_sequence), for good. Any
 * other is SM_ERR_INVALID.
 *
 * The session's transaction sees it at once; others once it
 * commits.
 */
int create_table(session_t* session, uint64_t nspoid, std::string_view name,
                 std::span<const column_spec_t> columns, uint64_t& oid);

/*
 * Create a temporary table named name (TEMPTABLE.md §4), as
 * create_table does but in no namespace: its store is a temporary
 * store, its _id sequence a temporary sequence, and its
 * definition is kept in the session's memory alone. No other
 * session sees it, and it ends with its session. A name another
 * of the session's temporary tables or their indexes has fails
 * with SM_ERR_DUPKEY. It is found by name with nsp_temp. An
 * identity column is SM_ERR_INVALID: a sequence is never
 * temporary.
 */
int create_temp_table(session_t* session, std::string_view name,
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
 * definition, its store with its rows and index entries, its
 * _id sequence, and the sequences its columns own. One a view
 * reads is refused with SM_ERR_INUSE.
 */
int drop_table(session_t* session, uint64_t oid);

/*
 * Empty the table oid: it gets a new, empty store, and the old one
 * is dropped, rows and index entries with it. The columns, the
 * indexes and their states, and the _id sequence are kept, so
 * new rows go on from the last _id handed out. Transactional,
 * and the same cost whatever the table holds. It takes the schema
 * lock exclusive, as other DDL does. A transaction that began
 * before it committed and first reads the table after finds it
 * empty, as in PostgreSQL.
 */
int truncate_table(session_t* session, uint64_t oid);

/* Rename the table oid to name, in its namespace. */
int rename_table(session_t* session, uint64_t oid,
                 std::string_view name);

/*
 * Append col to the table oid. A row written before it reads it as
 * missing -- its bytes as sm2 holds them, a varlena for a
 * variable-width type -- or as null without one. So a NOT NULL
 * column without one needs a table with no row: its table-oid lock
 * X is taken if it is free now, and kept only if the table's store
 * is empty, SM_ERR_BLOCKED if it is not free and SM_ERR_NOTNULL if
 * the store holds a version of any kind, a deleted row's too, until
 * compaction. A live column's name is SM_ERR_DUPKEY, and an
 * identity column SM_ERR_INVALID.
 */
int add_column(session_t* session, uint64_t oid,
               const column_spec_t& col,
               std::optional<std::string_view> missing);

/*
 * Make the live column name of the table oid NOT NULL. Every row
 * the transaction sees must hold a value in it, or it fails with
 * SM_ERR_NOTNULL and changes nothing: the table is opened
 * exclusive (tbl_open), so no row is written meanwhile, and a
 * scan reads each. One NOT NULL already is no error. ADD COLUMN
 * with a volatile default makes the column, gives each row its
 * value, and then calls this.
 */
int set_not_null(session_t* session, uint64_t oid, std::string_view name);

/*
 * A column's new type, as alter_column_type takes it: sm2's code
 * and typmod, and the host's default and update default for the
 * new type, opaque to sm, which replace the column's.
 */
struct column_type_t {
    uint8_t                    type_code;  /* sm2/TYPE.md §3 */
    int32_t                    typmod;
    std::optional<std::string> default_expr = std::nullopt;
    std::optional<std::string> on_update_expr = std::nullopt;
};

/*
 * The host's conversion of one row's value to a column's new type.
 * Given the row as values and null flags, one per column of the
 * table's schema before the change, it puts the new value's bytes
 * in out, as sm2::datum_bytes gives them -- a varlena for a
 * variable-width type -- or sets out_null. A by-reference value of
 * the row points at memory good only for the call. It returns 0,
 * or -1 with the error set (sm1::seterr). It runs on the calling
 * session's thread, inside alter_column_type, and must not call
 * the storage manager on that session.
 */
using column_conv_fn = std::function<int(std::span<const sm2::datum_t> values,
                                         std::span<const bool> isnull,
                                         std::string& out, bool& out_null)>;

/*
 * Change the live column name of the table oid to the type to
 * (../dg/ALTERTYPE.md at the top level), under the table-oid lock
 * exclusive.
 *
 * With conv, a table with rows is rewritten into a new store, as
 * rewrite_table does: each row's value of the column is conv's, the
 * others are kept, and every index on the column is made under the
 * new type, the primary key among them. Two rows with one key fail
 * with SM_ERR_DUPKEY, as a rewrite does; a null in a NOT NULL
 * column with SM_ERR_NOTNULL; conv's failure is the call's.
 *
 * Without conv, only the column's record changes: taken only when
 * every stored value and key stays as it is, the same type code,
 * or text and varchar; else SM_ERR_INVALID. The host decides that
 * the values need no conversion.
 *
 * Refused with SM_ERR_INUSE, changing nothing: a column an
 * expression or a partial index reads, one a foreign key of the
 * table names, one a foreign key that references the table names,
 * one a view reads. An identity column is SM_ERR_INVALID. The
 * column's statistics go.
 */
int alter_column_type(session_t* session, uint64_t oid, std::string_view name,
                      const column_type_t& to, const column_conv_fn* conv);

/*
 * Make the live column name of the table oid an identity column,
 * GENERATED ALWAYS ('a') or BY DEFAULT ('d'), whose values come
 * from the sequence seq, which it then owns for good, as
 * create_table makes one. The column must be a NOT NULL
 * smallint, integer or bigint with no default, the sequence in
 * the table's namespace and owned by none, and the table not
 * temporary, or SM_ERR_INVALID. ADD COLUMN of an identity column
 * makes it a plain one, gives each row a value from seq, makes it
 * NOT NULL, and then calls this.
 */
int set_identity(session_t* session, uint64_t oid, std::string_view name,
                 char identity, uint64_t seq);

/*
 * Rename the live column name of the table oid to new_name. Its
 * indexes name it by place, so they are untouched. One another live
 * column has is SM_ERR_DUPKEY.
 */
int rename_column(session_t* session, uint64_t oid,
                  std::string_view name, std::string_view new_name);

/*
 * Drop the column name of the table oid. It keeps its place, read
 * as null from then on (sm2/SM2_DESIGN.md §4); one an index uses
 * is refused, and one a view reads with SM_ERR_INUSE. The
 * sequences it owns are dropped with it.
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
 * Drop the namespace oid. One that still holds a table, a
 * collation, a sequence or a view is refused with SM_ERR_INUSE, and the two built in,
 * pg_catalog and public, with SM_ERR_INVALID. It waits for every
 * transaction that is creating something in the namespace, and
 * one that tries after it has committed finds no namespace
 * (CATALOG_INMEM.md §2).
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
 *
 * A primary one is the table's primary key: it must be unique, on
 * columns alone, every one NOT NULL, refused past
 * sm2::max_pk_len, and the table's only one, or it is
 * SM_ERR_INVALID. The table's rows are keyed by it, and are its
 * entries (../CLUSTER.md §2): it is made valid at once, on a table
 * this transaction made with no row yet; on any other the table is
 * rewritten into a new store under its table-oid lock, exclusive,
 * which drop_index of it does too (../CLUSTER.md §8). Two rows
 * with one key fail it with SM_ERR_DUPKEY. A constraint one is a
 * UNIQUE or PRIMARY KEY constraint's, another mark the host reads:
 * it must be unique and on columns alone.
 */
int create_index(session_t* session, uint64_t table,
                 std::string_view name,
                 std::span<const index_key_spec_t> keys,
                 std::span<const int32_t> includes, bool unique,
                 uint64_t& oid, bool primary = false,
                 bool constraint = false, bool tree = false);

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
                 uint64_t& oid, bool primary = false,
                 bool constraint = false, bool tree = false);

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
 * Say whether the catalog's host reads TOAST pointers
 * (sm2/SM2_DESIGN.md §4.2): once, before any session uses the
 * catalog. On, a table's long datums are stored out of line: a
 * tuple a table hands out may hold a pointer where a datum was,
 * which sm2::varlena_is_toast tells and tbl_fetch reads. Off, the
 * default, every datum stays in its tuple and a row is held to
 * sm2::max_tup_len, as before TOAST. A host does not turn it off
 * again over tables it wrote with it on.
 */
int cat_set_toast(catalog_t* cat, bool on);

/*
 * Drop the index oid of the table: its name, its place in the
 * table's definition, and its entries.
 */
int drop_index(session_t* session, uint64_t table, uint64_t oid);

/*
 * Add the foreign key def to the table, its child (FOREIGNKEY.md §5),
 * under the child's lock X and then the parent's, def.ref_table; its
 * oid, def.oid ignored, is set in oid. It needs 1 to 32 column pairs,
 * every column live, def.ref_index a valid unique index of the parent
 * with no predicate or expression whose keys are def.ref_attnums in
 * any order, both tables temporary or neither, and a name free among
 * the child's foreign keys, else SM_ERR_INVALID, or SM_ERR_DUPKEY for
 * the name. No row is read: def.validated is the caller's word.
 */
int create_fkey(session_t* session, uint64_t table, const fkey_rec_t& def,
                uint64_t& oid);

/* Drop the foreign key oid of the table, under both tables' locks X. */
int drop_fkey(session_t* session, uint64_t table, uint64_t oid);

/* Mark the foreign key oid of the table validated: its rows checked. */
int validate_fkey(session_t* session, uint64_t table, uint64_t oid);

/* Rename the foreign key oid of the table; a taken name is DUPKEY. */
int rename_fkey(session_t* session, uint64_t table, uint64_t oid,
                std::string_view name);

/*
 * The foreign key oid as s sees it, and its table, the child, into
 * table and out: 0, or 1 if there is none. It takes no lock.
 */
int cat_get_fkey(const session_t* s, uint64_t oid, uint64_t& table,
                 fkey_rec_t& out);

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
 * keeps why, error and reason, and dup for a duplicate key. It is
 * cat_put_index_state,
 * and like it does not wait for the table's readers or writers.
 */
int set_index_state(session_t* session, uint64_t table, uint64_t oid,
                    index_state_t state,
                    sm1::errnum_t error = sm1::SM_OK,
                    std::string_view reason = {},
                    const sm1::dupkey_t &dup = {});

/*
 * Create a collation named name in namespace nspoid for locale,
 * recording the ICU version it opens at; its oid is set in oid. A
 * locale ICU has no data for fails as sm2::collation_open does.
 */
int create_collation(session_t* session, uint64_t nspoid,
                     std::string_view name, std::string_view locale,
                     bool deterministic, uint64_t& oid);

/*
 * Create a sequence named name in namespace nspoid, SQL's CREATE
 * SEQUENCE, handing out start, start + increment, ...; its oid is
 * set in oid. It takes the name from the namespace's tables and
 * indexes, as a relation does. An increment of 0, or no name,
 * fails with SM_ERR_INVALID.
 */
int create_sequence(session_t* session, uint64_t nspoid,
                    std::string_view name, uint64_t start,
                    uint64_t increment, uint64_t& oid);

/*
 * Drop the sequence oid, with its counter. One a column owns is
 * refused with SM_ERR_INUSE: it goes with its table or column.
 */
int drop_sequence(session_t* session, uint64_t oid);

/*
 * Make the column attnum of the table table own the sequence oid,
 * SQL's ALTER SEQUENCE ... OWNED BY, so that dropping the table or
 * the column drops it; table 0 makes it owned by none. The table
 * must be in the sequence's namespace, and attnum one of its live
 * columns, or SM_ERR_INVALID. The table is held against a drop to
 * the end of the transaction. An identity column's sequence is its
 * for good: changing its owner is SM_ERR_INUSE.
 */
int own_sequence(session_t* session, uint64_t oid, uint64_t table,
                 int32_t attnum);

/*
 * Hand out the next value of the sequence oid. Values are not
 * transactional: a rollback does not give one back, as sm1's
 * sequences do not (sm1/API.md). One past UINT64_MAX fails with
 * SM_ERR_INVALID, and a sequence that is not there with
 * SM_ERR_NOTFOUND.
 */
int nextval(session_t* session, uint64_t oid, uint64_t& out);

/*
 * ---- views (VIEW.md) ----
 *
 * A view is its record alone: there is nothing outside the catalog
 * to make or drop. Its name is a relation's, taken from the
 * namespace's tables, indexes and sequences. A view has a table's
 * oid lock, on its oid: a query takes it intent_shared, DDL on the
 * view exclusive.
 */

/*
 * Create the view def describes, def's name and namespace being
 * name and nspoid, its oid set in oid. Each relation it reads is
 * locked intent_shared to the end of the transaction, and must be
 * a table, with a live column where one is named, or a view, with
 * none named: or SM_ERR_NOTFOUND, or SM_ERR_INVALID. nspoid nsp_temp
 * makes a temporary view, in the session's temporary catalog
 * (VIEW.md §12.2), which alone may read a temporary table or view,
 * locking none: a permanent one that would is SM_ERR_INVALID. A
 * taken name is SM_ERR_DUPKEY, a temporary view's among the
 * session's temporary names.
 */
int create_view(session_t* session, uint64_t nspoid, std::string_view name,
                const view_def_t& def, uint64_t& oid);

/*
 * Replace the query, options and reads of the view oid with def's,
 * SQL's CREATE OR REPLACE VIEW, under its lock exclusive; its name
 * and namespace stay. The reads are checked and locked as
 * create_view's, and one that reaches the view itself, through
 * other views, is SM_ERR_INVALID.
 */
int replace_view(session_t* session, uint64_t oid, const view_def_t& def);

/* Rename the view oid to name, in its namespace, under its lock X. */
int rename_view(session_t* session, uint64_t oid, std::string_view name);

/*
 * Drop the view oid, under its lock exclusive. One another view
 * reads is SM_ERR_INUSE. Another session's temporary view, an oid
 * cat_view_readers gives as other, takes no lock: at commit it
 * leaves the register, and its session sees it no more, nor its own
 * temporary views that read it, and drops them at its next begin
 * (VIEW.md §12.4).
 */
int drop_view(session_t* session, uint64_t oid);

/*
 * Take the view oid's lock in mode, held to the end of s's
 * transaction, then set out to its definition as s sees it, which
 * cannot change under the lock. A view dropped while s waited, or
 * none, fails with SM_ERR_NOTFOUND. A temporary view takes no lock.
 */
int cat_lock_view(session_t* s, uint64_t oid, sm1::lockmode_t mode,
                  std::shared_ptr<const view_def_t>& out);

/* The view oid as s sees it, with no lock, or nullptr. */
std::shared_ptr<const view_def_t> cat_get_view(const session_t* s,
                                               uint64_t oid);

/* The view named name in nspoid as s sees it: true with its oid set. */
bool cat_get_view(const session_t* s, uint64_t nspoid, std::string_view name,
                  uint64_t& oid);

/*
 * The version of the view oid as s sees it, as cat_table_version's
 * of a table: the count of the publish that last changed it, 0 if
 * none has since the catalog was loaded, table_version_own while
 * s's transaction has changed it.
 */
uint64_t cat_view_version(const session_t* s, uint64_t oid);

/* The names of the views in nspoid as s sees them, sorted. */
std::vector<std::string> cat_list_views(const session_t* s, uint64_t nspoid);

/*
 * A view that reads a relation: its oid and definition, and whether
 * it is another session's temporary view, which s finds nowhere else
 * (VIEW.md §12.3).
 */
struct view_reader_t {
    uint64_t                          oid;
    std::shared_ptr<const view_def_t> def;
    bool                              other;
};

/*
 * The views that read the relation oid, a table or a view, directly,
 * as s sees them, and other sessions' temporary views that do, but
 * those s's transaction has dropped; sorted by oid.
 */
std::vector<view_reader_t> cat_view_readers(const session_t* s, uint64_t oid);

/*
 * ---- tables: a row and its index entries (SM3_DESIGN.md §3) ----
 *
 * An open table: its definition under the table-oid lock, its heap,
 * and its indexes. It is the one way to write a row, and each
 * write keeps every index of the table in step with the heap.
 */
struct table_t;

/*
 * ---- pins: a row kept in memory, deformed as far as it is read
 * (../ROW.md §4) ----
 *
 * A pin keeps one row's bytes where RocksDB has them, read by
 * tbl_read or replaced by a write into a ref, or a cursor's current
 * row (tbl_scannxt), and remembers how far they are deformed. The
 * session owns every pin, in a pool; the caller holds a ref.
 */
struct pin_t;

/*
 * A pin ref: a plain value the caller keeps where it likes,
 * naming a pin and the pin's generation when it was taken. Once
 * the pin is released the ref is stale, and names nothing. A ref
 * of zeros names no pin.
 */
struct pin_ref_t {
    pin_t*   pin = nullptr;
    uint64_t gen = 0;
};

/*
 * Release the pin ref names, if it is still held, and zero ref. A
 * stale ref, or one naming no pin, is left alone; so is a cursor's,
 * which its cursor releases.
 */
void unpin(pin_ref_t& ref);

/*
 * Release every pin the session holds, leaving its transaction
 * open; a cursor's pin goes stale, and stays with its cursor. The
 * end of the transaction does the same: commit, prepare and abort.
 */
void release_all_pins(session_t* s);

/*
 * Attributes from to n - 1 of the row ref pins, into datums and
 * isnull at those places, each indexed by attribute and at least n
 * long. It resumes where the last deform stopped when from is
 * there; otherwise it reads from the row's header again, and may
 * write attributes before from again, with the values they had.
 * Each attribute checks only what it reads (../ROW.md §4.6). A
 * by-reference datum points into the row, and lasts as long as the
 * pin. SM_ERR_INVALID if ref is stale, or n is past the schema.
 */
int pin_deform(pin_ref_t ref, size_t from, size_t n,
               std::span<sm2::datum_t> datums, std::span<bool> isnull);

/*
 * One attribute of the row ref pins, by ordinal, as sm2::tup_decode
 * gives it; SM_ERR_INVALID if ref is stale.
 */
int pin_decode(pin_ref_t ref, size_t ordinal, sm2::datum_t& val,
               bool& isnull);

/* The row's bytes, valid while the pin is; SM_ERR_INVALID if stale. */
int pin_tuple(pin_ref_t ref, std::string_view& tuple);

/*
 * Open the table oid in session's transaction, taking its schema
 * lock in mode as cat_lock_table does: intent_shared to read its
 * rows, intent_exclusive to write them. The handle is good for
 * this transaction only, the lock ending with it; close it with
 * tbl_close, before or after the transaction ends.
 */
int tbl_open(session_t* session, uint64_t oid, sm1::lockmode_t mode,
             table_t*& out);

/*
 * As tbl_open, but never waiting for the table-oid lock: 0 with the
 * table open; 1, with nothing taken and out untouched, if another
 * transaction holds the lock in a mode that conflicts; or -1. SQL's
 * LOCK TABLE ... NOWAIT asks like this.
 */
int tbl_tryopen(session_t* session, uint64_t oid, sm1::lockmode_t mode,
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
 * and the transaction goes on as sm1 says (sm1/SM1_DESIGN.md
 * §5.4).
 * A table opened in a mode that does not cover intent_exclusive
 * fails with SM_ERR_INVALID, and a row with a NULL in a NOT NULL
 * column with SM_ERR_NOTNULL; both before anything is written. A
 * key a unique index holds already is SM_ERR_DUPKEY, the index in
 * sm1::errdup(): the host has the row, to say the key.
 *
 * A row is named by its key (../ROWKEY.md §1). A keyless table's
 * row is named by its _id, which is its key; a table with a primary
 * key names its row by the key's values (row_key_t, below), and its
 * calls by _id fail with SM_ERR_INVALID. A row another transaction
 * moved since, its primary key changed, is not followed
 * (../CLUSTER.md §4, ../ROWKEY.md §2.3): a write at its old key meets
 * SM_ERR_UPDATED, and tbl_read_latest there SM_ERR_MOVED, which the
 * host raises as a serialization failure. Above READ COMMITTED a
 * moved row is SM_ERR_CONFLICT, as any row changed since the
 * snapshot.
 */

/*
 * Insert a row of values and null flags, one per attribute of the
 * table's schema; id is its _id. Every index gets its entry.
 */
int tbl_insert(table_t* table, std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, uint64_t& id);

/*
 * Insert a row as tbl_insert does, unless a row already has its
 * key in one of the unique indexes arbiters names by oid, or in
 * any unique index of the table if arbiters is empty: SQL's
 * INSERT ... ON CONFLICT. Returns 0 with the new row's _id; 1,
 * with nothing written, with the _id of the row that has the
 * key, and its tuple in tuple if wanted, as the call found it,
 * from which the caller takes its key (../ROWKEY.md §3.3); or -1.
 *
 * How it finds the conflict:
 * 1. Look each arbiter's key up (sm2::idx_find, or a tree's lookup).
 *    A row the snapshot shows there is the conflict: return 1, with
 *    nothing locked and nothing written. At READ COMMITTED, where
 *    the snapshot may show a row deleted since, first confirm the
 *    row against its newest version.
 * 2. If an arbiter is a tree, set a savepoint.
 * 3. Put the row's entry into each arbiter that is not a tree, then
 *    write the row, the primary key's entry. sm1's insert waits for
 *    another transaction that has put the key in and not yet ended,
 *    and says at once if a row is there.
 * 4. Put the row's entry into each tree arbiter, after the row
 *    (../../MAJORDECISION.md D5): another's check would call an entry
 *    that names no row dead.
 * 5. If step 3 or 4 meets a row that has the key, take back what
 *    was written: the entries of step 3, or all, to the savepoint.
 *    Read that row under a lock given back at once, and return 1
 *    with it. If it is gone already, start again at step 1.
 * So in neither case is anything of the key left held: the lock on
 * a key is the entry of the row inserted, as in any insert.
 *
 * Above READ COMMITTED a key taken since the snapshot fails the
 * call, and the transaction, with SM_ERR_CONFLICT. A duplicate in
 * a unique index that is not an arbiter fails as tbl_insert's
 * does, with SM_ERR_DUPKEY.
 *
 * The row found is not locked, and may be changed or deleted by
 * another before the caller locks it (tbl_lock): a caller that
 * then finds it gone calls this again.
 */
int tbl_insert_arbiter(table_t* table, std::span<const sm2::datum_t> values,
                       std::span<const bool> isnull,
                       std::span<const uint64_t> arbiters, uint64_t& id,
                       std::string* tuple = nullptr);

/*
 * Replace the row with _id id, in a table without a primary key,
 * whose key the _id is: a table with one names its row by its key
 * (row_key_t, below), and SM_ERR_INVALID here. The calls below by
 * _id are so. An index is left alone if the row's
 * key columns compare equal before and after, by their types' cmp,
 * and its INCLUDE columns hold the same bytes; otherwise its old
 * entry is removed and a new one added. While an index is pending
 * an old entry that is not there is no error: the build has not
 * reached the row. A failed index is left alone by every write. The old entry is computed
 * from the image the heap write replaced, which at read committed
 * is the newest, not the one the caller read. The form with ref
 * hands that image back pinned into ref: valid until the pin is
 * released. The pin ref held before is released only when the
 * write succeeds, so values may point into its row; on failure old
 * is empty, and ref keeps what it held.
 */
int tbl_update(table_t* table, uint64_t id,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull);
int tbl_update(table_t* table, uint64_t id,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, pin_ref_t& ref,
               std::string_view& old);

/*
 * Delete the row with _id id, and its entry in every index. The form
 * with ref hands back the image deleted, pinned into ref, as
 * tbl_update's does.
 */
int tbl_delete(table_t* table, uint64_t id);
int tbl_delete(table_t* table, uint64_t id, pin_ref_t& ref,
               std::string_view& old);

/*
 * A BULK LOAD into an open table (BULKLOAD.md §5, §13): many rows
 * handed over one by one and written to sorted files that the
 * store takes whole, past its log and its compactions.
 *
 * It is for an EMPTY table: one whose store holds no version of
 * any row, as a new table's and a truncated one's do, and a table
 * whose rows were deleted does not. tbl_load_open takes such a
 * table's lock exclusive, to the end of the transaction, so that
 * no one else reads or writes it meanwhile -- if the lock is free
 * at that moment. It does not wait for it: with another
 * transaction using the table, the rows are inserted instead. Every row is checked
 * as tbl_insert checks one, and has its entry made in every index
 * of the table; but a key a unique index already has is found
 * only when the load is closed.
 *
 * A row a load does not take -- one to be stored out of line, or
 * too long for a tuple -- ends it: what was handed over goes into
 * the table at once, and that row and every one after it are
 * inserted as tbl_insert would. A load into a table that is not
 * empty inserts every row that way, and takes no lock beyond the
 * table's own. So a caller with many rows to add opens a load
 * whatever the table holds.
 *
 * While rows are going to files the transaction cannot commit,
 * set a savepoint or roll back to one, the table's other writes
 * are refused, and its reads do not see the load. After a load
 * the transaction cannot be prepared.
 */
struct loader_t;

/* Begin a load into table, opened to write. */
int tbl_load_open(table_t* table, loader_t*& out);

/*
 * Hand over one row; its _id is known at once. It fails as
 * tbl_insert does for what it sees in the row; a duplicate key
 * fails here once rows are being inserted, and at the close
 * otherwise. After a failure the transaction is to be aborted.
 */
int tbl_load_add(loader_t* loader, std::span<const sm2::datum_t> values,
                 std::span<const bool> isnull, uint64_t& id);

/*
 * Finish the load: sort, check and put into the table whatever is
 * still on its way. The loader is freed, even on failure; the
 * transaction is still to commit, or after a failure to abort.
 */
int tbl_load_close(loader_t* loader);

/*
 * Free a loader that will not be closed, its transaction having
 * ended without it: the abort ended the load itself. nullptr is a
 * no-op.
 */
void tbl_load_free(loader_t* loader);

/*
 * Read the datum the TOAST pointer ptr names, a varlena a tuple of
 * the table held, into out: its bytes, with no varlena header
 * (sm2::toast_fetch). A table's tuples hold pointers only when the
 * catalog's host reads them (cat_set_toast). A pointer is to be
 * fetched in the statement that read it; the old tuple a write
 * hands back names datums that write erased, which the
 * transaction's own later statements no longer see.
 */
int tbl_fetch(table_t* table, std::string_view ptr, std::string& out);

/* Read the row with _id id, as sm2::rel_read does. */
int tbl_read(table_t* table, uint64_t id, std::string_view& tuple);

/*
 * Read the row with _id id into a pin, with no copy: 0 with ref naming
 * it and tuple viewing its row, valid until the pin is released, 1
 * if there is none, or -1. A pin ref names still is released
 * first; on 1 or -1 ref is zero. A rollback to a savepoint releases
 * no pin, nor does the drop of the row's store.
 */
int tbl_read(table_t* table, uint64_t id, pin_ref_t& ref,
             std::string_view& tuple);

/*
 * Read the row at key, its key in the table's rel, as an index
 * entry names it (sm2::idx_scan_row), into a pin as tbl_read does.
 */
int tbl_read_key(table_t* table, std::string_view key, pin_ref_t& ref,
                 std::string_view& tuple);

/*
 * tbl_read_key of n keys in one call (../BATCHREAD.md): key i's row
 * into a pin, refs[i] and tuples[i], rc[i] 0, or rc[i] 1 with refs[i]
 * zero if there is none. A ref that holds a pin is read into again,
 * as tbl_read's is. Returns 0, or -1 on an error, every ref then
 * zero.
 */
int tbl_read_keys(table_t* table, const std::string_view* keys, size_t n,
                  pin_ref_t* refs, std::string_view* tuples, int* rc);

/*
 * Read the newest committed version of the row with _id id, as
 * sm2::rel_read_latest does: what a read committed writer that met
 * SM_ERR_UPDATED checks its filter against, and what lets sm1 take
 * its write again.
 */
int tbl_read_latest(table_t* table, uint64_t id, std::string_view& tuple);

/* tbl_read_latest into a pin, as tbl_read into one. */
int tbl_read_latest(table_t* table, uint64_t id, pin_ref_t& ref,
                    std::string_view& tuple);

/*
 * Lock the row with _id id in mode without writing it, as
 * sm2::rel_lock does, to the end of the transaction: shared, as a
 * foreign key check locks the parent row and SELECT ... FOR SHARE
 * a row it read, which other readers of the key may share and a
 * writer waits for; or exclusive, as SELECT ... FOR UPDATE. The
 * row need not exist. It waits as a write does. A mode other than
 * shared needs the table opened to write, SM_ERR_INVALID if not.
 * Above read committed, a row committed since the snapshot fails
 * with SM_ERR_CONFLICT, as sm1::lock_row says. The row is one the
 * running statement read (sm1::lock_row's as_read): one the
 * transaction changed after the statement began is not locked,
 * SM_ERR_SELFMODIFIED, which a FOR UPDATE cursor passes over.
 */
int tbl_lock(table_t* table, uint64_t id, sm1::lockmode_t mode);

/*
 * As tbl_lock, but never waiting (sm1::trylock_row): 0 with the row
 * locked, 1 with nothing taken if another transaction holds it or
 * has written it and not ended, or -1. SQL's NOWAIT and SKIP
 * LOCKED.
 */
int tbl_trylock(table_t* table, uint64_t id, sm1::lockmode_t mode);

/*
 * A row's name (../ROWKEY.md §1): the values of its table's primary
 * key's columns, in the key's order, with their null flags; in a
 * table without one, its _id alone, an int8. Both borrowed for the
 * call. Each call below that takes one is the call of its name by
 * _id, the row named by its key, which sm2::rel_key encodes: a null,
 * or a count that is not the key's, fails with SM_ERR_INVALID. A
 * write follows no row that moved. Until a row of a table with a
 * primary key leaves its _id (../ROWKEY.md §5, step 4), an update or
 * delete of one reads the row first for it, as the snapshot shows
 * it, or at READ COMMITTED the newest version when the snapshot
 * shows none.
 */
struct row_key_t {
    std::span<const sm2::datum_t> values;
    std::span<const bool>         isnull;
};

int tbl_update(table_t* table, const row_key_t& key,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull);
int tbl_update(table_t* table, const row_key_t& key,
               std::span<const sm2::datum_t> values,
               std::span<const bool> isnull, pin_ref_t& ref,
               std::string_view& old);
int tbl_delete(table_t* table, const row_key_t& key);
int tbl_delete(table_t* table, const row_key_t& key, pin_ref_t& ref,
               std::string_view& old);
int tbl_read(table_t* table, const row_key_t& key, std::string_view& tuple);
int tbl_read(table_t* table, const row_key_t& key, pin_ref_t& ref,
             std::string_view& tuple);
int tbl_read_latest(table_t* table, const row_key_t& key,
                    std::string_view& tuple);
int tbl_read_latest(table_t* table, const row_key_t& key, pin_ref_t& ref,
                    std::string_view& tuple);
int tbl_lock(table_t* table, const row_key_t& key, sm1::lockmode_t mode);

/*
 * The key's encoding in the table's rel, past the relid, into out, as
 * a cursor gives a row's (tbl_scan_key): to tell two names apart.
 */
int tbl_row_key(table_t* table, const row_key_t& key, std::string& out);
int tbl_trylock(table_t* table, const row_key_t& key, sm1::lockmode_t mode);

/*
 * Empty the table: its rows and every index's entries, in the
 * table's transaction (truncate_table). The table must be
 * opened exclusive, SM_ERR_INVALID if not. The handle stays open
 * on the emptied table.
 */
int tbl_truncate(table_t* table);

/*
 * A cursor over a table's rows, in _id order (../ROW.md §4.3):
 * an sm2 rel scan, and one pin, which holds the cursor's current
 * row.
 */
struct scan_t;

/*
 * Open a cursor over the rows first..last, as sm2::rel_scan does,
 * taking its pin from the session's pool.
 */
int tbl_scan(table_t* table, uint64_t first, uint64_t last, scan_t*& out);

/*
 * The next row: 0 with its _id, and the row in the cursor's pin, which
 * ref then names and tuple views; 1 at the end; -1 on error. Each
 * call makes every ref to the row before stale; so do the end of the
 * transaction, a rollback that closes the cursor, and tbl_scanend.
 */
int tbl_scannxt(scan_t* scan, uint64_t& id, pin_ref_t& ref,
                std::string_view& tuple);

/* tbl_scannxt for a caller that wants the bytes only. */
int tbl_scannxt(scan_t* scan, uint64_t& id, std::string_view& tuple);

/*
 * Open a cursor over every row of the table, in the order of its
 * key: its primary key's, one sorted run, or with none its _id's
 * (../CLUSTER.md §5). What a sequential scan reads. It seeks no _id
 * in a table with a primary key.
 */
int tbl_scan_rows(table_t* table, scan_t*& out);

/*
 * Open a cursor over the rows of the table's primary key, index,
 * between lo and hi as tbl_idx_scan takes them, in key order: the
 * rows themselves, each in the cursor's pin as tbl_scannxt gives
 * it, one sorted run (../CLUSTER.md §5). SM_ERR_INVALID if index
 * is not the primary key, or not usable; SM_ERR_NOTFOUND if the
 * table has no such index. It seeks no _id.
 */
int tbl_idx_scan_rows(table_t* table, uint64_t index, const sm2::bound_t* lo,
                      const sm2::bound_t* hi, scan_t*& out,
                      bool* widened = nullptr);

/*
 * Aim a tree's cursor of rows (tbl_idx_scan_rows) at the rows between
 * lo and hi, as if opened anew: an IN list's next probe, with no
 * cursor made for it. Refs to its row go stale. SM_ERR_INVALID on any
 * other cursor.
 */
int tbl_scan_rebound(scan_t* scan, const sm2::bound_t* lo,
                     const sm2::bound_t* hi, bool* widened = nullptr);

/*
 * Make a tree's cursor of rows (tbl_idx_scan_rows) an index-only
 * scan's: an entry on a leaf all visible, whose key gives its columns
 * back, is handed out with its row unread (../BTREE.md §4.1), its
 * tuple empty and ref naming no row, tbl_scan_cols to give its
 * columns. SM_ERR_INVALID on any other cursor.
 */
int tbl_scan_entries_only(scan_t* scan);

/*
 * The key columns of the entry a tree's cursor's tbl_scannxt last
 * gave, as sm2::idx_scan_key gives them: 0; 1 if its key does not
 * give them back, and its row, read, has them; -1 on error. A
 * by-reference datum is valid until the next call on the cursor.
 */
int tbl_scan_cols(scan_t* scan, std::span<sm2::datum_t> values,
                  std::span<bool> isnull);

/* Move the cursor forward to _id id, as sm2::rel_scan_seek does. */
int tbl_scan_seek(scan_t* scan, uint64_t id);

/*
 * Move a cursor in key order (tbl_scan_rows) forward to key, as
 * sm2::rel_scan_seek_key does.
 */
int tbl_scan_seek_key(scan_t* scan, std::string_view key);

/*
 * The key of the row tbl_scannxt last gave, as sm2::rel_scan_key
 * gives it: valid as the row is.
 */
std::string_view tbl_scan_key(const scan_t* scan);

/* Close a cursor, releasing its pin; nullptr is a no-op. */
void tbl_scanend(scan_t* scan);

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
 * tbl_size sets the heap's bytes, a chained heap's older versions
 * included, and tbl_idx_size the index
 * oid's, as sm2::relid_size gives them: in sorted files and in
 * memtables, apart (SM_ERR_NOTFOUND if the table has no such
 * index). tbl_id_limit sets N, the last _id handed out, 0
 * if none, as sm1::lastseq gives it. tbl_probes sets the sorted
 * runs a lookup in the table's store may visit, as sm1::probes.
 */
int tbl_size(table_t* table, uint64_t& disk, uint64_t& mem);
int tbl_idx_size(table_t* table, uint64_t index, uint64_t& disk,
                 uint64_t& mem);
int tbl_id_limit(table_t* table, uint64_t& n);

/*
 * The keys at fractions fs of the bytes of the table's rows, as
 * sm2::rel_keys_at gives them: positions in key order, to read the
 * rows between, which no row need have (../ROWKEY.md §2.4).
 */
int tbl_keys_at(table_t* table, std::span<const double> fs,
                std::vector<std::string>& keys);

/*
 * The bytes of the table's rows from key lo up to hi, or with no hi
 * to the end, as sm2::rel_range_size estimates them.
 */
int tbl_range_size(table_t* table, std::string_view lo, const std::string* hi,
                   uint64_t& out);
int tbl_probes(table_t* table, uint64_t& n);

/*
 * Compact the table's store, its heap and indexes, into one sorted
 * run, as sm1::compact: SQL's VACUUM. The handle's table-oid lock
 * keeps the store from being dropped meanwhile.
 */
int tbl_compact(table_t* table);

/*
 * ---- statistics (SM3_DESIGN.md §6.6, §6.7) ----
 *
 * What an analyze of a table records, in one catalog entry of kind
 * STATS, for the host's planner. A value -- a common value or a
 * histogram bound -- is its bytes as sm2::datum_bytes gives them,
 * a varlena for a variable-width type, of the type of what it
 * describes.
 */

/*
 * The datum of type whose bytes, as sm2::datum_bytes gives them and
 * a statistic keeps a value, are bytes: a by-value one built from
 * them, sign-extended as sm2 builds one; a by-reference one pointing
 * at bytes, which must outlive it.
 */
sm2::datum_t datum_of_bytes(const sm2::type_t* type, std::string_view bytes);

/*
 * A table's statistics, their parts, and the version of the parts,
 * as the root's include/sm_stats.h lays them out: sm.h hands out
 * the ones cat_get_stats keeps as they are.
 */
constexpr int32_t stats_format = ::sm_stats_format;
using stat_t = ::sm_stat_t;
using stats_index_t = ::sm_stats_index_t;
using stats_t = ::sm_stats_t;

/*
 * Record stats as the table oid's in session's transaction,
 * replacing any it had: they commit, roll back and survive a crash
 * with it, and other sessions see them once it commits. It takes
 * the STATS entry's lock X, so two analyzes of one table queue;
 * at REPEATABLE READ and above the second then fails as retryable.
 * Every stat_t must name a column or an index expression of the
 * table, and its values be of that one's type: SM_ERR_INVALID if
 * not, SM_ERR_NOTFOUND if there is no such table.
 */
int cat_put_stats(session_t* session, uint64_t oid, const stats_t& stats);

/*
 * The table oid's statistics, as session sees them: its own
 * transaction's, else the last committed, into out, or nullptr if
 * the table was never analyzed. The committed ones are loaded on
 * first use, in a transaction of their own, and then kept in
 * memory, so a plan takes no lock and reads nothing. A statistic
 * of an index dropped since is left out.
 */
int cat_get_stats(session_t* session, uint64_t oid,
                  std::shared_ptr<const stats_t>& out);

/*
 * Analyze the table, opened in a transaction (SM3_DESIGN.md §6.4 to
 * §6.7). A target below 1 is SM_ERR_INVALID.
 * 1. Wait for the transaction of another analyze of the table, if
 *    one runs, before sampling.
 * 2. Sample up to 300 x target of its rows, reading no more than ten
 *    times that in _ids whatever its size.
 * 3. Estimate each live column's statistics.
 * 4. Record them with the table's sizes and its indexes' by
 *    cat_put_stats.
 * A table whose heap holds no bytes and whose sample reads no row,
 * as a new or truncated one, is not analyzed, and keeps the
 * statistics it had.
 */
int tbl_analyze(table_t* table, int32_t target);

/*
 * As tbl_analyze, but never waiting for another analyze of the table:
 * 0 analyzed; 1, having sampled and recorded nothing, if another
 * transaction's analyze of it holds its statistics' lock; or -1.
 * SQL's ANALYZE (SKIP_LOCKED) asks like this.
 */
int tbl_tryanalyze(table_t* table, int32_t target);

/*
 * One batch of the build of the table's index oid, which must be
 * pending (SM3_DESIGN.md §3). The caller commits the batch, and
 * marks the index valid after the last.
 * 1. Cover up to max_rows rows in key order from the key from on,
 *    the table's first for "".
 * 2. For each row, take its lock shared.
 * 3. Read the row's newest committed version.
 * 4. Add the row's entry if it is not there.
 * 5. Set next to the key to go on from, past the last row's, and
 *    done once the end of the rows was reached.
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
int tbl_index_backfill(table_t* table, uint64_t index, std::string_view from,
                       size_t max_rows, std::string& next, bool& done);

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
 * indexes. Fails with SM_ERR_INVALID if cat is attached already.
 * The builder does not look for work: builder_enqueue gives it a
 * table and an index. A worker then builds the index:
 * 1. Take the table and the index from the queue.
 * 2. Open an sm1 session of its own from open, which returns one on
 *    the catalog's database or nullptr.
 * 3. Check the index is still pending.
 * 4. Fill it in batches of batch_rows rows, each a transaction.
 * 5. Mark the index valid; or failed, with the error kept in its
 *    record, if a batch met an error that is not retryable.
 * 6. Close the session with sm1::closedb.
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
 * or of a catalog not attached, is left alone, and so is one whose
 * worker cannot be started: it stays pending, for the next
 * builder_attach to find. It does not throw.
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

/*
 * The compiler flags this level was built with, its Makefile's
 * OPTFLAGS: "-O3", or "-O3 -DNDEBUG" for digres's release build.
 */
const char* build_flags();

} // namespace sm3

#endif /* SM3_H */

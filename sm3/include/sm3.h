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
#include <functional>
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

/*
 * Where an index's build stands (SM3_DESIGN.md §3). A building
 * index is maintained by every write but not yet filled from the
 * rows that were there before it; a failed one is not maintained
 * and can only be dropped.
 */
enum class index_state_t : uint8_t { building = 0, valid = 1, failed = 2 };

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
    index_state_t                     state = index_state_t::valid;
    /* Why its build failed, as the build met it; SM_OK unless failed. */
    sm1::errnum_t                     error = sm1::SM_OK;
    std::string                       reason;
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
std::shared_ptr<const table_def_t> cat_table(const session_t* s,
                                             uint64_t oid);

/* The table named name in nspoid, as s's transaction sees it. */
std::shared_ptr<const table_def_t> cat_table(const session_t* s,
                                             uint64_t nspoid,
                                             std::string_view name);

/* The names of the tables in nspoid, sorted, as s's transaction sees them. */
std::vector<std::string> cat_tables(const session_t* s, uint64_t nspoid);

/*
 * The index named name in nspoid, as s's transaction sees it:
 * true with its table's oid and its own set. A scan of every
 * table's indexes, since the catalog keeps no map of their names.
 */
bool cat_index(const session_t* s, uint64_t nspoid, std::string_view name,
               uint64_t& table, uint64_t& index);

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
int cat_truncate_table(session_t* session, uint64_t oid);

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
 * INCLUDE columns includes, taking the table's next relid; its
 * oid is set in oid. It records the index as building, for the
 * build to fill from the table's rows and then mark valid
 * (SM3_DESIGN.md §3). On a table this transaction created, which
 * no other can see, it fills the index itself, from the rows the
 * transaction has written, and records it valid; two of those
 * rows with one key fail a unique index with SM_ERR_DUPKEY.
 */
int cat_create_index(session_t* session, uint64_t table,
                     std::string_view name,
                     std::span<const index_key_spec_t> keys,
                     std::span<const int32_t> includes, bool unique,
                     uint64_t& oid);

/*
 * As cat_create_index, for a partial or an expression index
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
int cat_create_index(session_t* session, uint64_t table,
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
 * It must not throw nor longjmp out; it must give the same answer
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
int cat_drop_index(session_t* session, uint64_t table, uint64_t oid);

/*
 * Start the catalog's builder (SM3_DESIGN.md §3). From now on an
 * index recorded as building is queued when its transaction
 * commits, and every index found building now -- left so by a
 * restart -- is queued at once. At most max_workers builds run at
 * a time, each on a thread started when a build is queued and
 * ended when the queue is empty. A worker gets an sm1 session of
 * its own from open, which returns one on the catalog's database
 * or nullptr, and closes it with sm1::closedb; it covers
 * batch_rows rows a transaction. A batch that fails with an error
 * that is not retryable fails the build: the index is marked
 * failed, and the error is kept in its record. cat_close stops
 * the workers and
 * waits for them, so no transaction on a table being built may be
 * open then. Fails with SM_ERR_INVALID if started twice.
 */
int cat_builder_start(catalog_t* cat, std::function<sm1::session_t*()> open,
                      size_t max_workers = 4, size_t batch_rows = 1000);

/*
 * Stop the catalog's builder: its workers end after the batch they
 * are in, and their sessions are closed. A build cut short leaves
 * its index building, for the next cat_builder_start to queue.
 * cat_close does this itself; it is called apart to close the
 * workers' sessions before the database is closed under them.
 */
void cat_builder_stop(catalog_t* cat);

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
 * key too long for one; SM_ERR_INVALID if the index is building
 * and no build of it is queued or running; SM_ERR_INTRANS inside a
 * transaction; SM_ERR_NOTFOUND if there is no such table or index.
 */
int cat_index_wait(session_t* session, uint64_t table, uint64_t oid);

/*
 * Set the state of the table's index oid, as the build does when
 * it ends: valid, or failed. A failed index keeps why, error and
 * reason, in its record; any other state clears them.
 */
int cat_set_index_state(session_t* session, uint64_t table, uint64_t oid,
                        index_state_t state,
                        sm1::errnum_t error = sm1::SM_OK,
                        std::string_view reason = {});

/*
 * Create a collation named name in namespace nspoid for locale,
 * recording the ICU version it opens at; its oid is set in oid. A
 * locale ICU has no data for fails as sm2::collation_open does.
 */
int cat_create_collation(session_t* session, uint64_t nspoid,
                         std::string_view name, std::string_view locale,
                         bool deterministic, uint64_t& oid);

/*
 * ---- tables: a row and its index entries (SM3_DESIGN.md §3) ----
 *
 * An open table: its definition under the schema lock, its heap,
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
 * filled. A building or a failed index never may. An index the
 * session's own transaction made valid always may. Writes maintain
 * an index whether it is usable or not.
 */
bool tbl_index_usable(const table_t* table, uint64_t index);

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
 * entry is removed and a new one added. While an index is building
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
 * table's transaction (cat_truncate_table). The table must be
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
 * One batch of the build of the table's index oid, which must be
 * building (SM3_DESIGN.md §3): cover up to max_rows rows from
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
 * not building or the level does not allow it; SM_ERR_NOTFOUND if
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

} // namespace sm3

#endif /* SM3_H */

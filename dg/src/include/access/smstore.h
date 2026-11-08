/*-------------------------------------------------------------------------
 *
 * smstore.h
 *	  digres's way into sm, the storage stack (../sm/include/sm.h): its
 *	  catalog of what a user creates, and its tables.
 *
 * Only smstore.c includes sm.h; the rest of the backend speaks to sm in
 * PostgreSQL's types, through here.
 *
 * IDENTIFICATION
 *	  src/include/access/smstore.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef SMSTORE_H
#define SMSTORE_H

#include "catalog/objectaddress.h"
#include "access/skey.h"
#include "access/tableam.h"
#include "executor/tuptable.h"
#include "storage/lockdefs.h"
#include "nodes/parsenodes.h"
#include "utils/relcache.h"

/*
 * Whether rel is sm's: every relation a user made is, and only those.
 */
#define RelationIsSm(rel) (RelationGetRelid(rel) >= FirstNormalObjectId)

/* A relation in sm, as pg_class lists it (catalog/catrel.c). */
typedef struct smstore_relation_t {
  Oid oid;
  char *name;
  Oid nspoid;
  char kind;     /* RELKIND_RELATION, _INDEX, _SEQUENCE or _VIEW */
  int natts;     /* its columns; an index's key and include columns */
  bool hasindex; /* a table with an index */
  bool temp;     /* a temporary table, or an index on one */
  /* A table's foreign keys, and whether one of any table references it. */
  int nfkeys;
  bool referenced;
  /* A table's and an index's columns, natts of them, for pg_attribute. */
  char **attnames;
  Oid *typids; /* InvalidOid for a dropped column */
  int32 *typmods;
  bool *notnull;
  char **defaults; /* a table column's DEFAULT, nodeToString()'s, or NULL */
  char *identity;  /* a table column's identity kind, or 0 */
  /* An index's, as pg_index lists it. */
  Oid table;
  bool unique;
  bool primary;
  bool constraint;   /* a UNIQUE or PRIMARY KEY constraint's */
  bool valid;        /* built, and used by the planner */
  int nkeys;         /* its key columns, the first of its columns */
  int16 *attnums;    /* each column's place in the table, from 1; 0 an
                        expression's */
  /* A partial or an expression index's eval (EXPRINDEX.md), or NULL. */
  char *eval;
  /* A sequence's, as pg_sequence and pg_depend list it. */
  int64 seqstart;
  int64 seqincrement;
  Oid owner;           /* the table whose column owns it, or InvalidOid */
  int16 owner_attnum;  /* that column, from 1 */
  bool owner_identity; /* the column is an identity column */
  bool *desc;          /* each key column's */
  bool *nulls_first; /* each key column's */
  /* A view's, as pg_rewrite lists it: its ev_action, nodeToString()'s. */
  char *query;
  char *options; /* a view's, as smstore_view_reloptions() takes, or NULL */
} smstore_relation_t;

/* A column of a table in sm. */
typedef struct smstore_column_t {
  const char *name;
  Oid typid; /* InvalidOid if dropped */
  int32 typmod;
  bool not_null;
  bool dropped;
  const char *default_expr; /* its DEFAULT, nodeToString()'s, or NULL */
  char identity;            /* attidentity's: 'a', 'd', or 0 */
} smstore_column_t;

/*
 * The names CREATE INDEX ... USING gives sm's two kinds of index
 * (../sm/LSMINDEX.md section 8).  Both are pg_am's btree; the name chooses
 * the kind, which sm keeps.
 *  - lsm, the default: each entry a key of RocksDB's.
 *  - btree: a B+tree of the index's own pages.
 */
#define SMSTORE_INDEX_LSM "lsm"
#define SMSTORE_INDEX_TREE "btree"

/*
 * An index of a table in sm: its columns by place in the table, 1-based,
 * the keys first and the INCLUDE columns after, and how each key sorts.
 */
typedef struct smstore_index_t {
  Oid oid;
  const char *name;
  bool unique;
  bool primary; /* the table's primary key */
  bool tree;    /* a B+tree, USING btree; else USING lsm */
  bool usable;  /* the session may scan it now (sm_tbl_index_usable) */
  int nkeys;
  int ncolumns;
  int16 *attnums;    /* ncolumns of them; 0 an expression's */
  bool *desc;        /* nkeys of them */
  bool *nulls_first; /* nkeys of them */
  /* A partial or an expression index's eval (EXPRINDEX.md), or NULL. */
  char *eval;
} smstore_index_t;

/*
 * A foreign key in sm (../sm/FOREIGNKEY.md), as ri_triggers.c and the
 * relation cache read it: what PostgreSQL's pg_constraint row holds.
 * Columns are numbered from 1; the operators are those
 * ATAddForeignKeyConstraint() chose, kept in sm's host bytes.
 */
typedef struct smstore_fkey_t {
  Oid oid;
  NameData name;
  Oid table;     /* the referencing table, conrelid */
  Oid ref_table; /* the referenced, confrelid */
  Oid ref_index; /* its unique index on ref_attnums, conindid */
  int nkeys;
  AttrNumber attnums[INDEX_MAX_KEYS];
  AttrNumber ref_attnums[INDEX_MAX_KEYS];
  Oid pf_eq_oprs[INDEX_MAX_KEYS];
  Oid pp_eq_oprs[INDEX_MAX_KEYS];
  Oid ff_eq_oprs[INDEX_MAX_KEYS];
  int ndelsetcols; /* ON DELETE SET NULL/DEFAULT (cols) */
  AttrNumber delsetcols[INDEX_MAX_KEYS];
  char on_update; /* confupdtype */
  char on_delete; /* confdeltype */
  char match;     /* confmatchtype */
  bool deferrable;
  bool deferred;
  bool validated;
} smstore_fkey_t;

/* A table in sm, as its relation descriptor is built from it. */
typedef struct smstore_table_t {
  Oid oid;
  Oid nspoid;
  const char *name;
  int ncolumns;
  smstore_column_t *columns;
  int nindexes;
  smstore_index_t *indexes;
  uint64 version; /* sm_table_def_t::version, as this transaction reads it */
  int nfkeys;     /* its foreign keys */
  smstore_fkey_t *fkeys;
  int nreferenced; /* those of other tables, and its own, that reference it */
  smstore_fkey_t *referenced;
} smstore_table_t;

/*
 * A view in sm (../sm/VIEW.md), as its relation descriptor is built from
 * it: its query is pg_rewrite's ev_action, a list of the one Query.
 */
typedef struct smstore_view_t {
  Oid oid;
  Oid nspoid;
  const char *name;
  const char *query;
  const char *options; /* its options, as smstore_view_reloptions() takes */
  uint64 version; /* sm_view_def_t::version, as this transaction reads it */
} smstore_view_t;

extern void smstore_statement_start(void);
extern void smstore_statement_end(void);
extern void smstore_statement_advance(void);
extern void *smstore_view_save(void);
extern void *smstore_view_resume(void *view);
extern void smstore_view_end(void *outer);
extern void smstore_view_free(void *view);
extern void smstore_commit(void);
extern void smstore_abort(void);
extern void smstore_subcommit(void);
extern void smstore_subabort(void);
extern Oid smstore_namespace_oid(const char *name);
extern char *smstore_namespace_name(Oid nspoid);
extern List *smstore_namespace_names(void);
extern List *smstore_relations(void);
extern List *smstore_relations_of(Oid oid);
extern Oid smstore_relation_oid(const char *name, Oid nspoid);
extern Oid smstore_create_namespace(const char *name);
extern void smstore_drop_namespaces(DropStmt *stmt);
extern void smstore_analyze(List *rels, bool verbose, bool skip_locked);
extern void smstore_vacuum(List *rels, bool skip_locked);
extern bool smstore_estimate_size(Oid oid, double *pages, double *tuples);
extern int64 smstore_relation_bytes(Relation rel);
extern bool smstore_estimate_index_size(Oid table, Oid index,
                                        double table_tuples, double *pages,
                                        double *tuples);
extern HeapTuple smstore_statistic_tuple(Oid relid, AttrNumber attnum,
                                         bool inh);
extern int32 smstore_statistic_width(Oid relid, AttrNumber attnum);
extern ObjectAddress smstore_create_sequence(CreateSeqStmt *stmt);
extern void smstore_drop_sequences(DropStmt *stmt);
extern ObjectAddress smstore_own_sequence(AlterSeqStmt *stmt, List *owned_by);
extern int64 smstore_nextval(Oid oid);
extern bool smstore_sequence_exists(Oid oid);
extern char *smstore_sequence_lookup(Oid oid, Oid *nspoid);
extern const char *smstore_sequence_name(Oid oid);
extern Oid smstore_identity_sequence(Oid relid, AttrNumber attnum);
extern Node **smstore_update_defaults(Oid relid);
extern ObjectAddress smstore_create_table(CreateStmt *stmt);
extern void smstore_drop_tables(DropStmt *stmt);
extern void smstore_drop_table(Oid oid);
extern void smstore_drop_temp_tables(void);
extern void smstore_alter_table(Oid relid, AlterTableStmt *stmt);
extern void smstore_reassign_owner(Oid relid, Oid owner);
extern ObjectAddress smstore_rename_table(RenameStmt *stmt);
extern ObjectAddress smstore_rename_column(RenameStmt *stmt);
extern ObjectAddress smstore_create_index(Oid relid, IndexStmt *stmt);
extern void smstore_index_eval_parts(const char *eval, List **exprs,
                                     List **pred, List **names);
struct sm_env_t;
extern int smstore_set_index_evaluator(struct sm_env_t *env);
extern void smstore_wait_indexes(void);
extern bool smstore_get_fkey(Oid oid, smstore_fkey_t *out);
extern List *smstore_referencing_tables(Oid relid);
extern ObjectAddress smstore_rename_constraint(RenameStmt *stmt);
extern void smstore_drop_indexes(DropStmt *stmt);
extern void smstore_lock_table(Relation rel, LOCKMODE lockmode, bool nowait);
extern void smstore_lock_relation(Oid relid, LOCKMODE lockmode);
extern bool smstore_trylock_relation(Oid relid, LOCKMODE lockmode);
extern bool smstore_catalog_moved(void);
extern bool smstore_namespaces_moved(void);
extern bool smstore_relation_held(Oid relid, LOCKMODE lockmode);
extern void smstore_truncate(Relation rel);
extern smstore_table_t *smstore_table_desc(Oid oid);
extern smstore_view_t *smstore_view_desc(Oid oid);
extern ObjectAddress smstore_create_view(RangeVar *rv, Query *query,
                                         List *options);
extern ObjectAddress smstore_replace_view(Oid oid, const char *name,
                                          Query *query, List *options);
extern Datum smstore_view_reloptions(const char *options);
extern void smstore_drop_views(DropStmt *stmt);
extern Oid smstore_index_table(Oid indexoid);
extern const char *smstore_index_am(Oid table, Oid indexoid);
extern bool smstore_relation_name(Oid relid, uint64 version, char **name,
                                  Oid *nspoid);
extern bool smstore_index_usable(Relation rel, Oid indexoid);
extern void smstore_insert(Relation rel, TupleTableSlot *slot);
extern bool smstore_insert_arbiter(Relation rel, TupleTableSlot *slot,
                                   List *arbiters, struct RowKey *conflict);
extern bool smstore_load_usable(Relation rel);
extern void smstore_load(Relation rel, TupleTableSlot **slots, int n);
extern void smstore_load_end(Relation rel);
extern struct varlena *smstore_fetch_toast(struct varlena *attr);
extern Size smstore_toast_size(struct varlena *attr);
extern bool smstore_fetch_row(Relation rel, const struct RowKey *key,
                              int natts, TupleTableSlot *slot);
extern TM_Result smstore_update(Relation rel, const struct RowKey *key,
                                TupleTableSlot *slot, TM_FailureData *tmfd);
extern TM_Result smstore_delete(Relation rel, const struct RowKey *key,
                                TM_FailureData *tmfd);
extern TM_Result smstore_lock_row(Relation rel, const struct RowKey *key,
                                  TupleTableSlot *slot, LockTupleMode mode,
                                  LockWaitPolicy wait_policy,
                                  TM_FailureData *tmfd);

/*
 * The rownos a sample scan's block holds: a block of rownos is what a sample
 * scan of a table without a primary key picks, as PostgreSQL's picks a page.
 */
#define SMSTORE_ROWS_PER_BLOCK 256

/* A scan of a table in sm, in rowno order. */
typedef struct smstore_scan_t smstore_scan_t;

/* The slot type of a table in sm: a row deformed as it is read. */
extern PGDLLIMPORT const TupleTableSlotOps TTSOpsSmRow;

extern smstore_scan_t *smstore_beginscan(Relation rel, bool by_rowno);
extern bool smstore_getnext(smstore_scan_t *scan, TupleTableSlot *slot);
extern void smstore_endscan(smstore_scan_t *scan);
extern void smstore_scan_seek(smstore_scan_t *scan, uint64 rowno);
extern bool smstore_scan_row(smstore_scan_t *scan, uint64 rowno,
                             TupleTableSlot *slot);
extern uint64 smstore_rowno_limit(Relation rel);
extern uint64 smstore_sample_slots(Relation rel);
extern void smstore_sample_slot(smstore_scan_t *scan, uint64 slot,
                                uint64 nslots);
extern bool smstore_sample_row(smstore_scan_t *scan, TupleTableSlot *slot);
extern bool smstore_scan_nth(smstore_scan_t *scan, uint64 n,
                             TupleTableSlot *slot);
extern int smstore_scan_natts(const struct Scan *plan, Relation rel);
extern int smstore_expr_natts(Node *expr, Index varno, Relation rel);
extern smstore_scan_t *smstore_index_beginscan(Relation heap, Relation index,
                                               ScanKey keys, int nkeys);
extern bool smstore_index_recheck(smstore_scan_t *scan);
extern void smstore_index_nocols(smstore_scan_t *scan);
extern bool smstore_index_getnext(smstore_scan_t *scan, TupleTableSlot *slot);
extern bool smstore_index_getrowno(smstore_scan_t *scan);
extern bool smstore_index_getentry(smstore_scan_t *scan, TupleTableSlot *slot);

#endif /* SMSTORE_H */

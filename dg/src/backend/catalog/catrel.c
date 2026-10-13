/*-------------------------------------------------------------------------
 *
 * catrel.c
 *	  The catalog relations: catalogs a query can read (SYSCATALOG.md).
 *
 * digres has no catalog tables.  A catalog relation has a catalog's name,
 * oid and columns, compiled in (cat_relation()), and nothing stored: its
 * table access method makes its rows when a scan begins, from the rows
 * compiled in and from sm's catalog as this transaction sees it, into a
 * tuple store the scan reads.  Every write is refused.  The server's own
 * lookups do not come this way; a client's query on pg_namespace does.
 *
 * IDENTIFICATION
 *	  src/backend/catalog/catrel.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/smstore.h"
#include "access/tableam.h"
#include "catalog/cat_builtin.h"
#include "catalog/catrel.h"
#include "catalog/pg_authid_d.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_publication_d.h"
#include "catalog/pg_publication_namespace_d.h"
#include "executor/tuptable.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/tuplestore.h"

/* A callback of the access method that a catalog relation never takes. */
template <typename R, typename... A> static R refuse(A...) {
  ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                  errmsg("a catalog relation is read by a sequential scan, "
                         "and never written")));
  pg_unreachable();
}

/* A name column's value: a NameData, palloc'd. */
static Datum name_datum(const char *name) {
  Name n = (Name)palloc0(NAMEDATALEN);

  namestrcpy(n, name);
  return NameGetDatum(n);
}

/*
 * pg_namespace's rows: those compiled in, then sm's schemas not among them.
 * Every owner is the superuser, and no ACL is kept.
 */
static void gen_pg_namespace(Tuplestorestate *ts, TupleDesc desc) {
  int count;
  const cat_namespace_t *rows = cat_namespaces(&count);
  Datum values[4];
  bool nulls[4] = {false, false, false, true};
  ListCell *l;

  for (int i = 0; i < count; i++) {
    values[0] = ObjectIdGetDatum(rows[i].oid);
    values[1] = name_datum(rows[i].nspname);
    values[2] = ObjectIdGetDatum(BOOTSTRAP_SUPERUSERID);
    values[3] = (Datum)0;
    tuplestore_putvalues(ts, desc, values, nulls);
  }
  foreach (l, smstore_namespace_names()) {
    const char *name = (const char *)lfirst(l);
    Oid oid = smstore_namespace_oid(name);

    if (cat_namespace(oid) != NULL)
      continue;
    values[0] = ObjectIdGetDatum(oid);
    values[1] = name_datum(name);
    values[2] = ObjectIdGetDatum(BOOTSTRAP_SUPERUSERID);
    values[3] = (Datum)0;
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/* Fill ts with the rows of the catalog relation rel. */
static void generate(Relation rel, Tuplestorestate *ts) {
  TupleDesc desc = RelationGetDescr(rel);

  switch (RelationGetRelid(rel)) {
  case NamespaceRelationId:
    gen_pg_namespace(ts, desc);
    break;
  /* What digres has none of: read, and empty. */
  case PublicationRelationId:
  case PublicationNamespaceRelationId:
    break;
  default:
    elog(ERROR, "no rows for catalog relation %u", RelationGetRelid(rel));
  }
}

/* The slot a row of a catalog relation is read into, from a tuple store. */
static const TupleTableSlotOps *catrel_slot_callbacks(Relation rel) {
  return &TTSOpsMinimalTuple;
}

/* A scan of a catalog relation: its rows, made when it began. */
typedef struct catrel_scan_t {
  TableScanDescData base; /* what tableam.h reads */
  Tuplestorestate *rows;
} catrel_scan_t;

/* Begin a scan of rel: make its rows now.  It takes no scan keys. */
static TableScanDesc catrel_scan_begin(Relation rel, Snapshot snapshot,
                                       int nkeys, struct ScanKeyData *key,
                                       uint32 flags) {
  catrel_scan_t *scan;

  if (nkeys != 0)
    ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
                    errmsg("a scan of catalog \"%s\" takes no scan keys",
                           RelationGetRelationName(rel))));
  scan = (catrel_scan_t *)palloc0(sizeof(catrel_scan_t));
  scan->base.rs_rd = rel;
  scan->base.rs_snapshot = snapshot;
  scan->base.rs_flags = flags;
  /* Held in memory: a catalog's rows are few. */
  scan->rows = tuplestore_begin_heap(true, false, work_mem);
  generate(rel, scan->rows);
  return &scan->base;
}

/* Read the rows again from the first, as a rescan asks. */
static void catrel_scan_rescan(TableScanDesc sscan, struct ScanKeyData *key,
                               bool set_params, bool allow_strat,
                               bool allow_pagemode) {
  tuplestore_rescan(((catrel_scan_t *)sscan)->rows);
}

/* End a scan, and let go of a snapshot taken for it alone. */
static void catrel_scan_end(TableScanDesc sscan) {
  catrel_scan_t *scan = (catrel_scan_t *)sscan;

  tuplestore_end(scan->rows);
  if (sscan->rs_flags & SO_TEMP_SNAPSHOT)
    UnregisterSnapshot(sscan->rs_snapshot);
  pfree(scan);
}

/* The scan's next row into slot; false at the end. */
static bool catrel_scan_getnextslot(TableScanDesc sscan,
                                    ScanDirection direction,
                                    TupleTableSlot *slot) {
  return tuplestore_gettupleslot(((catrel_scan_t *)sscan)->rows,
                                 ScanDirectionIsForward(direction), false,
                                 slot);
}

/*
 * The planner's size of a catalog relation: a guess, as it has no
 * statistics.  A page, and a hundred rows.
 */
static void catrel_estimate_size(Relation rel, int32 *attr_widths,
                                 BlockNumber *pages, double *tuples,
                                 double *allvisfrac) {
  *pages = 1;
  *tuples = 100;
  *allvisfrac = 0;
}

/* Every callback set, as GetTableAmRoutine() requires. */
static const TableAmRoutine catrel_methods = [] {
  TableAmRoutine r = {};

  r.type = T_TableAmRoutine;
  r.slot_callbacks = catrel_slot_callbacks;
  r.scan_begin = catrel_scan_begin;
  r.scan_end = catrel_scan_end;
  r.scan_rescan = catrel_scan_rescan;
  r.scan_getnextslot = catrel_scan_getnextslot;
  r.scan_set_tidrange = refuse;
  r.scan_getnextslot_tidrange = refuse;
  r.index_fetch_begin = refuse;
  r.index_fetch_reset = refuse;
  r.index_fetch_end = refuse;
  r.index_fetch_tuple = refuse;
  r.tuple_fetch_row_version = refuse;
  r.tuple_tid_valid = refuse;
  r.tuple_get_latest_tid = refuse;
  r.tuple_satisfies_snapshot = refuse;
  r.index_delete_tuples = refuse;
  r.tuple_insert = refuse;
  r.tuple_insert_speculative = refuse;
  r.tuple_complete_speculative = refuse;
  r.multi_insert = refuse;
  r.finish_bulk_insert = refuse;
  r.tuple_delete = refuse;
  r.tuple_update = refuse;
  r.tuple_lock = refuse;
  r.relation_set_new_filelocator = refuse;
  r.relation_nontransactional_truncate = refuse;
  r.relation_copy_data = refuse;
  r.relation_copy_for_cluster = refuse;
  r.index_build_range_scan = refuse;
  r.index_validate_scan = refuse;
  r.relation_size = refuse;
  r.relation_needs_toast_table = refuse;
  r.relation_toast_am = refuse;
  r.relation_fetch_toast_slice = refuse;
  r.relation_estimate_size = catrel_estimate_size;
  r.scan_bitmap_next_tuple = refuse;
  r.scan_sample_next_block = refuse;
  r.scan_sample_next_tuple = refuse;
  return r;
}();

/* The table access method of a catalog relation. */
const TableAmRoutine *catrel_routine(void) { return &catrel_methods; }

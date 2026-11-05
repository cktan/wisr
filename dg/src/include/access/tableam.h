/*-------------------------------------------------------------------------
 *
 * tableam.h
 *	  POSTGRES table access method definitions.
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * src/include/access/tableam.h
 *
 * NOTES
 *		See tableam.sgml for higher level documentation.
 *
 *-------------------------------------------------------------------------
 */
#ifndef TABLEAM_H
#define TABLEAM_H

#include "access/relscan.h"
#include "access/sdir.h"
#include "access/xact.h"
#include "executor/tuptable.h"
#include "nodes/lockoptions.h"
#include "utils/rel.h"
#include "utils/snapshot.h"


#define DEFAULT_TABLE_ACCESS_METHOD	"heap"

/* GUCs */
extern thread_local /*LOCAL*/ PGDLLIMPORT char *default_table_access_method;


struct BulkInsertStateData;
struct IndexInfo;
struct RowKey;
struct SampleScanState;
struct VacuumParams;
struct ValidateIndexState;

/*
 * Bitmask values for the flags argument to the scan_begin callback.
 */
typedef enum ScanOptions
{
	/* one of SO_TYPE_* may be specified */
	SO_TYPE_SEQSCAN = 1 << 0,
	SO_TYPE_BITMAPSCAN = 1 << 1,
	SO_TYPE_SAMPLESCAN = 1 << 2,
	SO_TYPE_TIDSCAN = 1 << 3,
	SO_TYPE_ANALYZE = 1 << 5,

	/* several of SO_ALLOW_* may be specified */
	/* allow or disallow use of access strategy */
	SO_ALLOW_STRAT = 1 << 6,
	/* verify visibility page-at-a-time? */
	SO_ALLOW_PAGEMODE = 1 << 8,

	/* unregister snapshot at scan end? */
	SO_TEMP_SNAPSHOT = 1 << 9,
}			ScanOptions;

/*
 * Result codes for table_{update,delete,lock_tuple}, and for visibility
 * routines inside table AMs.
 */
typedef enum TM_Result
{
	/*
	 * Signals that the action succeeded (i.e. update/delete performed, lock
	 * was acquired)
	 */
	TM_Ok,

	/* The affected tuple wasn't visible to the relevant snapshot */
	TM_Invisible,

	/* The affected tuple was already modified by the calling backend */
	TM_SelfModified,

	/*
	 * The affected tuple was updated by another transaction. This includes
	 * the case where tuple was moved to another partition.
	 */
	TM_Updated,

	/* The affected tuple was deleted by another transaction */
	TM_Deleted,

	/*
	 * The affected tuple is currently being modified by another session. This
	 * will only be returned if table_(update/delete/lock_tuple) are
	 * instructed not to wait.
	 */
	TM_BeingModified,

	/* lock couldn't be acquired, action skipped. Only used by lock_tuple */
	TM_WouldBlock,
} TM_Result;

/*
 * Result codes for table_update(..., update_indexes*..).
 * Used to determine which indexes to update.
 */
typedef enum TU_UpdateIndexes
{
	/* No indexed columns were updated (incl. TID addressing of tuple) */
	TU_None,

	/* A non-summarizing indexed column was updated, or the TID has changed */
	TU_All,

	/* Only summarized columns were updated, TID is unchanged */
	TU_Summarizing,
} TU_UpdateIndexes;

/*
 * When table_tuple_update, table_tuple_delete, or table_tuple_lock fail
 * because the target tuple is already outdated, they fill in this struct to
 * provide information to the caller about what happened. When those functions
 * succeed, the contents of this struct should not be relied upon, except for
 * `traversed`, which may be set in both success and failure cases.
 *
 * digres: no row has a ctid to link to its replacement (../sm/ROWSTORE.md).
 *
 * xmax is the outdating transaction's XID.  If the caller wants to visit the
 * replacement tuple, it must check that this matches before believing the
 * replacement is really a match.  This is InvalidTransactionId if the target
 * was !LP_NORMAL (expected only for a TID retrieved from syscache).
 *
 * traversed indicates if an update chain was followed in order to try to lock
 * the target tuple.  (This may be set in both success and failure cases.)
 */
typedef struct TM_FailureData
{
	TransactionId xmax;
	bool		traversed;
} TM_FailureData;

/*
 * State used when calling table_index_delete_tuples().
 *
 * Represents the status of table tuples, referenced by table TID and taken by
 * index AM from index tuples.  State consists of high level parameters of the
 * deletion operation, plus two mutable palloc()'d arrays for information
 * about the status of individual table tuples.  These are conceptually one
 * single array.  Using two arrays keeps the TM_IndexDelete struct small,
 * which makes sorting the first array (the deltids array) fast.
 *
 * Some index AM callers perform simple index tuple deletion (by specifying
 * bottomup = false), and include only known-dead deltids.  These known-dead
 * entries are all marked knowndeletable = true directly (typically these are
 * TIDs from LP_DEAD-marked index tuples), but that isn't strictly required.
 *
 * Callers that specify bottomup = true are "bottom-up index deletion"
 * callers.  The considerations for the tableam are more subtle with these
 * callers because they ask the tableam to perform highly speculative work,
 * and might only expect the tableam to check a small fraction of all entries.
 * Caller is not allowed to specify knowndeletable = true for any entry
 * because everything is highly speculative.  Bottom-up caller provides
 * context and hints to tableam -- see comments below for details on how index
 * AMs and tableams should coordinate during bottom-up index deletion.
 *
 * Simple index deletion callers may ask the tableam to perform speculative
 * work, too.  This is a little like bottom-up deletion, but not too much.
 * The tableam will only perform speculative work when it's practically free
 * to do so in passing for simple deletion caller (while always performing
 * whatever work is needed to enable knowndeletable/LP_DEAD index tuples to
 * be deleted within index AM).  This is the real reason why it's possible for
 * simple index deletion caller to specify knowndeletable = false up front
 * (this means "check if it's possible for me to delete corresponding index
 * tuple when it's cheap to do so in passing").  The index AM should only
 * include "extra" entries for index tuples whose TIDs point to a table block
 * that tableam is expected to have to visit anyway (in the event of a block
 * orientated tableam).  The tableam isn't strictly obligated to check these
 * "extra" TIDs, but a block-based AM should always manage to do so in
 * practice.
 *
 * The final contents of the deltids/status arrays are interesting to callers
 * that ask tableam to perform speculative work (i.e. when _any_ items have
 * knowndeletable set to false up front).  These index AM callers will
 * naturally need to consult final state to determine which index tuples are
 * in fact deletable.
 *
 * The index AM can keep track of which index tuple relates to which deltid by
 * setting idxoffnum (and/or relying on each entry being uniquely identifiable
 * using tid), which is important when the final contents of the array will
 * need to be interpreted -- the array can shrink from initial size after
 * tableam processing and/or have entries in a new order (tableam may sort
 * deltids array for its own reasons).  Bottom-up callers may find that final
 * ndeltids is 0 on return from call to tableam, in which case no index tuple
 * deletions are possible.  Simple deletion callers can rely on any entries
 * they know to be deletable appearing in the final array as deletable.
 */
typedef struct TM_IndexDelete
{
	ItemPointerData tid;		/* table TID from index tuple */
	int16		id;				/* Offset into TM_IndexStatus array */
} TM_IndexDelete;

typedef struct TM_IndexStatus
{
	OffsetNumber idxoffnum;		/* Index am page offset number */
	bool		knowndeletable; /* Currently known to be deletable? */

	/* Bottom-up index deletion specific fields follow */
	bool		promising;		/* Promising (duplicate) index tuple? */
	int16		freespace;		/* Space freed in index if deleted */
} TM_IndexStatus;

/*
 * Index AM/tableam coordination is central to the design of bottom-up index
 * deletion.  The index AM provides hints about where to look to the tableam
 * by marking some entries as "promising".  Index AM does this with duplicate
 * index tuples that are strongly suspected to be old versions left behind by
 * UPDATEs that did not logically modify indexed values.  Index AM may find it
 * helpful to only mark entries as promising when they're thought to have been
 * affected by such an UPDATE in the recent past.
 *
 * Bottom-up index deletion casts a wide net at first, usually by including
 * all TIDs on a target index page.  It is up to the tableam to worry about
 * the cost of checking transaction status information.  The tableam is in
 * control, but needs careful guidance from the index AM.  Index AM requests
 * that bottomupfreespace target be met, while tableam measures progress
 * towards that goal by tallying the per-entry freespace value for known
 * deletable entries. (All !bottomup callers can just set these space related
 * fields to zero.)
 */
typedef struct TM_IndexDeleteOp
{
	Relation	irel;			/* Target index relation */
	BlockNumber iblknum;		/* Index block number (for error reports) */
	bool		bottomup;		/* Bottom-up (not simple) deletion? */
	int			bottomupfreespace;	/* Bottom-up space target */

	/* Mutable per-TID information follows (index AM initializes entries) */
	int			ndeltids;		/* Current # of deltids/status elements */
	TM_IndexDelete *deltids;
	TM_IndexStatus *status;
} TM_IndexDeleteOp;

/* "options" flag bits for table_tuple_insert */
/* TABLE_INSERT_SKIP_WAL was 0x0001; RelationNeedsWAL() now governs */
#define TABLE_INSERT_SKIP_FSM		0x0002
#define TABLE_INSERT_FROZEN			0x0004
#define TABLE_INSERT_NO_LOGICAL		0x0008

/* flag bits for table_tuple_lock */
/* Follow tuples whose update is in progress if lock modes don't conflict  */
#define TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS	(1 << 0)
/* Follow update chain and lock latest version of tuple */
#define TUPLE_LOCK_FLAG_FIND_LAST_VERSION		(1 << 1)


/* Typedef for callback function for table_index_build_scan */
typedef void (*IndexBuildCallback) (Relation index,
									ItemPointer tid,
									Datum *values,
									bool *isnull,
									bool tupleIsAlive,
									void *state);

/*
 * API struct for a table AM.  Note this must be allocated in a
 * server-lifetime manner, typically as a static const struct, which then gets
 * returned by FormData_pg_am.amhandler.
 *
 * In most cases it's not appropriate to call the callbacks directly, use the
 * table_* wrapper functions instead.
 *
 * GetTableAmRoutine() asserts that required callbacks are filled in, remember
 * to update when adding a callback.
 */
typedef struct TableAmRoutine
{
	/* this must be set to T_TableAmRoutine */
	NodeTag		type;


	/* ------------------------------------------------------------------------
	 * Slot related callbacks.
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Return slot implementation suitable for storing a tuple of this AM.
	 */
	const TupleTableSlotOps *(*slot_callbacks) (Relation rel);


	/* ------------------------------------------------------------------------
	 * Table scan callbacks.
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Start a scan of `rel`.  The callback has to return a TableScanDesc,
	 * which will typically be embedded in a larger, AM specific, struct.
	 *
	 * If nkeys != 0, the results need to be filtered by those scan keys.
	 *
	 * `flags` is a bitmask indicating the type of scan (ScanOptions's
	 * SO_TYPE_*, currently only one may be specified), options controlling
	 * the scan's behaviour (ScanOptions's SO_ALLOW_*, several may be
	 * specified, an AM may ignore unsupported ones) and whether the snapshot
	 * needs to be deallocated at scan_end (ScanOptions's SO_TEMP_SNAPSHOT).
	 */
	TableScanDesc (*scan_begin) (Relation rel,
								 Snapshot snapshot,
								 int nkeys, struct ScanKeyData *key,
								 uint32 flags);

	/*
	 * Release resources and deallocate scan. If TableScanDesc.temp_snap,
	 * TableScanDesc.rs_snapshot needs to be unregistered.
	 */
	void		(*scan_end) (TableScanDesc scan);

	/*
	 * Restart relation scan.  If set_params is set to true, allow_{strat,
	 * sync, pagemode} (see scan_begin) changes should be taken into account.
	 */
	void		(*scan_rescan) (TableScanDesc scan, struct ScanKeyData *key,
								bool set_params, bool allow_strat,
								bool allow_pagemode);

	/*
	 * Return next tuple from `scan`, store in slot.
	 */
	bool		(*scan_getnextslot) (TableScanDesc scan,
									 ScanDirection direction,
									 TupleTableSlot *slot);

	/* ------------------------------------------------------------------------
	 * Index Scan Callbacks
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Prepare to fetch tuples from the relation, as needed when fetching
	 * tuples for an index scan.  The callback has to return an
	 * IndexFetchTableData, which the AM will typically embed in a larger
	 * structure with additional information.
	 *
	 * Tuples for an index scan can then be fetched via index_fetch_tuple.
	 */
	struct IndexFetchTableData *(*index_fetch_begin) (Relation rel);

	/*
	 * Reset index fetch. Typically this will release cross index fetch
	 * resources held in IndexFetchTableData.
	 */
	void		(*index_fetch_reset) (struct IndexFetchTableData *data);

	/*
	 * Release resources and deallocate index fetch.
	 */
	void		(*index_fetch_end) (struct IndexFetchTableData *data);

	/*
	 * Fetch tuple at `tid` into `slot`, after doing a visibility test
	 * according to `snapshot`. If a tuple was found and passed the visibility
	 * test, return true, false otherwise.
	 *
	 * Note that AMs that do not necessarily update indexes when indexed
	 * columns do not change, need to return the current/correct version of
	 * the tuple that is visible to the snapshot, even if the tid points to an
	 * older version of the tuple.
	 *
	 * *call_again is false on the first call to index_fetch_tuple for a tid.
	 * If there potentially is another tuple matching the tid, *call_again
	 * needs to be set to true by index_fetch_tuple, signaling to the caller
	 * that index_fetch_tuple should be called again for the same tid.
	 *
	 * *all_dead, if all_dead is not NULL, should be set to true by
	 * index_fetch_tuple iff it is guaranteed that no backend needs to see
	 * that tuple. Index AMs can use that to avoid returning that tid in
	 * future searches.
	 */
	bool		(*index_fetch_tuple) (struct IndexFetchTableData *scan,
									  ItemPointer tid,
									  Snapshot snapshot,
									  TupleTableSlot *slot,
									  bool *call_again, bool *all_dead);


	/* ------------------------------------------------------------------------
	 * Callbacks for non-modifying operations on individual tuples
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Fetch tuple at `tid` into `slot`, after doing a visibility test
	 * according to `snapshot`. If a tuple was found and passed the visibility
	 * test, returns true, false otherwise.
	 */
	bool		(*tuple_fetch_row_version) (Relation rel,
											const struct RowKey *key,
											Snapshot snapshot,
											TupleTableSlot *slot);

	/*
	 * Does the tuple in `slot` satisfy `snapshot`?  The slot needs to be of
	 * the appropriate type for the AM.
	 */
	bool		(*tuple_satisfies_snapshot) (Relation rel,
											 TupleTableSlot *slot,
											 Snapshot snapshot);

	/* see table_index_delete_tuples() */
	TransactionId (*index_delete_tuples) (Relation rel,
										  TM_IndexDeleteOp *delstate);


	/* ------------------------------------------------------------------------
	 * Manipulations of physical tuples.
	 * ------------------------------------------------------------------------
	 */

	/* see table_tuple_insert() for reference about parameters */
	void		(*tuple_insert) (Relation rel, TupleTableSlot *slot,
								 int options,
								 struct BulkInsertStateData *bistate);

	/* a speculative insertion: unused, as ON CONFLICT is sm's */
	void		(*tuple_insert_speculative) (Relation rel,
											 TupleTableSlot *slot,
											 int options,
											 struct BulkInsertStateData *bistate,
											 uint32 specToken);

	/* the end of a speculative insertion: unused, as ON CONFLICT is sm's */
	void		(*tuple_complete_speculative) (Relation rel,
											   TupleTableSlot *slot,
											   uint32 specToken,
											   bool succeeded);

	/* see table_multi_insert() for reference about parameters */
	void		(*multi_insert) (Relation rel, TupleTableSlot **slots, int nslots,
								 int options, struct BulkInsertStateData *bistate);

	/* see table_tuple_delete() for reference about parameters */
	TM_Result	(*tuple_delete) (Relation rel,
								 const struct RowKey *key,
								 Snapshot snapshot,
								 Snapshot crosscheck,
								 bool wait,
								 TM_FailureData *tmfd,
								 bool changingPart);

	/* see table_tuple_update() for reference about parameters */
	TM_Result	(*tuple_update) (Relation rel,
								 const struct RowKey *key,
								 TupleTableSlot *slot,
								 Snapshot snapshot,
								 Snapshot crosscheck,
								 bool wait,
								 TM_FailureData *tmfd,
								 LockTupleMode *lockmode,
								 TU_UpdateIndexes *update_indexes);

	/* see table_tuple_lock() for reference about parameters */
	TM_Result	(*tuple_lock) (Relation rel,
							   const struct RowKey *key,
							   Snapshot snapshot,
							   TupleTableSlot *slot,
							   LockTupleMode mode,
							   LockWaitPolicy wait_policy,
							   uint8 flags,
							   TM_FailureData *tmfd);

	/*
	 * Perform operations necessary to complete insertions made via
	 * tuple_insert and multi_insert with a BulkInsertState specified. In-tree
	 * access methods ceased to use this.
	 *
	 * Typically callers of tuple_insert and multi_insert will just pass all
	 * the flags that apply to them, and each AM has to decide which of them
	 * make sense for it, and then only take actions in finish_bulk_insert for
	 * those flags, and ignore others.
	 *
	 * Optional callback.
	 */
	void		(*finish_bulk_insert) (Relation rel, int options);


	/* ------------------------------------------------------------------------
	 * DDL related functionality.
	 * ------------------------------------------------------------------------
	 */

	/*
	 * This callback needs to create new relation storage for `rel`, with
	 * appropriate durability behaviour for `persistence`.
	 *
	 * Note that only the subset of the relcache filled by
	 * RelationBuildLocalRelation() can be relied upon and that the relation's
	 * catalog entries will either not yet exist (new relation), or will still
	 * reference the old relfilelocator.
	 *
	 * As output *freezeXid, *minmulti must be set to the values appropriate
	 * for pg_class.{relfrozenxid, relminmxid}. For AMs that don't need those
	 * fields to be filled they can be set to InvalidTransactionId and
	 * InvalidMultiXactId, respectively.
	 *
	 * See also table_relation_set_new_filelocator().
	 */
	void		(*relation_set_new_filelocator) (Relation rel,
												 const RelFileLocator *newrlocator,
												 char persistence,
												 TransactionId *freezeXid,
												 MultiXactId *minmulti);

	/*
	 * This callback needs to remove all contents from `rel`'s current
	 * relfilelocator. No provisions for transactional behaviour need to be
	 * made.  Often this can be implemented by truncating the underlying
	 * storage to its minimal size.
	 *
	 * See also table_relation_nontransactional_truncate().
	 */
	void		(*relation_nontransactional_truncate) (Relation rel);

	/*
	 * See table_relation_copy_data().
	 *
	 * This can typically be implemented by directly copying the underlying
	 * storage, unless it contains references to the tablespace internally.
	 */
	void		(*relation_copy_data) (Relation rel,
									   const RelFileLocator *newrlocator);

	/* See table_relation_copy_for_cluster() */
	void		(*relation_copy_for_cluster) (Relation OldTable,
											  Relation NewTable,
											  Relation OldIndex,
											  bool use_sort,
											  TransactionId OldestXmin,
											  TransactionId *xid_cutoff,
											  MultiXactId *multi_cutoff,
											  double *num_tuples,
											  double *tups_vacuumed,
											  double *tups_recently_dead);

	/* see table_index_build_range_scan for reference about parameters */
	double		(*index_build_range_scan) (Relation table_rel,
										   Relation index_rel,
										   struct IndexInfo *index_info,
										   bool anyvisible,
										   bool progress,
										   BlockNumber start_blockno,
										   BlockNumber numblocks,
										   IndexBuildCallback callback,
										   void *callback_state);

	/* see table_index_validate_scan for reference about parameters */
	void		(*index_validate_scan) (Relation table_rel,
										Relation index_rel,
										struct IndexInfo *index_info,
										Snapshot snapshot,
										struct ValidateIndexState *state);


	/* ------------------------------------------------------------------------
	 * Miscellaneous functions.
	 * ------------------------------------------------------------------------
	 */

	/*
	 * See table_relation_size().
	 *
	 * Note that currently a few callers use the MAIN_FORKNUM size to figure
	 * out the range of potentially interesting blocks (brin, analyze). It's
	 * probable that we'll need to revise the interface for those at some
	 * point.
	 */
	uint64		(*relation_size) (Relation rel, ForkNumber forkNumber);


	/*
	 * This callback should return true if the relation requires a TOAST table
	 * and false if it does not.  It may wish to examine the relation's tuple
	 * descriptor before making a decision, but if it uses some other method
	 * of storing large values (or if it does not support them) it can simply
	 * return false.
	 */
	bool		(*relation_needs_toast_table) (Relation rel);

	/*
	 * This callback should return the OID of the table AM that implements
	 * TOAST tables for this AM.  If the relation_needs_toast_table callback
	 * always returns false, this callback is not required.
	 */
	Oid			(*relation_toast_am) (Relation rel);


	/* ------------------------------------------------------------------------
	 * Planner related functions.
	 * ------------------------------------------------------------------------
	 */

	/*
	 * See table_relation_estimate_size().
	 *
	 * While block oriented, it shouldn't be too hard for an AM that doesn't
	 * internally use blocks to convert into a usable representation.
	 *
	 * This differs from the relation_size callback by returning size
	 * estimates (both relation size and tuple count) for planning purposes,
	 * rather than returning a currently correct estimate.
	 */
	void		(*relation_estimate_size) (Relation rel, int32 *attr_widths,
										   BlockNumber *pages, double *tuples,
										   double *allvisfrac);


	/* ------------------------------------------------------------------------
	 * Executor related functions.
	 * ------------------------------------------------------------------------
	 */

	/*
	 * Fetch the next tuple of a bitmap table scan into `slot` and return true
	 * if a visible tuple was found, false otherwise.
	 *
	 * `lossy_pages` is incremented if the bitmap is lossy for the selected
	 * page; otherwise, `exact_pages` is incremented. These are tracked for
	 * display in EXPLAIN ANALYZE output.
	 *
	 * Prefetching additional data from the bitmap is left to the table AM.
	 *
	 * This is an optional callback.
	 */
	bool		(*scan_bitmap_next_tuple) (TableScanDesc scan,
										   TupleTableSlot *slot,
										   bool *recheck,
										   uint64 *lossy_pages,
										   uint64 *exact_pages);

	/*
	 * Prepare to fetch tuples from the next block in a sample scan. Return
	 * false if the sample scan is finished, true otherwise. `scan` was
	 * started via table_beginscan_sampling().
	 *
	 * Typically this will first determine the target block by calling the
	 * TsmRoutine's NextSampleBlock() callback if not NULL, or alternatively
	 * perform a sequential scan over all blocks.  The determined block is
	 * then typically read and pinned.
	 *
	 * As the TsmRoutine interface is block based, a block needs to be passed
	 * to NextSampleBlock(). If that's not appropriate for an AM, it
	 * internally needs to perform mapping between the internal and a block
	 * based representation.
	 *
	 * Note that it's not acceptable to hold deadlock prone resources such as
	 * lwlocks until scan_sample_next_tuple() has exhausted the tuples on the
	 * block - the tuple is likely to be returned to an upper query node, and
	 * the next call could be off a long while. Holding buffer pins and such
	 * is obviously OK.
	 *
	 * Currently it is required to implement this interface, as there's no
	 * alternative way (contrary e.g. to bitmap scans) to implement sample
	 * scans. If infeasible to implement, the AM may raise an error.
	 */
	bool		(*scan_sample_next_block) (TableScanDesc scan,
										   struct SampleScanState *scanstate);

	/*
	 * This callback, only called after scan_sample_next_block has returned
	 * true, should determine the next tuple to be returned from the selected
	 * block using the TsmRoutine's NextSampleTuple() callback.
	 *
	 * The callback needs to perform visibility checks, and only return
	 * visible tuples. That obviously can mean calling NextSampleTuple()
	 * multiple times.
	 *
	 * The TsmRoutine interface assumes that there's a maximum offset on a
	 * given page, so if that doesn't apply to an AM, it needs to emulate that
	 * assumption somehow.
	 */
	bool		(*scan_sample_next_tuple) (TableScanDesc scan,
										   struct SampleScanState *scanstate,
										   TupleTableSlot *slot);

} TableAmRoutine;


/* ----------------------------------------------------------------------------
 * Slot functions.
 * ----------------------------------------------------------------------------
 */

/*
 * Returns slot callbacks suitable for holding tuples of the appropriate type
 * for the relation.  Works for tables, views, foreign tables and partitioned
 * tables.
 */
extern const TupleTableSlotOps *table_slot_callbacks(Relation relation);

/*
 * Returns slot using the callbacks returned by table_slot_callbacks(), and
 * registers it on *reglist.
 */
extern TupleTableSlot *table_slot_create(Relation relation, List **reglist);


/* ----------------------------------------------------------------------------
 * Table scan functions.
 * ----------------------------------------------------------------------------
 */

/*
 * Start a scan of `rel`. Returned tuples pass a visibility test of
 * `snapshot`, and if nkeys != 0, the results are filtered by those scan keys.
 */
static inline TableScanDesc
table_beginscan(Relation rel, Snapshot snapshot,
				int nkeys, struct ScanKeyData *key)
{
	uint32		flags = SO_TYPE_SEQSCAN |
		SO_ALLOW_STRAT | SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, flags);
}

/*
 * Like table_beginscan(), but for scanning catalog. It'll automatically use a
 * snapshot appropriate for scanning catalog relations.
 */
extern TableScanDesc table_beginscan_catalog(Relation relation, int nkeys,
											 struct ScanKeyData *key);

/*
 * Like table_beginscan(), but table_beginscan_strat() offers an extended API
 * that lets the caller control whether a nondefault buffer access strategy
 * can be used.  It defaults to true with plain table_beginscan.
 */
static inline TableScanDesc
table_beginscan_strat(Relation rel, Snapshot snapshot,
					  int nkeys, struct ScanKeyData *key,
					  bool allow_strat)
{
	uint32		flags = SO_TYPE_SEQSCAN | SO_ALLOW_PAGEMODE;

	if (allow_strat)
		flags |= SO_ALLOW_STRAT;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, flags);
}

/*
 * table_beginscan_bm is an alternative entry point for setting up a
 * TableScanDesc for a bitmap heap scan.  Although that scan technology is
 * really quite unlike a standard seqscan, there is just enough commonality to
 * make it worth using the same data structure.
 */
static inline TableScanDesc
table_beginscan_bm(Relation rel, Snapshot snapshot,
				   int nkeys, struct ScanKeyData *key)
{
	uint32		flags = SO_TYPE_BITMAPSCAN | SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, flags);
}

/*
 * table_beginscan_sampling is an alternative entry point for setting up a
 * TableScanDesc for a TABLESAMPLE scan.  As with bitmap scans, it's worth
 * using the same data structure although the behavior is rather different.
 * In addition to the options offered by table_beginscan_strat, this call
 * also allows control of whether page-mode visibility checking is used.
 */
static inline TableScanDesc
table_beginscan_sampling(Relation rel, Snapshot snapshot,
						 int nkeys, struct ScanKeyData *key,
						 bool allow_strat,
						 bool allow_pagemode)
{
	uint32		flags = SO_TYPE_SAMPLESCAN;

	if (allow_strat)
		flags |= SO_ALLOW_STRAT;
	if (allow_pagemode)
		flags |= SO_ALLOW_PAGEMODE;

	return rel->rd_tableam->scan_begin(rel, snapshot, nkeys, key, flags);
}

/*
 * table_beginscan_tid is an alternative entry point for setting up a
 * TableScanDesc for a Tid scan. As with bitmap scans, it's worth using
 * the same data structure although the behavior is rather different.
 */
static inline TableScanDesc
table_beginscan_tid(Relation rel, Snapshot snapshot)
{
	uint32		flags = SO_TYPE_TIDSCAN;

	return rel->rd_tableam->scan_begin(rel, snapshot, 0, NULL, flags);
}

/*
 * End relation scan.
 */
static inline void
table_endscan(TableScanDesc scan)
{
	scan->rs_rd->rd_tableam->scan_end(scan);
}

/*
 * Restart a relation scan.
 */
static inline void
table_rescan(TableScanDesc scan,
			 struct ScanKeyData *key)
{
	scan->rs_rd->rd_tableam->scan_rescan(scan, key, false, false, false);
}

/*
 * Restart a relation scan after changing params.
 *
 * This call allows changing the buffer strategy and pagemode options before
 * starting a fresh scan.  The previously selected startblock will be kept.
 */
static inline void
table_rescan_set_params(TableScanDesc scan, struct ScanKeyData *key,
						bool allow_strat, bool allow_pagemode)
{
	scan->rs_rd->rd_tableam->scan_rescan(scan, key, true,
										 allow_strat,
										 allow_pagemode);
}

/*
 * Return next tuple from `scan`, store in slot.
 */
static inline bool
table_scan_getnextslot(TableScanDesc sscan, ScanDirection direction, TupleTableSlot *slot)
{
	slot->tts_tableOid = RelationGetRelid(sscan->rs_rd);

	/* We don't expect actual scans using NoMovementScanDirection */
	Assert(direction == ForwardScanDirection ||
		   direction == BackwardScanDirection);

	return sscan->rs_rd->rd_tableam->scan_getnextslot(sscan, direction, slot);
}

/* ----------------------------------------------------------------------------
 * Parallel table scan related functions.
 * ----------------------------------------------------------------------------
 */

/* ----------------------------------------------------------------------------
 *  Index scan related functions.
 * ----------------------------------------------------------------------------
 */

/*
 * Prepare to fetch tuples from the relation, as needed when fetching tuples
 * for an index scan.
 *
 * Tuples for an index scan can then be fetched via table_index_fetch_tuple().
 */
static inline IndexFetchTableData *
table_index_fetch_begin(Relation rel)
{
	return rel->rd_tableam->index_fetch_begin(rel);
}

/*
 * Reset index fetch. Typically this will release cross index fetch resources
 * held in IndexFetchTableData.
 */
static inline void
table_index_fetch_reset(struct IndexFetchTableData *scan)
{
	scan->rel->rd_tableam->index_fetch_reset(scan);
}

/*
 * Release resources and deallocate index fetch.
 */
static inline void
table_index_fetch_end(struct IndexFetchTableData *scan)
{
	scan->rel->rd_tableam->index_fetch_end(scan);
}

/*
 * Fetches, as part of an index scan, tuple at `tid` into `slot`, after doing
 * a visibility test according to `snapshot`. If a tuple was found and passed
 * the visibility test, returns true, false otherwise. Note that *tid may be
 * modified when we return true (see later remarks on multiple row versions
 * reachable via a single index entry).
 *
 * *call_again needs to be false on the first call to table_index_fetch_tuple() for
 * a tid. If there potentially is another tuple matching the tid, *call_again
 * will be set to true, signaling that table_index_fetch_tuple() should be called
 * again for the same tid.
 *
 * *all_dead, if all_dead is not NULL, will be set to true by
 * table_index_fetch_tuple() iff it is guaranteed that no backend needs to see
 * that tuple. Index AMs can use that to avoid returning that tid in future
 * searches.
 *
 * The difference between this function and table_tuple_fetch_row_version()
 * is that this function returns the currently visible version of a row if
 * the AM supports storing multiple row versions reachable via a single index
 * entry (like heap's HOT). Whereas table_tuple_fetch_row_version() only
 * evaluates the tuple exactly at `tid`. Outside of index entry ->table tuple
 * lookups, table_tuple_fetch_row_version() is what's usually needed.
 */
static inline bool
table_index_fetch_tuple(struct IndexFetchTableData *scan,
						ItemPointer tid,
						Snapshot snapshot,
						TupleTableSlot *slot,
						bool *call_again, bool *all_dead)
{
	return scan->rel->rd_tableam->index_fetch_tuple(scan, tid, snapshot,
													slot, call_again,
													all_dead);
}

/*
 * This is a convenience wrapper around table_index_fetch_tuple() which
 * returns whether there are table tuple items corresponding to an index
 * entry.  This likely is only useful to verify if there's a conflict in a
 * unique index.
 */


/* ------------------------------------------------------------------------
 * Functions for non-modifying operations on individual tuples
 * ------------------------------------------------------------------------
 */


/*
 * Fetch tuple at `tid` into `slot`, after doing a visibility test according to
 * `snapshot`. If a tuple was found and passed the visibility test, returns
 * true, false otherwise.
 *
 * See table_index_fetch_tuple's comment about what the difference between
 * these functions is. It is correct to use this function outside of index
 * entry->table tuple lookups.
 */
static inline bool
table_tuple_fetch_row_version(Relation rel,
							  const struct RowKey *key,
							  Snapshot snapshot,
							  TupleTableSlot *slot)
{
	return rel->rd_tableam->tuple_fetch_row_version(rel, key, snapshot, slot);
}

/*
 * Return true iff tuple in slot satisfies the snapshot.
 *
 * This assumes the slot's tuple is valid, and of the appropriate type for the
 * AM.
 *
 * Some AMs might modify the data underlying the tuple as a side-effect. If so
 * they ought to mark the relevant buffer dirty.
 */
static inline bool
table_tuple_satisfies_snapshot(Relation rel, TupleTableSlot *slot,
							   Snapshot snapshot)
{
	return rel->rd_tableam->tuple_satisfies_snapshot(rel, slot, snapshot);
}

/*
 * Determine which index tuples are safe to delete based on their table TID.
 *
 * Determines which entries from index AM caller's TM_IndexDeleteOp state
 * point to vacuumable table tuples.  Entries that are found by tableam to be
 * vacuumable are naturally safe for index AM to delete, and so get directly
 * marked as deletable.  See comments above TM_IndexDelete and comments above
 * TM_IndexDeleteOp for full details.
 *
 * Returns a snapshotConflictHorizon transaction ID that caller places in
 * its index deletion WAL record.  This might be used during subsequent REDO
 * of the WAL record when in Hot Standby mode -- a recovery conflict for the
 * index deletion operation might be required on the standby.
 */
static inline TransactionId
table_index_delete_tuples(Relation rel, TM_IndexDeleteOp *delstate)
{
	return rel->rd_tableam->index_delete_tuples(rel, delstate);
}


/* ----------------------------------------------------------------------------
 *  Functions for manipulations of physical tuples.
 * ----------------------------------------------------------------------------
 */

/*
 * Insert a tuple from a slot into table AM routine.
 *
 * The options bitmask allows the caller to specify options that may change the
 * behaviour of the AM. The AM will ignore options that it does not support.
 *
 * If the TABLE_INSERT_SKIP_FSM option is specified, AMs are free to not reuse
 * free space in the relation. This can save some cycles when we know the
 * relation is new and doesn't contain useful amounts of free space.
 * TABLE_INSERT_SKIP_FSM is commonly passed directly to
 * RelationGetBufferForTuple. See that method for more information.
 *
 * TABLE_INSERT_FROZEN should only be specified for inserts into
 * relation storage created during the current subtransaction and when
 * there are no prior snapshots or pre-existing portals open.
 * This causes rows to be frozen, which is an MVCC violation and
 * requires explicit options chosen by user.
 *
 * TABLE_INSERT_NO_LOGICAL force-disables the emitting of logical decoding
 * information for the tuple. This should solely be used during table rewrites
 * where RelationIsLogicallyLogged(relation) is not yet accurate for the new
 * relation.
 *
 * Note that most of these options will be applied when inserting into the
 * heap's TOAST table, too, if the tuple requires any out-of-line data.
 *
 * The BulkInsertState object (if any; bistate can be NULL for default
 * behavior) is also just passed through to RelationGetBufferForTuple. If
 * `bistate` is provided, table_finish_bulk_insert() needs to be called.
 *
 * On return the slot's tts_rowno and tts_tableOid are updated to reflect the
 * insertion. But note that any toasting of fields within the slot is NOT
 * reflected in the slots contents.
 */
static inline void
table_tuple_insert(Relation rel, TupleTableSlot *slot, int options, struct BulkInsertStateData *bistate)
{
	rel->rd_tableam->tuple_insert(rel, slot, options,
								  bistate);
}

/*
 * Insert multiple tuples into a table.
 *
 * This is like table_tuple_insert(), but inserts multiple tuples in one
 * operation. That's often faster than calling table_tuple_insert() in a loop,
 * because e.g. the AM can reduce WAL logging and page locking overhead.
 *
 * Except for taking `nslots` tuples as input, and an array of TupleTableSlots
 * in `slots`, the parameters for table_multi_insert() are the same as for
 * table_tuple_insert().
 *
 * Note: this leaks memory into the current memory context. You can create a
 * temporary context before calling this, if that's a problem.
 */
static inline void
table_multi_insert(Relation rel, TupleTableSlot **slots, int nslots,
				   int options, struct BulkInsertStateData *bistate)
{
	rel->rd_tableam->multi_insert(rel, slots, nslots,
								  options, bistate);
}

/*
 * Delete a tuple.
 *
 * NB: do not call this directly unless prepared to deal with
 * concurrent-update conditions.  Use simple_table_tuple_delete instead.
 *
 * Input parameters:
 *	relation - table to be modified (caller must hold suitable lock)
 *	tid - TID of tuple to be deleted
 *	crosscheck - if not InvalidSnapshot, also check tuple against this
 *	wait - true if should wait for any conflicting update to commit/abort
 * Output parameters:
 *	tmfd - filled in failure cases (see below)
 *	changingPart - true iff the tuple is being moved to another partition
 *		table due to an update of the partition key. Otherwise, false.
 *
 * Normal, successful return value is TM_Ok, which means we did actually
 * delete it.  Failure return codes are TM_SelfModified, TM_Updated, and
 * TM_BeingModified (the last only possible if wait == false).
 *
 * In the failure cases, the routine fills *tmfd with the tuple's t_xmax,
 * and, if possible, t_cmax.  See comments for struct TM_FailureData for
 * additional info.
 */
static inline TM_Result
table_tuple_delete(Relation rel, const struct RowKey *key, Snapshot snapshot, Snapshot crosscheck, bool wait,
				   TM_FailureData *tmfd, bool changingPart)
{
	return rel->rd_tableam->tuple_delete(rel, key, snapshot, crosscheck,
										 wait, tmfd, changingPart);
}

/*
 * Update a tuple.
 *
 * NB: do not call this directly unless you are prepared to deal with
 * concurrent-update conditions.  Use simple_table_tuple_update instead.
 *
 * Input parameters:
 *	relation - table to be modified (caller must hold suitable lock)
 *	otid - TID of old tuple to be replaced
 *	slot - newly constructed tuple data to store
 *	crosscheck - if not InvalidSnapshot, also check old tuple against this
 *	wait - true if should wait for any conflicting update to commit/abort
 * Output parameters:
 *	tmfd - filled in failure cases (see below)
 *	lockmode - filled with lock mode acquired on tuple
 *	update_indexes - in success cases this is set if new index entries
 *		are required for this tuple; see TU_UpdateIndexes
 *
 * Normal, successful return value is TM_Ok, which means we did actually
 * update it.  Failure return codes are TM_SelfModified, TM_Updated, and
 * TM_BeingModified (the last only possible if wait == false).
 *
 * On success, the slot's tts_tableOid is updated to match the new stored
 * tuple.  However, any TOAST changes in the new tuple's data are not
 * reflected into *newtup.
 *
 * In the failure cases, the routine fills *tmfd with the tuple's t_xmax,
 * and, if possible, t_cmax.  See comments for struct TM_FailureData for
 * additional info.
 */
static inline TM_Result
table_tuple_update(Relation rel, const struct RowKey *key, TupleTableSlot *slot,
				   Snapshot snapshot, Snapshot crosscheck,
				   bool wait, TM_FailureData *tmfd, LockTupleMode *lockmode,
				   TU_UpdateIndexes *update_indexes)
{
	return rel->rd_tableam->tuple_update(rel, key, slot,
										 snapshot, crosscheck,
										 wait, tmfd,
										 lockmode, update_indexes);
}

/*
 * Lock a tuple in the specified mode.
 *
 * Input parameters:
 *	relation: relation containing tuple (caller must hold suitable lock)
 *	tid: TID of tuple to lock (updated if an update chain was followed)
 *	snapshot: snapshot to use for visibility determinations
 *	mode: lock mode desired
 *	wait_policy: what to do if tuple lock is not available
 *	flags:
 *		If TUPLE_LOCK_FLAG_LOCK_UPDATE_IN_PROGRESS, follow the update chain to
 *		also lock descendant tuples if lock modes don't conflict.
 *		If TUPLE_LOCK_FLAG_FIND_LAST_VERSION, follow the update chain and lock
 *		latest version.
 *
 * Output parameters:
 *	*slot: contains the target tuple
 *	*tmfd: filled in failure cases (see below)
 *
 * Function result may be:
 *	TM_Ok: lock was successfully acquired
 *	TM_Invisible: lock failed because tuple was never visible to us
 *	TM_SelfModified: lock failed because tuple updated by self
 *	TM_Updated: lock failed because tuple updated by other xact
 *	TM_Deleted: lock failed because tuple deleted by other xact
 *	TM_WouldBlock: lock couldn't be acquired and wait_policy is skip
 *
 * In the failure cases other than TM_Invisible and TM_Deleted, the routine
 * fills *tmfd with the tuple's t_xmax, and, if possible, t_cmax.
 * Additionally, in both success and failure cases, tmfd->traversed is set if
 * an update chain was followed.  See comments for struct TM_FailureData for
 * additional info.
 */
static inline TM_Result
table_tuple_lock(Relation rel, const struct RowKey *key, Snapshot snapshot,
				 TupleTableSlot *slot, LockTupleMode mode,
				 LockWaitPolicy wait_policy, uint8 flags,
				 TM_FailureData *tmfd)
{
	return rel->rd_tableam->tuple_lock(rel, key, snapshot, slot,
									   mode, wait_policy,
									   flags, tmfd);
}

/*
 * Perform operations necessary to complete insertions made via
 * tuple_insert and multi_insert with a BulkInsertState specified.
 */
static inline void
table_finish_bulk_insert(Relation rel, int options)
{
	/* optional callback */
	if (rel->rd_tableam && rel->rd_tableam->finish_bulk_insert)
		rel->rd_tableam->finish_bulk_insert(rel, options);
}


/* ------------------------------------------------------------------------
 * DDL related functionality.
 * ------------------------------------------------------------------------
 */

/*
 * Create storage for `rel` in `newrlocator`, with persistence set to
 * `persistence`.
 *
 * This is used both during relation creation and various DDL operations to
 * create new rel storage that can be filled from scratch.  When creating
 * new storage for an existing relfilelocator, this should be called before the
 * relcache entry has been updated.
 *
 * *freezeXid, *minmulti are set to the xid / multixact horizon for the table
 * that pg_class.{relfrozenxid, relminmxid} have to be set to.
 */
static inline void
table_relation_set_new_filelocator(Relation rel,
								   const RelFileLocator *newrlocator,
								   char persistence,
								   TransactionId *freezeXid,
								   MultiXactId *minmulti)
{
	rel->rd_tableam->relation_set_new_filelocator(rel, newrlocator,
												  persistence, freezeXid,
												  minmulti);
}

/*
 * Remove all table contents from `rel`, in a non-transactional manner.
 * Non-transactional meaning that there's no need to support rollbacks. This
 * commonly only is used to perform truncations for relation storage created in
 * the current transaction.
 */
static inline void
table_relation_nontransactional_truncate(Relation rel)
{
	rel->rd_tableam->relation_nontransactional_truncate(rel);
}

/*
 * Copy data from `rel` into the new relfilelocator `newrlocator`. The new
 * relfilelocator may not have storage associated before this function is
 * called. This is only supposed to be used for low level operations like
 * changing a relation's tablespace.
 */
static inline void
table_relation_copy_data(Relation rel, const RelFileLocator *newrlocator)
{
	rel->rd_tableam->relation_copy_data(rel, newrlocator);
}

/*
 * Copy data from `OldTable` into `NewTable`, as part of a CLUSTER or VACUUM
 * FULL.
 *
 * Additional Input parameters:
 * - use_sort - if true, the table contents are sorted appropriate for
 *   `OldIndex`; if false and OldIndex is not InvalidOid, the data is copied
 *   in that index's order; if false and OldIndex is InvalidOid, no sorting is
 *   performed
 * - OldIndex - see use_sort
 * - OldestXmin - computed by the caller, even when
 *   not needed for the relation's AM
 * - *xid_cutoff - ditto
 * - *multi_cutoff - ditto
 *
 * Output parameters:
 * - *xid_cutoff - rel's new relfrozenxid value, may be invalid
 * - *multi_cutoff - rel's new relminmxid value, may be invalid
 * - *tups_vacuumed - stats, for logging, if appropriate for AM
 * - *tups_recently_dead - stats, for logging, if appropriate for AM
 */
static inline void
table_relation_copy_for_cluster(Relation OldTable, Relation NewTable,
								Relation OldIndex,
								bool use_sort,
								TransactionId OldestXmin,
								TransactionId *xid_cutoff,
								MultiXactId *multi_cutoff,
								double *num_tuples,
								double *tups_vacuumed,
								double *tups_recently_dead)
{
	OldTable->rd_tableam->relation_copy_for_cluster(OldTable, NewTable, OldIndex,
													use_sort, OldestXmin,
													xid_cutoff, multi_cutoff,
													num_tuples, tups_vacuumed,
													tups_recently_dead);
}

/*
 * table_index_build_scan - scan the table to find tuples to be indexed
 *
 * This is called back from an access-method-specific index build procedure
 * after the AM has done whatever setup it needs.  The parent table relation
 * is scanned to find tuples that should be entered into the index.  Each
 * such tuple is passed to the AM's callback routine, which does the right
 * things to add it to the new index.  After we return, the AM's index
 * build procedure does whatever cleanup it needs.
 *
 * The total count of live tuples is returned.  This is for updating pg_class
 * statistics.  (It's annoying not to be able to do that here, but we want to
 * merge that update with others; see index_update_stats.)  Note that the
 * index AM itself must keep track of the number of index tuples; we don't do
 * so here because the AM might reject some of the tuples for its own reasons,
 * such as being unable to store NULLs.
 *
 * If 'progress', the PROGRESS_SCAN_BLOCKS_TOTAL counter is updated when
 * starting the scan, and PROGRESS_SCAN_BLOCKS_DONE is updated as we go along.
 *
 * A side effect is to set indexInfo->ii_BrokenHotChain to true if we detect
 * any potentially broken HOT chains.  Currently, we set this if there are any
 * RECENTLY_DEAD or DELETE_IN_PROGRESS entries in a HOT chain, without trying
 * very hard to detect whether they're really incompatible with the chain tip.
 * This only really makes sense for heap AM, it might need to be generalized
 * for other AMs later.
 */
static inline double
table_index_build_scan(Relation table_rel,
					   Relation index_rel,
					   struct IndexInfo *index_info,
					   bool progress,
					   IndexBuildCallback callback,
					   void *callback_state)
{
	return table_rel->rd_tableam->index_build_range_scan(table_rel,
														 index_rel,
														 index_info,
														 false,
														 progress,
														 0,
														 InvalidBlockNumber,
														 callback,
														 callback_state);
}

/*
 * As table_index_build_scan(), except that instead of scanning the complete
 * table, only the given number of blocks are scanned.  Scan to end-of-rel can
 * be signaled by passing InvalidBlockNumber as numblocks.
 *
 * When "anyvisible" mode is requested, all tuples visible to any transaction
 * are indexed and counted as live, including those inserted or deleted by
 * transactions that are still in progress.
 */
static inline double
table_index_build_range_scan(Relation table_rel,
							 Relation index_rel,
							 struct IndexInfo *index_info,
							 bool anyvisible,
							 bool progress,
							 BlockNumber start_blockno,
							 BlockNumber numblocks,
							 IndexBuildCallback callback,
							 void *callback_state)
{
	return table_rel->rd_tableam->index_build_range_scan(table_rel,
														 index_rel,
														 index_info,
														 anyvisible,
														 progress,
														 start_blockno,
														 numblocks,
														 callback,
														 callback_state);
}

/*
 * table_index_validate_scan - second table scan for concurrent index build
 *
 * See validate_index() for an explanation.
 */
static inline void
table_index_validate_scan(Relation table_rel,
						  Relation index_rel,
						  struct IndexInfo *index_info,
						  Snapshot snapshot,
						  struct ValidateIndexState *state)
{
	table_rel->rd_tableam->index_validate_scan(table_rel,
											   index_rel,
											   index_info,
											   snapshot,
											   state);
}


/* ----------------------------------------------------------------------------
 * Miscellaneous functionality
 * ----------------------------------------------------------------------------
 */

/*
 * Return the current size of `rel` in bytes. If `forkNumber` is
 * InvalidForkNumber, return the relation's overall size, otherwise the size
 * for the indicated fork.
 *
 * Note that the overall size might not be the equivalent of the sum of sizes
 * for the individual forks for some AMs, e.g. because the AMs storage does
 * not neatly map onto the builtin types of forks.
 */
static inline uint64
table_relation_size(Relation rel, ForkNumber forkNumber)
{
	return rel->rd_tableam->relation_size(rel, forkNumber);
}

/*
 * table_relation_needs_toast_table - does this relation need a toast table?
 */
static inline bool
table_relation_needs_toast_table(Relation rel)
{
	return rel->rd_tableam->relation_needs_toast_table(rel);
}

/*
 * Return the OID of the AM that should be used to implement the TOAST table
 * for this relation.
 */
static inline Oid
table_relation_toast_am(Relation rel)
{
	return rel->rd_tableam->relation_toast_am(rel);
}


/* ----------------------------------------------------------------------------
 * Planner related functionality
 * ----------------------------------------------------------------------------
 */

/*
 * Estimate the current size of the relation, as an AM specific workhorse for
 * estimate_rel_size(). Look there for an explanation of the parameters.
 */
static inline void
table_relation_estimate_size(Relation rel, int32 *attr_widths,
							 BlockNumber *pages, double *tuples,
							 double *allvisfrac)
{
	rel->rd_tableam->relation_estimate_size(rel, attr_widths, pages, tuples,
											allvisfrac);
}


/* ----------------------------------------------------------------------------
 * Executor related functionality
 * ----------------------------------------------------------------------------
 */

/*
 * Fetch / check / return tuples as part of a bitmap table scan. `scan` needs
 * to have been started via table_beginscan_bm(). Fetch the next tuple of a
 * bitmap table scan into `slot` and return true if a visible tuple was found,
 * false otherwise.
 *
 * `recheck` is set by the table AM to indicate whether or not the tuple in
 * `slot` should be rechecked. Tuples from lossy pages will always need to be
 * rechecked, but some non-lossy pages' tuples may also require recheck.
 *
 * `lossy_pages` is incremented if the block's representation in the bitmap is
 * lossy; otherwise, `exact_pages` is incremented.
 */
static inline bool
table_scan_bitmap_next_tuple(TableScanDesc scan,
							 TupleTableSlot *slot,
							 bool *recheck,
							 uint64 *lossy_pages,
							 uint64 *exact_pages)
{
	return scan->rs_rd->rd_tableam->scan_bitmap_next_tuple(scan,
														   slot,
														   recheck,
														   lossy_pages,
														   exact_pages);
}

/*
 * Prepare to fetch tuples from the next block in a sample scan. Returns false
 * if the sample scan is finished, true otherwise. `scan` needs to have been
 * started via table_beginscan_sampling().
 *
 * This will call the TsmRoutine's NextSampleBlock() callback if necessary
 * (i.e. NextSampleBlock is not NULL), or perform a sequential scan over the
 * underlying relation.
 */
static inline bool
table_scan_sample_next_block(TableScanDesc scan,
							 struct SampleScanState *scanstate)
{
	return scan->rs_rd->rd_tableam->scan_sample_next_block(scan, scanstate);
}

/*
 * Fetch the next sample tuple into `slot` and return true if a visible tuple
 * was found, false otherwise. table_scan_sample_next_block() needs to
 * previously have selected a block (i.e. returned true), and no previous
 * table_scan_sample_next_tuple() for the same block may have returned false.
 *
 * This will call the TsmRoutine's NextSampleTuple() callback.
 */
static inline bool
table_scan_sample_next_tuple(TableScanDesc scan,
							 struct SampleScanState *scanstate,
							 TupleTableSlot *slot)
{
	return scan->rs_rd->rd_tableam->scan_sample_next_tuple(scan, scanstate,
														   slot);
}


/* ----------------------------------------------------------------------------
 * Functions to make modifications a bit simpler.
 * ----------------------------------------------------------------------------
 */

extern void simple_table_tuple_insert(Relation rel, TupleTableSlot *slot);
extern void simple_table_tuple_delete(Relation rel, const struct RowKey *key,
									  Snapshot snapshot);
extern void simple_table_tuple_update(Relation rel, const struct RowKey *key,
									  TupleTableSlot *slot, Snapshot snapshot,
									  TU_UpdateIndexes *update_indexes);


/* ----------------------------------------------------------------------------
 * Functions in tableamapi.c
 * ----------------------------------------------------------------------------
 */

extern const TableAmRoutine *GetTableAmRoutine(Oid amhandler);

/* ----------------------------------------------------------------------------
 * Functions in access/sm/smtable.c
 * ----------------------------------------------------------------------------
 */

extern const TableAmRoutine *smtable_routine(void);

#endif							/* TABLEAM_H */

/*-------------------------------------------------------------------------
 *
 * execIndexing.c
 *	  routines for opening and closing a result relation's indexes.
 *
 * digres: sm writes a table's indexes with its row, and checks its unique
 * ones then (access/smstore.h); ON CONFLICT's arbiter is sm's too.  No
 * index tuple is inserted here, and no exclusion constraint or deferred
 * unique constraint is checked, as digres has neither (UNSUPPORTED.md).
 *
 *
 * Portions Copyright (c) 1996-2025, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/executor/execIndexing.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/smstore.h"
#include "catalog/index.h"
#include "executor/executor.h"

/* ----------------------------------------------------------------
 *		ExecOpenIndices
 *
 *		Find the indices associated with a result relation, open them,
 *		and save information about them in the result ResultRelInfo.
 *
 *		At entry, caller has already opened and locked
 *		resultRelInfo->ri_RelationDesc.
 * ----------------------------------------------------------------
 */
void ExecOpenIndices(ResultRelInfo *resultRelInfo, bool speculative) {
  Relation resultRelation = resultRelInfo->ri_RelationDesc;
  List *indexoidlist;
  ListCell *l;
  int len, i;
  RelationPtr relationDescs;
  IndexInfo **indexInfoArray;

  resultRelInfo->ri_NumIndices = 0;

  /* fast path if no indexes */
  if (!RelationGetForm(resultRelation)->relhasindex)
    return;

  /*
   * Get cached list of index OIDs
   */
  indexoidlist = RelationGetIndexList(resultRelation);
  len = list_length(indexoidlist);
  if (len == 0)
    return;

  /* This Assert will fail if ExecOpenIndices is called twice */
  Assert(resultRelInfo->ri_IndexRelationDescs == NULL);

  /*
   * allocate space for result arrays
   */
  relationDescs = (RelationPtr)palloc(len * sizeof(Relation));
  indexInfoArray = (IndexInfo **)palloc(len * sizeof(IndexInfo *));

  resultRelInfo->ri_NumIndices = len;
  resultRelInfo->ri_IndexRelationDescs = relationDescs;
  resultRelInfo->ri_IndexRelationInfo = indexInfoArray;

  /*
   * For each index, open the index relation and save pg_index info. We
   * acquire RowExclusiveLock, signifying we will update the index.
   *
   * Note: we do this even if the index is not indisready; it's not worth
   * the trouble to optimize for the case where it isn't.
   */
  i = 0;
  foreach (l, indexoidlist) {
    Oid indexOid = lfirst_oid(l);
    Relation indexDesc;
    IndexInfo *ii;

    indexDesc = index_open(indexOid, RowExclusiveLock);

    /* extract index key information from the index's pg_index info */
    ii = BuildIndexInfo(indexDesc);

    /*
     * If the indexes are to be used for speculative insertion, add extra
     * information required by unique index entries.
     */
    if (speculative && ii->ii_Unique && !indexDesc->rd_index->indisexclusion)
      BuildSpeculativeIndexInfo(indexDesc, ii);

    relationDescs[i] = indexDesc;
    indexInfoArray[i] = ii;
    i++;
  }

  list_free(indexoidlist);
}

/* ----------------------------------------------------------------
 *		ExecCloseIndices
 *
 *		Close the index relations stored in resultRelInfo
 * ----------------------------------------------------------------
 */
void ExecCloseIndices(ResultRelInfo *resultRelInfo) {
  int i;
  int numIndices;
  RelationPtr indexDescs;
  IndexInfo **indexInfos;

  numIndices = resultRelInfo->ri_NumIndices;
  indexDescs = resultRelInfo->ri_IndexRelationDescs;
  indexInfos = resultRelInfo->ri_IndexRelationInfo;

  for (i = 0; i < numIndices; i++) {
    /* This Assert will fail if ExecCloseIndices is called twice */
    Assert(indexDescs[i] != NULL);

    /* Give the index a chance to do some post-insert cleanup */
    index_insert_cleanup(indexDescs[i], indexInfos[i]);

    /* Drop lock acquired by ExecOpenIndices */
    index_close(indexDescs[i], RowExclusiveLock);

    /* Mark the index as closed */
    indexDescs[i] = NULL;
  }

  /*
   * We don't attempt to free the IndexInfo data structures or the arrays,
   * instead assuming that such stuff will be cleaned up automatically in
   * FreeExecutorState.
   */
}

/* ----------------------------------------------------------------
 *		ExecInsertIndexTuples
 *
 *		Insert the index tuples of the row in slot, just written to
 *		the result relation, a table in sm: sm wrote them with the
 *		row, so there is nothing to insert, and no index to recheck.
 * ----------------------------------------------------------------
 */
List *ExecInsertIndexTuples(ResultRelInfo *resultRelInfo, TupleTableSlot *slot,
                            EState *estate, bool update, bool noDupErr,
                            bool *specConflict, List *arbiterIndexes,
                            bool onlySummarizing) {
  Assert(RelationIsSm(resultRelInfo->ri_RelationDesc));
  return NIL;
}

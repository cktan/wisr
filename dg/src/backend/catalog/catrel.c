/*-------------------------------------------------------------------------
 *
 * catrel.c
 *	  The catalog relations: catalogs a query can read (SYSCATALOG.md).
 *
 * digres has no catalog tables.  A catalog relation has a catalog's name,
 * oid and columns, compiled in (cat_relation()), and nothing stored: its
 * table access method makes its rows when a scan begins, from the rows
 * compiled in and from sm's catalog as this transaction sees it, into a
 * tuple store the scan reads.  Every write is refused.  A client's query
 * on pg_namespace comes this way; of the server's own lookups, only a
 * syscache miss in a catalog of compiled-in rows alone does
 * (catrel_is_builtin()).
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
#include "catalog/pg_am_d.h"
#include "catalog/pg_attrdef_d.h"
#include "catalog/pg_attribute_d.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_cast_d.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_description_d.h"
#include "catalog/pg_extension_d.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_proc_d.h"
#include "catalog/dependency.h"
#include "catalog/pg_database_d.h"
#include "catalog/pg_depend_d.h"
#include "catalog/pg_sequence_d.h"
#include "catalog/pg_tablespace_d.h"
#include "catalog/pg_inherits_d.h"
#include "catalog/pg_language_d.h"
#include "catalog/pg_policy_d.h"
#include "catalog/pg_publication_rel_d.h"
#include "catalog/pg_rewrite_d.h"
#include "rewrite/rewriteDefine.h"
#include "rewrite/rewriteSupport.h"
#include "catalog/pg_statistic_ext_d.h"
#include "catalog/pg_trigger_d.h"
#include "catalog/pg_type_d.h"
#include "catalog/pg_auth_members_d.h"
#include "catalog/pg_authid_d.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_index.h"
#include "catalog/pg_namespace_d.h"
#include "catalog/pg_publication_d.h"
#include "catalog/pg_publication_namespace_d.h"
#include "access/stratnum.h"
#include "executor/executor.h"
#include "executor/tuptable.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "commands/defrem.h"
#include "optimizer/optimizer.h"
#include "mb/pg_wchar.h"
#include "nodes/makefuncs.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/datadir.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/pg_global.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/timestamp.h"
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
 * The relations of sm's a scan makes rows for: those of the relation only
 * (smstore_relations_of()), or all of them if only is InvalidOid.
 */
static List *sm_relations(Oid only) {
  return OidIsValid(only) ? smstore_relations_of(only) : smstore_relations();
}

/*
 * The catalog relations a scan makes rows for, and how many in *count: the
 * one whose oid is only, or all of them if only is InvalidOid.
 */
static const cat_relation_t *cat_rels(Oid only, int *count) {
  const cat_relation_t *r;

  if (!OidIsValid(only))
    return cat_relations(count);
  r = cat_relation(only);
  *count = r != NULL ? 1 : 0;
  return r;
}

/*
 * An ACL of two items, granted by owner: owner_privs to owner, and
 * grantee_privs to grantee, a role or PUBLIC, if there are any.
 */
static Datum make_acl_to(Oid owner, AclMode owner_privs, Oid grantee,
                         AclMode grantee_privs) {
  int n = ACL_NO_RIGHTS == grantee_privs ? 1 : 2;
  /* As acl.c's allocacl() makes one: a one-dimensional aclitem array. */
  Acl *acl = (Acl *)palloc0(ACL_N_SIZE(n));
  AclItem *items;

  SET_VARSIZE(acl, ACL_N_SIZE(n));
  acl->ndim = 1;
  acl->dataoffset = 0;
  acl->elemtype = ACLITEMOID;
  ARR_LBOUND(acl)[0] = 1;
  ARR_DIMS(acl)[0] = n;
  items = ACL_DAT(acl);

  items[0].ai_grantee = owner;
  items[0].ai_grantor = owner;
  ACLITEM_SET_PRIVS_GOPTIONS(items[0], owner_privs, ACL_NO_RIGHTS);
  if (ACL_NO_RIGHTS != grantee_privs) {
    items[1].ai_grantee = grantee;
    items[1].ai_grantor = owner;
    ACLITEM_SET_PRIVS_GOPTIONS(items[1], grantee_privs, ACL_NO_RIGHTS);
  }
  return PointerGetDatum(acl);
}

/*
 * An ACL of two items, granted by owner: owner_privs to owner, and
 * public_privs to PUBLIC if there are any.
 */
static Datum make_acl(Oid owner, AclMode owner_privs, AclMode public_privs) {
  return make_acl_to(owner, owner_privs, ACL_ID_PUBLIC, public_privs);
}

/*
 * The ACL of the built-in function oid, owned by owner, as PostgreSQL's
 * initdb leaves it, or (Datum) 0 for none, which lets PUBLIC run it: one
 * initdb takes EXECUTE on from PUBLIC is its owner's alone, and a
 * predefined role's too if initdb grants it that (cat_function_acl()).
 */
static Datum function_acl(Oid oid, Oid owner) {
  const cat_function_acl_t *f = cat_function_acl(oid);

  if (NULL == f)
    return (Datum)0;
  return make_acl_to(owner, ACL_EXECUTE, f->grantee,
                     OidIsValid(f->grantee) ? ACL_EXECUTE : ACL_NO_RIGHTS);
}

/*
 * pg_namespace's rows: those compiled in, then sm's schemas not among them.
 * The compiled-in ones are as initdb leaves PostgreSQL 18's: pg_catalog's
 * USAGE is PUBLIC's, and public is pg_database_owner's, its USAGE PUBLIC's.
 * Every other is the superuser's, with no ACL kept.
 */
static void gen_pg_namespace(Tuplestorestate *ts, TupleDesc desc) {
  int count;
  const cat_namespace_t *rows = cat_namespaces(&count);
  Datum values[4];
  bool nulls[4] = {false, false, false, true};
  Acl *acl;
  ListCell *l;

  for (int i = 0; i < count; i++) {
    Oid owner = rows[i].nspowner;

    /* As ALTER SCHEMA ... OWNER TO left it, in pg_global, if it did. */
    (void)pg_global_owner_of(rows[i].oid, &owner);
    values[0] = ObjectIdGetDatum(rows[i].oid);
    values[1] = name_datum(rows[i].nspname);
    values[2] = ObjectIdGetDatum(owner);
    nulls[3] = true;
    values[3] = (Datum)0;
    /* As GRANT and REVOKE left it, in pg_global, if they did. */
    if (NULL !=
        (acl = pg_global_acl(pg_global_dbid(MyDatabaseId), rows[i].oid))) {
      nulls[3] = false;
      values[3] = PointerGetDatum(acl);
    } else if (PG_CATALOG_NAMESPACE == rows[i].oid ||
               PG_PUBLIC_NAMESPACE == rows[i].oid) {
      nulls[3] = false;
      values[3] = make_acl(owner, ACL_ALL_RIGHTS_SCHEMA, ACL_USAGE);
    }
    tuplestore_putvalues(ts, desc, values, nulls);
  }
  nulls[3] = true;
  foreach (l, smstore_namespace_names()) {
    const char *name = (const char *)lfirst(l);
    Oid oid = smstore_namespace_oid(name);

    if (cat_namespace(oid) != NULL)
      continue;
    values[0] = ObjectIdGetDatum(oid);
    values[1] = name_datum(name);
    values[2] = ObjectIdGetDatum(pg_global_owner(oid));
    acl = pg_global_acl(pg_global_dbid(MyDatabaseId), oid);
    nulls[3] = NULL == acl;
    values[3] = PointerGetDatum(acl);
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/*
 * A row of pg_class, into ts: a relation's own columns given, the rest as
 * digres has them -- owned by the superuser, in no tablespace, with no
 * statistics, no ACL, and no options but a view's.  A view has its rule,
 * and no storage.  A table with a foreign key, or that one references, has
 * triggers: the relation cache gives it RI triggers (sm_fkey_triggers()).
 */
static void class_row(Tuplestorestate *ts, TupleDesc desc, Oid oid,
                      const char *name, Oid nspoid, Oid reltype, Oid owner,
                      char kind, int natts, bool hasindex, bool temp,
                      Datum reloptions = (Datum)0, bool hastriggers = false,
                      Datum acl = (Datum)0) {
  Datum values[Natts_pg_class];
  bool nulls[Natts_pg_class];

  memset(values, 0, sizeof(values));
  memset(nulls, false, sizeof(nulls));
  values[Anum_pg_class_oid - 1] = ObjectIdGetDatum(oid);
  values[Anum_pg_class_relname - 1] = name_datum(name);
  values[Anum_pg_class_relnamespace - 1] = ObjectIdGetDatum(nspoid);
  values[Anum_pg_class_reltype - 1] = ObjectIdGetDatum(reltype);
  values[Anum_pg_class_relowner - 1] = ObjectIdGetDatum(owner);
  values[Anum_pg_class_relam - 1] =
      ObjectIdGetDatum(kind == RELKIND_RELATION ? HEAP_TABLE_AM_OID
                       : kind == RELKIND_INDEX  ? BTREE_AM_OID
                                                : InvalidOid);
  values[Anum_pg_class_relfilenode - 1] =
      ObjectIdGetDatum(kind == RELKIND_VIEW ? InvalidOid : oid);
  /* Never analyzed, as PostgreSQL says it: no pages, -1 rows. */
  values[Anum_pg_class_reltuples - 1] = Float4GetDatum(-1);
  values[Anum_pg_class_relhasindex - 1] = BoolGetDatum(hasindex);
  values[Anum_pg_class_relpersistence - 1] =
      CharGetDatum(temp ? RELPERSISTENCE_TEMP : RELPERSISTENCE_PERMANENT);
  values[Anum_pg_class_relkind - 1] = CharGetDatum(kind);
  values[Anum_pg_class_relnatts - 1] = Int16GetDatum(natts);
  values[Anum_pg_class_relhasrules - 1] = BoolGetDatum(kind == RELKIND_VIEW);
  values[Anum_pg_class_relhastriggers - 1] = BoolGetDatum(hastriggers);
  values[Anum_pg_class_relispopulated - 1] = BoolGetDatum(true);
  values[Anum_pg_class_relreplident - 1] =
      CharGetDatum(kind == RELKIND_RELATION ? REPLICA_IDENTITY_DEFAULT
                                            : REPLICA_IDENTITY_NOTHING);
  values[Anum_pg_class_relacl - 1] = acl;
  nulls[Anum_pg_class_relacl - 1] = acl == (Datum)0;
  values[Anum_pg_class_reloptions - 1] = reloptions;
  nulls[Anum_pg_class_reloptions - 1] = reloptions == (Datum)0;
  nulls[Anum_pg_class_relpartbound - 1] = true;
  tuplestore_putvalues(ts, desc, values, nulls);
}

/*
 * The owner of the relation r of sm's, pg_global's (utils/init/pg_global.c):
 * an index's is its table's, and a sequence's a column owns its table's.
 */
static Oid relation_owner(const smstore_relation_t *r) {
  if (RELKIND_INDEX == r->kind)
    return pg_global_owner(r->table);
  if (RELKIND_SEQUENCE == r->kind && OidIsValid(r->owner))
    return pg_global_owner(r->owner);
  return pg_global_owner(r->oid);
}

/*
 * The ACL of the relation r of sm's, pg_global's, or (Datum) 0 if it keeps
 * none: an index has none.
 */
static Datum relation_acl(const smstore_relation_t *r) {
  Acl *acl = RELKIND_INDEX == r->kind
                 ? NULL
                 : pg_global_acl(pg_global_dbid(MyDatabaseId), r->oid);

  return NULL == acl ? (Datum)0 : PointerGetDatum(acl);
}

/*
 * pg_class's rows: the catalog relations, in pg_catalog, then sm's tables,
 * indexes, views and sequences, the session's temporary tables among them.
 */
static void gen_pg_class(Tuplestorestate *ts, TupleDesc desc, Oid only) {
  int count;
  const cat_relation_t *cats = cat_rels(only, &count);
  ListCell *l;

  /*
   * A catalog relation is readable by PUBLIC, as initdb grants PostgreSQL's,
   * but pg_authid, which holds the passwords.
   */
  for (int i = 0; i < count; i++)
    class_row(
        ts, desc, cats[i].oid, cats[i].name, PG_CATALOG_NAMESPACE,
        cats[i].rowtype, BOOTSTRAP_SUPERUSERID, RELKIND_RELATION, cats[i].natts,
        false, false, (Datum)0, false,
        make_acl(BOOTSTRAP_SUPERUSERID, ACL_ALL_RIGHTS_RELATION,
                 AuthIdRelationId == cats[i].oid ? ACL_NO_RIGHTS : ACL_SELECT));
  foreach (l, sm_relations(only)) {
    smstore_relation_t *r = (smstore_relation_t *)lfirst(l);

    class_row(ts, desc, r->oid, r->name, r->nspoid, InvalidOid,
              relation_owner(r), r->kind, r->natts, r->hasindex, r->temp,
              smstore_view_reloptions(r->options),
              r->nfkeys > 0 || r->referenced, relation_acl(r));
  }
}

/*
 * A row of pg_index, into ts, for the index r of sm's: a btree on columns,
 * each with its type's default operator class and collation.  No index is
 * an exclusion constraint, deferred, clustered or the replica identity.
 */
static void index_row(Tuplestorestate *ts, TupleDesc desc,
                      const smstore_relation_t *r) {
  Datum values[Natts_pg_index];
  bool nulls[Natts_pg_index];
  Oid *colls = (Oid *)palloc0(r->nkeys * sizeof(Oid));
  Oid *classes = (Oid *)palloc0(r->nkeys * sizeof(Oid));
  int16 *options = (int16 *)palloc0(r->nkeys * sizeof(int16));

  for (int k = 0; k < r->nkeys; k++) {
    colls[k] = get_typcollation(r->typids[k]);
    classes[k] = GetDefaultOpClass(r->typids[k], BTREE_AM_OID);
    options[k] = (r->desc[k] ? INDOPTION_DESC : 0) |
                 (r->nulls_first[k] ? INDOPTION_NULLS_FIRST : 0);
  }
  memset(values, 0, sizeof(values));
  memset(nulls, false, sizeof(nulls));
  values[Anum_pg_index_indexrelid - 1] = ObjectIdGetDatum(r->oid);
  values[Anum_pg_index_indrelid - 1] = ObjectIdGetDatum(r->table);
  values[Anum_pg_index_indnatts - 1] = Int16GetDatum(r->natts);
  values[Anum_pg_index_indnkeyatts - 1] = Int16GetDatum(r->nkeys);
  values[Anum_pg_index_indisunique - 1] = BoolGetDatum(r->unique);
  values[Anum_pg_index_indisprimary - 1] = BoolGetDatum(r->primary);
  values[Anum_pg_index_indimmediate - 1] = BoolGetDatum(true);
  values[Anum_pg_index_indisvalid - 1] = BoolGetDatum(r->valid);
  values[Anum_pg_index_indisready - 1] = BoolGetDatum(true);
  values[Anum_pg_index_indislive - 1] = BoolGetDatum(true);
  values[Anum_pg_index_indkey - 1] =
      PointerGetDatum(buildint2vector(r->attnums, r->natts));
  values[Anum_pg_index_indcollation - 1] =
      PointerGetDatum(buildoidvector(colls, r->nkeys));
  values[Anum_pg_index_indclass - 1] =
      PointerGetDatum(buildoidvector(classes, r->nkeys));
  values[Anum_pg_index_indoption - 1] =
      PointerGetDatum(buildint2vector(options, r->nkeys));
  nulls[Anum_pg_index_indexprs - 1] = true;
  nulls[Anum_pg_index_indpred - 1] = true;
  /* A partial or an expression index's, as nodeToString() wrote them. */
  if (r->eval != NULL) {
    List *exprs, *pred, *names;

    smstore_index_eval_parts(r->eval, &exprs, &pred, &names);
    if (exprs != NIL) {
      values[Anum_pg_index_indexprs - 1] =
          CStringGetTextDatum(nodeToString(exprs));
      nulls[Anum_pg_index_indexprs - 1] = false;
    }
    /* The predicate one clause, an AND of its clauses, as index_create()'s. */
    if (pred != NIL) {
      values[Anum_pg_index_indpred - 1] =
          CStringGetTextDatum(nodeToString(make_ands_explicit(pred)));
      nulls[Anum_pg_index_indpred - 1] = false;
    }
  }
  tuplestore_putvalues(ts, desc, values, nulls);
}

/* pg_index's rows: an index of sm's, a row. */
static void gen_pg_index(Tuplestorestate *ts, TupleDesc desc, Oid only) {
  ListCell *l;

  foreach (l, sm_relations(only)) {
    smstore_relation_t *r = (smstore_relation_t *)lfirst(l);

    if (r->kind == RELKIND_INDEX)
      index_row(ts, desc, r);
  }
}

/* An int2[] of n column numbers, as pg_constraint's conkey holds them. */
static Datum int2_array(const AttrNumber *attnums, int n) {
  Datum *elems = (Datum *)palloc(sizeof(Datum) * Max(n, 1));

  for (int i = 0; i < n; i++)
    elems[i] = Int16GetDatum(attnums[i]);
  return PointerGetDatum(construct_array_builtin(elems, n, INT2OID));
}

/* An oid[] of n operators, as pg_constraint's conpfeqop holds them. */
static Datum oid_array(const Oid *oids, int n) {
  Datum *elems = (Datum *)palloc(sizeof(Datum) * Max(n, 1));

  for (int i = 0; i < n; i++)
    elems[i] = ObjectIdGetDatum(oids[i]);
  return PointerGetDatum(construct_array_builtin(elems, n, OIDOID));
}

/*
 * A foreign key's row of pg_constraint, into ts, from what sm keeps of it
 * (../sm/FOREIGNKEY.md): in the namespace of its table, enforced, local and
 * inheritable, with no parent.
 */
static void fkey_row(Tuplestorestate *ts, TupleDesc desc, Oid nspoid,
                     const smstore_fkey_t *fk) {
  Datum values[Natts_pg_constraint];
  bool nulls[Natts_pg_constraint];

  memset(values, 0, sizeof(values));
  memset(nulls, false, sizeof(nulls));
  values[Anum_pg_constraint_oid - 1] = ObjectIdGetDatum(fk->oid);
  values[Anum_pg_constraint_conname - 1] = NameGetDatum(&fk->name);
  values[Anum_pg_constraint_connamespace - 1] = ObjectIdGetDatum(nspoid);
  values[Anum_pg_constraint_contype - 1] = CharGetDatum(CONSTRAINT_FOREIGN);
  values[Anum_pg_constraint_condeferrable - 1] = BoolGetDatum(fk->deferrable);
  values[Anum_pg_constraint_condeferred - 1] = BoolGetDatum(fk->deferred);
  values[Anum_pg_constraint_conenforced - 1] = BoolGetDatum(true);
  values[Anum_pg_constraint_convalidated - 1] = BoolGetDatum(fk->validated);
  values[Anum_pg_constraint_conrelid - 1] = ObjectIdGetDatum(fk->table);
  values[Anum_pg_constraint_contypid - 1] = ObjectIdGetDatum(InvalidOid);
  values[Anum_pg_constraint_conindid - 1] = ObjectIdGetDatum(fk->ref_index);
  values[Anum_pg_constraint_conparentid - 1] = ObjectIdGetDatum(InvalidOid);
  values[Anum_pg_constraint_confrelid - 1] = ObjectIdGetDatum(fk->ref_table);
  values[Anum_pg_constraint_confupdtype - 1] = CharGetDatum(fk->on_update);
  values[Anum_pg_constraint_confdeltype - 1] = CharGetDatum(fk->on_delete);
  values[Anum_pg_constraint_confmatchtype - 1] = CharGetDatum(fk->match);
  values[Anum_pg_constraint_conislocal - 1] = BoolGetDatum(true);
  values[Anum_pg_constraint_coninhcount - 1] = Int16GetDatum(0);
  values[Anum_pg_constraint_connoinherit - 1] = BoolGetDatum(false);
  values[Anum_pg_constraint_conperiod - 1] = BoolGetDatum(false);
  values[Anum_pg_constraint_conkey - 1] = int2_array(fk->attnums, fk->nkeys);
  values[Anum_pg_constraint_confkey - 1] =
      int2_array(fk->ref_attnums, fk->nkeys);
  values[Anum_pg_constraint_conpfeqop - 1] =
      oid_array(fk->pf_eq_oprs, fk->nkeys);
  values[Anum_pg_constraint_conppeqop - 1] =
      oid_array(fk->pp_eq_oprs, fk->nkeys);
  values[Anum_pg_constraint_conffeqop - 1] =
      oid_array(fk->ff_eq_oprs, fk->nkeys);
  if (fk->ndelsetcols > 0)
    values[Anum_pg_constraint_confdelsetcols - 1] =
        int2_array(fk->delsetcols, fk->ndelsetcols);
  else
    nulls[Anum_pg_constraint_confdelsetcols - 1] = true;
  nulls[Anum_pg_constraint_conexclop - 1] = true;
  nulls[Anum_pg_constraint_conbin - 1] = true;
  tuplestore_putvalues(ts, desc, values, nulls);
}

/*
 * A primary key's or unique constraint's row of pg_constraint, into ts, for
 * r, the index of sm's it made: named as its index, on the index's key
 * columns, not deferrable, enforced and validated.
 */
static void key_row(Tuplestorestate *ts, TupleDesc desc,
                    const smstore_relation_t *r) {
  Datum values[Natts_pg_constraint];
  bool nulls[Natts_pg_constraint];
  AttrNumber attnums[INDEX_MAX_KEYS];

  memset(values, 0, sizeof(values));
  memset(nulls, true, sizeof(nulls));
  for (int i = 0; i < Natts_pg_constraint; i++)
    nulls[i] = i >= Anum_pg_constraint_conkey - 1;
  for (int i = 0; i < r->nkeys; i++)
    attnums[i] = r->attnums[i];
  values[Anum_pg_constraint_oid - 1] = ObjectIdGetDatum(r->oid);
  values[Anum_pg_constraint_conname - 1] = name_datum(r->name);
  values[Anum_pg_constraint_connamespace - 1] = ObjectIdGetDatum(r->nspoid);
  values[Anum_pg_constraint_contype - 1] =
      CharGetDatum(r->primary ? CONSTRAINT_PRIMARY : CONSTRAINT_UNIQUE);
  values[Anum_pg_constraint_condeferrable - 1] = BoolGetDatum(false);
  values[Anum_pg_constraint_condeferred - 1] = BoolGetDatum(false);
  values[Anum_pg_constraint_conenforced - 1] = BoolGetDatum(true);
  values[Anum_pg_constraint_convalidated - 1] = BoolGetDatum(true);
  values[Anum_pg_constraint_conrelid - 1] = ObjectIdGetDatum(r->table);
  values[Anum_pg_constraint_contypid - 1] = ObjectIdGetDatum(InvalidOid);
  values[Anum_pg_constraint_conindid - 1] = ObjectIdGetDatum(r->oid);
  values[Anum_pg_constraint_conparentid - 1] = ObjectIdGetDatum(InvalidOid);
  values[Anum_pg_constraint_confrelid - 1] = ObjectIdGetDatum(InvalidOid);
  values[Anum_pg_constraint_confupdtype - 1] = CharGetDatum(' ');
  values[Anum_pg_constraint_confdeltype - 1] = CharGetDatum(' ');
  values[Anum_pg_constraint_confmatchtype - 1] = CharGetDatum(' ');
  values[Anum_pg_constraint_conislocal - 1] = BoolGetDatum(true);
  values[Anum_pg_constraint_coninhcount - 1] = Int16GetDatum(0);
  values[Anum_pg_constraint_connoinherit - 1] = BoolGetDatum(true);
  values[Anum_pg_constraint_conperiod - 1] = BoolGetDatum(false);
  values[Anum_pg_constraint_conkey - 1] = int2_array(attnums, r->nkeys);
  nulls[Anum_pg_constraint_conkey - 1] = false;
  tuplestore_putvalues(ts, desc, values, nulls);
}

/*
 * pg_constraint's rows, for the tables of sm's, those of the table only if
 * it is given (a scan by conrelid), else every table's: a primary key's or
 * unique constraint's, its index's (key_row()), and each foreign key's.  A
 * unique index CREATE UNIQUE INDEX made has none, as in PostgreSQL; nor
 * does NOT NULL, which is pg_attribute's.
 */
static void gen_pg_constraint(Tuplestorestate *ts, TupleDesc desc, Oid only) {
  ListCell *l;

  foreach (l, sm_relations(only)) {
    smstore_relation_t *r = (smstore_relation_t *)lfirst(l);
    smstore_table_t *t;

    if (r->kind == RELKIND_INDEX && r->constraint &&
        (!OidIsValid(only) || r->table == only))
      key_row(ts, desc, r);
    if (r->kind != RELKIND_RELATION || r->nfkeys == 0 ||
        (OidIsValid(only) && r->oid != only))
      continue;
    t = smstore_table_desc(r->oid);
    if (t == NULL)
      continue;
    for (int i = 0; i < t->nfkeys; i++)
      fkey_row(ts, desc, t->nspoid, &t->fkeys[i]);
  }
}

/*
 * A row of pg_attribute, into ts: column attnum of relid, of type typid
 * under typmod, or dropped if typid is InvalidOid.  Its storage is its
 * type's, its collation its type's; nothing is missing, generated or
 * inherited, and there is no ACL.
 */
static void attribute_row(Tuplestorestate *ts, TupleDesc desc, Oid relid,
                          int attnum, const char *name, Oid typid, int32 typmod,
                          bool notnull, bool hasdef, char identity) {
  Datum values[Natts_pg_attribute];
  bool nulls[Natts_pg_attribute];
  char dropped[NAMEDATALEN];
  int16 typlen = 4;
  bool typbyval = true;
  char typalign = TYPALIGN_INT;
  char typstorage = TYPSTORAGE_PLAIN;

  memset(values, 0, sizeof(values));
  memset(nulls, true, sizeof(nulls));
  if (OidIsValid(typid)) {
    get_typlenbyvalalign(typid, &typlen, &typbyval, &typalign);
    typstorage = get_typstorage(typid);
  } else {
    /* As PostgreSQL renames a dropped column. */
    snprintf(dropped, sizeof(dropped), "........pg.dropped.%d........", attnum);
    name = dropped;
  }
  for (int i = 0; i < Natts_pg_attribute; i++)
    nulls[i] = i >= Anum_pg_attribute_attstattarget - 1;
  values[Anum_pg_attribute_attrelid - 1] = ObjectIdGetDatum(relid);
  values[Anum_pg_attribute_attname - 1] = name_datum(name);
  values[Anum_pg_attribute_atttypid - 1] = ObjectIdGetDatum(typid);
  values[Anum_pg_attribute_attlen - 1] = Int16GetDatum(typlen);
  values[Anum_pg_attribute_attnum - 1] = Int16GetDatum(attnum);
  values[Anum_pg_attribute_atttypmod - 1] = Int32GetDatum(typmod);
  values[Anum_pg_attribute_attndims - 1] =
      Int16GetDatum(OidIsValid(get_element_type(typid)) ? 1 : 0);
  values[Anum_pg_attribute_attbyval - 1] = BoolGetDatum(typbyval);
  values[Anum_pg_attribute_attalign - 1] = CharGetDatum(typalign);
  values[Anum_pg_attribute_attstorage - 1] = CharGetDatum(typstorage);
  values[Anum_pg_attribute_attcompression - 1] = CharGetDatum('\0');
  values[Anum_pg_attribute_attnotnull - 1] = BoolGetDatum(notnull);
  values[Anum_pg_attribute_atthasdef - 1] = BoolGetDatum(hasdef);
  values[Anum_pg_attribute_atthasmissing - 1] = BoolGetDatum(false);
  values[Anum_pg_attribute_attidentity - 1] = CharGetDatum(identity);
  values[Anum_pg_attribute_attgenerated - 1] = CharGetDatum('\0');
  values[Anum_pg_attribute_attisdropped - 1] = BoolGetDatum(!OidIsValid(typid));
  values[Anum_pg_attribute_attislocal - 1] = BoolGetDatum(true);
  values[Anum_pg_attribute_attinhcount - 1] = Int16GetDatum(0);
  values[Anum_pg_attribute_attcollation - 1] = ObjectIdGetDatum(
      OidIsValid(typid) ? get_typcollation(typid) : InvalidOid);
  tuplestore_putvalues(ts, desc, values, nulls);
}

/*
 * pg_attribute's rows: the columns of the catalog relations, then of sm's
 * tables, indexes and views.  A sequence's, and the system columns, are not
 * listed.
 */
static void gen_pg_attribute(Tuplestorestate *ts, TupleDesc desc, Oid only) {
  int count;
  const cat_relation_t *cats = cat_rels(only, &count);
  ListCell *l;

  for (int i = 0; i < count; i++)
    for (int a = 0; a < cats[i].natts; a++)
      attribute_row(ts, desc, cats[i].oid, a + 1, cats[i].columns[a].name,
                    cats[i].columns[a].typid, -1, false, false, '\0');
  foreach (l, sm_relations(only)) {
    smstore_relation_t *r = (smstore_relation_t *)lfirst(l);

    if (r->kind == RELKIND_SEQUENCE)
      continue;
    for (int a = 0; a < r->natts; a++)
      attribute_row(ts, desc, r->oid, a + 1, r->attnames[a], r->typids[a],
                    r->typmods[a], r->notnull[a], r->defaults[a] != NULL,
                    r->identity[a]);
  }
}

/*
 * pg_attrdef's rows: a column of sm's with a DEFAULT, a row.  sm keeps no
 * oid for one, so each is 0.
 */
static void gen_pg_attrdef(Tuplestorestate *ts, TupleDesc desc, Oid only) {
  Datum values[Natts_pg_attrdef];
  bool nulls[Natts_pg_attrdef] = {false, false, false, false};
  ListCell *l;

  foreach (l, sm_relations(only)) {
    smstore_relation_t *r = (smstore_relation_t *)lfirst(l);

    if (r->kind != RELKIND_RELATION)
      continue;
    for (int a = 0; a < r->natts; a++) {
      if (r->defaults[a] == NULL)
        continue;
      values[Anum_pg_attrdef_oid - 1] = ObjectIdGetDatum(InvalidOid);
      values[Anum_pg_attrdef_adrelid - 1] = ObjectIdGetDatum(r->oid);
      values[Anum_pg_attrdef_adnum - 1] = Int16GetDatum(a + 1);
      values[Anum_pg_attrdef_adbin - 1] = CStringGetTextDatum(r->defaults[a]);
      tuplestore_putvalues(ts, desc, values, nulls);
    }
  }
}

/*
 * Whether value, null if isnull, passes the scan key key, as a heap scan
 * tests one (HeapKeyTest()): a null passes none.
 */
static bool key_matches(ScanKey key, Datum value, bool isnull) {
  if (isnull || (key->sk_flags & SK_ISNULL))
    return false;
  return DatumGetBool(FunctionCall2Coll(&key->sk_func, key->sk_collation, value,
                                        key->sk_argument));
}

/*
 * The rows of a catalog compiled in as text (cat_relation_t's text): each
 * value read by its column type's input function, as COPY reads one.  A
 * pg_node_tree, which no input function reads, is taken as its text.
 */
static void gen_text_rows(const cat_relation_t *cat, Tuplestorestate *ts,
                          TupleDesc desc, int nkeys, ScanKey key) {
  AttInMetadata *meta = TupleDescGetAttInMetadata(desc);
  Datum *values = (Datum *)palloc(cat->natts * sizeof(Datum));
  bool *nulls = (bool *)palloc(cat->natts * sizeof(bool));

  /* One column of a row: its text read by the column type's input. */
  auto read = [&](const char *const *row, int a) {
    nulls[a] = row[a] == NULL;
    if (nulls[a])
      values[a] = (Datum)0;
    else if (TupleDescAttr(desc, a)->atttypid == PG_NODE_TREEOID)
      values[a] = CStringGetTextDatum(row[a]);
    else
      values[a] = InputFunctionCall(&meta->attinfuncs[a], (char *)row[a],
                                    meta->attioparams[a], meta->atttypmods[a]);
  };

  for (int i = 0; i < cat->nrows; i++) {
    const char *const *row = &cat->text[(size_t)i * cat->natts];
    bool match = true;

    /* The key columns first: a row a key passes over is read no further. */
    for (int k = 0; k < nkeys && match; k++) {
      int a = key[k].sk_attno - 1;

      read(row, a);
      match = key_matches(&key[k], values[a], nulls[a]);
    }
    if (!match)
      continue;
    for (int a = 0; a < cat->natts; a++)
      read(row, a);
    /* A function's ACL is not in its text: initdb's, which no SQL sets. */
    if (ProcedureRelationId == cat->oid) {
      values[Anum_pg_proc_proacl - 1] =
          function_acl(DatumGetObjectId(values[Anum_pg_proc_oid - 1]),
                       DatumGetObjectId(values[Anum_pg_proc_proowner - 1]));
      nulls[Anum_pg_proc_proacl - 1] =
          (Datum)0 == values[Anum_pg_proc_proacl - 1];
    }
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/*
 * pg_authid's rows: pg_global's roles, as this session sees them
 * (utils/init/pg_global.c).
 */
static void gen_pg_authid(Tuplestorestate *ts, TupleDesc desc) {
  std::shared_ptr<const pg_global_roles_t> all = pg_global_roles();
  Datum values[Natts_pg_authid];
  bool nulls[Natts_pg_authid];

  for (const pg_global_role_t &r : all->roles) {
    memset(nulls, false, sizeof(nulls));
    values[Anum_pg_authid_oid - 1] = ObjectIdGetDatum(r.oid);
    values[Anum_pg_authid_rolname - 1] = name_datum(r.rolname.c_str());
    values[Anum_pg_authid_rolsuper - 1] = BoolGetDatum(r.rolsuper);
    values[Anum_pg_authid_rolinherit - 1] = BoolGetDatum(r.rolinherit);
    values[Anum_pg_authid_rolcreaterole - 1] = BoolGetDatum(r.rolcreaterole);
    values[Anum_pg_authid_rolcreatedb - 1] = BoolGetDatum(r.rolcreatedb);
    values[Anum_pg_authid_rolcanlogin - 1] = BoolGetDatum(r.rolcanlogin);
    values[Anum_pg_authid_rolreplication - 1] = BoolGetDatum(r.rolreplication);
    values[Anum_pg_authid_rolbypassrls - 1] = BoolGetDatum(r.rolbypassrls);
    values[Anum_pg_authid_rolconnlimit - 1] = Int32GetDatum(r.rolconnlimit);
    nulls[Anum_pg_authid_rolpassword - 1] = !r.has_password;
    values[Anum_pg_authid_rolpassword - 1] =
        r.has_password ? CStringGetTextDatum(r.rolpassword.c_str()) : (Datum)0;
    nulls[Anum_pg_authid_rolvaliduntil - 1] = !r.has_validuntil;
    values[Anum_pg_authid_rolvaliduntil - 1] =
        TimestampTzGetDatum(r.rolvaliduntil);
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/* pg_auth_members's rows: pg_global's memberships, as this session sees. */
static void gen_pg_auth_members(Tuplestorestate *ts, TupleDesc desc) {
  std::shared_ptr<const pg_global_roles_t> all = pg_global_roles();
  Datum values[Natts_pg_auth_members];
  bool nulls[Natts_pg_auth_members] = {};

  for (const pg_global_member_t &m : all->members) {
    values[Anum_pg_auth_members_oid - 1] = ObjectIdGetDatum(m.oid);
    values[Anum_pg_auth_members_roleid - 1] = ObjectIdGetDatum(m.roleid);
    values[Anum_pg_auth_members_member - 1] = ObjectIdGetDatum(m.member);
    values[Anum_pg_auth_members_grantor - 1] = ObjectIdGetDatum(m.grantor);
    values[Anum_pg_auth_members_admin_option - 1] =
        BoolGetDatum(m.admin_option);
    values[Anum_pg_auth_members_inherit_option - 1] =
        BoolGetDatum(m.inherit_option);
    values[Anum_pg_auth_members_set_option - 1] = BoolGetDatum(m.set_option);
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/*
 * pg_roles's rows: pg_authid's, as PostgreSQL's view has them, the
 * password hidden.  No role has settings of its own yet (rolconfig).
 */
static void gen_pg_roles(Tuplestorestate *ts, TupleDesc desc) {
  std::shared_ptr<const pg_global_roles_t> all = pg_global_roles();
  Datum values[13];
  bool nulls[13];

  for (const pg_global_role_t &r : all->roles) {
    memset(nulls, false, sizeof(nulls));
    values[0] = name_datum(r.rolname.c_str());
    values[1] = BoolGetDatum(r.rolsuper);
    values[2] = BoolGetDatum(r.rolinherit);
    values[3] = BoolGetDatum(r.rolcreaterole);
    values[4] = BoolGetDatum(r.rolcreatedb);
    values[5] = BoolGetDatum(r.rolcanlogin);
    values[6] = BoolGetDatum(r.rolreplication);
    values[7] = Int32GetDatum(r.rolconnlimit);
    values[8] = CStringGetTextDatum("********");
    nulls[9] = !r.has_validuntil;
    values[9] = TimestampTzGetDatum(r.rolvaliduntil);
    values[10] = BoolGetDatum(r.rolbypassrls);
    nulls[11] = true;
    values[11] = (Datum)0;
    values[12] = ObjectIdGetDatum(r.oid);
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/*
 * pg_database's rows: sm's databases, each owned by the superuser, in
 * UTF-8, with the C locale, as every database of digres is (WORKNOTE.md
 * step 8), in the default tablespace, with no ACL.
 */
static void gen_pg_database(Tuplestorestate *ts, TupleDesc desc) {
  Datum values[Natts_pg_database];
  bool nulls[Natts_pg_database];
  Acl *acl;
  ListCell *l;

  foreach (l, datadir_database_names()) {
    const char *name = (const char *)lfirst(l);

    memset(values, 0, sizeof(values));
    memset(nulls, false, sizeof(nulls));
    values[Anum_pg_database_oid - 1] =
        ObjectIdGetDatum(datadir_database_oid(name));
    values[Anum_pg_database_datname - 1] = name_datum(name);
    values[Anum_pg_database_datdba - 1] = ObjectIdGetDatum(
        pg_global_database_owner(pg_global_dbid(datadir_database_oid(name))));
    values[Anum_pg_database_encoding - 1] = Int32GetDatum(PG_UTF8);
    values[Anum_pg_database_datlocprovider - 1] =
        CharGetDatum(COLLPROVIDER_LIBC);
    values[Anum_pg_database_datistemplate - 1] = BoolGetDatum(false);
    values[Anum_pg_database_datallowconn - 1] = BoolGetDatum(true);
    values[Anum_pg_database_dathasloginevt - 1] = BoolGetDatum(false);
    values[Anum_pg_database_datconnlimit - 1] = Int32GetDatum(-1);
    values[Anum_pg_database_dattablespace - 1] =
        ObjectIdGetDatum(DEFAULTTABLESPACE_OID);
    values[Anum_pg_database_datcollate - 1] = CStringGetTextDatum("C");
    values[Anum_pg_database_datctype - 1] = CStringGetTextDatum("C");
    nulls[Anum_pg_database_datlocale - 1] = true;
    nulls[Anum_pg_database_daticurules - 1] = true;
    nulls[Anum_pg_database_datcollversion - 1] = true;
    /* Its ACL is pg_global's, by its id, if GRANT made one. */
    acl = pg_global_acl(pg_global_dbid(datadir_database_oid(name)), InvalidOid);
    nulls[Anum_pg_database_datacl - 1] = NULL == acl;
    values[Anum_pg_database_datacl - 1] = PointerGetDatum(acl);
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/*
 * pg_sequence's rows: a sequence of sm's, a row.  sm's sequences are
 * bigint, count up from START by INCREMENT with no bound but bigint's, and
 * neither cycle nor cache.
 */
static void gen_pg_sequence(Tuplestorestate *ts, TupleDesc desc, Oid only) {
  Datum values[Natts_pg_sequence];
  bool nulls[Natts_pg_sequence];
  ListCell *l;

  memset(nulls, false, sizeof(nulls));
  foreach (l, sm_relations(only)) {
    smstore_relation_t *r = (smstore_relation_t *)lfirst(l);

    if (r->kind != RELKIND_SEQUENCE)
      continue;
    values[Anum_pg_sequence_seqrelid - 1] = ObjectIdGetDatum(r->oid);
    values[Anum_pg_sequence_seqtypid - 1] = ObjectIdGetDatum(INT8OID);
    values[Anum_pg_sequence_seqstart - 1] = Int64GetDatum(r->seqstart);
    values[Anum_pg_sequence_seqincrement - 1] = Int64GetDatum(r->seqincrement);
    values[Anum_pg_sequence_seqmax - 1] = Int64GetDatum(PG_INT64_MAX);
    values[Anum_pg_sequence_seqmin - 1] = Int64GetDatum(1);
    values[Anum_pg_sequence_seqcache - 1] = Int64GetDatum(1);
    values[Anum_pg_sequence_seqcycle - 1] = BoolGetDatum(false);
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/*
 * pg_depend's rows: digres records no dependencies but a sequence's on the
 * column that owns it -- automatic for a serial's, internal for an identity
 * column's, as PostgreSQL records them.
 */
static void gen_pg_depend(Tuplestorestate *ts, TupleDesc desc, Oid only) {
  Datum values[Natts_pg_depend];
  bool nulls[Natts_pg_depend];
  ListCell *l;

  memset(nulls, false, sizeof(nulls));
  foreach (l, sm_relations(only)) {
    smstore_relation_t *r = (smstore_relation_t *)lfirst(l);

    if (r->kind != RELKIND_SEQUENCE || !OidIsValid(r->owner))
      continue;
    values[Anum_pg_depend_classid - 1] = ObjectIdGetDatum(RelationRelationId);
    values[Anum_pg_depend_objid - 1] = ObjectIdGetDatum(r->oid);
    values[Anum_pg_depend_objsubid - 1] = Int32GetDatum(0);
    values[Anum_pg_depend_refclassid - 1] =
        ObjectIdGetDatum(RelationRelationId);
    values[Anum_pg_depend_refobjid - 1] = ObjectIdGetDatum(r->owner);
    values[Anum_pg_depend_refobjsubid - 1] = Int32GetDatum(r->owner_attnum);
    values[Anum_pg_depend_deptype - 1] =
        CharGetDatum(r->owner_identity ? DEPENDENCY_INTERNAL : DEPENDENCY_AUTO);
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/*
 * pg_rewrite's rows: a view of sm's, a row, its _RETURN rule, ON SELECT DO
 * INSTEAD its query, the view's oid its own, as the relation cache makes it
 * (RelationBuildSmViewDesc()).  digres has no other rule.
 */
static void gen_pg_rewrite(Tuplestorestate *ts, TupleDesc desc, Oid only) {
  Datum values[Natts_pg_rewrite];
  bool nulls[Natts_pg_rewrite];
  ListCell *l;

  memset(nulls, false, sizeof(nulls));
  foreach (l, sm_relations(only)) {
    smstore_relation_t *r = (smstore_relation_t *)lfirst(l);

    if (r->kind != RELKIND_VIEW)
      continue;
    values[Anum_pg_rewrite_oid - 1] = ObjectIdGetDatum(r->oid);
    values[Anum_pg_rewrite_rulename - 1] = name_datum(ViewSelectRuleName);
    values[Anum_pg_rewrite_ev_class - 1] = ObjectIdGetDatum(r->oid);
    values[Anum_pg_rewrite_ev_type - 1] = CharGetDatum('0' + (int)CMD_SELECT);
    values[Anum_pg_rewrite_ev_enabled - 1] = CharGetDatum(RULE_FIRES_ON_ORIGIN);
    values[Anum_pg_rewrite_is_instead - 1] = BoolGetDatum(true);
    /* No qual: what nodeToString() writes of a NULL. */
    values[Anum_pg_rewrite_ev_qual - 1] = CStringGetTextDatum("<>");
    values[Anum_pg_rewrite_ev_action - 1] = CStringGetTextDatum(r->query);
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/*
 * pg_views's rows: a view of sm's, a row, its definition pg_get_viewdef()'s,
 * as PostgreSQL's view over pg_class lists them.  The catalog relations
 * compiled in are not views here, as pg_class lists them.
 */
static void gen_pg_views(Tuplestorestate *ts, TupleDesc desc) {
  Datum values[4];
  bool nulls[4] = {false, false, false, false};
  ListCell *l;

  foreach (l, sm_relations(InvalidOid)) {
    smstore_relation_t *r = (smstore_relation_t *)lfirst(l);

    if (r->kind != RELKIND_VIEW)
      continue;
    values[0] = name_datum(get_namespace_name(r->nspoid));
    values[1] = name_datum(r->name);
    values[2] = name_datum(GetUserNameFromId(relation_owner(r), false));
    values[3] = DirectFunctionCall1(pg_get_viewdef, ObjectIdGetDatum(r->oid));
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/* pg_am's rows: those compiled in. */
static void gen_pg_am(Tuplestorestate *ts, TupleDesc desc) {
  int count;
  const cat_am_t *rows = cat_ams(&count);
  Datum values[4];
  bool nulls[4] = {false, false, false, false};

  for (int i = 0; i < count; i++) {
    values[0] = ObjectIdGetDatum(rows[i].oid);
    values[1] = name_datum(rows[i].amname);
    values[2] = ObjectIdGetDatum(rows[i].amhandler);
    values[3] = CharGetDatum(rows[i].amtype);
    tuplestore_putvalues(ts, desc, values, nulls);
  }
}

/*
 * Whether the column attno of the catalog relation relid names, by its
 * oid, the relation its row is about: a scan that asks for one value of it
 * needs the rows of that relation alone.
 */
static bool is_filter_column(Oid relid, AttrNumber attno) {
  switch (relid) {
  case RelationRelationId:
    return attno == Anum_pg_class_oid;
  case AttributeRelationId:
    return attno == Anum_pg_attribute_attrelid;
  case AttrDefaultRelationId:
    return attno == Anum_pg_attrdef_adrelid;
  case IndexRelationId:
    return attno == Anum_pg_index_indexrelid || attno == Anum_pg_index_indrelid;
  case SequenceRelationId:
    return attno == Anum_pg_sequence_seqrelid;
  case DependRelationId:
    return attno == Anum_pg_depend_objid;
  case RewriteRelationId:
    return attno == Anum_pg_rewrite_oid || attno == Anum_pg_rewrite_ev_class;
  case ConstraintRelationId:
    return attno == Anum_pg_constraint_conrelid;
  default:
    return false;
  }
}

/*
 * Fill ts with the rows of the catalog relation rel: all of them, or with a
 * key on a filter column (is_filter_column()), at least those of the
 * relation it names.  The scan's quals are still checked on each row.
 */
static void generate(Relation rel, Tuplestorestate *ts, int nkeys,
                     ScanKey key) {
  TupleDesc desc = RelationGetDescr(rel);
  Oid only = InvalidOid;

  for (int i = 0; i < nkeys && !OidIsValid(only); i++)
    if (is_filter_column(RelationGetRelid(rel), key[i].sk_attno))
      only = DatumGetObjectId(key[i].sk_argument);

  switch (RelationGetRelid(rel)) {
  case NamespaceRelationId:
    gen_pg_namespace(ts, desc);
    break;
  case RelationRelationId:
    gen_pg_class(ts, desc, only);
    break;
  case AccessMethodRelationId:
    gen_pg_am(ts, desc);
    break;
  case IndexRelationId:
    gen_pg_index(ts, desc, only);
    break;
  case AttributeRelationId:
    gen_pg_attribute(ts, desc, only);
    break;
  case AttrDefaultRelationId:
    gen_pg_attrdef(ts, desc, only);
    break;
  case DatabaseRelationId:
    gen_pg_database(ts, desc);
    break;
  case SequenceRelationId:
    gen_pg_sequence(ts, desc, only);
    break;
  case DependRelationId:
    gen_pg_depend(ts, desc, only);
    break;
  case AuthIdRelationId:
    gen_pg_authid(ts, desc);
    break;
  case AuthMemRelationId:
    gen_pg_auth_members(ts, desc);
    break;
  case CATREL_PG_ROLES:
    gen_pg_roles(ts, desc);
    break;
  case CATREL_PG_VIEWS:
    gen_pg_views(ts, desc);
    break;
  case RewriteRelationId:
    gen_pg_rewrite(ts, desc, only);
    break;
  case ConstraintRelationId:
    gen_pg_constraint(ts, desc, only);
    break;
  case TypeRelationId:
  case CollationRelationId:
  case LanguageRelationId:
  case ProcedureRelationId:
  case OperatorRelationId:
  case CastRelationId:
    gen_text_rows(cat_relation(RelationGetRelid(rel)), ts, desc, nkeys, key);
    break;
  /*
   * What digres has none of: read, and empty.
   */
  case DescriptionRelationId:
  case ExtensionRelationId:
  case InheritsRelationId:
  case PolicyRelationId:
  case PublicationRelationId:
  case PublicationNamespaceRelationId:
  case PublicationRelRelationId:
  case StatisticExtRelationId:
  case TriggerRelationId:
    break;
  default:
    elog(ERROR, "no rows for catalog relation %u", RelationGetRelid(rel));
  }
}

/*
 * pg_relation_is_publishable(): false, for any relation.  digres has no
 * publications (WORKNOTE.md step 61), and psql's \d asks.
 */
Datum pg_relation_is_publishable(PG_FUNCTION_ARGS) { PG_RETURN_BOOL(false); }

/* The slot a row of a catalog relation is read into, from a tuple store. */
static const TupleTableSlotOps *catrel_slot_callbacks(Relation rel) {
  return &TTSOpsMinimalTuple;
}

/* A scan of a catalog relation: its rows, made when it began. */
typedef struct catrel_scan_t {
  TableScanDescData base; /* what tableam.h reads */
  Tuplestorestate *rows;
  int nkeys; /* the scan keys a row read must pass */
  ScanKey keys;
} catrel_scan_t;

/*
 * Begin a scan of rel: make its rows now, those keys on a filter column
 * name if there are any (catrel_beginscan()).
 */
static TableScanDesc catrel_scan_begin(Relation rel, Snapshot snapshot,
                                       int nkeys, struct ScanKeyData *key,
                                       uint32 flags) {
  catrel_scan_t *scan;

  scan = (catrel_scan_t *)palloc0(sizeof(catrel_scan_t));
  scan->base.rs_rd = rel;
  scan->base.rs_snapshot = snapshot;
  scan->base.rs_flags = flags;
  /* Held in memory: a catalog's rows are few. */
  scan->rows = tuplestore_begin_heap(true, false, work_mem);
  generate(rel, scan->rows, nkeys, key);
  scan->nkeys = nkeys;
  scan->keys = (ScanKey)palloc(Max(nkeys, 1) * sizeof(ScanKeyData));
  memcpy(scan->keys, key, nkeys * sizeof(ScanKeyData));
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
  pfree(scan->keys);
  if (sscan->rs_flags & SO_TEMP_SNAPSHOT)
    UnregisterSnapshot(sscan->rs_snapshot);
  pfree(scan);
}

/*
 * The scan's next row that passes its keys into slot; false at the end.
 * A generator may have made rows the keys pass over, so each is tested.
 */
static bool catrel_scan_getnextslot(TableScanDesc sscan,
                                    ScanDirection direction,
                                    TupleTableSlot *slot) {
  catrel_scan_t *scan = (catrel_scan_t *)sscan;

  while (tuplestore_gettupleslot(scan->rows, ScanDirectionIsForward(direction),
                                 false, slot)) {
    bool match = true;

    for (int k = 0; k < scan->nkeys && match; k++) {
      bool isnull;
      Datum value = slot_getattr(slot, scan->keys[k].sk_attno, &isnull);

      match = key_matches(&scan->keys[k], value, isnull);
    }
    if (match)
      return true;
  }
  return false;
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
  r.index_fetch_begin = refuse;
  r.index_fetch_reset = refuse;
  r.index_fetch_end = refuse;
  r.index_fetch_tuple = refuse;
  r.tuple_fetch_row_version = refuse;
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
  r.relation_estimate_size = catrel_estimate_size;
  r.scan_bitmap_next_tuple = refuse;
  r.scan_sample_next_block = refuse;
  r.scan_sample_next_tuple = refuse;
  return r;
}();

/* The table access method of a catalog relation. */
const TableAmRoutine *catrel_routine(void) { return &catrel_methods; }

/*
 * Whether the catalog relation relid has the rows compiled in alone, none
 * of sm's: a syscache miss in it is looked up in its rows (systable_
 * beginscan()), as they are fixed, and no DDL makes a cached one stale.
 */
bool catrel_is_builtin(Oid relid) {
  switch (relid) {
  case AccessMethodRelationId:
  case CastRelationId:
  case CollationRelationId:
  case LanguageRelationId:
  case OperatorRelationId:
  case ProcedureRelationId:
  case TypeRelationId:
    return true;
  default:
    return false;
  }
}

/*
 * Whether the catalog relation relid has pg_global's rows, the roles':
 * a session drops what it caches of them when pg_global's epoch moves
 * (utils/init/pg_global.c).
 */
bool catrel_is_global(Oid relid) {
  return AuthIdRelationId == relid || AuthMemRelationId == relid;
}

/*
 * Whether the syscache may keep a row of the catalog relid: one compiled in,
 * or of pg_global's, or of a catalog that is no catalog relation; not one
 * made from sm's catalog, which no DDL invalidates (SYSCATALOG.md).
 */
bool catrel_is_cached(Oid relid) {
  return NULL == cat_relation(relid) || catrel_is_builtin(relid) ||
         catrel_is_global(relid);
}

/*
 * The quals of a sequential scan of rel, scanrelid in its plan, that can
 * narrow the rows the scan makes: a filter column's oid equal to a value
 * that reads nothing of rel and is not volatile, a constant or a
 * parameter.  catrel_key_t, the value's expression made ready under parent,
 * for catrel_beginscan() to evaluate.
 */
List *catrel_scan_keys(Relation rel, List *qual, Index scanrelid,
                       PlanState *parent) {
  List *keys = NIL;
  ListCell *l;

  foreach (l, qual) {
    OpExpr *op = (OpExpr *)lfirst(l);
    Node *var, *value;

    /* oid = oid: its function is oideq. */
    if (!IsA(op, OpExpr) || get_opcode(op->opno) != F_OIDEQ ||
        list_length(op->args) != 2)
      continue;
    for (int side = 0; side < 2; side++) {
      var = (Node *)list_nth(op->args, side);
      value = (Node *)list_nth(op->args, 1 - side);
      while (IsA(var, RelabelType))
        var = (Node *)((RelabelType *)var)->arg;
      if (IsA(var, Var) && ((Var *)var)->varno == (int)scanrelid &&
          ((Var *)var)->varlevelsup == 0 &&
          is_filter_column(RelationGetRelid(rel), ((Var *)var)->varattno) &&
          !contain_var_clause(value) && !contain_volatile_functions(value)) {
        catrel_key_t *key = (catrel_key_t *)palloc(sizeof(catrel_key_t));

        key->attno = ((Var *)var)->varattno;
        key->value = ExecInitExpr((Expr *)value, parent);
        keys = lappend(keys, key);
        break;
      }
    }
  }
  return keys;
}

/*
 * Begin a scan of rel with keys (catrel_scan_keys()), each value evaluated
 * in econtext now: a parameter's, as a rescan has it.  A null value narrows
 * nothing, and is left out.
 */
TableScanDesc catrel_beginscan(Relation rel, Snapshot snapshot, List *keys,
                               ExprContext *econtext) {
  ScanKeyData *scankeys =
      (ScanKeyData *)palloc(Max(list_length(keys), 1) * sizeof(ScanKeyData));
  int n = 0;
  ListCell *l;

  foreach (l, keys) {
    catrel_key_t *key = (catrel_key_t *)lfirst(l);
    bool isnull;
    Datum value = ExecEvalExprSwitchContext(key->value, econtext, &isnull);

    if (!isnull)
      ScanKeyInit(&scankeys[n++], key->attno, BTEqualStrategyNumber, F_OIDEQ,
                  value);
  }
  return table_beginscan(rel, snapshot, n, scankeys);
}

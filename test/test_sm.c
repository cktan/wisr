/*
 * test_sm.c -- the C API, driven from C.
 *
 * Built with -std=c11 -pedantic -Werror, so it also proves that
 * sm.h and the headers it includes are pure C, and linked
 * against build/libsm.a the way PostgreSQL will be.
 */
#define _POSIX_C_SOURCE 200809L

#include "sm.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;

/* Count and report a failed check, then carry on. */
#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "%s:%d: CHECK(%s) failed: %s %s\n", __FILE__, __LINE__,  \
              #cond, sm_errname(), sm_errmsg());                               \
      g_failures++;                                                            \
    }                                                                          \
  } while (0)

/* A fresh home directory under /tmp, made a wisr home. */
static void make_home(char *path, size_t size) {
  snprintf(path, size, "/tmp/wisr-test-sm-XXXXXX");
  if (mkdtemp(path) == NULL) {
    perror("mkdtemp");
    exit(1);
  }
  CHECK(sm_init(path) == 0);
}

/* Remove a home directory made by make_home. */
static void remove_home(const char *path) {
  char cmd[256];
  snprintf(cmd, sizeof(cmd), "rm -rf '%s'", path);
  if (system(cmd) != 0) {
    fprintf(stderr, "could not remove %s\n", path);
  }
}

/* A log hook that counts its calls into *arg. */
static void count_log(void *arg, sm_loglevel_t level, const char *msg) {
  (void)level;
  (void)msg;
  (*(int *)arg)++;
}

/* Verify the error calls and sm_options_init's defaults. */
static void test_errors_and_options(void) {
  /*
   * 1. After sm_reseterr the thread's error is SM_OK, "OK".
   * 2. Attaching a directory that does not exist fails with a
   *    code, a name and a message, and is not retryable.
   * 3. sm_options_init fills sm1's defaults.
   * 4. A log hook installs and is removed.
   */
  sm_options_t opt;
  int calls = 0;

  sm_reseterr();
  CHECK(sm_errnum() == SM_OK);
  CHECK(strcmp(sm_errname(), "OK") == 0);

  sm_env_t *env = NULL;
  CHECK(sm_attach("/nonexistent/wisr", &env) == -1);
  CHECK(env == NULL);
  CHECK(sm_errnum() != SM_OK);
  CHECK(strncmp(sm_errname(), "ERR_", 4) == 0);
  CHECK(sm_errmsg()[0] != '\0');
  CHECK(sm_retryable() == 0);

  sm_options_init(&opt);
  CHECK(opt.durability_sync == 1);
  CHECK(opt.deadlock_ms == 50);
  CHECK(opt.ts_lease == 1000000);
  CHECK(opt.rocksdb_options == NULL);
  CHECK(opt.commit_cache_entries == 65536);

  CHECK(sm_set_log(count_log, &calls) == 0);
  CHECK(sm_set_log(NULL, NULL) == 0);
}

/* Verify databases, sessions, transactions and savepoints from C. */
static void test_lifecycle(void) {
  /*
   * 1. Make a home, attach it, and create a database with
   *    options; sm_lsdb lists it.
   * 2. Open a session; create a store and a sequence, and
   *    commit; sm_lsstore lists the store.
   * 3. A savepoint returns depth 1, a rollback to it and a
   *    forget of it succeed, and the transaction aborts.
   * 4. Aborting again, with no transaction, returns 0.
   * 5. Dropping the store and the sequence commits; the
   *    session closes and the home detaches.
   */
  char home[64];
  sm_env_t *env = NULL;
  sm_session_t *s = NULL;
  sm_options_t opt;
  char **names = NULL;
  size_t n = 0;
  uint64_t storeid = 0;
  uint64_t *ids = NULL;
  int found = 0;

  make_home(home, sizeof(home));
  CHECK(sm_attach(home, &env) == 0);
  sm_options_init(&opt);
  opt.durability_sync = 0;
  CHECK(sm_mkdb(env, "db1", &opt) == 0);
  CHECK(sm_mkdb(env, "db1", NULL) == -1);
  CHECK(sm_errnum() == SM_ERR_EXISTS);
  CHECK(sm_lsdb(env, &names, &n) == 0);
  for (size_t i = 0; i < n; i++) {
    found += strcmp(names[i], "db1") == 0;
  }
  CHECK(found == 1);
  sm_free(names);

  CHECK(sm_opendb(env, "db1", NULL, &s) == 0);
  CHECK(sm_begin(s, SM_XCT_SERIALIZABLE) == 0);
  CHECK(sm_mkstore(s, &storeid) == 0);
  CHECK(sm_mkseq(s, "t_rowno", 1, 1) == 0);
  CHECK(sm_commit(s) == 0);
  CHECK(sm_begin(s, SM_XCT_READ_COMMITTED) == 0);
  CHECK(sm_lsstore(s, &ids, &n) == 0);
  found = 0;
  for (size_t i = 0; i < n; i++) {
    found += ids[i] == storeid;
  }
  CHECK(found == 1);
  sm_free(ids);

  CHECK(sm_stmt_begin(s) == 0);
  CHECK(sm_savepoint(s) == 1);
  CHECK(sm_rollback(s, 1) == 0);
  CHECK(sm_forget(s, 1) == 0);
  CHECK(sm_rollback(s, 1) == -1);
  CHECK(sm_errnum() == SM_ERR_NOSAVEPOINT);
  CHECK(sm_abort(s) == 0);
  CHECK(sm_abort(s) == 0);

  CHECK(sm_begin(s, SM_XCT_REPEATABLE_READ) == 0);
  CHECK(sm_rmstore(s, storeid) == 0);
  CHECK(sm_rmseq(s, "t_rowno") == 0);
  CHECK(sm_commit(s) == 0);
  CHECK(sm_closedb(s) == 0);
  CHECK(sm_detach(env, 0) == 0);
  remove_home(home);
}

/* Verify two-phase commit from C: prepare, list, resolve. */
static void test_2pc(void) {
  /*
   * 1. Session s prepares a transaction as "g1".
   * 2. Session u lists exactly "g1", with a prepared_at time.
   * 3. u commits it; the list is then empty and NULL.
   * 4. Resolving an unknown gid fails.
   */
  char home[64];
  sm_env_t *env = NULL;
  sm_session_t *s = NULL;
  sm_session_t *u = NULL;
  sm_prepared_t *list = NULL;
  size_t n = 0;
  uint64_t storeid = 0;

  make_home(home, sizeof(home));
  CHECK(sm_attach(home, &env) == 0);
  CHECK(sm_mkdb(env, "db", NULL) == 0);
  CHECK(sm_opendb(env, "db", NULL, &s) == 0);
  CHECK(sm_opendb(env, "db", NULL, &u) == 0);

  CHECK(sm_begin(s, SM_XCT_SERIALIZABLE) == 0);
  CHECK(sm_mkstore(s, &storeid) == 0);
  CHECK(sm_prepare(s, "g1") == 0);

  CHECK(sm_list_prepared(u, &list, &n) == 0);
  CHECK(n == 1);
  if (n == 1) {
    CHECK(strcmp(list[0].gid, "g1") == 0);
    CHECK(list[0].prepared_at > 0);
  }
  sm_free(list);

  CHECK(sm_commit_prepared(u, "g1") == 0);
  CHECK(sm_list_prepared(u, &list, &n) == 0);
  CHECK(n == 0);
  CHECK(list == NULL);
  CHECK(sm_abort_prepared(u, "nope") == -1);

  CHECK(sm_closedb(u) == 0);
  CHECK(sm_closedb(s) == 0);
  CHECK(sm_detach(env, 0) == 0);
  remove_home(home);
}

/* An int4 datum over *v: its four bytes in memory. */
static sm_datum_t int4_datum(const int32_t *v) {
  sm_datum_t d = {v, sizeof(*v), false};
  return d;
}

/* A short varlena of s into buf: a 1-byte header, then s. */
static sm_datum_t text_datum(char *buf, const char *s) {
  size_t n = strlen(s);
  buf[0] = (char)(((n + 1) << 1) | 1);
  memcpy(buf + 1, s, n);
  sm_datum_t d = {buf, n + 1, false};
  return d;
}

/* The int4 a decoded datum holds, or -1 for a null. */
static int32_t int4_of(sm_datum_t d) {
  int32_t v = -1;
  if (!d.null && d.len == sizeof(v)) {
    memcpy(&v, d.data, sizeof(v));
  }
  return v;
}

/* Verify types, collations, a heap rel and a unique index from C. */
static void test_heap_and_index(void) {
  /*
   * 1. sm_type finds int4 and text by code and refuses 99; the
   *    C collation opens with no version.
   * 2. A heap rel of (int4, text) takes three rows, numbered
   *    1, 2 and 3, and a unique index on the int4 takes their
   *    entries; a duplicate key is SM_ERR_DUPKEY.
   * 3. Row 2 reads back, decoded by ordinal and deformed whole;
   *    deform with the wrong count is SM_ERR_INVALID.
   * 4. Updating row 2 hands back its old tuple, deleting row 3
   *    hands back its tuple, and row 3 then reads as none.
   * 5. A heap scan returns rows 1 and 2 in order.
   * 6. After moving row 2's entry to key 20 and dropping row
   *    3's, an index scan from key 2 finds only row 2, whose
   *    value decodes to its rowno.
   */
  char home[64];
  sm_env_t *env = NULL;
  sm_session_t *s = NULL;
  sm_collation_t *coll = NULL;
  sm_rel_t *rel = NULL;
  sm_idx_t *idx = NULL;
  sm_rel_scan_t *rscan = NULL;
  sm_idx_scan_t *iscan = NULL;
  uint64_t storeid = 0;
  uint64_t rowno = 0;
  uint64_t rownos[3];
  const void *tuple = NULL;
  size_t len = 0;
  sm_datum_t out[2];
  char vbuf[3][16];
  int32_t ints[3] = {1, 2, 3};
  const char *texts[3] = {"a", "b", "c"};

  const sm_type_t *int4 = sm_type(3);
  const sm_type_t *text = sm_type(8);
  CHECK(int4 != NULL);
  CHECK(text != NULL);
  CHECK(sm_type(99) == NULL);
  CHECK(sm_collation_open("C", true, NULL, &coll) == 0);
  CHECK(strcmp(sm_collation_version(coll), "") == 0);
  sm_collation_close(coll);

  make_home(home, sizeof(home));
  CHECK(sm_attach(home, &env) == 0);
  CHECK(sm_mkdb(env, "db", NULL) == 0);
  CHECK(sm_opendb(env, "db", NULL, &s) == 0);
  CHECK(sm_begin(s, SM_XCT_SERIALIZABLE) == 0);
  CHECK(sm_mkstore(s, &storeid) == 0);
  CHECK(sm_mkseq(s, "t_rowno", 1, 1) == 0);

  sm_attribute_t schema[2] = {{int4, -1, false, false, NULL, 0},
                              {text, -1, false, false, NULL, 0}};
  sm_key_column_t key[1] = {{int4, -1, NULL, false, false}};
  CHECK(sm_rel_open(s, storeid, 0, schema, 2, "t_rowno", &rel) == 0);
  CHECK(sm_idx_open(s, storeid, 1, key, 1, true, NULL, 0, &idx) == 0);
  CHECK(sm_rel_ncols(rel) == 2);
  CHECK(sm_idx_ncols(idx) == 2);

  for (int i = 0; i < 3; i++) {
    sm_datum_t row[2] = {int4_datum(&ints[i]), text_datum(vbuf[i], texts[i])};
    CHECK(sm_rel_insert(rel, row, 2, &rownos[i]) == 0);
    CHECK(rownos[i] == (uint64_t)(i + 1));
    CHECK(sm_idx_insert(idx, row, 1, rownos[i], NULL, 0) == 0);
  }
  sm_datum_t dup = int4_datum(&ints[0]);
  CHECK(sm_idx_insert(idx, &dup, 1, 99, NULL, 0) == -1);
  CHECK(sm_errnum() == SM_ERR_DUPKEY);

  CHECK(sm_rel_read(rel, 2, &tuple, &len) == 0);
  CHECK(sm_rel_decode(rel, tuple, len, 0, &out[0]) == 0);
  CHECK(int4_of(out[0]) == 2);
  CHECK(sm_rel_deform(rel, tuple, len, out, 2) == 0);
  CHECK(int4_of(out[0]) == 2);
  CHECK(out[1].len == 2 && memcmp(out[1].data, vbuf[1], 2) == 0);
  CHECK(sm_rel_deform(rel, tuple, len, out, 1) == -1);
  CHECK(sm_errnum() == SM_ERR_INVALID);

  int32_t twenty = 20;
  char bb[16];
  sm_datum_t row2[2] = {int4_datum(&twenty), text_datum(bb, "bb")};
  CHECK(sm_rel_update(rel, 2, row2, 2, &tuple, &len) == 0);
  CHECK(sm_rel_decode(rel, tuple, len, 0, &out[0]) == 0);
  CHECK(int4_of(out[0]) == 2);
  CHECK(sm_rel_delete(rel, 3, &tuple, &len) == 0);
  CHECK(sm_rel_decode(rel, tuple, len, 0, &out[0]) == 0);
  CHECK(int4_of(out[0]) == 3);
  CHECK(sm_rel_read(rel, 3, &tuple, &len) == 1);

  CHECK(sm_rel_scan(rel, 1, UINT64_MAX, &rscan) == 0);
  CHECK(sm_rel_scannxt(rscan, &rowno, &tuple, &len) == 0);
  CHECK(rowno == 1);
  CHECK(sm_rel_scannxt(rscan, &rowno, &tuple, &len) == 0);
  CHECK(rowno == 2);
  CHECK(sm_rel_decode(rel, tuple, len, 0, &out[0]) == 0);
  CHECK(int4_of(out[0]) == 20);
  CHECK(sm_rel_scannxt(rscan, &rowno, &tuple, &len) == 1);
  sm_rel_scanend(rscan);

  sm_datum_t k2 = int4_datum(&ints[1]);
  sm_datum_t k3 = int4_datum(&ints[2]);
  CHECK(sm_idx_delete(idx, &k2, 1, 2) == 0);
  CHECK(sm_idx_insert(idx, &row2[0], 1, 2, NULL, 0) == 0);
  CHECK(sm_idx_delete(idx, &k3, 1, 3) == 0);
  sm_bound_t lo = {&k2, 1, true};
  bool widened = true;
  bool cut = true;
  CHECK(sm_idx_scan(idx, &lo, NULL, &widened, &iscan) == 0);
  CHECK(!widened);
  CHECK(sm_idx_scannxt(iscan, &rowno, &cut, &tuple, &len) == 0);
  CHECK(rowno == 2);
  CHECK(!cut);
  CHECK(sm_idx_decode(idx, tuple, len, 0, &out[0]) == 0);
  CHECK(out[0].len == 8);
  CHECK(sm_idx_scannxt(iscan, &rowno, &cut, &tuple, &len) == 1);
  sm_idx_scanend(iscan);

  CHECK(sm_commit(s) == 0);
  sm_idx_close(idx);
  sm_rel_close(rel);
  CHECK(sm_closedb(s) == 0);
  CHECK(sm_detach(env, 0) == 0);
  remove_home(home);
}

/* Run every test; exit nonzero if any check failed. */
int main(void) {
  test_errors_and_options();
  test_lifecycle();
  test_2pc();
  test_heap_and_index();
  if (g_failures != 0) {
    fprintf(stderr, "test_sm: %d check(s) failed\n", g_failures);
    return 1;
  }
  printf("test_sm: all checks passed\n");
  return 0;
}

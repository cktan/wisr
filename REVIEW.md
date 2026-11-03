# Code Reviews

Two reviews, at `high` effort, from reading the code; neither ran
a test. In `dg/`, each read only the lines this repository
changed, not PostgreSQL's own code (`CLAUDE.md`). In each, the
most severe finding is first.

1. **ALTER COLUMN TYPE**, `fa1c683b..16a4480f` on
   `wip-altertype`.
2. **Code merged from main**, `fa1c683b..471c870a`.

# Review 1: ALTER COLUMN TYPE

- **Target:** `fa1c683b..16a4480f`, the five commits of
  `wip-altertype`.
- **Verification:** findings 1 and 2 were checked against the
  code. `smstore_create_index()` refuses an interval index key
  (`smstore.c:7148`); PostgreSQL refuses a column altered twice
  (`tablecmds.c:13493`).
- **Result:** two correctness defects in digres, two in sm3, a
  robustness gap, an order of errors that differs, and four
  cleanups.

## Findings

### 1. ALTER COLUMN TYPE makes an interval index key

- **Location:** `dg/src/backend/access/sm/smstore.c`,
  `alter_column_type()`
- **Defect:** `smstore_create_index()` refuses an interval index
  key: sm orders intervals that PostgreSQL calls equal as two
  keys. `alter_column_type()` does not refuse a change of an
  indexed column, or a key column, to interval.
- **Scenario:**
  1. `CREATE TABLE t (a text PRIMARY KEY)`, with rows `'1 mon'`
     and `'30 days'`.
  2. `ALTER TABLE t ALTER a TYPE interval USING a::interval`.
  3. The rewrite builds the primary key under interval and takes
     both rows.
  4. The key holds two values PostgreSQL calls equal, and `=`
     through the index misses one.

### 2. Two ALTER COLUMN TYPE in one statement convert in turn

- **Location:** `dg/src/backend/access/sm/smstore.c`, the
  subcommand loop and `alter_column_type()`
- **Defect:** each subcommand rewrites in turn, after a
  `CommandCounterIncrement()`, so a later `USING` reads a column
  an earlier one converted. PostgreSQL evaluates every transform
  on the row as it was, and refuses a column altered twice.
  Decision 4 of `dg/ALTERTYPE.md`, one rewrite each, is not only
  slower, as §8 says: it gives other values.
- **Scenario:**
  1. `ALTER TABLE t ALTER a TYPE int8 USING a * 10, ALTER b TYPE
     int8 USING a`.
  2. PostgreSQL sets b to the old a; digres sets b to a * 10.
  3. `ALTER a TYPE int8, ALTER a TYPE text` runs on digres;
     PostgreSQL says `cannot alter type of column "a" twice`.

### 3. sm3 takes any typmod change without a converter

- **Location:** `sm/sm3/src/ddl/ddl.cc:881`
- **Defect:** without a converter, `alter_column_type()` checks
  the type code, or text and varchar, and takes any typmod
  change. `sm.h` says the call is taken only when every value
  stays as it is.
- **Scenario:**
  1. rel's `(alter-column-type t d :decimal64 10 4 :keep)` on a
     `decimal64(10, 2)` column.
  2. Each datum keeps scale 2 under a typmod of scale 4.
  3. `bpchar(5)` to `bpchar(10)` keeps the old padding;
     `varchar(10)` to `varchar(5)` keeps values too long.
  4. Values and keys no longer agree with the column's type.
- **Note:** digres passes no converter only when PostgreSQL's
  test says no value changes, so this is reached through `sm.h`
  and rel, not SQL.

### 4. A retyped column's index keeps a key collation

- **Location:** `sm/sm3/src/ddl/ddl.cc:890`
- **Defect:** when the new type takes no collation, the column's
  collation is cleared, but an index key's collation on that
  column is kept. `create_index` refuses that state, `a
  collation on a column not text`.
- **Scenario:**
  1. Through `sm.h` or rel: an index on b, text, with a key
     collation.
  2. b goes to int4, with a converter.
  3. `build_table` gives the int4 key a text collation.
  4. A later change back to text keeps the stale collation.

### 5. The converter can read a column it did not set

- **Location:** `dg/src/backend/access/sm/smstore.c:6777`
- **Defect:** `alter_conv()` copies `min(n, natts)` values into
  the slot, and `ExecStoreVirtualTuple()` marks all `natts`
  valid. A column past `n` keeps the last row's value, or
  memory never set.
- **Scenario:** if sm's row is shorter than the relcache
  descriptor, a `USING` that reads a later column reads garbage,
  and does not fail.
- **Fix:** set the columns past `n` null, or fail when `n` is not
  `natts`.

### 6. The refusals come before the cast's errors

- **Location:** `dg/src/backend/access/sm/smstore.c:6933`
- **Defect:** `check_retype()` runs before the transform is made.
  PostgreSQL reports the cast and `USING` errors first, and a
  view's or a dependent's refusal after.
- **Scenario:** b, text, is read by a view; `ALTER b TYPE int4`
  with no `USING`. PostgreSQL says `column "b" cannot be cast
  automatically to type integer`, with its hint; digres says
  `cannot alter type of a column used by a view or rule`.

### 7. digres and sm3 keep one set of refusals twice

- **Location:** `dg/src/backend/access/sm/smstore.c:6842`
- **Defect:** `check_retype()` repeats sm3's `column_typed_in()`:
  the expression and partial index, the foreign keys on either
  side, the views, the identity column. It reads each referencing
  child's definition again. The `texts` test repeats sm3's
  `text_pair`.
- **Scenario:** a refusal added on one side is missed on the
  other, and each ALTER reads the views and the children twice.

### 8. rel keeps its own table of type widths

- **Location:** `sm/rel/src/rel_ops.cc:867`
- **Defect:** `byval_width()` hard-codes sm2's type codes and
  widths, which `sm2::type_t::len` holds; `sm.h` gives no width.
- **Scenario:** a by-value type added to sm2 leaves rel's table
  stale: `retype_conv` refuses a valid conversion, or writes the
  wrong count of bytes.
- **Fix:** a width call in `sm.h`, such as `sm_type_len()`.

### 9. Two branches clear `missing` for one reason

- **Location:** `sm/sm3/src/ddl/ddl.cc:897`
- **Defect:** `c.missing` is cleared under `if (conv && !empty)`
  and again under `if (conv)`.
- **Fix:** one `if (conv) c.missing = std::nullopt;` before the
  test of an empty table.

### 10. `collatable()` repeats a check in `create_index`

- **Location:** `sm/sm3/src/ddl/ddl.cc:584`
- **Defect:** `collatable()` repeats the text, varchar and bpchar
  test that `create_index` makes inline in the same file.
- **Scenario:** a collatable type added to one and not the other
  makes `create_index` and `alter_column_type` disagree.

# Review 2: Code Merged from main

- **Target:** `fa1c683b..471c870a`: the fixes of the group-sync
  review, and the index kinds of `471c870a`.
- **Verification:** findings 1 and 3 were checked against
  `471c870a`: the tree's name is `"btree"` (`smstore.h:93`), and
  `smstore_index_am()` can return NULL.
- **Result:** three correctness defects, three rule and document
  defects, and three cleanups.

## Findings

### 1. An index definition can say `USING (null)`

- **Location:** `dg/src/backend/utils/adt/ruleutils.c`,
  `sm_indexdef()`
- **Defect:** `sm_indexdef()` passes the result of
  `smstore_index_am()` to `"%s"`. That function returns NULL when
  the table has no descriptor, or its descriptor does not list
  the index. The code above it allows a NULL descriptor.
- **Scenario:**
  1. `smstore_table_desc(idx->indrelid)` returns NULL, or a
     descriptor without the index.
  2. `smstore_index_am()` returns NULL.
  3. `pg_get_indexdef()` returns `CREATE INDEX i ON public.t
     USING (null) (a)`.
  4. A dump and restore, or an index made again from the text,
     fails to parse it.

### 2. The one-sync test can use its state after its scope

- **Location:** `sm/sm1/test/test_groupsync.cc:323`
- **Defect:** the futures `a_rc` and `rcs` are declared before
  `released`, `calls` and `let_go`, so they are destroyed last.
  A commit thread can still be in the `test_before_sync` hook,
  which uses `released` and `calls` by reference.
- **Scenario:**
  1. The REQUIRE on `wait_for_waiters` or on `entered` fails.
  2. `let_go` releases the stall, and `calls` and `released` go
     out of scope.
  3. The stalled thread returns from `released.wait()` on a
     destroyed future, or the next leader increments a destroyed
     `calls`.
  4. The behavior is undefined while the futures' destructors
     wait: a crash, or an ASan report.

### 3. `USING btree` gets the experimental tree kind

- **Location:** `dg/src/include/access/smstore.h:93`
- **Defect:** `SMSTORE_INDEX_TREE` is `"btree"`, PostgreSQL's
  default access method's name. `BTREE.md` §12 says the tree is
  built behind the kind and off by default.
- **Scenario:**
  1. A schema from pg_dump is restored: pg_dump writes `USING
     btree` for every index.
  2. Every index becomes a tree.
  3. Unique keys are capped near 1.3 KB, not 4 KB; there are no
     index-only scans; dead entries stay until vacuum.
  4. Inserts that worked on PostgreSQL, or with `lsm`, fail or
     slow down.
- **Fix:** give the tree a name of its own, such as `smtree`,
  and take `btree` as the default kind.

### 4. `fatal_v` holds a mutex without a guard

- **Location:** `sm/sm0/src/log.cc:516`
- **Defect:** `fatal_v` takes `g_log_mu`, now a
  `std::timed_mutex`, with a bare `try_lock_for()` and
  `unlock()`. `sm/CODE.md` §7 says every mutex is held by an
  RAII guard.
- **Fix:** `std::unique_lock<std::timed_mutex> lk(g_log_mu,
  fatal_hook_wait)`, then `if (lk.owns_lock())`: the same
  bounded wait, with a guard.

### 5. pgregress hides every rule line's width

- **Location:** `dg/test/pgregress/pgregress.py:573`
- **Defect:** `norm()` turns every rule line of every test into
  `-`, not only the lines whose width `as_postgres()` changed.
- **Scenario:** a regression that changes a result's count of
  columns or their widths, `---+---` against `-------`, no
  longer shows in a rule line anywhere in pgregress.
- **Fix:** apply the tolerance only to output `as_postgres()`
  rewrote, or compute the rule again from the rewritten rows.

### 6. `BTREE.md` says `\d` shows one name

- **Location:** `sm/BTREE.md:986`
- **Defect:** the document says psql's `\d` shows `btree` for
  both kinds. `\d` of a table prints the kind of
  `pg_get_indexdef()`, `lsm`; `\d` of the index prints the name
  of `pg_am`, `btree`. `server.sh`'s expected output shows both.
- **Scenario:** `\d dd1.t` lists `"t_cb" UNIQUE, lsm (c DESC)`,
  and `\d dd1.t_cb` says `unique, btree, for table`. A user sees
  two names for one index.

### 7. `smstore_index_am()` builds a descriptor to read one flag

- **Location:** `dg/src/backend/access/sm/smstore.c:2342`
- **Defect:** `smstore_index_am()` builds the whole table
  descriptor to read one bool. `sm_indexdef()` holds that
  descriptor already, and loops over `desc->indexes`.
- **Scenario:** each `pg_get_indexdef()` builds the descriptor
  twice: `open_table`, a copy of every column and index, and
  `sm_tbl_index_usable()` and `remember_index()` for each index.
  psql's `\d` calls it once an index. `LIKE ... INCLUDING
  INDEXES` pays the same, once a cloned index.
- **Fix:** read `desc->indexes[i].tree` in the loop already
  there.

### 8. A check in `tree_kind()` cannot fail

- **Location:** `dg/src/backend/access/sm/smstore.c:6441`
- **Defect:** `tree && stmt->primary` cannot be true. A PRIMARY
  KEY has no USING clause and gets `lsm` from
  `transformIndexConstraint`; a cloned primary key gets `lsm`
  from `smstore_index_am()`, as no primary key is a tree.
- **Fix:** reduce `tree_kind()` to its `strcmp`, or move the
  check to `check_create_index()`.

### 9. `sync_waiters` is a field for tests

- **Location:** `sm/sm1/src/db.h:1008`
- **Defect:** `db_t` keeps `sync_waiters` only for tests to read.
  Each commit that waits for a group sync changes it, under
  `sync_mu`.
- **Note:** this was decided in the group-sync review's finding
  4, a count in place of a test hook. It stays unless that
  decision changes.

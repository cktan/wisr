# Decisions for review

Made while working unattended, each on a branch of its own:
`wip-slt1` from `wip-slt`, `wip-slt2` from `wip-slt1`,
`wip-slt3` from `wip-slt2`. Each is implemented as recommended
and can be reverted alone.

---

## wip-slt1: CREATE INDEX waits for its build

### The problem

On a table that has rows, sm3 records a new index as pending
and builds it in the background once the transaction commits
(`sm/include/sm.h`, `sm_cat_create_index`). digres's
`CREATE INDEX` never waits for that build
(`sm_cat_index_wait` has no caller). So:

- **`CREATE UNIQUE INDEX` over duplicates succeeds.** It
  returns `CREATE INDEX`; the build then fails in the
  background, the index stays `indisvalid = f`, and a later
  duplicate insert succeeds. PostgreSQL 18 fails the
  statement: `could not create unique index "uk"`, `Key
  (k)=(1) is duplicated.` A user is told they have a unique
  index they do not have.
- **The next statements plan without the index**, for as long
  as the build runs, in any session.

sm3's design means this to be the host's choice: "`builder_wait`
blocks until the index is valid ... It is called outside a
transaction, so that the caller's next snapshot is new enough
to scan the index" (`sm/sm3/SM3_DESIGN.md` §3).

### Options

1. **Wait after the commit, report the build's error, drop the
   failed index** (recommended, implemented). When a
   transaction that made indexes commits, digres waits for
   each build before it reports the command complete, outside
   any transaction, as PostgreSQL's `CREATE INDEX
   CONCURRENTLY` waits between its transactions. A build that
   failed is dropped, in a transaction of its own, and its
   error reported: `could not create unique index` and sm's
   detail.
   - Autocommit `CREATE INDEX` then behaves as PostgreSQL's:
     an error and no index, or an index usable at once.
   - **Inside a transaction block** the error comes at
     `COMMIT`, after the block's other work has committed:
     PostgreSQL would have failed the `CREATE INDEX` itself
     and aborted the block. Within the block the index is not
     used before the commit, where PostgreSQL's is.
   - A cancel ends the wait; the build goes on in the
     background, as it does today.
2. **Wait, report, and leave the failed index** as `INVALID`,
   as a failed `CREATE INDEX CONCURRENTLY` leaves one. Simpler
   by a transaction, but a plain `CREATE INDEX` that failed
   leaves nothing in PostgreSQL, and a failed sm index is good
   for nothing but dropping.
3. **Refuse `CREATE INDEX` on a table with rows inside a
   transaction block**, as PostgreSQL refuses `CREATE INDEX
   CONCURRENTLY` there, and wait in autocommit as in 1. No
   error is ever reported late, but scripts that wrap DDL in
   `BEGIN ... COMMIT` fail.
4. **Build in the statement**, inside the transaction, as
   PostgreSQL does. sm3 builds a table made in the same
   transaction that way already, but for any other table its
   build is deliberately in the background, under weak locks
   (`sm/sm3/SM3_DESIGN.md` §3): an sm3 change, out of scope
   here.

### What to review

- Whether the late error at `COMMIT` in a block (option 1) is
  acceptable, or the refusal of option 3 is wanted instead.
- Whether a failed index should be dropped (1) or kept (2).

---

## wip-slt2: SQLite's whole corpus, fetched, not kept

### The problem

SQLite's sqllogictest corpus beyond `select1` to `select5` is
`index/`, `random/` and `evidence/`: about 650 files, 1.2 GB,
millions of records of standard SQL, with `skipif
postgresql` already on what PostgreSQL cannot run. It found
nothing but views wrong in a sample of 1.5 million records,
which is what makes it worth running; it is too big to keep in
the repository.

### Options

1. **Fetch it on demand** (recommended, implemented):
   `test/slt_corpus.sh` clones a mirror on GitHub,
   `gregrahn/sqllogictest`, at a pinned commit, without file
   contents, into `build/slt-corpus`, and checks out only the
   files asked for. It runs them on PostgreSQL 18 and on
   digres and reports what digres alone fails. `make
   slt-corpus`; not in `make test`.
   - Needs the network the first time, and the mirror to
     stay. The commit pins what is run; SQLite's own fossil
     repository is the source if the mirror goes.
2. **Vendor a sample** of a few MB, as `select1` to `select5`
   are. No network, but a fixed and small part of the corpus.
3. **Leave it** to be fetched by hand.

### What the sample found

- **Views are missing.** Every one of the 24,408 records
  digres failed and PostgreSQL 18 passed uses `CREATE VIEW`,
  which digres refuses: `a relation's storage cannot be made:
  digres has no catalog table`. A view needs a place in sm3's
  catalog for its query, which is a design decision, not made
  here; it is recorded in `sm/NEXT.md` §2. Views are built
  since (`sm/VIEW.md`).
- Everything else in the sample, index scans, `IN`,
  `BETWEEN`, `ORDER BY`, aggregates, expressions, `GROUP BY`,
  `DELETE`, digres answers as PostgreSQL 18 does.

### What to review

- Whether fetching from the GitHub mirror is acceptable, or
  the corpus should come from SQLite's fossil repository.
- Whether views should be next.

---

## wip-slt3: sm says whom a session waits for

### The problem

`pg_blocking_pids()` returned `{}` for a session waiting in sm,
on a row's writer or on a table's lock: digres's locks are
sm1's, and sm told no one about its waits-for graph. PostgreSQL's
`isolationtester` asks `pg_isolation_test_session_is_blocked()`,
which reads `pg_blocking_pids()`, to tell a step that waits from
one still running; without it every blocked step hung for its
300 seconds and failed. So PostgreSQL's isolation suite, the
check of concurrency PostgreSQL itself relies on, could not run
on digres.

### Options

1. **An sm call that reads the waits-for graph**
   (recommended, implemented). `sm_session_tag(session, n)`
   gives an sm session the host's number for it;
   `sm_waits_for(env, n, &numbers)` returns the numbers of the
   sessions the one numbered n waits for, from sm1's
   `waiting_for` edges, under the env's mutex and the
   database's lock mutex. digres numbers each sm session by
   its session record's id, which is never reused, and
   `pg_blocking_pids()` adds those sessions' pids.
   - Public API in `sm.h`, and a new lock order in sm1: the
     env's mutex, then a database's `lock_mu`. Nothing took them
     the other way.
   - Read only: no change to how sm1 waits or orders.
2. **A flag digres sets around each sm call that may wait.**
   No sm change, but a call that may wait is not one that is
   waiting: a long statement would look blocked, and
   isolationtester would print `<waiting ...>` where PostgreSQL
   does not.
3. **Hand sm's session pointers to the host**, and let it map
   them. Simpler in sm, but a pointer to a session another
   thread may close is a use after free waiting to happen; the
   numbers avoid it.

**Decided on review**: digres numbers each sm session by its
pid, its thread's id, not its record's id (`dg/WORKNOTE.md`
step 172). The numbers `sm_waits_for` gives are pids, with no
lookup in the session registry; a pid is distinct among the
sessions running, which is what sm asks of the numbers.

### What to review

- The public API's shape: numbers chosen by the host, asked of
  the env.
- What to do with PostgreSQL's isolation suite. A first run of
  the 37 specs that use nothing digres lacks outright, with
  this change, had passed 17 of the first 33 when this was
  written, `lock-nowait`, `skip-locked*`, `nowait*`,
  `delete-abort-savept` and `insert-conflict-do-*` among them;
  without it `lock-nowait` hung and failed. The failures, among
  them the deadlock specs, foreign keys and tuple-lock modes
  digres does not have (`FOR KEY SHARE`), are not triaged yet.
  Run with `pg_isolation_regress --use-existing` from
  `~/p/postgres18/src/test/isolation`; it is in no `make`
  target yet. It is since: `make -j4 isolation` in `dg/`
  (`dg/CODE.md` §1).

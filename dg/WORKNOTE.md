# Work notes: decisions taken without asking

Written while working through `DGWORK.md` unattended. Each entry
is a guess made in place of a question; any of them can be
reversed. Newest step last.

## Step 1: the client

- **Deleted, not carved out**: `src/interfaces/libpq`,
  `src/bin/psql`, `src/fe_utils` and `src/include/fe_utils`.
  Stock PostgreSQL 18 `psql` is the client.
- **`zic` is deleted, and the time zone data with it.** `zic.c`
  is a client program, and the only reason left to compile
  anything twice. `pg_config.h` now sets `SYSTEMTZDIR` to
  `/usr/share/zoneinfo`, so the server reads the system's time
  zone database. Guess: a Linux-only server can rely on the
  `tzdata` package.
- **`dg.mk` lost `BOTH` and `FRONTEND`**: every file is compiled
  once, for the server.
- **Left in place**: the `#ifdef FRONTEND` branches inside
  `src/common` and `src/port`, and the headers only they use,
  such as `postgres_fe.h` and `common/logging.h`. They are dead
  code now, and removing them is a separate pass over 40 files.
- **Fixed on the way**: `make clean` removed `*.d`, which took
  `src/backend/utils/probes.d`, a source file, with it. It now
  removes only the objects' own `.d` files.

## Step 2: getting the server to link

`make -j4` now links `build/digres`, with no undefined symbol.
It prints its version and nothing more useful: there is no
bootstrap, so no catalog and no database to start. That is the
state `../sm/PART4.md` §10 expects before the lookup layer.

How it was done: the headers of each subsystem that was left out
of the copy were deleted, and then every use the compiler
reported was removed. Nothing was stubbed, with the one exception
of the hash access method below.

**Built-ins**

- `src/tools/cut_builtins.py` is new. It removes `pg_proc` rows by
  C function name and then every row `genbki.pl` reports as
  dangling. About 1,700 rows went.
- **The type hash functions are back**, as
  `src/backend/utils/adt/hashfunc.c`. They were in
  `access/hash`, which was left out as an index method, but hash
  joins and hash aggregates need them.
- **There is a hash access method in name only**, `hashhandler` in
  the same file. A hash operator class has to belong to an access
  method, and the planner asks that method what a strategy number
  means. It answers that and has no callbacks. Guess: this is
  better than losing every hash operator class. Open: `CREATE
  INDEX ... USING hash` would call a null callback; nothing
  refuses it yet.
- **gist, gin, spgist and brin are gone from the catalog data**,
  with their operator classes, and so are their support functions
  in `utils/adt`, such as `rangetypes_gist.c`.

**Text search**

- **Removed, types included.** The copy had left out `tsearch`
  and `snowball` but kept `tsvector` and `tsquery` in `utils/adt`,
  which could not link without the parser. All of it is deleted,
  with `commands/tsearchcmds.c` and `utils/cache/ts_cache.c`.
- Text search DDL parses, then reports that it is not supported.
- **Left in place**: the `pg_ts_*` catalogs, the `regconfig` and
  `regdictionary` types, and the code in `catalog/` that knows
  them. It goes when the catalog does.

**Replication**

- Everything under `src/include/replication` is deleted, and
  every caller: WAL senders and receivers, slots, origins, logical
  decoding, the apply workers, synchronous replication.
- **A replication connection is refused** at the startup packet,
  with a FATAL saying it is not supported.
- **Standby mode has no streaming.** `xlogrecovery.c` kept its
  state machine; the "stream" state now fails at once, so a
  standby polls the archive and `pg_wal` only. Guess: the least
  rewriting of code that the storage stack replaces anyway.
- **`synchronous_commit` stays** as a setting, without the hook
  that belonged to synchronous replication.
- **Publications are left in place and inert.** They link, and
  nothing reads them. `CheckCmdReplicaIdentity` is gone, so an
  UPDATE no longer checks a published table's replica identity.
- Subscriptions parse, then report that they are not supported.
  `catalog/pg_subscription.c` is still there.
- **The backend types remain**: `B_WAL_SENDER`, `B_WAL_RECEIVER`,
  `B_WAL_SUMMARIZER` and `B_SLOTSYNC_WORKER` are still in the
  enum, with no main function. Removing them touches the
  postmaster's state machine and the I/O statistics tables.

**Other subsystems**

- **JIT**: removed, with `es_jit*` in `EState`, `jitFlags` in
  `PlannedStmt`, and EXPLAIN's JIT section.
- **Base backup**: `pg_backup_start` and `pg_backup_stop` are
  gone, and the tablespace map. Recovery still reads a
  `backup_label` if it finds one.
- **WAL summarizer**: removed.
- **Bootstrap**: `--boot` and `--check` print that there is no
  bootstrap mode. `IsBootstrapProcessingMode()` is still tested in
  some 40 places and is always false.
- **Statistics**: the replication slot and subscription kinds are
  gone, and the kinds after them are renumbered so the table has
  no holes.

**Settings removed**: every `jit*`, `max_wal_senders`,
`max_replication_slots`, `wal_sender_timeout`,
`wal_receiver_*`, `hot_standby_feedback`, `primary_conninfo`,
`primary_slot_name`, `recovery_min_apply_delay`,
`synchronous_standby_names`, `synchronized_standby_slots`,
`sync_replication_slots`, `idle_replication_slot_timeout`,
`max_logical_replication_workers` and its two per-subscription
limits, `max_active_replication_origins`,
`logical_decoding_work_mem`, `debug_logical_replication_streaming`,
`log_replication_commands`, `summarize_wal`,
`wal_summary_keep_time`, `gin_fuzzy_search_limit`,
`gin_pending_list_limit`, `default_text_search_config`.

**Known leftovers**, all dead code that still links:

- `XLogSetReplicationSlotMinimumLSN` and `max_slot_wal_keep_size`
  in `xlog.c`, and the replication slot xmin in `procarray.c`.
- `max_wal_senders` in the control file, written as 0.
- The `XACT_XINFO_HAS_ORIGIN` part of a commit record is still
  parsed, never written.
- The catalogs `pg_replication_origin`, `pg_subscription`,
  `pg_subscription_rel` and `pg_publication*`. (The first three
  went, and the last were left empty, in step 214.)

## Step 3: sq, the base and the raw parser

`../sq` exists: `make -C ../sq test` builds `sqparse`, which
parses SQL with PostgreSQL's raw parser and prints the tree, and
runs eight tests through it. It is the first of the converted
code to be run rather than only compiled.

- **The sources were not moved.** sq compiles them from
  `dg/src` in place. They include 285 of the 763 headers, from
  every directory, some generated from the catalog data; moving
  the `.c` files alone would make sq need digres to compile.
  Guess: a boundary with tests, and an honest list of what
  crosses it, is worth more tonight than a move that leaves the
  two projects needing each other. `../sq/README.md` gives the
  order to finish it in. `dg/` is therefore no slimmer for this
  step.
- **The name `sq`** and its place beside `sm/` and `dg/` are as
  `DGWORK.md` proposed.
- **`sq/src/host.c` answers 32 names** that the parser's layers
  ask of the server, as a process with no client and no
  transaction would. They are stand-ins for a test program, and
  the list of what to cut. Error reporting asks for 24 of them.
- **The link keeps only what is reached** (`-ffunction-sections`
  and `--gc-sections`). Without that, the same objects need 124
  names, most of them from code the parser never runs.
- **The expected trees were checked by reading**, not against a
  running PostgreSQL, which does not print the raw parse tree.
- sq has its own `.clang-format`, a copy of digres's: includes
  are not sorted, because `postgres.h` must come first.

## Step 4: what was left alone

As `DGWORK.md` says: `src/timezone` and `regex` are not carved
out, the planner is not touched, and the TLS and GSSAPI files
are still skipped.

## Step 5: the parser is sq

`../sq` became a library of its own (`../sq/PLAN.md`): it
compiles nothing of digres's, and builds a tree of its own
nodes. digres now parses with it and has no grammar or scanner
of its own for SQL. This supersedes what step 3 says of sq.

- **`raw_parser()` is still the way in**, with the signature
  it had, so its five callers did not change. `parser.c` is new:
  it hands sq the text and the session's three settings, and
  turns sq's tree into PostgreSQL's nodes.
- **The converter is generated**: `sq_convert.c`, written at
  `make gen` by sq's generator from the node headers sq keeps.
  One function a node, field by field.
- **Deleted**: `gram.y`, `scan.l`, `gramparse.h`,
  `parser/scanner.h` and `check_keywords.pl`.
- **`scansup.c` stayed.** Twelve files outside the parser call
  it, to fold or cut an identifier. sq has a copy of what its
  scanner needs.
- **The three settings** that change how a string is read are
  defined in `parser.c` now, where `scan.l` had them, and so are
  `SystemFuncName` and `SystemTypeName`, which the analyzer
  calls.
- **An error from sq is raised as the server's own**, with its
  SQLSTATE, position, detail and hint. A notice is kept until
  sq has returned and reported then: reported from sq's hook, an
  error while reporting would jump out through sq.
- **sq's tree is C++ and an error here is a `longjmp`**, which
  runs no destructor. The tree is therefore held on the heap and
  deleted in a `PG_FINALLY`.
- **The PL/pgSQL parse modes are refused.** Nothing asks for
  them: digres has no procedural language.
- **`digres --parse`** parses standard input and prints the
  trees, with no server started. It is how the parser is tested:
  `make test` gives it to sq's tests, which keep what
  PostgreSQL's parser printed for each of their files.
- **Checked before the old parser went**: PostgreSQL's parser
  and sq with the converter, run in one process over 49,119
  texts, the regression scripts cut into pieces and sq's own
  test files. They agreed on every tree with its locations, and
  on every error's code, position, detail and hint.
- `spi.c` still passes whatever parse mode its plan holds; a
  PL/pgSQL one would now be an error. Not cut, since nothing
  sets one.

## Step 6: `SELECT 1 + 1` runs

The second thing `../sm/PART4.md` §10 says must run does:
`echo 'select 1 + 1;' | build/digres --parse --run` prints 2,
with no server started and no catalog table read. The operator,
its function and the type are found compiled in.

- **The built-in catalog rows are C tables.** `gen_cat.py` writes
  them from `postgres.bki`, where `genbki.pl` has already
  resolved every reference to an oid: a struct a catalog, the
  rows in oid order, and a lookup by oid and one by name. Four
  catalogs so far: `pg_proc`, `pg_operator`, `pg_type`,
  `pg_namespace`. A row is read-only and lives for ever, so a
  caller has nothing to release.
- **Callers are rewritten as they are reached**, not served by a
  cache made to look like the old one. `--parse --analyze`,
  `--plan` and `--run` take a statement as far as each says,
  and stop at the next lookup that has not been rewritten, which
  names it.
- **Rewritten so far**: the lookup of an operator by name
  (`OpernameGetOprid`); the parser's `Operator`, which is now a
  pointer to a row; 47 one-row helpers in `lsyscache.c`; the
  expression simplifier in `clauses.c`; `TupleDescInitEntry`.
  The other 1,000 or so calls still go to the syscache, which
  nothing initializes: each raises "invalid cache ID" when
  reached.
- **Every operator is in `pg_catalog`.** There is no
  `CREATE OPERATOR`, so the operator lookup needs no search
  path, and the parser's operator cache is keyed on name and
  argument types alone and never invalidated.
- **SQL-language functions are cut.** PostgreSQL gives its 50
  built-in ones their bodies at `initdb`, from
  `system_functions.sql`, and digres has no `initdb`; and there
  is no `CREATE FUNCTION`. Gone: those 50 rows, the language
  itself, `executor/functions.c`, the inlining of SQL functions
  in the planner, the SQL body in `CREATE FUNCTION`'s code, and
  `pg_get_function_sqlbody`.
- **That took more than functions with it.** Thirteen operators
  and a cast were implemented by one of the 50, and went as
  dangling: the forms of `+` with the date or time on the right
  (`interval + date`, `int4 + date` and the like), `text ||
  anynonarray` and `anynonarray || text`, `path @> point`, and
  the cast from `circle` to `polygon`. So did the functions
  behind `OVERLAPS` with an interval, `SUBSTRING ... SIMILAR`,
  two-argument `lpad` and `rpad`, `log(numeric)`, `age` and
  others. Each is a few lines of C to bring back. None is
  brought back yet.
- **The user is the bootstrap superuser** in `--plan` and
  `--run`, as in a standalone backend. digres has no roles yet;
  that is what lets the permission check on a function pass
  without one.
- **What runs is small**: integer arithmetic and comparison
  where the operator matches its arguments exactly. A function
  call, a cast, or an operator that needs a coercion stops at a
  lookup not yet rewritten.
- `make test` runs `test/*.sql` this way and compares with
  `test/expected`.

## Step 7: an error is an exception

The first step toward threads (`../sm/PART4.md` §2): digres no
longer leaves an error by `longjmp`. `ereport(ERROR)` throws a
C++ exception, `pg_error_t`, and what catches it is a `catch`.

- **The 6,450 places that raise did not change.** They end in
  `pg_re_throw()`, which threw by `siglongjmp` and now throws by
  `throw`.
- **`PG_TRY`, `PG_CATCH`, `PG_FINALLY` and `PG_END_TRY` are a
  `try` and a `catch`**, so the 44 blocks that use them did not
  change either.
- **The exception carries nothing.** The error is where it was,
  on `elog.c`'s stack, for `CopyErrorData()` and the rest.
- **`PG_exception_handled`, a flag, replaces
  `PG_exception_stack`**, the pointer to a jump buffer. Both
  say whether an ERROR has a handler; with none it is promoted
  to FATAL, as before.
- **Ten functions carried on after an error by jumping back to a
  `sigsetjmp`** at their top: the session's main loop, and the
  loops of the background writer, checkpointer, WAL writer,
  autovacuum launcher and worker, background worker, archiver
  and I/O worker. Nine are now wrapped in `PG_ERROR_LOOP_BEGIN`
  and `PG_ERROR_LOOP_END`, which run the recovery and then the
  function again from where the `sigsetjmp` was, and put the
  signal mask back as `sigsetjmp(..., 1)` did. The archiver's
  guarded one call, and is a plain `try`.
- **Destructors now run on an error.** `raw_parser()` held
  sq's tree on the heap and deleted it in a `PG_FINALLY`,
  because a `longjmp` runs none; it holds it on the stack now.
- **Two uses of `setjmp` are left**, neither of them
  PostgreSQL's errors: the configuration file's scanner jumps
  out of flex's fatal error, inside one function; and
  `pg_get_line.c` jumps out of a prompt that was interrupted.
- **What an exception cannot cross** is a frame compiled as C
  with no unwinding tables: a callback from a C library that
  raises would end the process. PostgreSQL's own `qsort` and
  its flex and bison output are compiled here as C++, and none
  was met.
- **Not run**: the nine loops. No server starts, so only the
  `PG_TRY` path is exercised, by `--parse` and `--run`.
  `test/select_error.sql` raises from the executor and
  `test/select_overflow.sql` from the planner.

## Step 8: initdb

`build/initdb`, or `build/digres --init`, makes a data
directory. It is the server under another name: PostgreSQL's
`initdb` runs the server in bootstrap mode to load the catalog,
and digres has no catalog to load.

- **A data directory is an sm home directory**, with one
  database, `postgres`, and two files of digres's: `PG_VERSION`,
  and `superuser`, which holds the superuser's name and, on a
  second line, the SCRAM secret of its password if one was
  given.
- **This is the first code in digres that calls sm.** The server
  now links `../sm/build/libsm.a` and RocksDB.
- **Authentication is not configured**, so there is no
  `pg_hba.conf`: peer on the local socket, SCRAM-SHA-256 over
  TCP. A superuser given no password can connect only locally.
- **One superuser, and no other roles yet.** `-U` names it, the
  OS user if it is absent. The server is to give that name to
  the built-in role with oid 10.
- **The encoding is UTF8 and the default collation C.** `-E` and
  `--locale` accept only those, and store nothing.
- **Every other option of PostgreSQL's `initdb` is rejected**,
  not ignored: `--auth`, `-k`, `--no-sync`, `-X` and the rest.
- **A failed run removes what it made.** There is no
  `--no-clean`.
- **The secret is built by the server's own SCRAM code**, called
  directly; PostgreSQL's `initdb` sends an `ALTER USER` to a
  bootstrap backend.
- **`src/common/sprompt.c` came back** from PostgreSQL for the
  password prompt, allocating with `palloc`.
- **`initdb -V` prints `initdb (digres) 18.6`**, where
  PostgreSQL's prints `initdb (PostgreSQL) 18.6`.
- **The two files and the directory are synced to disk.**
- **Not done**: an interrupt does not clean up.
  `test/initdb.sh` checks what is made and what is refused.

## Step 9: psql connects

`build/digres -D DIR` starts on a directory `initdb` made, and
PostgreSQL's `psql` connects and runs `SELECT 1 + 1`. It is
still a postmaster that forks a process a connection.

- **The server needs no installation.** The check for the
  library directory is gone, and it runs from where it was built.
- **It runs with no configuration file**, on the defaults; one
  that is there is read. A reload with none logs a complaint.
- **Time zone abbreviations are not loaded**, since they are
  read from an installed share directory. `EST` and the like
  are not known; a zone is named in full or by its offset.
- **There is no `pg_control`.** `ReadControlFile()` makes the
  structure in memory, as a bootstrap would, for one run of the
  server; nothing writes it.
- **There is no WAL.** `StartupXLOG()` gives the transaction
  counters their first values and declares recovery over.
  `CreateCheckPoint()` and `ShutdownXLOG()` do nothing, and
  `XLogInsertRecord()` refuses with an error. PostgreSQL's
  transactions still run a statement, and begin afresh at each
  start.
- **The rest of `xlog.c` and `xlogrecovery.c` is still there**,
  unreached. The 11 functions the cuts left unused in `xlog.c`
  were removed; the files go when sm's transactions come in.
- **The postmaster makes PostgreSQL's directories** at start
  (`make_legacy_dirs()`): `pg_wal`, `pg_xact`, `pg_notify` and
  the others its `initdb` makes. A name leaves the list when
  its subsystem is cut. `initdb` does not make them, so that a
  data directory is not left holding them. (The list emptied,
  and the function went, in step 209.)
- **Autovacuum is off by default.** Its launcher reads catalog
  tables, and sm collects its own garbage.
- **`pg_hba.conf` and `pg_ident.conf` are not read.**
  `check_hba()` is the fixed rule: peer on the local socket,
  SCRAM-SHA-256 over TCP. The parser of the files is still in
  `hba.c`, unreached; the 8 functions that matched a line were
  removed.
- **`utils/init/datadir.c` is what the server reads from the
  directory**: the `superuser` file, and the names of sm's
  databases.
- **The databases are listed once, by a child process** of the
  postmaster. Attaching sm leaves RocksDB's threads in the
  process that did it, and a postmaster that forks must have
  none. A backend cannot ask sm either (`../sm/PART4.md` §2):
  until connections are threads, the databases are the ones
  there were at start.
- **A database's oid is its place in sm's list**, from 16384.
  sm does not hand one out. It holds for one run of the server.
- **The one role is oid 10.** `InitializeSessionUserId()`
  accepts the name in the `superuser` file and no other,
  `get_role_password()` returns the secret from it, and
  `superuser_arg(10)` is true. `current_user` is that name.
- **A backend loads no catalog.** The two relcache phases, the
  ACL cache, the `pg_database` row and its recheck, and the
  per-database settings are gone from `InitPostgres()`.
  `CheckMyDatabase()` sets UTF8 and the C locale.
- **The default collation is not set up.**
  `init_database_collation()` reads `pg_database`. Nothing
  that compares text can run until it is.
- **Opening a catalog table is an error**, raised in
  `ScanPgRelation()`: it used to recurse until the stack ran
  out, which took the server down. A lookup not yet moved to
  the compiled-in rows (step 6) now fails the statement, and
  the session carries on.
- **What runs is what ran in step 6**, now over the protocol.
  A function call or a table name still fails: the search
  path's `"$user"` is looked up in `pg_authid`.
- **The error loop has run.** `test/server.sh` raises an error
  in a session and runs a statement after it. A backend killed
  by a signal makes the postmaster reinitialize, and the server
  takes connections again; that was tried by hand, not in the
  test.
- **`digres --parse --run` crashes on a function call**, as it
  did before this step: it does not set up the catalog caches,
  and a lookup dereferences a null one. Not fixed here.
- `test/server.sh` starts a server and checks both transports,
  the refusals, an error, and a clean shutdown. It needs `psql`
  on the path and skips itself without one.

## Step 10: the parallel backend is gone

Parallel query, parallel index build and parallel vacuum are
removed, with the planner's side of them. 28 files went, and
about 27,000 lines. A worker was a forked process that attached
the leader's shared memory; with sessions as threads, the
machinery would have had to be rebuilt, and nothing needs it
before sm's own scans exist.

- **What went**: `access/parallel.c`, `execParallel.c`, Gather
  and Gather Merge, the tuple queues (`tqueue.c`, `shm_mq.c`,
  `pqmq.c`), `shm_toc.c`, `barrier.c`, `sharedtuplestore.c`,
  `fileset.c`, `sharedfileset.c`, `vacuumparallel.c` and
  `access/common/session.c`.
- **The executor**: each node's DSM and worker functions,
  parallel hash join, parallel append, the shared bitmap of a
  bitmap heap scan, and the per-worker instrumentation. EXPLAIN
  prints no workers and no "Parallel" label.
- **The planner**: partial paths, the Gather paths and plans,
  `parallel_aware` and `parallel_safe` on plans and paths,
  `consider_parallel` on rels, and the parallel costs. Partial
  aggregation stays, since partitionwise aggregation uses it.
  Async append is now considered without a parallel-safety
  test.
- **Access methods**: the parallel scan descriptors and
  callbacks, heap's parallel scan, B-tree's parallel scan and
  build. `index_build()` and `scan_begin` lost their parallel
  arguments.
- **Sorting and temporary files keep their serial paths.** What
  went is the part only a worker used: `SortCoordinate`, the
  leader/worker merge, a tape imported from another process,
  and the file sets of `buffile.c` and `fd.c`.
- **Transactions**: parallel mode, the serialisation of
  transaction, snapshot, combo CID, GUC and library state for a
  worker, and SSI's sharing of a transaction with workers.
  `RestoreGUCState()` is among what went.
- **12 settings are gone**: the `max_parallel_*` and
  `min_parallel_*` ones, `parallel_setup_cost`,
  `parallel_tuple_cost`, `parallel_leader_participation`,
  `enable_gathermerge`, `enable_parallel_append`,
  `enable_parallel_hash` and `debug_parallel_query`; so is the
  `parallel_workers` reloption.
- **The grammar is not touched**, and neither is sq: `VACUUM
  (PARALLEL n)` parses and then fails with "parallel vacuum is
  not supported", and `PARALLEL SAFE` on a function is still
  stored.
- **11 LWLock tranches and 25 wait events** used only by the
  parallel code were removed.
- **Left in place, inert**: the lock-group code in `lock.c`,
  `proc.c` and `deadlock.c`, which only a worker joined; the
  `parallel_workers_to_launch` and `_launched` statistics,
  always 0; and `contain_context_dependent_node()` in
  `clauses.c`, unused before this step.
- `make test` passes as before.

## Step 11: VACUUM is gone

sm collects its own garbage (`../sm/PART4.md`), so VACUUM,
autovacuum, freezing and the wraparound defences built around
them are removed. 5 files went, and about 11,000 lines.
ANALYZE stays.

- **`VACUUM` does nothing and succeeds**, whatever its
  options and tables, which are not looked at; `VACUUM
  (PARALLEL n)` no longer fails either. The grammar and sq are
  not touched.
- **`ANALYZE` runs through what is left of `vacuum.c`**:
  `ExecVacuum()` takes ANALYZE's options (`VERBOSE`,
  `SKIP_LOCKED`, `BUFFER_USAGE_LIMIT`), and `vacuum()` runs
  `analyze_rel()` on each table. `VacuumParams` keeps only
  `options`. The cost-based delay settings stay, since ANALYZE
  uses them.
- **What went**: `vacuumlazy.c`, `autovacuum.c` and its
  header, `tidstore.c` and its header, the table AM's
  `relation_vacuum` callback, and the functions only they
  called: the clog, commit timestamp and multixact
  truncations, `vac_update_datfrozenxid()`, and a dozen in
  `lmgr.c`, `freespace.c` and elsewhere.
- **Autovacuum's two process types are gone**, with their
  child slots, their PGPROCs, the deadlock detector's cancel of
  a blocking autovacuum, and the `pg_signal_autovacuum_worker`
  check. The free list they shared with the slot sync worker
  is now `specialWorkerFreeProcs`. The statistics file format
  id changed, since its per-type arrays are shorter.
- **The wraparound limits keep only the stop.** The limits that
  launched autovacuum and the "must be vacuumed" warnings are
  gone. Past the stop limit a new XID or MultiXactId is still
  refused, so the counters cannot wrap; the hint to run VACUUM
  is gone from the message.
- **CLUSTER computes its own cutoffs**: `OldestXmin` and the
  oldest multixact, which is what `vacuum_get_cutoffs()` gave
  it.
- **45 settings are gone**: 25 server settings
  (`autovacuum*`, `vacuum_freeze_*`, `vacuum_multixact_*`,
  `vacuum_failsafe_age`, `vacuum_truncate`,
  `vacuum_max_eager_freeze_failure_rate`,
  `log_autovacuum_min_duration`) and 20 table options
  (`autovacuum_*`, `vacuum_truncate`, `vacuum_index_cleanup`
  and the like). `postgresql.conf.sample` still lists them,
  as it lists the parallel settings: it is not maintained.
- **3 LWLocks and 2 wait events** went: `Autovacuum`,
  `AutovacuumSchedule`, `WrapLimitsVacuum`, `AUTOVACUUM_MAIN`
  and `VACUUM_TRUNCATE`.
- **Left in place, inert**: the index AMs' `ambulkdelete` and
  `amvacuumcleanup` (ANALYZE still calls the second); heap
  pruning, HOT and the visibility map, which go with the heap;
  CLUSTER's code for VACUUM FULL; the vacuum and autovacuum
  counters in the statistics, always 0.
- `test/server.sh` checks that `VACUUM` succeeds and the
  session carries on. `make test` passes as before.

## Step 12: WAL is gone

Step 9 had stopped WAL being written: `XLogInsertRecord()`
refused, and `StartupXLOG()` only gave the transaction counters
their first values. This step removes the WAL code itself. 68
files went, and about 44,000 lines. sm keeps what is durable.

- **What went**: everything that writes, reads, replays,
  archives or describes WAL: `xloginsert.c`, `xlogreader.c`,
  `xlogrecovery.c`, `xlogprefetcher.c`, `xlogarchive.c`,
  `xlogfuncs.c`, `xlogstats.c`, `xlogutils.c`, `timeline.c`,
  `generic_xlog.c`, `rmgr.c`, all of `rmgrdesc/`, the heap and
  b-tree redo files, `standby.c`, `walwriter.c`, `pgarch.c`,
  `archive/`, every redo, desc and mask function, and the
  record headers (`heapam_xlog.h`, `nbtxlog.h` and the rest).
- **`xlog.c` is rewritten**, about 150 lines: the control
  record, made in memory for one run of the server, which
  keeps the authentication nonce and the checksum version, and
  the start and shutdown of the transaction counters.
  `pg_control.h` holds only that record.
- **The callers no longer write records.** The 61 blocks that
  built one went; where a page change sat in a critical section
  with its record, the change stays. Tests for
  `RelationNeedsWAL()`, `XLogIsNeeded()`, `wal_level` and the
  logical-decoding variants were always false, and were folded
  away by a script.
- **There is no recovery.** `RecoveryInProgress()`,
  `InRecovery`, `InHotStandby` and `standbyState` are gone, and
  with them hot standby: the known-assigned-xids machinery in
  `procarray.c`, recovery conflicts, and a snapshot's
  `takenDuringRecovery`.
- **Commit and abort mark `pg_xact` and nothing else.** There is
  no commit record, no asynchronous commit, and no LSN kept for
  a `pg_xact` page; `synchronous_commit` is gone.
- **`PREPARE TRANSACTION` is refused**: a prepared transaction's
  state was kept in WAL. `COMMIT PREPARED` finds nothing to
  commit, as before. The shared array of prepared transactions
  stays, always empty.
- **CREATE DATABASE copies file by file**: the `wal_log`
  strategy is refused. Bulk writes and relation copies are
  synced as PostgreSQL syncs them with `wal_level=minimal`.
- **Checkpoints still do nothing**, as since step 9; the
  checkpointer keeps its loop, requests and statistics, without
  restartpoints or WAL-based pacing. The startup process gives
  the counters their values and exits.
- **The WAL writer and the archiver are gone** as process
  types, with their statistics; the stats file format id
  changed.
- **45 settings and 27 SQL functions are gone**: `wal_*`,
  `archive_*`, `recovery_*`, `hot_standby`, `full_page_writes`,
  `max_wal_size`, `synchronous_commit`, `commit_delay`,
  `checkpoint_warning`, `in_hot_standby` and the like;
  `pg_current_wal_lsn()`, `pg_is_in_recovery()`,
  `pg_switch_wal()`, `pg_control_*()`, `pg_ls_waldir()` and the
  like. `postgresql.conf.sample` still lists the settings.
- **`pg_wal` and `pg_twophase` are no longer made.**
- **Left in place**: `XLogRecPtr` and page LSNs, which are
  always 0; the heap's conflict horizons, which nothing reads;
  the checkpoint work functions (`CheckPointBuffers()` and
  the rest) and `ProcessSyncRequests()`, which no checkpoint
  calls; the postmaster's recovery states, for step 4.
- `test/server.sh` checks that `PREPARE TRANSACTION` is refused
  and the session carries on. `make test` passes as before.

## Step 13: asynchronous I/O is gone

PostgreSQL 18 reads buffers through its asynchronous I/O
subsystem, even with `io_method=sync`: a read takes a handle,
is staged, and completes in callbacks. Nearly all of digres's
disk I/O is to go through sm, and the rest is network I/O, so
the subsystem went: `storage/aio/` (about 3,400 lines) and its
headers.

- **A buffer read is synchronous.** `WaitReadBuffers()` reads
  each run of blocks with `smgrreadv()` (`ReadBuffersIO()` in
  `bufmgr.c`), then checks each page and marks its buffer valid
  (`ReadBufferComplete()`): a page that fails verification is
  an ERROR, or is zeroed with a WARNING under
  `zero_damaged_pages`, as before PostgreSQL 18.
  `StartReadBuffers()` only pins and issues advice. A buffer
  has no I/O handle, and `WaitIO()` waits on the buffer's
  condition variable alone.
- **`read_stream.c` stays**, moved to `storage/buffer/`: heap
  scans, ANALYZE and b-tree vacuum read through it. It gives
  read-ahead advice and combines reads; batch mode is gone.
- **smgr, md and fd lost their asynchronous entry points**
  (`smgrstartreadv()`, `mdstartreadv()`, `FileStartReadV()`)
  and the raw descriptor lookup (`smgrfd()`, `mdfd()`).
- **The I/O worker process type is gone**, with the
  postmaster's `PM_WAIT_IO_WORKERS` state; the server runs a
  checkpointer and a background writer and no I/O workers. 32
  auxiliary process slots went with them.
- **Gone too**: the settings `io_method`, `io_workers` and
  `io_max_concurrency`; `pg_get_aios()` and the `pg_aios` view;
  the AIO wait events and LWLocks; the resource owners' list of
  I/O handles. The stats file format id changed.
  `effective_io_concurrency`, `maintenance_io_concurrency` and
  `io_combine_limit` stay: they size read-ahead.
- No test reads a relation's pages today (the catalog is
  compiled in), so the new read path is built but not run.
  `make test` and `make tsan` pass as before.

## Step 14: the checkpointer and the background writer are gone

A checkpoint had done nothing since WAL went (step 12), and sm
keeps what is durable: RocksDB syncs a commit to its own log and
flushes in the background, which bounds its recovery. So the two
processes went, with all that only they used.

- **`CHECKPOINT` succeeds and does nothing**, as `VACUUM` does.
  `test/server.sh` checks it.
- **The sync request machinery is gone**: `storage/sync/`, the
  queue the checkpointer absorbed and never processed, and the
  fsync and forget requests md.c and slru.c registered. Without
  a checkpointer a forget request would have retried forever.
  md.c unlinks a dropped relation's files at once: the empty
  first segment PostgreSQL kept until the next checkpoint only
  guarded a relfilenumber against WAL replay. SLRU files are
  not synced.
- **CREATE DATABASE and ALTER DATABASE SET TABLESPACE** flush
  the source database's buffers (`FlushDatabaseBuffers()`)
  before copying its files; their checkpoint requests are gone.
- **The postmaster's shutdown** goes from "backends gone" to
  waiting for dead-end children: no shutdown checkpoint, and
  the `PM_WAIT_XLOG_*`/`PM_WAIT_CHECKPOINTER` states and
  `PMSIGNAL_XLOG_IS_SHUTDOWN` are gone. It logs "shutting down"
  itself.
- **Gone with them**: `BufferSync()`, `BgBufferSync()`,
  `CheckPointBuffers()` and the other `CheckPoint*()` functions,
  the checkpoint sort array in shared memory, the free list's
  background writer wake-up; the settings `checkpoint_timeout`,
  `checkpoint_completion_target`, `checkpoint_flush_after`,
  `log_checkpoints` and the four `bgwriter_*`; the
  `pg_stat_bgwriter` and `pg_stat_checkpointer` views, their 15
  functions and two statistics kinds (the stats file format id
  changed); their wait events and the `CheckpointerComm` lock.
- **Left**: `PGPROC.delayChkptFlags`, which nothing reads, with
  the WAL-era comments around it in heapam.c and storage.c.
- The server has no child process now, but the startup process
  at start (`test/server.sh` checks for none while 8 sessions
  are open).

## Step 15: synchronized scans are gone

A sequential scan of a large table started where another one on
it was, and reported its place in shared memory as it went
(`access/common/syncscan.c`). digres does not need it: heap
scans are to be sm's.

- **A heap scan starts at block zero**, unless
  `heap_setscanlimits()` says otherwise, and reports nothing.
- **The plumbing went too**: the `SO_ALLOW_SYNC` scan flag, and
  the `allow_sync` argument of `table_beginscan_strat()`,
  `table_beginscan_sampling()`, `table_rescan_set_params()`,
  `heap_rescan()`, the `scan_rescan` and
  `index_build_range_scan` callbacks, and
  `table_index_build_scan()` and its range form, with their
  callers.
- **Gone with it**: the `synchronize_seqscans` setting,
  `trace_syncscan` and `TRACE_SYNCSCAN`, its shared memory, the
  `SyncScan` lock, and the comments that explained choices by
  it.

## Step 16: background workers are gone

A background worker is server code in a process of its own,
forked by the postmaster: a static one an extension registers
at start, a dynamic one a backend asks for. Nothing in digres
registered one since parallel query and logical replication
went (steps 10 and 12), and as forked processes attached to
shared memory they stood in the way of making it private.

- **Gone**: `postmaster/bgworker.c`, `bgworker.h` and
  `bgworker_internals.h`; the `B_BG_WORKER` process type; the
  postmaster's start, restart and notification of workers, and
  `PMSIGNAL_BACKGROUND_WORKER_CHANGE`; their PGPROC free list
  and shared memory; the `max_worker_processes` setting, which
  no longer counts toward `MaxBackends`; the `BackgroundWorker`
  lock and two wait events.
- An extension that wants one can have it back as a thread, as
  connections were made.

## Step 17: the startup process is gone

The postmaster forked the startup process to replay WAL before
it let connections in. With no WAL (step 12), all it did was
call `StartupXLOG()`, which gave the transaction counters their
first values, and exit.

- **The counters get their first values with shared memory**:
  `VarsupShmemInit()` sets `nextXid`, `nextOid`,
  `latestCompletedXid` and the wraparound limits when it makes
  `TransamVariables`, as `StartupXLOG()` did. That covers the
  postmaster, a standalone backend and bootstrap, and the
  restart after a crash, which makes shared memory anew.
- **The postmaster opens at once**: `StartRunning()` does what
  the startup process's clean exit did (state `PM_RUN`, "ready
  to accept connections", `postmaster.pid`).
- **Gone**: `postmaster/startup.c` and `startup.h`, the
  `B_STARTUP` process type, `StartupXLOG()`; the postmaster's
  `PM_STARTUP`, `PM_RECOVERY` and `PM_HOT_STANDBY` states and
  the startup child's status; the connection refusals "starting
  up" and "not accepting connections"; the three recovery
  postmaster signals; the `log_startup_progress_interval`
  setting and its timeout; `storage/file/reinit.c`, which reset
  unlogged relations after a crash and had no caller.
- The postmaster forks nothing now but the syslogger, and that
  only with `logging_collector`.

## Step 18: xlog.c is gone

What steps 9 and 17 left of the write-ahead log manager was
the control record, made in memory for one run, and the
message at shutdown.

- **The mock authentication nonce is `auth-scram.c`'s**: the
  postmaster, or a standalone backend, makes it once with
  `scram_init_mock_nonce()` before any session starts, and a
  session copies it (SHARED).
- **`DataChecksumsEnabled()` is an inline `false`** in
  `bufpage.h`: digres makes no data checksums.
- **"shutting down"** is logged by the postmaster itself, and
  by a callback in `postinit.c` for a standalone backend.
- **Gone**: `access/transam/xlog.c`, `access/xlog.h`,
  `catalog/pg_control.h`, `LocalProcessControlFile()` and the
  control record's shared memory.

## Step 19: the commit log is gone

The commit log (`clog.c`, `pg_xact`) kept two bits per
transaction, committed or aborted, in SLRU pages. sm1's status
record is to replace it; until the heap moves to sm, its
callers get a dummy.

- **Every valid transaction id is said to have committed**:
  `TransactionIdDidCommit()` is true for it, and
  `TransactionIdDidAbort()` only for the invalid id. Callers
  ask `TransactionIdIsInProgress()` first, so a running
  transaction is still seen as running; but an aborted
  transaction's changes are visible.
- **Gone**: `clog.c` and `clog.h`; commit and abort no longer
  mark anything (`TransactionIdCommitTree()`,
  `TransactionIdAbortTree()`); the group update fields of
  PGPROC; the `transaction_buffers` setting; the `XactBuffer`
  and `XactSLRU` lock tranches; the `pg_xact` directory.
- **Left**: `pg_xact_status()` and its `oldestClogXid`, which
  now answer from the dummy; the "transaction" row of
  `pg_stat_slru`, always 0.

## Step 20: the syslogger is gone

The syslogger was a process the postmaster forked when
`logging_collector` was on. Every process's stderr was a pipe
to it, written in chunks with a small header so that messages
from different processes did not interleave, and it wrote them
to log files it rotated. In a process of threads it was the
last fork that ran server code rather than exec, which is
unsafe once there are threads.

- **Every message goes to stderr**, the plain, csv and json
  forms alike, in one `write()` under one `std::mutex` in
  `elog.c` (`write_console()`). A logger that writes files is
  to come later.
- **The mutex is one for the process**, not a thread's:
  `src/tools/process_wide.txt` lists it, and
  `cxx_thread_local.py --check` accepts what that file lists
  and nothing else outside `thread_local`.
- **Gone**: `syslogger.c` and `syslogger.h`, the `B_LOGGER`
  process type, the pipe and its chunk protocol
  (`write_pipe_chunks()`, `redirection_done`),
  `PMSIGNAL_ROTATE_LOGFILE`, the `SYSLOGGER_MAIN` wait event;
  the settings `logging_collector`, `log_directory`,
  `log_filename`, `log_rotation_age`, `log_rotation_size`,
  `log_truncate_on_rotation` and `log_file_mode`; the SQL
  functions `pg_rotate_logfile()`, `pg_current_logfile()` and
  `pg_ls_logdir()`.
- **Gone with it**, having lost their last callers:
  `postmaster_child_launch()` and `StartChildProcess()`.
  `fork_process()` is left to the Windows-only `EXEC_BACKEND`
  code, which has its own step.
- Nothing forks now but `popen()`, for `COPY ... PROGRAM` and
  `pg_import_system_collations()`, which execs at once.

## Step 21: csvlog and jsonlog are gone

With the syslogger gone (step 20), the CSV and JSON forms of a
log message only went to stderr beside the plain one. They are
dropped: `log_destination` takes `stderr`, `syslog` and, on
Windows, `eventlog`, and rejects `csvlog` and `jsonlog`.

- **Gone**: `utils/error/csvlog.c` and `jsonlog.c`,
  `write_csvlog()` and `write_jsonlog()`, and the
  `LOG_DESTINATION_CSVLOG` and `LOG_DESTINATION_JSONLOG` bits.
- The helpers they shared with the plain form
  (`get_formatted_log_time()`, `check_log_of_query()` and the
  rest) stay: `log_line_prefix` uses them.

## Step 22: the process and thread control blocks

Every writable global is `thread_local`, marked SHARED or
LOCAL, and a new thread is given a copy of its parent's SHARED
ones. SHARED does not mean shared: each thread has its own
copy, and what one sets after another starts the other never
sees. The globals are to move into two blocks instead
(`utils/control_block.h`):

- **`pcb_t`, one for the process**, made by `main()` before
  anything else, never freed. A field is set before the first
  thread starts and only read after, or guards itself. `pcb`
  is the one writable global outside `thread_local`:
  `src/tools/process_wide.txt` lists it, and
  `cxx_thread_local.py --check` accepts what that file lists.
- **`tcb_t`, one for each thread** that runs digres code:
  `main()` makes the first thread's, `session_thread_main()`
  a session's, and `session_thread_end()` frees it last.
- **A field is a value or a `std::unique_ptr`** to what the
  block owns. A module's state is a struct the header only
  declares; the module defines it, the function that makes it
  and its deleter, so its layout stays its own.

The pilot moves two things:

- **`stderr_mutex` into the pcb** (`elog.c`).
- **All of `timeout.c`'s globals into `timeout_state_t`**,
  `tcb->timeout`. Its destructor deletes the thread's timer,
  which an `on_proc_exit` callback did. `tcb_free()` clears
  `tcb` before freeing it, and the SIGALRM handler returns when
  `tcb` is NULL. Fifty sessions leave no timer behind; without
  the destructor's `timer_delete()` they leave a hundred.

Nothing else is to move for its own sake: a global goes into a
block when a change touches it, and new state goes into one
from the start. `tcb` stays a pointer: a `thread_local tcb_t`
object would call its init function on every access from
another file, to register its destructor, and would be
destroyed after `session_thread_end()`, with no NULL for a
signal handler to see.

## Step 23: crash restart is gone

When a child process crashed, PostgreSQL's postmaster told the
others to quit, waited for them, made shared memory anew and
started again. In digres a crash, a fault in any thread, ends
the whole process, so what was left to reach that path was a
thread ending with an unexpected exit code, or without
releasing its slot cleanly. Restarting under such a thread
would rebuild the state the threads share inside the running
process.

- **An abnormal thread end stops the server**: the postmaster
  tells the other sessions to quit, waits for them, logs
  "abnormal database system shutdown" and exits 1, as
  PostgreSQL does with `restart_after_crash` off. The settings
  `restart_after_crash` and `remove_temp_files_after_crash`
  are gone.
- **The postmaster reaps no child.** Its SIGCHLD handler and
  its `waitpid(-1)` loop went: it has no child process, and the
  loop took the child of a session's `popen()` before the
  session's `pclose()` could, so `COPY ... TO PROGRAM` failed
  half the time with "could not close pipe to external command:
  No child processes". `test/server.sh` now runs it twenty
  times; it failed before this step.
- **Gone with them**: the walreceiver, WAL summarizer and slot
  sync worker process types, which nothing started since WAL
  and replication went (step 12), their slots,
  `PMSIGNAL_START_WALRECEIVER`, and the PGPROC pool for
  "special workers", so `MaxBackends` is `max_connections`;
  and `postmaster/auxprocess.c`, which nothing called since the
  startup process went (step 17).
- **Left**: the auxiliary PGPROC slots
  (`NUM_AUXILIARY_PROCS`), sized with shared memory, and the
  Windows `waitpid()` emulation, which goes with
  `EXEC_BACKEND`.

## Step 24: `EXEC_BACKEND` is gone

`EXEC_BACKEND` was PostgreSQL's way to start a child where there
is no `fork()`: `fork()` and `exec()`, or `CreateProcess()` on
Windows, and code to hand the child the postmaster's state
through a file. It was defined only on Windows, so it was never
compiled here; with no child process left, nothing could use it.

- **Every `#ifdef EXEC_BACKEND` is resolved** as not defined,
  in 27 files: the branch for it is gone, the other kept.
- **Gone**: `SubPostmasterMain()`, `internal_forkexec()`, the
  saving and restoring of backend variables, the `forkchild`
  dispatch option, `fork_process.c`, the `NON_EXEC_STATIC`
  macro (its uses are `static`), and what lost its last caller:
  `ClosePostmasterPorts()` and the Windows emulation of
  `waitpid()` with its dead-child queue. `launch_backend.c`
  keeps only the names of the kinds of backend.
- **Left**: comments in code that stays still explain choices
  by `EXEC_BACKEND`; `load_hba()` and `load_ident()`, whose
  last caller was its reload of the configuration, go with
  the rest of `hba.c` when authentication's files do;
  `PGSharedMemoryDetach()`, which goes with shared memory.

## Step 25: shared memory is the process's own

PostgreSQL's shared memory was a segment its processes attached:
a small System V segment, whose key went into `postmaster.pid`
so that a new postmaster could tell whether an old one's
processes were still attached, and an anonymous shared mapping
for the rest, with huge pages if asked for. digres is one
process, whose threads share all its memory.

- **The block is a private anonymous mapping**
  (`port/private_shmem.c`): one `mmap`, page aligned, zero, its
  pages committed as first touched, never unmapped. The
  structures in it and the code that lays them out are as they
  were.
- **No segment, no interlock.** Line 7 of `postmaster.pid`,
  which held the key, is left empty, so pg_ctl still finds the
  lines after it. A postmaster that finds a stale
  `postmaster.pid` no longer looks for orphaned processes
  attached to a segment: a session is a thread, and ends with
  its process. `postmaster.pid` and RocksDB's own locks still
  keep a second server off the data directory.
- **Gone**: `port/sysv_shmem.c`; the settings `huge_pages`,
  `huge_page_size`, `shared_memory_type`, and the reported
  `huge_pages_status` and `shared_memory_size_in_huge_pages`.
- **Left**: the semaphores (unnamed POSIX ones, which work
  between threads as between processes), and dynamic shared
  memory (`dsm.c`, `dsm_impl.c`), which the statistics use.

## Step 26: there is no shared memory

Step 25 kept PostgreSQL's one block, sized ahead of time and
carved up by name, only as a private mapping. The block itself
goes: each structure the threads share is an ordinary
allocation.

- **`ShmemAlloc()` is a heap allocation**: zeroed, cache-line
  aligned, never freed. Large ones are committed only as
  touched, since the allocator maps them afresh: with
  `shared_buffers` at 128MB an idle server is 28MB resident.
- **The index of names is in the pcb** (`pcb->shmem_index`, a
  map and its mutex), made once by
  `CreateSharedMemoryAndSemaphores()` before any thread starts.
  `ShmemInitStruct()` and `ShmemInitHash()` keep their
  contract: a session thread that asks for a structure by name
  finds the postmaster's. The circular bootstrap of the old
  index, a shared hash table found through the segment header,
  went with it, and so did `ShmemIndexLock`.
- **Nothing is sized ahead.** `CalculateShmemSize()` went, and
  with it every `*ShmemSize()` function it alone called, and
  `hash_estimate_size()`; those a module's initialization still
  uses to size its structure stay. Also gone:
  `RequestAddinShmemSpace()`, the `shared_memory_size`
  setting, `PGShmemHeader` and `storage/pg_shmem.h`,
  `port/private_shmem.c`, `ShmemAllocUnlocked()` (its callers
  use `ShmemAlloc()`), `ShmemAddrIsValid()`, and the
  `pg_shmem_allocations` and `pg_shmem_allocations_numa`
  views with their functions.
- **Dynamic shared memory no longer records its control
  segment in a header**, and `dsm_cleanup_using_control_segment()`,
  whose caller went in step 25, is gone.
- The names `ShmemInitStruct()` and the like are kept: renaming
  some ninety calls would change nothing they do.

## Step 27: what was ShmemAlloc'd is malloc'd, its pointer in the pcb

Three things called `ShmemAlloc()` directly, outside
`ShmemInitStruct()`. Each is now `malloc()`'d by the postmaster
or a standalone backend, never freed, its pointer a field of
the pcb, which is never freed either:

- **The main LWLock array**: `pcb->main_lwlock_array`.
  `MainLWLockArray` is a macro for it, so the generated
  `lwlocknames.h` and the headers that index it are unchanged.
- **The tranche counter**, which sat in the same allocation
  just before the first lock, is a value, `pcb->lwlock_counter`,
  with its own mutex, `pcb->lwlock_counter_mutex`. It was
  guarded by `ShmemLock`, a spinlock that `ShmemAlloc()` had
  used too; it had no other user left, and is gone.
- **The semaphores** (`posix_sema.c`): `pcb->shared_semas`.
  `sysv_sema.c`, which this build skips, is unchanged.

## Step 28: the statistics system is gone

Everything named `pgstat_*` went: the cumulative statistics
(per table, index, function, database, I/O, WAL and SLRU
counters, kept in a dynamic shared memory hash table and written
to `pg_stat/` at shutdown), the backend status behind
`pg_stat_activity`, progress reporting for the
`pg_stat_progress_*` views, and wait-event reporting. sm is to
keep what statistics a table has; monitoring can come back on
the pcb when it is wanted.

- **Gone**: `utils/activity/pgstat*.c`, `backend_status.c`,
  `backend_progress.c`, `wait_event.c` and
  `wait_event_funcs.c`, `utils/adt/pgstatfuncs.c`, and the
  headers `pgstat.h`, `utils/backend_status.h`,
  `utils/backend_progress.h`, `commands/progress.h`,
  `utils/pgstat_internal.h`, `utils/pgstat_kind.h`; their 108
  SQL functions (`pg_stat_*`, `pg_get_wait_events`) and the
  views over them in `system_views.sql`.
- **Settings gone**: `track_activities`, `track_counts`,
  `track_functions`, `track_activity_query_size`,
  `track_cost_delay_timing` and `stats_fetch_consistency`.
  `track_io_timing` stays: EXPLAIN, ANALYZE and temporary files
  time their I/O with it.
- **What fed only statistics went with it**: the `IOContext`
  and `IOObject` arguments threaded through the buffer manager,
  `IOContextForStrategy()`; function usage tracking, the
  `fn_stats` field of `FmgrInfo` and the two
  `EEOP_FUNCEXPR_*_FUSAGE` expression steps; the idle stats
  timeout; the session end cause; `PGPROC.wait_event_info`;
  `RelationData.pgstat_info`; the `progress` argument of
  `WaitForLockers()`, `WaitForLockersMultiple()` and
  `WaitForOlderSnapshots()`; COPY's `bytes_processed`; and the
  query text in deadlock reports and in the postmaster's log of
  a backend that ended abnormally.
- **Kept**: `pg_backend_pid()`, which lived in
  `pgstatfuncs.c`, is in `misc.c`. The wait-event ids
  (`wait_event_types.h`, from `wait_event_names.txt`) are still
  passed to `WaitLatch()` and the like, and go nowhere: removing
  that argument is a later change. So is the `progress`
  argument of the table AM's index build scan, and
  `REINDEXOPT_REPORT_PROGRESS`, now inert. `%Q` in
  `log_line_prefix` printed the query id from the backend
  status, and is gone.
- **Caught on the way**: removing a progress call dropped the
  `++processed` inside its arguments, so COPY counted no rows;
  the counts are back. A script that removed statements also
  removed the block before each one in a few files, which
  `test/server.sh`'s COPY TO PROGRAM check found.

## Step 29: dynamic shared memory is gone

Dynamic shared memory let PostgreSQL's processes share memory
made after startup: segments (`dsm.c`, `dsm_impl.c`, POSIX
`shm_open` by default), an allocator across them (`dsa.c`) that
hands out segment-relative pointers because each process maps a
segment at its own address, a concurrent hash table on it
(`dshash.c`), and named segments for extensions
(`dsm_registry.c`). Parallel query and the statistics system
were its users; both are gone, and in one process a plain
pointer works in every thread.

- **Gone**: those five files and their headers, and
  `lib/radixtree.h`, which nothing included since VACUUM's TID
  store went and which needed `dsa.h`; the settings
  `dynamic_shared_memory_type` and `min_dynamic_shared_memory`;
  `DSMRegistryLock`; the lock tranches of the DSM registry, the
  statistics and the logical replication launcher.
- **Shutdown has one list of callbacks fewer**: `shmem_exit()`
  no longer runs the segments' detach callbacks.
- The server makes nothing under `/dev/shm`.

## Step 30: threads are std::thread

Session threads were started with `pthread_create()`, and the
postmaster's queue of ended threads was guarded by a
`pthread_mutex_t`.

- **`session_thread_start()` returns a `std::thread`**, which
  the caller joins (`parse_tool.c`'s `--threads`) or detaches
  (`backend_thread.c`). A `std::thread` has no result, so the
  session's exit code goes where the caller asks
  (`exit_code`), instead of being the thread's result.
  `run.sh`'s FATAL check depends on it: with the code never
  written, the process exits 0, not 1.
- **The exit queue's lock is a `std::mutex`**, taken with
  `std::lock_guard`, and the queue is made with `new`.
- **`pthread_sigmask()` stays**, to set a thread's signal
  mask, and so does `pthread_is_threaded_np()`, which only
  macOS has and this build does not compile.

## Step 31: a latch wakes its thread through sys

A latch's setter woke its owner with `tgkill(SIGURG)`, which
the owner read from a `signalfd` in its epoll set: a signal to
carry a wake-up from one thread of the process to another.
sys (`../sm/sm0/INTERRUPT.md`) has one already: each thread's
wake fd, an eventfd that `sys_interrupt()` writes.

- **A latch keeps its owner's handle** (`sys_thread_ref_t`),
  made by `OwnLatch()` or `InitLatch()`, dropped by
  `DisownLatch()` or, for a thread's local latch, by
  `ShutdownProcessLocalLatch()` as the thread ends. A
  spinlock in the latch guards it; `SetLatch()` copies the
  handle under it and wakes the copy, so an owner that lets
  go meanwhile is no hazard.
- **Another thread is woken with `sys_interrupt(owner, 0)`**:
  no bit, a write to its wake fd. The latch is the news.
- **The thread itself, from a signal handler, with
  `sys_interrupt_self(0)`**, which takes no lock: the
  postmaster's handlers set its latch.
- **The epoll wait set waits on `sys_wake_fd()`**, made by
  `InitializeWaitEventSupport()` before a handler may need it,
  and drains it with `sys_clear_wake_fd()`. sys closes it as
  the thread ends.
- **Gone**: the `signalfd`, SIGURG and `tgkill()`. The
  self-pipe, kqueue and Windows paths are untouched: no build
  of digres compiles them.
- **A backend's launch record keeps the thread's handle**, so
  the postmaster can wake it before it has a PGPROC.
- **`make tsan` links a sanitized libsys**: sys's atomics hand
  a thread's handle from one thread to another, and the
  sanitizer reported races in an uninstrumented copy.
- **`server.sh`'s shutdown must take under ten seconds**: each
  side wakes the other's latch, and a lost wake-up showed only
  as a minute's stall, not a failure.

## Step 32: signals between threads are named interrupt bits

A session thread took no real signal: `SignalProc()` set bit
*n*, for signal *n*, in the target's PGPROC (or, before it had
one, in its launch record), the thread found it through
`MyPendingSignals`, and ran the handler it had set with
`pqsignal()`. That was signals emulated over a word of bits;
sys gives each thread such a word of its own.

- **The bits are named** (`storage/procsignal.h`):
  `INTERRUPT_CANCEL` (was SIGINT), `INTERRUPT_DIE` (SIGTERM),
  `INTERRUPT_QUIT` (SIGQUIT), `INTERRUPT_RELOAD` (SIGHUP),
  `INTERRUPT_PROCSIG` (SIGUSR1, with its reason in the
  ProcSignal slot as before).
- **They live in the thread's sys word**, not the PGPROC:
  `InterruptProc(proc, bits)` raises them in the thread that
  owns the PGPROC's latch, and the postmaster raises them in a
  backend through the handle in its launch record, PGPROC or
  not. Gone: `PGPROC.pendingSignals`, the launch record's
  word, `MyPendingSignals`, `AdoptPendingSignals()`.
- **A handler is set per bit** with `SetInterruptHandler()`,
  a `void (void)` function: `StatementCancelHandler`, `die`,
  `quickdie`, `SignalHandlerForConfigReload`,
  `SignalHandlerForCrashExit`, `procsignal_sigusr1_handler`,
  `process_startup_packet_die`. `pqsignal()` is left to the
  postmaster's real signals and to faults (SIGFPE);
  `pqsignal_handler()`, which `SignalSelf()` used, is gone.
- **`DeliverInterrupts()`** takes the pending bits and runs
  their handlers, from `ProcessInterrupts()` and the wait loop
  as before; `InterruptSelf()` runs them at once (the
  statement and lock timeouts), `InterruptSelfLater()` leaves
  them for later (the startup packet's timeout).
- **`CHECK_FOR_INTERRUPTS()` loads the word inline**, through
  `MyInterrupts` (`sys_pending_word()`), set by
  `InitPostmasterChild()` and `InitStandaloneProcess()`.
- **Signal numbers are left at the edges**:
  `InterruptsOfSignal()` maps the postmaster's `signal_child()`
  and `pg_signal_backend()`'s signals to bits.
- `server.sh`'s shutdown check kills a server that does not
  stop, and fails, rather than hanging the test.

## Step 33: a thread's timeouts are a sys timer

`timeout.c` multiplexed a thread's timeouts on one POSIX timer
of its own (`timer_create()` with `SIGEV_THREAD_ID`), which
sent SIGALRM to that thread; the handler ran in signal
context. sys has timers that raise a bit in a thread.

- **`set_alarm_timer()` arms a sys timer** for the nearest
  timeout, raising `INTERRUPT_ALARM`, and cancels the one armed
  before: one timer a thread, as before. The timeout state
  cancels it as the thread ends.
- **`handle_sig_alarm()` is the bit's handler**, run where
  interrupts are delivered, not in a signal handler. The rest
  of `timeout.c`, its bookkeeping of what is due and its
  tolerance of an early or a spare alarm, is as it was.
- **A session thread blocks SIGALRM too**: it takes no signal
  but the faults it causes itself.
- sys's timer thread starts with the signal mask of the thread
  that arms the first timer, a session thread's: every signal
  blocked.

## Step 34: the postmaster ignores SIGUSR1

SIGUSR1 told the postmaster two things: `pg_ctl logrotate`'s
request, and a child's pmsignal, a reason flag in shared
memory (`SendPostmasterSignal()`). There is no log to rotate
yet, and nothing sends a pmsignal: its one reason,
`PMSIGNAL_ADVANCE_STATE_MACHINE`, had no sender left.

- **SIGUSR1 is `SIG_IGN` in the postmaster.**
- **Gone**: `handle_pm_pmsignal_signal()`,
  `process_pm_pmsignal()`, `SendPostmasterSignal()`,
  `CheckPostmasterSignal()`, `PMSignalReason` and the flags it
  indexed. pmsignal.c keeps the per-child flags and the reason
  for SIGQUIT.
- `server.sh` sends the server SIGUSR1 and confirms it still
  runs a statement.

## Step 35: signal leftovers

- **SIGUSR2 is `SIG_IGN` in the postmaster**, not a dummy
  handler that reserved it for children: there are no child
  processes to reserve it for. `dummy_handler()` is gone.
- **`pqsignal()` in a session thread sets no process handler
  at all**: it kept one exception, SIGALRM from the thread's
  own timer, and that timer is now sys's.
- SIGALRM stays `SIG_IGN` in the postmaster: nothing sends it,
  and a stray one should not end the server.
- **Left on purpose**: SIGURG in `waiteventset.c`'s self-pipe
  and kqueue paths, which only builds without epoll compile
  (a platform choice, not a removed module); and the
  backend's `pqsignal()` calls for SIGPIPE, SIGUSR2, SIGFPE and
  SIGCHLD, which a standalone backend, the main thread, still
  takes. In a session thread they set nothing, so a session's
  SIGFPE has no handler: it ends the server.
- `server.sh` sends SIGUSR2 too.

## Step 36: only the postmaster's thread takes a signal

A signal sent to the process goes to any one thread that does
not block it. Session threads blocked them, but RocksDB's
threads and a thread of sm's, started while the postmaster
attached sm with its signals unblocked, did not: a SIGHUP
landing there would have run the postmaster's handler, which
sets the latch of the thread it runs in, in a thread with none.

- **`datadir_load()` blocks every signal around `sm_attach()`**,
  so the threads sm and RocksDB start then inherit the mask.
  Those started later are started by session threads, which
  block every signal already.
- **sys's timer thread blocks every signal itself**, whichever
  thread arms the first timer.
- `server.sh` reads each thread's mask from `/proc`, and fails
  if a thread other than the postmaster's leaves SIGHUP,
  SIGINT, SIGQUIT, SIGTERM, SIGUSR1, SIGUSR2 or SIGALRM
  unblocked; before the change, RocksDB's two threads and one
  more did.

## Step 37: CREATE TABLE makes a table in sm

The first of `../sm/PART4.md` §9.5's three statements. A table
of `int2`, `int4` and `int8` columns, some NOT NULL, is created
in sm3's catalog, in the session's transaction, and seen by
every session once it commits.

- **`access/smstore.h` is digres's one way into sm.** Only
  `smstore.c` includes `sm.h`; the rest of the backend asks it
  in PostgreSQL's types. Its session is the one
  `datadir_open_session()` opened (`datadir_sm_session()`).
- **An sm transaction follows the PostgreSQL one.** It begins
  at the first call that needs one, at the transaction's
  isolation level and read-only flag; it commits at
  `XACT_EVENT_PRE_COMMIT`, so a commit sm refuses fails the
  transaction, and aborts at `XACT_EVENT_ABORT`. A transaction
  that touches no table begins none. Not yet: savepoints, and a
  READ COMMITTED transaction's new snapshot each statement
  (`sm_stmt_begin`).
- **sm's errors map to SQLSTATEs**: a deadlock to `40P01`, an
  interrupted wait to `57014`, anything retryable to `40001`,
  the rest to `XX000`.
- **Names are sm's.** `get_namespace_oid()` and
  `get_relname_relid()` ask sm3's catalog, which has
  `pg_catalog` and `public` built in; a relation is a table or
  an index. The search path's `"$user"` is the role's name from
  `GetUserNameFromId()`, not a `pg_authid` row.
- **A type is its compiled-in row.** The parser's `Type` is a
  `const cat_type_t *`, as `Operator` became one in step 6:
  nothing to release. The name lookup, `TypeIsVisible()` and
  `format_type_extended()` read `cat_type()`.
  `getTypeIOParam()` has an overload for a row, and
  `check_of_type()` takes one. The ALTER TYPE paths look the
  type up by oid in the syscache, as before, and fail there.
- **`ProcessUtilitySlow()` creates the table with
  `smstore_create_table()`**, not `DefineRelation()`: no heap,
  and no TOAST table beside it. What sm's tables have no room
  for yet is refused: inheritance, partitions, `OF`, CHECK,
  defaults, collations, identity, generated columns, options,
  tablespaces, temporary and unlogged tables, and any type but
  the three integers. A PRIMARY KEY or UNIQUE becomes an index
  statement, which still fails at the catalog.
- **Event triggers find none**: `EventCacheLookup()` returns
  an empty list, since `pg_event_trigger` does not exist. The
  rest of event triggers is not removed yet.
- `server.sh` creates a table, and confirms another session
  sees it, that IF NOT EXISTS passes over it, and that a text
  column is refused.

## Step 38: INSERT and a sequential scan go to sm

The other two of `../sm/PART4.md` §9.5's statements: rows go
into a table in sm, and a scan reads them back, under the
transaction's isolation level.

- **A user's relation is built from sm3's definition**
  (`RelationBuildSmDesc()`): any oid from `FirstNormalObjectId`
  up. It is a plain permanent table whose row type is a record,
  with no options, rules, triggers, row security, indexes or
  extended statistics; its columns and NOT NULL are sm's. It
  keeps the heap's table AM routine, which the executor still
  asks for some things, and its rows are read and written by
  sm. `RelationIsSm()` tells such a relation.
- **smstore opens a table once a transaction**, to read or to
  write, and closes it as the transaction ends; the table's
  lock is held until then.
- **`ExecInsert()` calls `smstore_insert()`**, which writes the
  row and every index; a row's tid carries its rowno, 65535
  to a block. Anything else that writes, UPDATE, DELETE, MERGE,
  ON CONFLICT, is refused at `ExecInitModifyTable()`, before a
  heap that is not there is touched.
- **A sequential scan of a table in sm is sm's**: forwards,
  in rowno order, into a virtual slot.
- **Each command begins a statement in sm** (`sm_stmt_begin`,
  from `StartTransactionCommand()`): READ COMMITTED reads at a
  snapshot a statement, and a statement does not see its own
  rows.
- **The planner reads what is compiled in, or nothing**:
  `pg_cast` is compiled in (`gen_cat.py` takes a catalog with
  no name column), with `get_builtin_cast()`; a coercion's and
  a function's cost read `cat_proc()`. There are no statistics
  (`get_statistic_tuple()` finds none) and no tablespace
  options, and a table in sm is estimated as ten pages, as a
  heap never analyzed is.
- **RELOID lookups of a user's relation read its descriptor**
  (`RelationGetClassForm()`): `has_subclass()`, row security.
  Its permissions are the default of its owner, the one
  superuser.
- **An abort no longer checks the commit log**:
  `TransactionIdDidCommit()` is a dummy since step 19, true for
  every xid, so the check turned any error after an xid was
  assigned into a PANIC that ended the server.
- **Not yet**: `=` in a WHERE, which needs `pg_amop`; aggregates;
  INSERT ... SELECT, which reads `pg_constraint`; savepoints.
- `make tsan` suppresses races with a RocksDB frame in them
  (`test/tsan.supp`): RocksDB is not built for the sanitizer.
- `server.sh` inserts and scans, with and without a WHERE,
  checks NOT NULL, a rollback, that UPDATE is refused, and that
  a scan at READ COMMITTED sees another session's commit and
  one at REPEATABLE READ does not.

## Step 39: = in a WHERE, and DROP TABLE

The rest of the first milestone: a scan filtered by equality,
and a table dropped.

- **`pg_am` and `pg_amop` are compiled in.** Every pg_amop
  lookup in `lsyscache.c` reads the compiled-in rows: by
  operator, by operator, purpose and family, and by family,
  types and strategy, a search of a thousand-odd rows in order.
  `GetIndexAmRoutineByAmId()` reads `cat_am()`. The planner
  needs them for any `=`, to find its btree and hash families.
- **DROP TABLE is `smstore_drop_tables()`**: each table named
  is looked up, locked, dropped from sm with its indexes, rows
  and sequence, and its descriptor invalidated in every session
  at commit. IF EXISTS passes over one that is not there.
  CASCADE and RESTRICT do the same: nothing but its indexes
  depends on a table in sm. DROP of any other kind of relation
  is as it was.
- A session that used a table finds the one created in its
  place under the same name, with other columns: a name is
  looked up in sm each statement.
- `server.sh` filters with `=`, and drops a table from one
  session while another has used it.

## Step 40: a wait in sm ends on an interrupt

sm's waits for another transaction are a `sys_wait()` on the
thread's wait mask (`../sm/sm0/INTERRUPT.md` §8), and fail
with `SM_ERR_INTERRUPTED` when a bit in it is raised.

- **A session thread's wait mask** is `INTERRUPT_CANCEL`,
  `_DIE`, `_QUIT` and `_ALARM`, set where `MyInterrupts` is:
  what cancels or ends a statement, and a timeout, which may.
  A latch's wake raises no bit, and `_RELOAD` and `_PROCSIG`
  wait for the statement's next `CHECK_FOR_INTERRUPTS()`.
- **A call into sm that may wait is made through
  `waiting()`** (`smstore.c`): opening a table, creating and
  dropping one, inserting a row. On `SM_ERR_INTERRUPTED` it
  runs `CHECK_FOR_INTERRUPTS()`, which raises what the
  interrupt calls for, a cancel's or a timeout's error; one
  that calls for nothing, or must wait until interrupts are
  allowed, has been taken, and the call is made again.
- **No SQL statement waits in sm yet.** A table is also locked
  in PostgreSQL's lock manager, before sm's, so a session that
  conflicts waits there; a row's lock is not reached until
  UPDATE and DELETE are. The path is untested end to end.
- **Two lock managers lock the same table**, PostgreSQL's and
  sm's (`../sm/PART4.md` §3 warns of it): a deadlock across
  them is one neither detector sees. Not resolved here.
- `server.sh` holds a table's lock in one session, and confirms
  a scan in another is ended by its statement_timeout, and by
  a cancel request from its psql: the interrupt bits of steps
  32 and 33 reach a thread waiting in the lock manager.

## Step 41: sm locks its tables, and nothing else does

PLAN.md §7 now records it: sm's lock manager is the only one, in
the end. sm locks a table as it is opened and a record as it is
read or written; the parser and executor lock nothing of their
own, and only a special operation asks sm for a lock.

- **PostgreSQL's relation locks are no-ops for a table in sm**
  (`lmgr.c`, `locked_by_sm()`): taking one, conditionally or
  for the session, does nothing but take the invalidation
  messages waiting, as acquiring a lock would; releasing one
  does nothing; and a check whether one is held says it is.
  Catalog relations, which are not sm's, lock as before.
- **A session that conflicts now waits in sm**: a scan of a
  table another session is dropping waits in `sm_tbl_open()`
  for the drop's lock. So step 40's path is reached, and
  `server.sh`'s lock-wait checks test it: PostgreSQL logs no
  lock wait, and with `waiting()` not acting on
  `SM_ERR_INTERRUPTED` both checks fail.
- One deadlock detector for tables in sm. The lock manager
  keeps every other tag: advisory locks, transaction ids,
  objects.

## Step 42: a savepoint rolls back its rows in sm

A subtransaction that aborted left its writes in sm: ROLLBACK
TO SAVEPOINT undid nothing there. Until sm's transactions
replace `xact.c` (PLAN.md §7), sm's follow PostgreSQL's
subtransactions as they follow its transactions.

- **Each subtransaction gets an sm savepoint at its first call
  into sm** (`savepoints_begin()`), and every open one below
  it that has none: one that has made no call has nothing to
  undo. Their depths are kept by nesting level.
- **A subtransaction's end ends its savepoint**
  (`subxact_callback()`): as it commits, `sm_forget()` keeps
  its writes; as it aborts, `sm_rollback()` undoes them and
  `sm_forget()` lets it go. ROLLBACK TO SAVEPOINT is an abort
  and a new subtransaction, which gets a savepoint of its own
  when it next writes. An error raised in a subtransaction
  aborts it the same way.
- An abort cannot raise an error; a savepoint sm cannot undo
  belongs to a transaction sm has already ended, and the next
  call into sm says so.
- `server.sh` rolls back, releases and nests savepoints, with
  one set before any row and one whose insert failed.

## Step 43: LISTEN and NOTIFY are gone

PLAN.md §7: sm's transactions are to replace `xact.c`, and
PostgreSQL's transaction ids go with it. NOTIFY needs one: its
queue, an SLRU under `pg_notify`, is ordered by the notifying
transaction's xid, and every listener reads it at commit.

- **Gone**: `commands/async.c` and its header; the SQL
  functions `pg_notify`, `pg_listening_channels` and
  `pg_notification_queue_usage`; the settings `notify_buffers`,
  `max_notify_queue_pages` and `trace_notify`; the LWLocks
  `NotifyQueue` and `NotifyQueueTail` and the tranches
  `NotifyBuffer` and `NotifySLRU`, with their wait events; the
  `PROCSIG_NOTIFY_INTERRUPT` reason; the commit, abort and
  subtransaction hooks in `xact.c`; the reads of the client's
  notifies in `postgres.c`; the `pg_notify` directory.
- **LISTEN and NOTIFY are refused** when they run. UNLISTEN
  succeeds and does nothing, as it did for a session listening
  to nothing, and DISCARD ALL no longer unlistens: poolers send
  both.
- The grammar and nodes stay: sq parses the statements, and
  digres refuses them.

## Step 44: xact.c calls sm, and has no callbacks

The first of PLAN.md §7's steps toward sm's transactions
replacing `xact.c`: the transaction's ends are sm's, said in
`xact.c` itself, not through a callback.

- **`CommitTransaction()` calls `smstore_commit()`** where the
  pre-commit callbacks ran, so a commit sm refuses still fails
  the transaction; **`AbortTransaction()` calls
  `smstore_abort()`**, before the resource owner's release and
  whether or not one was made. A subtransaction's commit and
  abort call `smstore_subcommit()` and `smstore_subabort()`:
  its savepoint is forgotten, or rolled back and forgotten.
- **The transaction callbacks are gone**:
  `Register`/`UnregisterXactCallback()`, their subtransaction
  pair, the lists and the event types. smstore was their only
  user; digres loads no module that would register one.
- The begin is still lazy, at the first call into sm, so that
  `SET TRANSACTION ISOLATION LEVEL` after BEGIN still sets
  sm's level.

## Step 45: digres assigns no transaction ids

A transaction is sm's (PLAN.md §7), and a table in sm is no
heap: nothing digres runs needs a PostgreSQL xid.

- **Asking for one is refused**: `GetCurrentTransactionId()`,
  `GetTopTransactionId()` and their FullTransactionId pair
  raise "digres assigns no transaction ids"; the `IfAny`
  forms return the invalid xid. Only the heap and what writes
  through it ask, and none of it runs on a table in sm.
- **Gone with assignment**: `AssignTransactionId()`, the
  transaction state's xid and its array of subcommitted child
  xids, `RecordTransactionCommit()` and
  `RecordTransactionAbort()`, and what only they called:
  `GetNewTransactionId()`, `XactLockTableInsert()` and
  `XactLockTableDelete()`, `RegisterPredicateLockingXid()`,
  `XidCacheRemoveRunningXids()`, `TransactionIdLatest()`.
  `TransactionIdIsCurrentTransactionId()` is false for every
  xid, and `xactGetCommittedChildren()` finds none.
- **Left for the heap**, whose machinery they are: the commit
  status functions of `transam.c`, multixact, snapshots and
  the proc array's xmin, and `predicate.c`.
- `age(xid)`'s reference point is the next xid, unchanging.
- Nothing in `server.sh` asks for an xid: a function call by
  name, `txid_current()` say, is not resolved yet, so the
  refusal is not tested.

## Step 46: subtrans and commit_ts are gone

Both map an xid to something: `pg_subtrans` a subtransaction's
xid to its parent's, `pg_commit_ts` a committed xid to its
commit time. Step 45 left no xid to map.

- **Gone**: `subtrans.c` and `commit_ts.c` with their headers;
  the settings `subtransaction_buffers`,
  `commit_timestamp_buffers` and `track_commit_timestamp`;
  the SQL functions `pg_xact_commit_timestamp`,
  `pg_xact_commit_timestamp_origin` and
  `pg_last_committed_xact`; the LWLock `CommitTs`, the
  tranches `CommitTsBuffer`, `CommitTsSLRU`, `SubtransBuffer`
  and `SubtransSLRU`, and their wait events; the commit
  timestamp fields of `TransamVariables`; the `pg_subtrans`
  and `pg_commit_ts` directories.
- **An xid is its own topmost**: the heap, the lock manager,
  the proc array and the snapshot code asked subtrans for a
  subtransaction's top xid, and now take the xid as it is.
  They are the heap's machinery, and go with it.

## Step 47: two-phase commit is gone

PREPARE TRANSACTION was already refused (step 12), but
`twophase.c` kept its shared state, its setting, and a slot
in every structure sized by backends: a prepared transaction
held a dummy PGPROC, its locks and its predicate locks. When
two-phase commit comes back it is to be sm's (`sm_prepare`).

- **Gone**: `twophase.c` and its header; the setting
  `max_prepared_transactions`; the SQL function
  `pg_prepared_xact` and the view `pg_prepared_xacts`; the
  LWLock `TwoPhaseState` and its wait event; the abort hook in
  `xact.c`; the 2PC record types of the lock manager and of
  `predicate.c`, already unused.
- **Nothing is sized for prepared transactions**: the lock
  table, the proc array, the predicate lock tables, multixact's
  member slots and the PGPROC array count backends alone.
- **A virtual transaction lock waits for no prepared
  transaction**: `XactLockForVirtualXact()` has none to find.
- **COMMIT PREPARED and ROLLBACK PREPARED** fail as for a gid
  that does not exist, which every gid is. `server.sh` checks
  it.

## Step 48: object locks are sm's, or not needed

PLAN.md §7: sm's lock manager is to be the only one. Step 41
moved a table's locks; this moves the rest that are not the
heap's.

- **Object locks are no-ops** (`lmgr.c`):
  `LockDatabaseObject()`, `LockSharedObject()`, their
  conditional, unlock and session forms. A built-in is compiled
  in and never changes; every other object, a namespace, a
  table, the database a session opens, is sm's, which locks it
  itself. A lock request still takes the invalidation messages
  waiting.
- **Left in PostgreSQL's lock manager**: the heap's and the
  indexes' own locks (tuple, page, relation extension, waits
  for a transaction, speculative insertion), which go with the
  heap; advisory locks, which need sm to lock for a session;
  and `LOCK TABLE`, next.
- No test sees the difference: no two sessions conflict on an
  object's lock that sm does not also take.

## Step 49: LOCK TABLE is sm's

- **`LockTableCommand()` locks a table in sm through
  `smstore_lock_table()`**: sm's table lock, held until the
  transaction ends, in the mode closest to PostgreSQL's. sm has
  five modes to PostgreSQL's eight; each maps to the weakest
  that conflicts with at least what PostgreSQL's does:
  ACCESS SHARE and ROW SHARE to IS, ROW EXCLUSIVE to IX, SHARE
  to S, SHARE UPDATE EXCLUSIVE and SHARE ROW EXCLUSIVE to SIX,
  EXCLUSIVE and ACCESS EXCLUSIVE to X. SHARE UPDATE EXCLUSIVE
  and EXCLUSIVE so conflict with more than PostgreSQL's do:
  with ROW EXCLUSIVE, and with ACCESS SHARE.
- **NOWAIT is refused**: sm is to take a wait policy as a flag
  on the calls that can wait (`../sm/sm1/SM1_DESIGN.md` §5.1),
  and has none yet.
- **smstore keeps a table's handle by lock mode**: a handle
  serves a later use its lock covers, and a stronger use opens
  it again, so an insert after LOCK TABLE ... SHARE takes IX
  beside S.
- **`get_rel_name()`, `get_rel_relkind()` and the other
  `get_rel_*` helpers read the relation's descriptor**
  (`RelationGetClassForm()`), as `has_subclass()` does since
  step 38: a user's relation has no pg_class row.
- `server.sh` holds a SHARE lock and an ACCESS EXCLUSIVE one in
  one session, and confirms another reads under the first, and
  times out writing under it and reading under the second.

## Step 50: CREATE INDEX and DROP INDEX are sm's

The first step to the heap's removal (PLAN.md §7): before
nbtree goes, an index is to be sm's. Here its DDL; the planner
and the executor next.

- **`ProcessUtilitySlow()` creates an index on a table in sm
  with `smstore_create_index()`**, after parse analysis: btree,
  on columns, ASC or DESC with either null order, UNIQUE or
  not, with INCLUDE columns. The rest is refused: another
  access method, expressions, a predicate, collations and
  operator classes, options, tablespaces, NULLS NOT DISTINCT,
  DEFERRABLE. On a table made in the same transaction sm fills
  it at once; on any other, in the background once the
  transaction commits (`sm_cat_index_wait` waits for it).
- **DROP INDEX is `smstore_drop_indexes()`**, which looks the
  name up in sm, on the search path, and drops it from its
  table. DROP INDEX CONCURRENTLY is refused.
- **An index's default name is PostgreSQL's**:
  `ChooseIndexName()` and `ChooseIndexColumnNames()` are
  exported, and `ChooseRelationName()` asks sm whether a name
  is taken, not pg_class; a constraint's name is an index's.
- **A duplicate key is SQLSTATE 23505**, and a NULL in a NOT
  NULL column 23502, from any call into sm.
- `RangeVarCallbackOwnsRelation()` reads the relation's
  descriptor.
- `server.sh` creates plain and unique indexes, checks a
  default name, a duplicate refused, and DROP INDEX.

## Step 51: an index scan reads a table in sm through its index

The planner and the executor now use an index in sm, the
second step before nbtree goes.

- **A table in sm lists its indexes** (`rd_indexlist`), and an
  index's descriptor is built from sm3's definition
  (`RelationBuildSmIndexDesc()`): a btree whose columns are
  the table's, keyed by each type's default btree operator
  class, with DESC and NULLS FIRST in `rd_indoption`, and a
  `pg_index` form made for it. smstore remembers each index's
  table, so the descriptor can be built from the index's oid.
  It keeps nbtree's routine for the planner's flags and its
  cost estimate.
- **`pg_opclass` is compiled in**; `gen_cat.py` names the list
  of a catalog ending in s with -es (`cat_opclasses()`).
- **The planner asks sm whether an index may be scanned**
  (`smstore_index_usable()`): one built in the background may
  have become valid since its descriptor was made. Not yet: an
  index-only scan, an ordered or backward scan, a bitmap scan,
  an array of values (`a IN (...)`). A bitmap scan is to come:
  an index scan yields rownos, and a rowno is a tid. sm has no
  pages; an index is estimated as large as its table.
- **`IndexNext()` reads a table in sm through
  `smstore_index_beginscan()`**: equal keys on the index's
  leading columns are a prefix both bounds share, and the next
  column's ranges bound it, in the index's own order, so a
  DESC column's upper value is its lower bound. A key whose
  value is not of the column's type bounds nothing. Each entry's
  row is read with `sm_tbl_read()`, and every row is checked
  against every scan key, so a bound only narrows the scan.
- **EXPLAIN names operators** from the compiled-in rows
  (`generate_operator_name()`).
- `server.sh` builds an index with its table, so sm fills it at
  once, and checks a range, an equal key with a range on the
  second column, IS NULL and an empty range, and that the plan
  is an index scan.

## Step 52: make tsan instruments RocksDB and sm

`make tsan` failed now and then after step 51: a race inside
RocksDB's group commit, where two threads write at once -- sm1's
sequence lease for a CREATE TABLE and sm1's GC -- and the
group's leader copies the other's batch, handed over through
RocksDB's atomics. The sanitizer did not see them in the
system's RocksDB, which is not built for it, and no suppression
could name the GC thread's side.

- **`test/rocksdb_tsan.sh` builds RocksDB 8.9.1**, the system's
  version, from its source with `-fsanitize=thread`, as a
  static library with the system's compression libraries, under
  `build/`: cloned and built once, for `make tsan` alone.
- **`make tsan` links it, and sm instrumented too**:
  `sm/Makefile` builds its levels' libraries under another
  `BUILD` with other `OPTFLAGS` (`build/tsan/libsm.a`). Every
  library that runs threads is now instrumented: RocksDB, sm and
  sys. sq, the parser, is not.
- **`test/tsan.supp` is gone**: its two RocksDB suppressions are
  not needed with RocksDB instrumented. `server.sh` and `run.sh`
  ran clean under it twelve times running, with no suppression.
- The first `make tsan` builds RocksDB, which takes a while.

## Step 53: PRIMARY KEY and UNIQUE in CREATE TABLE

- **A column's PRIMARY KEY or UNIQUE is taken**: parse analysis
  already makes each an index statement of its own, which
  `smstore_create_index()` creates as a unique index in sm. A
  primary key's columns are NOT NULL, from the statement's NOT
  NULL constraints, as PostgreSQL's are.
- **`CommandCounterIncrement()` begins a statement in sm**: the
  next command is to see this one's changes, as PostgreSQL's
  command id says. Creating a table and then its index in one
  CREATE TABLE needs it: sm refused the index, the table's
  definition having been changed by the same statement.
- Not yet: a constraint is not recorded apart from its index,
  so DROP INDEX drops a primary key's index, which PostgreSQL
  refuses; and a unique violation does not name the constraint.
- `server.sh` checks duplicates and a NULL key refused, and a
  primary key lookup planned on its index.

## Step 54: ORDER BY reads an index in sm in order

- **An index in sm is ordered for the planner**: its opfamilies
  are its sort families, with DESC and NULLS FIRST as the
  index has them, so an ORDER BY in the index's order, a merge
  join's input, a LIMIT, can use an index scan with no sort.
- **But only forwards**: `IndexOptInfo` has `amcanbackward`,
  false for an index in sm, and `build_index_paths()` makes no
  backward path without it; an order the index does not have is
  sorted. sm's cursors run forwards, and keep no mark for a
  merge join's restore (`amcanmarkpos` is false too).
- **ORDER BY needed the type cache**: `lookup_type_cache()` reads
  `cat_type()`, and hashes a type's oid itself, not by
  pg_type's catalog cache, which no invalidation reaches. The
  hash was what broke: it opened pg_type, and an error raised
  in the abort path, which looks types up, recursed until it
  PANICked.
- **`pg_amproc` and `pg_opfamily` are compiled in**, and
  `get_opfamily_proc()`, `get_opfamily_method()`,
  `get_opfamily_name()`, the `get_opclass_*()` helpers and
  `GetDefaultOpClass()` read them.
- `server.sh` orders by an index's columns, with nulls last,
  in reverse, with nulls first, and with a LIMIT.

## Step 55: an index in sm has an access method of its own

- **`smindex_routine()`** (`access/sm/smindex.c`) is the
  `IndexAmRoutine` of an index in sm, which
  `RelationBuildSmIndexDesc()` puts in `rd_indam` in place of
  nbtree's: ordered, unique, of several columns, scanned
  forwards a row at a time, with no array keys, no mark, no
  bitmap and no index-only scan yet. Its costs
  (`btcostestimate`) and strategies are btree's. Its
  `amgettuple` raises an error: smstore.c scans an index in sm,
  but the planner plans a plain index scan only for an access
  method that has one.
- **The planner reads it as any other**: `get_relation_info()`
  loses its special cases for sm, keeping only the size, as an
  index in sm has no pages.
- **`IndexSupportsBackwardScan()` asks the descriptor**, not
  pg_class and pg_am, which have no index in sm: a scroll
  cursor over one failed opening pg_class. Now the planner puts
  a Materialize over the forward-only scan.
- `server.sh` fetches back through a scroll cursor over an
  index in sm.
- Not yet: `int` as a column's type is not resolved in CREATE
  TABLE (it opens pg_type); `int4` is.

## Step 56: bitmap scans of tables in sm, over rownos

- **A `RowBitmap`** (`nodes/rowbitmap.c`) is a set of rownos:
  bit r for row r in a 64-bit space, with no page or block. Its
  words are kept sparsely, in a hash table, and it is exact; it
  grows as it must, with no lossy form. It is a node, so
  BitmapAnd and BitmapOr take it as they take a `TIDBitmap`:
  `rbm_intersect()`, `rbm_union()`.
- **A bitmap index scan of an index in sm** has smstore.c walk
  the index between the bounds its keys make, adding each
  entry's rowno (`smstore_index_getrowno()`) without reading the
  row. It opens no `IndexScanDesc`. An IN list is the executor's
  array keys, one walk per value.
- **A bitmap heap scan of a table in sm** reads each rowno in
  order (`smstore_fetch()`), passes a row the transaction does
  not see, and rechecks every row against the original quals:
  an entry is only within the bounds, and a key that bounds
  nothing is checked there.
- `smindex_routine()` has an `amgetbitmap`, which raises an
  error, so the planner plans bitmap scans.
- The planner now prefers a bitmap scan for a small range;
  `server.sh` turns them off for its index scan plan check.
- `server.sh` checks a BitmapOr, a BitmapAnd, an OR of ANDs, an
  IN list, a recheck, and an uncommitted row passed over.
- Not yet: `count(*)` and `generate_series()`, which look a
  function up by name in pg_proc.

## Step 57: index-only scans of tables in sm

- **sm gives an entry's key columns back**:
  `sm_idx_scan_key()` decodes the key of the entry the cursor
  last returned, and returns 1 where the key keeps no datum: a
  key cut to fit, or text under a non-deterministic collation.
- **`smstore_index_getentry()`** fills a slot of the index's
  columns: the key's from the key, the INCLUDE ones from the
  entry's value, and no row read. An entry sm returns is one
  the transaction sees, as entries are versioned with rows.
  Where the key keeps no datum, the row is read. The scan keys
  are checked on the entry's columns.
- **The planner**: `smindex_routine()` can return every column,
  and sm's size estimate says every row is visible
  (`allvisfrac` 1), so an index-only scan costs no table fetch.
- `nodeIndexonlyscan.c` reads an index in sm through smstore,
  forwards, as `nodeIndexscan.c` does.
- `server.sh` checks an index-only scan with INCLUDE columns, a
  negative int2 and nulls, and one on a unique index with a
  null key. A key that keeps no datum needs a text key, which
  a table in sm does not have yet: that path, reading the row,
  is not reached by a test.

## Step 58: nbtree is gone

- **The btree access method is deleted**: nbtree.c and its
  insert, page, search, sort, split, dedup, key preprocessing,
  utility and validation files, about 18,000 lines. An index is
  sm's, and nothing called them.
- **pg_am's btree is sm's**: `bthandler` is in `smindex.c` and
  returns `smindex_routine()`, which translates strategies
  itself. `CREATE INDEX ... USING btree` still names it.
- **What stays**: `nbtcompare.c`, the operator classes'
  comparison functions; and `nbtree.h`, cut to the strategy and
  support function numbers and the scan key flags for DESC and
  NULLS FIRST.
- **Gone with it**: the btree index build and CLUSTER sorts in
  `tuplesortvariants.c`, btree's shared memory, and its
  reloptions (`fillfactor`, `deduplicate_items`,
  `vacuum_cleanup_index_scale_factor`). The heap's CLUSTER
  rewrite refuses a sort until the heap goes too.

## Step 59: the heap is gone

- **`access/heap` is deleted**: heapam, its table access
  method, visibility rules, pruning, rewrite, hio, TOAST
  storage and the visibility map, about 15,000 lines, with
  `toast_helper.c` and the headers `heapam.h`, `hio.h`,
  `rewriteheap.h` and `visibilitymap*.h`. No relation can be a
  heap: a table is sm's, and a catalog table cannot be opened.
- **pg_am's heap is sm's**: `heap_tableam_handler` is in
  `access/sm/smtable.c` and returns a routine whose slot is
  virtual; every other callback raises an error, all filled by
  one C++ template that takes each callback's type. The
  relcache gives it to every relation.
- **What stays of TOAST**: `heaptoast.c`, moved to
  `access/common`, flattens a tuple built in memory, a
  composite's, as before; `toast_internals.c` keeps
  compression. Writing to a TOAST table went with it.
- **Callers kept, refusing**: writing a catalog table
  (`CatalogTuple*()`, `indexing.c`), scanning one
  (`catalog_scan_next()`, in place of `heap_getnext()`), and
  overwriting a row in place (`systable_inplace_update_*()`)
  raise the error opening a catalog table would. They go with
  their callers: VACUUM, CLUSTER, tablespaces, databases,
  publications and the rest.
- **No visibility map**: an index-only scan of what would be a
  heap, and the planner's endpoint probe, visit the table for
  every entry; ANALYZE and index builds record no all-visible
  pages. COPY, CREATE TABLE AS, REFRESH and ALTER TABLE's
  rewrite pass no bulk insert state.

## Step 60: large objects are gone

PLAN.md §7: storage and replication features are deleted, not
ported. Large objects are the first.

- **Gone**: `inv_api.c`, `be-fsstubs.c` and
  `pg_largeobject.c`, with their headers; the SQL functions
  `lo_*`, `loread`, `lowrite` and `has_largeobject_privilege`;
  the setting `lo_compat_privileges`; the end-of-transaction
  hooks in `xact.c`; the ACL checks and GRANT of a large object
  in `aclchk.c` and `acl.c`; the large object rows of the
  `pg_seclabels` view and the REVOKEs of `lo_import` and
  `lo_export`.
- **Refused**: naming a large object, as GRANT, ALTER DEFAULT
  PRIVILEGES, COMMENT or ALTER LARGE OBJECT do, fails with
  "large objects are not supported".
- The grammar, nodes and catalog headers stay, as for LISTEN:
  sq parses the statements, and the catalog's ids are still
  named in tables of object classes.

## Step 61: publications and subscriptions are gone

PLAN.md §7: storage and replication features are deleted.
Logical replication's workers went with the WAL; its catalogs'
code goes now.

- **Gone**: `pg_publication.c`, `publicationcmds.c` and
  `pg_subscription.c` with their command headers; the SQL
  functions `pg_get_publication_tables` and
  `pg_relation_is_publishable`; the `pg_publication_tables`
  view, the publication and subscription rows of
  `pg_seclabels`, and the column grants on `pg_subscription`.
- **A relation publishes nothing**:
  `RelationBuildPublicationDesc()` says so without looking,
  and the relcache keeps no publication descriptor. ALTER
  TABLE no longer asks whether a table is published, nor DROP
  DATABASE whether it has subscriptions, nor dropping a table
  for its subscription states.
- **Refused**: CREATE, ALTER and DROP PUBLICATION, and naming a
  publication or subscription anywhere (COMMENT, owners,
  dependencies), fail with "publications are not supported" or
  "subscriptions are not supported", as CREATE SUBSCRIPTION
  already did.
- The grammar, nodes and catalog headers stay; the headers lose
  their functions' prototypes.

## Step 62: tablespaces are gone

PLAN.md §7: storage and replication features are deleted.
sm keeps its own files; a tablespace has nothing to place.

- **Gone**: the commands of `tablespace.c`; the settings
  `default_tablespace`, `temp_tablespaces` and
  `allow_in_place_tablespaces`; the SQL functions
  `pg_tablespace_location`, `pg_tablespace_size` and
  `pg_tablespace_databases`; `fd.c`'s list of temporary
  tablespaces, and preparing it before a sort, a hash join or
  a tuplestore spills.
- **What is left**: `tablespace.c` names the two built in,
  `pg_default` and `pg_global`, by name and oid, which no
  longer opens `pg_tablespace`. A relation's default
  tablespace, and a temporary file's, is the database's.
- **Refused**: CREATE, ALTER, rename and DROP TABLESPACE fail
  with "tablespaces are not supported".
- `server.sh` checks the refusals, the settings gone, and a
  sort that spills to temporary files.

## Step 63: CREATE and DROP DATABASE are sm's

PLAN.md §7. Connecting already opened a database in sm; now
making and dropping one does too.

- **CREATE DATABASE name** makes it in sm (`sm_mkdb`), at
  once, outside a transaction block as before. Its options are
  refused: sm has no template, owner, encoding or locale to
  keep. Its oid is the id sm gives it.
- **DROP DATABASE [IF EXISTS] name** drops it from sm
  (`sm_rmdb`): not the session's own; and not one another
  session has open, which sm refuses. A session leaves sm a
  moment after it ends, so the drop is tried again for 5
  seconds, as PostgreSQL waits. `WITH (FORCE)` first tells the
  other sessions to end.
- **ALTER DATABASE is refused**, every form: sm keeps no name
  change, owner, settings or other attribute of a database.
- **`get_database_oid()` and `get_database_name()` ask sm**
  (`datadir.c`), where they read pg_database, which digres
  does not have.
- **Gone with the old `dbcommands.c`**: copying a template
  (`copydir.c` and `file_copy_method`), the SQL function
  `pg_database_collation_actual_version`, and the buffers and
  dependencies a dropped database left.
- `server.sh` creates, uses, drops, force-drops and recreates a
  database.

## Step 64: CLUSTER and REINDEX do nothing, and say so

PLAN.md §7. VACUUM already did nothing; CLUSTER and REINDEX
join it, each with a warning, so a tool that runs them goes on.

- **CLUSTER** warns that a table in sm is kept by rowno, not in
  an index's order, and succeeds; so do ALTER TABLE ...
  CLUSTER ON and SET WITHOUT CLUSTER. `cluster.c` is gone: the
  heap's rewrite in an index's order, and its swap of a new
  heap's files for the old (`make_new_heap()`,
  `finish_heap_swap()`).
- **Without that swap**, an ALTER TABLE that rewrites its table
  and REFRESH MATERIALIZED VIEW are refused, "not supported
  yet"; the refresh's own code goes (`matview.c` keeps its
  receiver and the check that DML may not touch a view).
- **REINDEX**, of an index, table, schema or database, warns
  that sm keeps an index's entries with its table's rows, and
  succeeds. Its code in `indexcmds.c` goes, with
  `index_concurrently_create_copy()` and
  `index_concurrently_swap()`. `reindex_relation()` stays for
  the heap's TRUNCATE, until TRUNCATE is sm's.
- `server.sh` checks both warnings.

## Step 65: the buffer manager, smgr and free-space map are gone

PLAN.md §7. With the heap and nbtree gone, nothing kept pages
in shared buffers or relation files on disk: every relation
with storage is sm's.

- **Gone**: `storage/buffer` (the buffer manager, local
  buffers, read streams), `storage/smgr` (smgr, md, bulk
  writes), `storage/freespace`, and `catalog/storage.c`, the
  creating, truncating and deleting of relation files at
  commit; their headers; the buffer-backed tuple slot
  (`TTSOpsBufferHeapTuple`); the hooks in `xact.c`,
  `postinit.c`, `ipci.c`, `inval.c` and `postgres.c`; and the
  settings `temp_buffers`, `zero_damaged_pages`,
  `vacuum_buffer_usage_limit`, `effective_io_concurrency`,
  `maintenance_io_concurrency`, `io_combine_limit`,
  `io_max_combine_limit` and `backend_flush_after`.
  `track_io_timing` stays, for temporary files, in
  `buffile.c`; `shared_buffers` stays while SLRU sizes itself
  by it.
- **Sequences are refused**, "not supported yet":
  `sequence.c`, a one-row heap read through the buffer
  manager, is now its entry points, each refusing, until sm's
  sequences replace it. DISCARD SEQUENCES runs: there is no
  cache.
- **Refused where a relation's file was needed**: making a
  relation with storage outside sm, TRUNCATE's heap paths (and
  with them `reindex_relation()` and the reindex state),
  ALTER TABLE SET TABLESPACE that would move data, and
  ANALYZE's block sampler, which the table access method no
  longer offers.
- **The planner** estimates an index as large as its table,
  and an ANALYZE or index build records no pages.
- The proc signal barrier has no type left; its machinery is
  next to go.
- `server.sh` checks sequences refused and the settings gone.

## Step 66: proc signal barriers are gone

Nothing emits a barrier any more: DROP DATABASE and the
tablespace commands were the last, and smgr's release was the
last kind of barrier.

- **Gone**: `EmitProcSignalBarrier()`,
  `WaitForProcSignalBarrier()` and
  `ProcessProcSignalBarrier()`, the barrier generation and
  check mask in each slot and the header, `PROCSIG_BARRIER`,
  `ProcSignalBarrierPending` and the `ProcSignalBarrier` wait
  event.
- **The ProcSignal array** is the slots alone, with no header.

## Step 67: multixacts are gone

PLAN.md §7: a row in sm names no lockers, so no set of them.

- **Gone**: `multixact.c` and its header, its SLRUs and their
  settings (`multixact_offset_buffers`,
  `multixact_member_buffers`), the SQL functions `mxid_age`
  and `pg_get_multixact_members`, the end-of-transaction hook,
  the LWLocks `MultiXactGen` and `MultiXactTruncation`, the
  multixact tranches and wait events, and the `pg_multixact`
  directories.
- **`vac_update_relstats()`** no longer takes a minimum
  multixact; every caller passed none. `InvalidMultiXactId`
  stays, in `access/transam.h`, for pg_class's `relminmxid`.
- **Leftovers of steps 62 and 65 go too**: the
  `BufferContent` and `BufferMapping` tranches, the buffer
  mapping partitions of the main LWLock array, and the
  `pg_tblspc` directory.
- `shared_buffers` stays while `predicate.c`'s SLRU sizes
  itself by it.
- `server.sh` checks the setting and the directory gone.

## Step 68: predicate locks and SLRUs are gone

PLAN.md §7: SERIALIZABLE is sm's, which tracks the reads and
writes of a serializable transaction itself.

- **Gone**: `predicate.c` and its headers, with every hook into
  them: index scans and inserts, DROP TABLE, commit, the
  index-only scan, resource release, and the serializable
  snapshot; the settings `max_pred_locks_per_transaction`,
  `max_pred_locks_per_relation`, `max_pred_locks_per_page` and
  `serializable_buffers`; the SIReadLock rows of `pg_locks`, the
  SQL function `pg_safe_snapshot_blocking_pids`, and the safe
  snapshot wait in `pg_isolation_test_session_is_blocked`;
  their LWLocks, tranches, lock partitions and wait events; the
  `pg_serial` directory.
- **`slru.c` is gone**, its last user with it, and so is
  `shared_buffers`, by which SLRUs sized themselves.
- `server.sh` stages a write skew between two serializable
  transactions: the second commit fails with 40001. It checks
  the settings and `pg_serial` gone.

## Step 69: TRUNCATE is sm's

- **TRUNCATE of tables in sm** goes to `smstore_truncate()`,
  each named table in turn: `sm_tbl_truncate()` empties it and
  its indexes in the transaction, with the table opened
  exclusively, so a rollback, or a rollback to a savepoint
  taken before it, brings the rows back. The names are looked
  up without TRUNCATE's pg_class check, which failed on every
  table. ONLY and CASCADE change nothing: a table in sm has no
  children or foreign keys. RESTART IDENTITY is refused until
  sequences are sm's. TRUNCATE under this session's own open
  cursor of the table is refused, as in PostgreSQL.
- **A truncate moves the table's rows to a new store**, which
  only the handle that truncated knows. The transaction's other
  handles of the table, such as the one its descriptor opened,
  are closed, so the next use opens one afresh.
- **A subtransaction's handles close when it aborts**: sm's
  rollback undoes what they did, a truncate's new store among
  it, and they would point at a store gone. Each handle notes
  the nesting level that opened it, and passes to the parent
  when the subtransaction commits.
- `server.sh` truncates in a transaction, in a savepoint, and
  committed, and checks the refusals.

## Step 70: functions are called by name

The lookups a function call makes are rewritten as reached
(step 6): every function is built in, so each reads the rows
compiled in, not pg_proc.

- **`FuncnameGetCandidates()`** searches `cat_procs_named()`.
  The rows are in oid order, so a candidate is compared with
  every earlier one, not just the last. `MatchNamedCall()`
  matches argument names from the row, through
  `cat_proc_arg_info()` (`funcapi.c`), which reads its
  argument types, names and modes as `get_func_arg_info()`
  reads a tuple's.
- **`func_get_detail()`**, **`internal_get_result_type()`**,
  **`get_func_result_name()`** and **`get_function_rows()`**
  read the row by oid; a function's OUT parameters make its
  result's tuple descriptor from the row.
- **pg_aggregate is compiled in.** `gen_cat.py` takes a key
  column other than the oid, and keys pg_aggregate by
  `aggfnoid`. The parser, the planner (`prepagg.c`,
  `planagg.c`), and `nodeAgg.c` and `nodeWindowAgg.c` read it;
  an initial value is the row's string.
- **No built-in function has argument defaults** in its row:
  PostgreSQL sets them in `system_functions.sql`, which digres
  does not run.
- `server.sh` calls a function plainly, by argument names, and
  as a set; and aggregates and a window function over a table.
- Not yet: GROUP BY, joins and EXPLAIN VERBOSE, which read
  pg_constraint; `pg_typeof()`, whose output reads pg_type; and
  functions that need the default locale.

## Step 71: a database is its directory

`CODE.md`, the overriding objective, decided by the owner:
`base/X` can be copied to another machine and imported into a
new instance. An audit of `global/` and `base/` came first.

- **What already held**: `global/root` has the names, the
  `dbid`s, the next `dbid` and a format version, and no more.
  Each database has its own timestamps, object ids, sequence
  leases, catalog, locks and prepared transactions; a table in
  each of two databases gets the same oid. Nothing in a
  database's directory records its name, its `dbid` or a path.
- **Temporary files stay shared**, in `base/pgsql_tmp`, by the
  owner's decision: they are the one thing under `base` that
  is no database, and no part of the transactionally
  consistent package, which is a database's data and WAL in
  RocksDB. Putting
  them in `base/{dbid}/pgsql_tmp` was tried and taken back.
- **Left, and to hold to the objective when it is reached**:
  `GetNewObjectId()` is one counter for the process, in memory
  alone. Its callers all open a catalog table first and fail
  there; each must take the database's own id from sm as it is
  moved. Sequences are refused still; sm's are per database.
- **Not built**: the import. sm has no call that takes a
  directory in under a name and gives it a `dbid`, and a
  database does not record which built-in catalog its rows
  refer to.
- **Left broken, as found**: `pg_ls_tmpdir()` opens
  `pg_tablespace` and fails.

## Step 72: CREATE SCHEMA and DROP SCHEMA are sm's

- **CREATE SCHEMA makes the namespace in sm**
  (`smstore_create_namespace()`, `sm_cat_create_namespace`), in
  the transaction, so a rollback takes it back. What
  `CreateSchemaCommand()` did around `NamespaceCreate()` stays:
  IF NOT EXISTS, the reserved `pg_` prefix, and the statements
  inside it run with the new schema first in the search path.
  sm records no owner: AUTHORIZATION is checked as before and
  then not kept.
- **Every session's search path is recomputed** once a schema
  is made or dropped: the change registers an invalidation of
  `pg_namespace`, which runs namespace.c's syscache callback in
  each session at commit, and in this one at its next command.
  A session whose path named a schema before it existed finds
  it after.
- **DROP SCHEMA is `smstore_drop_namespaces()`**, not
  PostgreSQL's dependency walk, which read `pg_depend`. RESTRICT
  fails on a schema that still holds a table (sm's
  `SM_ERR_INUSE`); CASCADE drops its tables first, each with its
  indexes, telling of them as PostgreSQL does. `pg_catalog` and
  `public` cannot be dropped: sm builds them in.
- **Not done**: ALTER SCHEMA (rename, owner), and a schema's
  collations under CASCADE, which sm still refuses as in use.
- `server.sh` makes, uses, rolls back and drops schemas, and
  checks a held session finds a schema made after its search
  path was computed.

## Step 73: sequences are sm's

- **A sequence is an object in sm's catalog**, in a schema, with
  an sm counter behind it (`../sm/sm3/CATALOG_ONDISK.md` kind 11;
  `REVIEW.md` §5 has why). CREATE SEQUENCE is
  `smstore_create_sequence()`, in the transaction: START and
  INCREMENT, each at least 1, and AS bigint. Every other option,
  TEMPORARY, an owner and an identity column are refused, as are
  ALTER SEQUENCE and `setval()`: sm's counters only go forward.
- **Its name is a relation's.** `smstore_relation_oid()` finds
  a sequence after a table and an index, reading sm's catalog in
  the transaction, which it begins; a table and a sequence
  cannot share a name in one schema.
- **`nextval()` is sm's**, by `regclass` and by name, and like
  PostgreSQL's is not transactional. `currval()` and
  `lastval()` read this session's draws, kept in the thread's
  control block (`tcb->sequence`); DISCARD SEQUENCES forgets
  them. A value past `bigint`'s range is an error.
- **DROP SEQUENCE is `smstore_drop_sequences()`**, and DROP
  SCHEMA ... CASCADE drops a schema's sequences after its
  tables.
- **Not done**: reading a sequence as a relation (`SELECT *
  FROM q`), SERIAL and identity columns, and a column's
  DEFAULT, which CREATE TABLE still refuses.
- `server.sh` makes, draws from, drops and refuses sequences,
  four sessions drawing at once.

## Step 74: ANALYZE is sm's, and a table is sized by it

`STATISTICS.md` is the design; this is its first two steps.

- **ANALYZE goes to `smstore_analyze()`** from `ExecVacuum()`,
  not `vacuum()`: each table named, or with none every table in
  every schema, opened as a reader and analyzed by
  `sm_tbl_analyze()` at `default_statistics_target`, in the
  statement's transaction. A sequence is passed over with a
  warning, a column list refused. VERBOSE reports each table's
  live rows, the rows sampled, its row numbers and run length.
  The table's descriptor is invalidated, so its plans are made
  again.
- **A table is sized by its statistics**
  (`smstore_estimate_size()`): its bytes now in pages, and the
  rows the last analyze counted, grown with its bytes since at
  the compression recorded. One never analyzed, or empty, is
  still planned as ten pages. An index is sized by its own
  bytes and entries, a partial one by its share of the rows.
- **The statistics are sm's, shared**: `sm_tbl_stats()` hands
  every session one read-only copy a process; digres keeps none.
- **Left unused, for a step of its own**: `vacuum()` and the
  analyze path of `analyze.c` it reached.
- `server.sh` checks the estimate before and after ANALYZE, a
  rolled-back one, growth without another, and the refusals.

## Step 75: PostgreSQL's ANALYZE is gone

Step 74 left ANALYZE's own path with no caller; this removes it,
and what only it reached.

- **`vacuum.c`** is `ExecVacuum()`, which hands ANALYZE to sm,
  and `vacuum_delay_point()`, which the type-specific analyze
  routines still call. `vacuum()`, its relation lists,
  `vac_update_relstats()`, `vac_open_indexes()` and the rest
  are gone.
- **`analyze.c`** keeps `std_typanalyze()` and the estimators it
  sets, `compute_scalar_stats()` among them, which the compiled-in
  typanalyze functions (arrays, ranges) and extended statistics
  name. `analyze_rel()`, the block sampler, the index statistics
  and `update_attstats()` are gone: sm samples and estimates.
- **Extended statistics are read, not built.** What built them
  at ANALYZE is gone -- `BuildRelationExtStatistics()`,
  `ComputeExtStatisticsRows()`, `statext_ndistinct_build()`,
  `statext_dependencies_build()`, `statext_mcv_build()`, their
  serializers, the multi-sort helpers and the types they used.
  The planner's side, the deserializers and
  `statext_clauselist_selectivity()`, stays; with no
  `pg_statistic_ext_data` it finds nothing.
- Removed until nothing was left unused, by the compiler's
  unused-function warnings and by finding no caller of an
  extern: about 4,800 lines.

## Step 76: the planner reads sm's column statistics

`STATISTICS.md` §2.3 and §2.4; its third step.

- **`get_statistic_tuple()` makes a `pg_statistic` row** from
  `sm_tbl_stats()` (`smstore_statistic_tuple()`): the null
  fraction, width and distinct count, and the slots
  PostgreSQL's ANALYZE fills -- the common values with the
  type's `=`, the histogram and the correlation with its `<`,
  operators from the type cache, the column's collation. The
  row is the caller's, freed with `heap_freetuple()`; every
  `freefunc` and release of one is changed to match.
- **`pg_statistic` is a descriptor, not a table.**
  `statistic_tupdesc()` builds it from `Schema_pg_statistic`
  (`BKI_SCHEMA_MACRO` on its header), and `get_attstatsslot()`
  reads with it rather than through the `STATRELATTINH` cache,
  and takes the element type from the compiled-in catalog
  rather than `TYPEOID`. Nailing `pg_statistic` in the relcache
  was tried: its entry is rebuilt from `pg_class` once the
  relcache is built, which fails.
- **`get_actual_variable_range()` passes over an index in sm**:
  it has no index AM scan; the histogram's ends stand in.
- **Found, not fixed here**: a `GROUP BY` or a join on a table in
  sm fails planning, before and after this step:
  `get_relation_foreign_keys()` reaches `RelationGetFKeyList()`,
  which opens `pg_constraint`.
- `server.sh` checks `=`, `<>`, `IS NULL`, `<` and `DISTINCT`
  estimates before and after ANALYZE.

## Step 77: GROUP BY and joins of tables in sm plan

Found in step 76: a `GROUP BY` or a join on a table in sm failed
planning, as `get_relation_foreign_keys()` reached
`RelationGetFKeyList()`, which opens `pg_constraint`.

- **A table in sm has no foreign keys**: its relcache entry is
  built with the list valid and empty, as its extended
  statistics are, so `pg_constraint` is never scanned. digres
  refuses a foreign key at CREATE TABLE.
- `server.sh` runs a `GROUP BY`, and checks a join's estimate
  by the distinct values on each side.

## Step 78: pg_stats is a function

`STATISTICS.md` §2.5; its fourth step.

- **`pg_stats()` and `pg_stats(regclass)`** are built in
  (`pg_proc.dat`, oids 8844 and 8845): the rows of PostgreSQL's
  `pg_stats` view, by its columns, for every table in sm or for
  one, from `sm_tbl_stats()`. Common values and bounds are text
  arrays, each value through its type's output function; the
  element and range columns are null. A table never analyzed,
  an index, a sequence or an oid not in sm gives no rows.
- **A schema's name is sm's**: `get_namespace_name()` reads
  only the compiled-in schemas, so a schema made by CREATE
  SCHEMA is found in `sm_cat_list_namespaces()` by its oid.
- **Found, not fixed here**: `unnest()` of an array opens
  `pg_range`; a cast to `numeric`, `pg_type`; a comparison of
  `float4` with a `numeric` literal, `pg_operator`.
- `server.sh` checks no rows before ANALYZE, a row a column
  after, every table's, and the common values, null fraction
  and bounds of a known distribution.
- **`make tsan` is clean** (`STATISTICS.md` §3 step 5), and so
  is a run of eight sessions on `digres-tsan`, each fifteen
  times analyzing two shared tables, inserting, planning a
  filter and a join on them and reading `pg_stats`: no report.

## Step 79: a schema made by CREATE SCHEMA has a name

`get_namespace_name()` read only the compiled-in schemas, and
gave NULL for one made by CREATE SCHEMA; `pg_stats` found the
name by listing sm's schemas (step 78).

- **sm names a schema by its oid**: `sm_cat_namespace_name()`,
  over sm3's `cat_get_namespace_name()`, which was the internal
  `session_nspace()`.
- **`get_namespace_name()` asks sm** for an oid past the
  built-in ones (`smstore_namespace_name()`), and `pg_stats`
  uses it again.
- **Found, not fixed here**: a `regclass`'s output opens
  `pg_class`.
- `server.sh` shows a table in a schema made by CREATE SCHEMA
  in `pg_stats` by its schema's name.

## Step 80: temporary tables

`../sm/TEMPTABLE.md`; its sixth step. `CREATE TEMPORARY TABLE`
was refused.

- **A temporary table is sm's** (`sm_cat_create_temp_table`):
  the session's alone, its rows unlogged and unlocked, in the
  session's transactions like any table's. sm drops it when the
  session closes its database.
- **`pg_temp` is compiled in**, at oid 8846
  (`PG_TEMP_NAMESPACE`, `pg_namespace.dat`), the same in every
  session. PostgreSQL made a `pg_temp_N` and a `pg_toast_temp_N`
  row per backend: rows in the database named after a slot of
  this instance, which a copied `base/X` must not carry, and
  which a read-only transaction could not write. Nothing is
  written for the namespace now, in any database.
- **sm has no temporary namespace.** `smstore.c` maps
  `PG_TEMP_NAMESPACE` to sm's `SM_NSP_TEMP`, which is 0, on the
  way in, and a definition with no namespace back to it. The
  oid never reaches sm or the disk.
- **The namespace is still set up on first use**
  (`InitTempTableNamespace()`), so a session with no temporary
  table does not look for one on every name. An abort of the
  transaction that set it up forgets it.
- **Went with it**: `RemoveTempRelations()` and its exit
  callback, since sm drops the tables; the cleaning out of a
  crashed backend's namespace, since a crash leaves none; the
  TOAST namespace's creation; `MyProc->tempNamespaceId` being
  set. `DISCARD TEMP` drops the session's temporary tables
  through sm (`smstore_drop_temp_tables()`).
- **A relation descriptor of one says so**: `relpersistence` is
  `RELPERSISTENCE_TEMP` and `rd_islocaltemp` is set, so
  PostgreSQL's own checks hold: a read-only transaction writes
  to it, and the transaction is marked as having used the
  temporary namespace.
- **`ON COMMIT DROP` and `DELETE ROWS` are done in sm**, by
  `PreCommit_on_commit_actions()`, which now runs before sm's
  transaction commits rather than after: a drop or a truncate
  there is part of it. `DELETE ROWS` is a TRUNCATE, a new store
  in sm at each commit that used a temporary table.
- **Refused**: `UNLOGGED`; `ON COMMIT` on a table that is not
  temporary; a temporary table in a schema that is not; a
  temporary sequence, as before.
- **ANALYZE skips a temporary table with a warning**: sm keeps
  no statistics for one yet.
- **Left on purpose**: `myTempToastNamespace` and the functions
  that read it, never set now; the `CREATE TEMP` privilege
  check, which the tests run only as the superuser.
- **Found, not fixed here**: `int` and `integer` do not resolve
  as type names (`pg_type` is opened); `int4` does.
- `server.sh` checks a temporary table read by name, through
  `pg_temp` and in a join; one hiding a table; rollback,
  savepoints and a read-only transaction; both `ON COMMIT`
  actions; two sessions each with one of the same name;
  `DISCARD TEMP`; and the refusals. Breaking the `ON COMMIT`
  registration, and creating the table in `public` instead,
  each fail it.

## Step 81: a regclass prints its relation's name

Found in step 79: `regclassout()` looked the oid up in the
`RELOID` cache, which opens `pg_class`.

- **`regclassout()` and `RelationIsVisibleExt()`** read the
  relation's `pg_class` form from its descriptor
  (`RelationGetClassForm()`), as `get_rel_name()` does. A name
  is qualified when the search path would not find it, and an
  oid that is no relation prints as its number.
- `server.sh` prints regclasses on the default path and on one
  where a schema hides `public`'s table.

## Step 82: a type named by its schema is found

Every type name of the SQL standard -- `integer`, `real`,
`varchar`, `numeric` -- is `pg_catalog.`-qualified by the
grammar, and `LookupTypeNameExtended()` looked a qualified name
up in the `TYPENAMENSP` cache, which opens `pg_type`: a cast to
any of them failed.

- **A qualified name is found among the compiled-in types**,
  by `builtin_type_oid()` (`namespace.c`), as an unqualified
  one already was, now extern.
- `server.sh` casts to the standard's names, and looks a type
  up in `pg_catalog`, in a schema without it, and in none.

## Step 83: an operator is chosen among the compiled-in ones

`OpernameGetCandidates()` listed an operator name's candidates
from the `OPERNAMENSP` cache, which opens `pg_operator`: any
operator whose argument types matched none exactly -- `float4 >
0.3`, `numeric + 1` -- failed.

- **The candidates are the compiled-in rows of the name**
  (`cat_operators_named()`), as `OpernameGetOprid()` already
  read them. They are not ordered by argument types, as the
  cache's list was, so a duplicate is looked for in the whole
  result list, PostgreSQL's path for an unordered list.
- `'a' || 1` takes the array operator, as PostgreSQL would
  without the `text || anynonarray` operators step 6 cut with
  their SQL-language functions.
- `server.sh` checks operators chosen by coercion, and one
  qualified by its schema.

## Step 84: pg_range is compiled in

`unnest()` of an array failed: choosing among its forms asks
whether the argument is a multirange (`get_multirange_range()`),
which searched the `RANGEMULTIRANGE` cache and opened
`pg_range`.

- **`gen_cat.py` writes pg_range**, keyed by `rngtypid`:
  `cat_range()` finds a range type's row, `cat_ranges()` gives
  them all.
- **The range lookups read it**: `get_range_subtype()`,
  `get_range_collation()`, `get_range_multirange()`, and
  `get_multirange_range()`, which looks at each row, as there is
  no key by multirange; the type cache's
  `load_rangetype_info()` too.
- **Found, not fixed here**: `array_agg()` opens `pg_type`, from
  the planner's `agg_args_support_sendreceive()`.
- `server.sh` runs `unnest`, with ordinality, and ranges and a
  multirange.

## Step 85: array_agg plans

Found in step 84: the planner asks whether an aggregate's
argument types have send and receive functions
(`agg_args_support_sendreceive()`, for a partial aggregate's
state), and looked each type up in the `TYPEOID` cache, which
opens `pg_type`: `array_agg()` failed.

- **The type's row is the compiled-in one** (`cat_type()`).
- `server.sh` runs `array_agg`, plain and ordered in groups.

## Step 86: UPDATE and DELETE

A table in sm took a plain INSERT and nothing else that writes.

- **UPDATE and DELETE write a table in sm**, with RETURNING, at
  every isolation level, a temporary table included. sm writes
  the table's indexes with the row, so the executor is told to
  write none (`TU_None`). A row keeps its rowno, so its tid.
- **They come through the table access method**: `smtable.c`
  now has `tuple_fetch_row_version`, `tuple_update`,
  `tuple_delete` and `tuple_lock`, each over a function of
  `smstore.c`. A scan and an INSERT still go to `smstore.c`
  from the executor; but `nodeModifyTable.c` fetches, writes and
  locks a row in some ten places, which stay PostgreSQL's this
  way.
- **sm's answers are the executor's**: `SM_ERR_UPDATED`, a
  commit since the statement's snapshot under READ COMMITTED,
  is `TM_Updated`; the executor then asks for the row's newest
  version (`sm_tbl_read_latest`), checks its WHERE again
  (EvalPlanQual) and writes again. `SM_ERR_SELFMODIFIED`, a row
  the statement already changed, is `TM_SelfModified` by this
  command, and the row is passed over. At REPEATABLE READ and
  SERIALIZABLE sm aborts the transaction instead, and the error
  carries sm's SQLSTATE, 40001.
- **A statement keeps one row** (`kept_*`, `smstore.c`): the
  newest version it read after `TM_Updated`, or the row it just
  deleted, as sm gave it. The executor fetches that row again by
  its tid -- for the columns an UPDATE does not set, and for
  RETURNING -- and the statement's own read of the rowno would
  give the version of its snapshot.
- **Still refused**: MERGE, INSERT ... ON CONFLICT, and SELECT
  ... FOR UPDATE or SHARE, which reaches `tuple_lock` for a row
  no write found changed.
- `server.sh` checks an update by key and of every row, of a
  key, to a taken key and to NULL; a delete; RETURNING;
  rollback and a savepoint; a temporary table; and, against a
  session holding the row updated, an update made to its row,
  one that keeps its column, an update and a delete whose WHERE
  no longer holds, a delete returning its row, and the failure
  at REPEATABLE READ. A fetch that ignores the kept row, and a
  delete that keeps none, each fail it.

## Step 87: what PostgreSQL wrote in SQL is written in C

Step 6 cut the 50 built-in functions PostgreSQL writes in SQL,
since digres has no SQL language, and with them thirteen
operators and a cast. 43 come back, as 42 C functions in
`utils/adt/sql_bodies.c` (`log` and `log10` of a numeric share
one), each doing what its SQL body did by calling the C
functions that SQL reached.

- **Their rows are PostgreSQL's**, oid, strictness and all,
  with `prolang` internal and `prosrc` the C function; the
  operators and the cast are back as they were, commutators
  too.
- **What comes back**: `||` of text and any type, both ways;
  `+` with an interval, integer or date on the left, and of
  `int8` and `inet`, `numeric` and `pg_lsn`; `path @> point`;
  the cast from `circle` to `polygon`; `OVERLAPS` with lengths;
  `lpad` and `rpad` of two arguments; `round`, `trunc`, `log`
  and `log10` of one numeric; `bit_length`; `age` of one
  timestamp; `date_part` of a date; `timestamptz(date, time)`;
  `SUBSTRING ... SIMILAR`; `quote_literal` and `quote_nullable`
  of any type; `pg_sleep_for` and `pg_sleep_until`; `xpath` and
  `xpath_exists` of two arguments.
- **A value is made text by the cast PostgreSQL would choose**
  (`find_coercion_pathway()`), as `$2::text` did: `'a' || true`
  is `atrue`, by bool's cast, not its output `t`.
- **Left out**: `obj_description`, `col_description` and
  `shobj_description`, as there is no `pg_description`;
  `ts_debug`, a query over text search's catalogs;
  `pg_relation_size(regclass)`, which reads relation files; and
  the SQL language's validator and `pg_get_function_sqlbody`.
- **Found, not fixed here**: the default collation is never set
  up (`init_database_collation()` reads `pg_database`), so
  nothing that compares or cases text runs -- `'a' < 'b'`,
  `upper()`, a regular expression, and so `SUBSTRING ...
  SIMILAR`.
- `server.sh` checks each against what PostgreSQL gives.

## Step 88: ALTER TABLE is refused plainly

Every ALTER of a table or an index -- `ADD FOREIGN KEY`, `ADD
COLUMN`, `RENAME`, `SET SCHEMA` -- failed opening `pg_class`,
in the permission check of the relation's lookup.

- **It is refused before the lookup**, as `ALTER SEQUENCE` is:
  `AlterTableLookupRelation()`, `RenameRelation()`,
  `renameatt()`, `RenameConstraint()` and
  `AlterTableNamespace()` say the statement is not supported
  yet, by its own name (`AlterObjectTypeCommandTag()`, from
  `utility.c`, now extern).
- `server.sh` checks the refusals.

## Step 89: the default collation is C

Step 8 made a database's collation and character
classification C, but left the default collation unset:
`init_database_collation()` read `pg_database`. So nothing that
compares or cases text ran -- `'a' < 'b'`, `ORDER BY` of text,
`upper()`, `LIKE`, a regular expression -- with "default locale
not initialized".

- **`init_database_collation()` makes the default C**, as the
  process's locale is, marked the default, with no catalog
  read; `CheckMyDatabase()` calls it where it sets the locale.
- **Found, not fixed here**: an explicit `COLLATE` opens
  `pg_collation`; and `digres --run`, with no data directory,
  crashes on any name looked up through the search path, as
  the role's name is read from a cache it never set up.
- `server.sh` compares, sorts, cases and matches text.

## Step 90: digres --run looks names up through the search path

Found in step 89: `digres --run`, with no data directory,
crashed on any name looked up through the search path -- an
unqualified cast, an operator matched by coercion -- reading
the role's name from a cache it never set up.

- **`GetUserNameFromId()` reads no cache**: digres has one role
  and no `pg_authid`. The superuser's name is initdb's, or with
  no data directory the operating system user's, initdb's
  default; any other oid is no role.
- **With no storage, the schemas are the built-in ones**:
  `get_namespace_oid()` finds a name among the compiled-in rows
  when there is no sm session.
- **A `--run` session has the C default collation**, as a
  server session does (step 89).
- `test/select_text.sql` compares, matches and concatenates
  text, casts to a standard type name and names the user, with
  no server, alone and in 8 threads.

## Step 91: the built-in collations are compiled in

Found in step 89: an explicit `COLLATE` looked the name up in
`pg_collation`, which digres does not have.

- **`gen_cat.py` writes pg_collation**: `cat_collation()` finds
  a collation by oid, `cat_collations_named()` by name.
- **What read the `COLLOID` and `COLLNAMEENCNSP` caches reads
  the rows**: `lookup_collation()` and
  `CollationIsVisibleExt()` (`namespace.c`),
  `get_collation_name()` and `get_collation_isdeterministic()`,
  and making a collation's locale, `create_pg_locale()` and the
  builtin, ICU and libc providers' constructors.
- **So the seven built-in collations work**: `C` and `POSIX`,
  ICU's `unicode`, and the builtin provider's `ucs_basic`,
  `pg_c_utf8` and `pg_unicode_fast`. A collation of one's own,
  CREATE COLLATION, is not wired to sm yet.
- `server.sh` uses each provider's.

## Step 92: hba.c is gone

`pg_hba.conf` and `pg_ident.conf` were not read (step 9), but
their parser was still compiled, unreached. What chooses how a
connection is authenticated, once digres has roles, will not be
those files.

- **`hba.c` is deleted**: the tokenizer, the parsers of both
  files, the matching of a line to a connection and of a user
  to a map. `auth.c` keeps what was still called, some forty
  lines: the fixed rule (`hba_getauthmethod()`), the names of
  the methods (`hba_authname()`), `pg_isblank()`, and
  `check_usermap()`, which now only compares the two names --
  there is no map to look in.
- **Went with it**: `hbafuncs.c` and its functions,
  `pg_hba_file_rules()` and `pg_ident_file_mappings()`, with
  their views; `ifaddr.c` and `ifaddr.h`, which matched
  `samehost` and `samenet`; `check_oauth_validator()`, which
  only the parser called; the settings `hba_file` and
  `ident_file`; and the two sample files.
- **`hba.h` keeps `HbaLine`**, less the fields that matched a
  line to a connection or named it in a file: the
  authentication methods read the rest. A log line of a
  connection authenticated no longer ends in a file and line,
  which were `(null):0`.
- **Left on purpose**: the authentication methods the fixed
  rule never chooses -- ident, RADIUS, password, md5, trust --
  and those that are build options (GSS, SSPI, PAM, BSD, LDAP,
  certificates, OAuth). `auth.c` still holds them, unreached.
- `server.sh` checks the two settings and the two functions are
  gone; its peer and password checks are as before, and a
  `check_usermap()` that admits any name fails them.


## Step 92: bool, floats and text in tables

A table in sm took `int2`, `int4` and `int8` alone, though sm2
has seventeen types. sm2 keeps PostgreSQL's own datums
(`../sm/sm2/TYPE.md` §1), so a type needs only its code.

- **`bool`, `float4`, `float8`, `text`, `varchar` and `bytea`**
  are mapped to sm2's codes 1, 5, 6, 8, 9 and 10, and back
  (`type_code()`, `type_oid()`). A varlena goes across in its
  short or long form; sm2 refuses a compressed one or a TOAST
  pointer, which a table in sm never makes.
- **Their keys are PostgreSQL's order**: a float's total order,
  NaN last and -0 one key with 0; text's byte order, which is
  the default collation, C.
- **An index's opclass is the type's default, or one it is
  binary-coercible to**, as `GetDefaultOpClass()` finds it:
  `varchar` takes `text_ops`. Its collation is the column's,
  so the planner matches it to a clause in that collation; one
  in another, `COLLATE "unicode"`, is not served by it.
- `server.sh` stores, reads, indexes and keys each, and refuses
  a type sm does not take yet, `point`.

## Step 93: dates, times, intervals and uuids in tables

- **`date`, `time`, `timestamp`, `timestamptz`, `interval` and
  `uuid`** are mapped to sm2's codes 11 to 16, and back. Their
  datums are PostgreSQL's: four or eight bytes by value, an
  interval's 16-byte struct and a uuid's bytes by reference.
- **An index keyed on interval is refused.** sm2 keys an
  interval by its span and then its months and days
  (`../sm/sm2/TYPE.md` §3.10): an order finer than
  PostgreSQL's, which calls `'1 mon'` and `'30 days'` equal. So
  `=` through such an index would find one of two equal
  intervals, and UNIQUE would take both. An interval may still
  be an index's INCLUDE column. Decided in step 99: it stays
  refused.
- **Found, not fixed here**: an INSERT naming its columns fails
  opening `pg_type`, for a table of any types.
- `server.sh` stores, reads, indexes and keys each, and refuses
  an interval key.

## Step 94: an INSERT names its columns

Found in step 93: an INSERT naming its columns failed for every
table, as the rewriter asks each left-out column's type for its
default (`get_typdefault()`), which read the `TYPEOID` cache.

- **`get_typdefault()` reads the compiled-in row**: no built-in
  type has a default, so a left-out column is null, or refused
  if it is NOT NULL.
- `server.sh` inserts naming columns, in another order and from
  a SELECT, and refuses leaving out a NOT NULL one.

## Step 95: numeric is a decimal of 38 digits

`DECIMAL.md` §3, steps 1 to 3: PostgreSQL's arbitrary-precision
numeric is gone, and `numeric` is a 128-bit integer with its
scale.

- **`utils/adt/decimal.c` replaces `numeric.c`** (some 2,900
  lines for 11,400), keeping every function's name, so its
  `pg_proc` rows and its callers stand: jsonb, jsonpath,
  `extract`, `to_char`, `money`, `pg_lsn` and the rest.
- **The type is 32 bytes by reference** (`decimal_t`,
  `utils/numeric.h`), oid 1700 and name `numeric` as before;
  its text and binary forms are PostgreSQL's.
- **What PostgreSQL did with arbitrary precision is done
  exactly** in a 512-bit integer where 128 bits do not reach --
  a product, a quotient's dividend, a sum of squares -- and
  rounded to 38 digits; each result's scale is the one
  PostgreSQL's would have. `ln`, `exp`, `log` and a power to a
  fraction are computed in `long double`.
- **No NaN, no infinity**; a typmod's precision is 1 to 38; a
  result past 38 digits before the point is an error; a literal
  past 38 digits is a `float8`.
- **Sums and variances are exact**: one state for every numeric
  aggregate, of numerics and integers, summing in 512 bits.
- `test/select_numeric.sql` checks PostgreSQL's results with no
  server; `server.sh` checks the errors.

## Step 96: numeric columns are sm's decimals

`DECIMAL.md` §2.3, step 4 of its §3.

- **A `numeric(p, s)` column is sm2's `decimal64`** up to 18
  digits, **`decimal128`** up to 38, its typmod sm2's own. A
  bare `numeric` column is `numeric(18, 3)`, DuckDB's default,
  as sm2 keeps a decimal's scale in its schema.
- **Values cross at the sm boundary** (`smstore.c`): a row's
  numerics are rounded to their columns' scales and checked
  against their precisions, as an assignment does, and go as
  integers; read back, they are made numerics at the column's
  scale and the slot is materialized, as sm's datums point into
  a tuple kept only until the next row. Statistics' values are
  made numerics the same way.
- **A scan's key is widened to the column's scale**: the
  integer at or below a lower bound, at or above an upper one,
  within the column's precision, inclusive. Each row is checked
  against the keys again, so `= 1.505` on a `numeric(10, 2)`
  finds nothing, and `<= 1.505` finds `1.50`.
- `server.sh` stores, reads, indexes, keys and analyzes numeric
  columns.

## Step 97: a JSON number with an exponent is a float8

`DECIMAL.md` §2.7: jsonb's numbers were numeric's alone, so a
JSON number past 38 digits, `1e300`, was refused.

- **jsonb has a second kind of number, `jbvFloat8`**, stored as
  a container entry of its own (`JENTRY_ISFLOAT8`), eight
  bytes. A number token with an exponent is parsed as one, one
  without as a numeric.
- **The two are one kind** (`JsonbKind()`): equal, ordered and
  contained across the kinds, a numeric and a float8 compared
  as float8s, as SQL compares them; both hash as the float8
  they compare as. A float prints as `float8` does.
- **jsonpath computes in numeric**: a float8 is converted where
  it is computed with -- arithmetic, a number method -- and
  compared as a float8; one only selected is returned as it is.
- **Casts and text** (`->>`, `::numeric`, `::float8`, the
  integers, `jsonb_array_elements_text`, JSON_VALUE) take
  either kind.
- `test/select_jsonb_numbers.sql` checks each, with no server.

## Step 98: a column has a DEFAULT

CREATE TABLE refused a column's DEFAULT: there was nowhere to
keep it, as there is no `pg_attrdef`.

- **sm keeps it, opaque**: a column spec's `default_expr`, the
  column part's eighth attribute, bytes sm stores and loads and
  never runs (`../sm/sm3/CATALOG_ONDISK.md`). A part written
  before it reads as null, so the catalog's format is unchanged.
- **CREATE TABLE cooks it** as PostgreSQL does
  (`cookDefault()`): coerced to the column's type, a column
  reference or a subquery refused; its `nodeToString()` is
  what sm keeps.
- **The relcache gives it back** as `pg_attrdef` would: the
  column's `atthasdef`, the table's `defval`. So the rewriter
  fills a column an INSERT leaves out, `DEFAULT VALUES` and
  `SET col = DEFAULT`, by PostgreSQL's own path.
- `test_ddl` keeps a default across a restart; `server.sh`
  inserts and updates by defaults in later sessions, and
  refuses bad ones.

## Step 99: an interval is never an index key

- **Decided, not deferred.** Step 93 left open whether sm2's
  interval key should share PostgreSQL's ties, or dg scan every
  key of a span. Neither: an index keyed on interval stays
  refused, PRIMARY KEY and UNIQUE included; an interval may be
  an INCLUDE column.
- Why: sm2's key keeps `'1 mon'` and `'30 days'` apart on
  purpose (`../sm/sm2/TYPE.md` section 3.10), and dg scanning
  spans would still leave UNIQUE wrong. Interval keys are rare;
  a query on one scans the table. Lifting the refusal later is
  compatible.
- The error drops "yet": `an index key of type interval is not
  supported`.

## Step 100: COPY reads and writes a table in sm

`COPY t FROM` and `COPY t TO` failed: both go to the table access
method, whose callbacks refused. `COPY (SELECT ...) TO` ran, as
a query.

- **`smtable.c` scans and inserts**: `scan_begin`,
  `scan_getnextslot` and `scan_end`, a forward scan of every
  row over `smstore_beginscan()`, with no scan keys; and
  `tuple_insert` and `multi_insert`, over `smstore_insert()`.
  A batch is inserted a row at a time: sm has no batched insert
  yet (`../sm/BULKLOAD.md`).
- **The executor writes no index of a table in sm**:
  `ExecInsertIndexTuples()` returns at once for one. sm writes
  a table's indexes with its row, and an index in sm has no
  `aminsert`; COPY called it for each row.
- **What works**: text, CSV and binary, a column list, a file,
  STDIN and STDOUT, psql's `\copy`, a temporary table. A row
  sm refuses -- a key that is taken, a NULL in a NOT NULL
  column -- fails the statement and leaves none of its rows.
- **Left on purpose**: `scan_rescan` and the rest of the scan
  callbacks, which nothing reaches.
- `server.sh` checks each of those. A `multi_insert` that takes
  one row of a batch, and index maintenance not skipped, each
  fail it.

## Step 101: a collation's name is printed from the rows compiled in

Found in step 91: `EXPLAIN VERBOSE` of a constant in a collation
not the default, and `regcollation` output, read `pg_collation`.

- **`generate_collation_name()`** (`ruleutils.c`) and
  **`regcollationout()`** (`regproc.c`) find the collation with
  `cat_collation()`, as `get_collation_name()` does.
- `server.sh` prints a `regcollation` and explains a collated
  constant.

## Step 102: a SERIAL column, its sequence owned by it

- **sm3: a sequence has an owner**, a table's column, set by
  `own_sequence` (`sm_cat_own_sequence`): appended, nullable
  fields of the sequence part (`../sm/sm3/CATALOG_ONDISK.md`).
  Dropping the table or the column drops the sequences it owns;
  dropping an owned one alone is `SM_ERR_INUSE`.
- **`ALTER SEQUENCE ... OWNED BY`** is taken, `OWNED BY NONE`
  too, and nothing else ALTER SEQUENCE asks. The table must be
  in the sequence's schema, as PostgreSQL requires.
- **`CREATE SEQUENCE ... AS smallint` or `integer`** is taken.
  sm's counter keeps no bound: a value past the type's range
  fails where it is stored, `integer out of range`, not in
  `nextval`. A type other than the three is PostgreSQL's error.
- **So `serial`, `smallserial` and `bigserial` work**, as
  PostgreSQL expands them: a sequence, the column's `DEFAULT
  nextval(...)` and NOT NULL, and the sequence owned by the
  column. A temporary table's is refused, with temporary
  sequences.
- **DROP SEQUENCE of an owned one** fails as PostgreSQL's does
  without CASCADE, hinting `OWNED BY NONE`.
- **DROP SCHEMA ... CASCADE drops its tables first**, then lists
  the sequences left in a new statement: an owned one went with
  its table and is not named, as in PostgreSQL.
- Identity columns, on the same ownership, are a step of their
  own: `GENERATED ALWAYS` needs INSERT's `OVERRIDING`.
- sm3 `test_ddl`: "a column owns a sequence, which goes with its
  table or column"; `server.sh`: serial columns.

## Step 103: COPY into an empty table is a bulk load

`COPY` inserted its rows one by one (step 100): a write to sm for
each row and for each of its index entries, every one logged,
put in a memtable and compacted. sm now has a bulk load
(`../sm/BULKLOAD.md` §13).

- **`multi_insert` hands COPY's rows to sm's load**
  (`smstore_load()`, over `sm_tbl_load_begin` and
  `sm_tbl_load_add`), and `finish_bulk_insert`, which COPY calls
  when it has no more, ends it (`smstore_load_end()`).
- **Into an empty table the rows go to sorted files**, which
  sm's store takes whole at the end, past its log and its
  compactions. sm takes the table's lock exclusive for that,
  until the transaction ends: a session that reads the table
  waits for the COPY's transaction, where PostgreSQL would let
  it read an empty table.
- **A duplicate key is then found at the end**, not at its row;
  the COPY fails all the same, and leaves no row.
- **Into a table with rows nothing changes**: sm inserts each
  row, and takes no lock beyond the table's own.
- **The files are under `base/pgsql_tmp/sm/load`**, never in a
  database's directory.
- **A commit sm refuses ends sm's transaction too**
  (`smstore_commit()`). One refused without being aborted, as a
  commit with a load still open is, was left running and kept
  its locks.
- **Measured**, 300,000 rows of two integers and a short text,
  this build: 4.0 s before and 1.6 s now with no index; 6.6 s
  and 1.7 s with a primary key; 13.0 s and 2.2 s with a primary
  key and two more indexes.
- **Not done**: `INSERT ... SELECT` and `CREATE TABLE AS` still
  insert row by row; and a `ROLLBACK TO` a savepoint set before
  such a COPY fails, since sm cannot undo a load in part.
- `server.sh` loads 100,000 rows and reads them through both
  indexes; fails a load with a repeated key and loads the
  table again; shows a reader waiting; and a COPY into a table
  with rows failing at its row. A load never ended, and one
  that takes no lock, each fail it.

## Step 104: an identity column, its sequence its own

- **sm3: a column may be an identity column**, its kind (`a` or
  `d`, PostgreSQL's `attidentity`) and its sequence appended,
  nullable fields of the column part
  (`../sm/sm3/CATALOG_ONDISK.md`). `create_table` makes the
  column own the sequence; it must be a NOT NULL smallint,
  integer or bigint with no default, and not in a temporary
  table. `own_sequence` on that sequence is `SM_ERR_INUSE`, and
  `add_column` refuses an identity column.
- **`GENERATED ALWAYS | BY DEFAULT AS IDENTITY`** is taken in
  CREATE TABLE, with START and INCREMENT. Parse analysis makes
  the sequence first; `smstore_create_table` passes its oid, and
  the `OWNED BY` it adds after is a no-op (`AlterSequence`).
- **The relcache sets `attidentity`**, so the rewriter's rules
  for ALWAYS, `OVERRIDING SYSTEM | USER VALUE` and UPDATE are
  PostgreSQL's unchanged. `getIdentitySequence` reads the
  column's sequence from sm (`smstore_identity_sequence`), not
  `pg_depend`.
- **A `NextValueExpr` past its type's range fails**, `integer
  out of range` or `smallint out of range`, rather than
  wrapping: sm's sequence keeps no bound, as with `serial`.
- **EXPLAIN VERBOSE names the sequence from sm**; PostgreSQL
  read `pg_class`.
- **ALTER SEQUENCE ... OWNED BY on an identity's sequence** is
  PostgreSQL's `cannot change ownership of identity sequence`.
  DROP SEQUENCE of an owned one now hints at dropping the
  column.
- ALTER TABLE's identity forms (ADD, SET GENERATED, RESTART,
  DROP IDENTITY) wait with ALTER TABLE. EXPLAIN VERBOSE of a
  `serial` INSERT still fails, on `pg_proc`, printing
  `nextval()`.
- sm3 `test_ddl`: "an identity column owns its sequence from
  its CREATE TABLE"; `server.sh`: identity columns.

## Step 105: a bulk load does not wait for its table

Step 103's load took an empty table's lock exclusive, and waited
for it: a COPY into an empty table that another session had only
read, in a transaction still open, waited for that transaction
to end.

- **sm takes the lock only if it is free at once**
  (`sm1::trylock`, under `sm_tbl_load_begin`). With another
  transaction using the table the COPY's rows are inserted, as
  into a table with rows.
- **Nothing changes in digres's code.** What changes is when a
  COPY into an empty table is a load from files: when no other
  transaction has the table open.
- A COPY that does take the lock still keeps readers out until
  it commits (step 103).
- `server.sh` holds a transaction that read an empty table, and
  runs a COPY into it from another session: it does not wait,
  and its rows are there.

## Step 106: SELECT ... FOR UPDATE and FOR SHARE

Locking a row was refused: the table access method's `tuple_lock`
read a row's newest version only after an UPDATE or a DELETE had
found it changed (step 86).

- **`tuple_lock` locks a row in sm** (`smstore_lock_row()`, over
  `sm_tbl_lock`): exclusive for FOR UPDATE and FOR NO KEY UPDATE,
  shared for FOR SHARE and FOR KEY SHARE, to the end of the
  transaction. sm has the two modes and no key-only ones, so the
  weaker of each pair is as strong as the stronger: an UPDATE
  that leaves a key alone still waits for a FOR KEY SHARE.
- **It waits as a write does**, for a transaction that holds the
  row or has written it. A plain SELECT takes no lock and waits
  for none.
- **At READ COMMITTED the newest version is read** once the lock
  is held, and the executor checks the row's WHERE again
  (EvalPlanQual) if that version is another than the statement
  saw: the two tuples are compared, so a row no one changed
  costs no recheck. A row deleted meanwhile is not returned.
- **At REPEATABLE READ and SERIALIZABLE** sm fails the lock, and
  the transaction, if the row changed since the snapshot: 40001.
- **The slot owns the row it is given**: the executor keeps it
  past sm's next call.
- **Refused**: NOWAIT and SKIP LOCKED. sm has no row lock that
  does not wait yet.
- `server.sh` checks the four strengths; a FOR UPDATE that waits
  for an updater and returns its row, rechecks its WHERE, and
  drops a row deleted meanwhile; an UPDATE waiting for a FOR
  UPDATE and for a FOR SHARE; a SELECT and a FOR SHARE passing;
  and the failure at REPEATABLE READ. A row left unlocked, a
  changed row not rechecked, and a share lock taken exclusive
  each fail it.

## Step 107: a function's and a sequence's names are printed

- **`generate_function_name()` reads `cat_proc()`**, the rows
  compiled in, as `generate_operator_name()` does: any EXPLAIN
  VERBOSE, or other deparse, that printed a function call failed
  opening `pg_proc`.
- **`regclassout()` names a sequence from sm**
  (`smstore_sequence_lookup`), qualified only when the search
  path does not find it: a sequence has no descriptor, so a
  serial's `nextval('t_id_seq'::regclass)` failed to print.
- The strings are made in the caller's context, as PostgreSQL's
  are, and are copied by every caller; none is kept in a cache.
- `server.sh`: EXPLAIN VERBOSE of functions and of serials, and
  a sequence's regclass.

## Step 108: NOWAIT and SKIP LOCKED

Step 106 refused both: sm had no row lock that did not wait.

- **sm has one now** (`sm_tbl_trylock`, over `sm1::trylock`): the
  lock `sm_tbl_lock` takes, or 1 at once if it would have waited
  -- another transaction holds the row in a mode that conflicts,
  or has written it and not yet ended.
- **`smstore_lock_row()` asks that way** for a wait policy other
  than "block". NOWAIT raises `could not obtain lock on row in
  relation`, 55P03, as PostgreSQL does; SKIP LOCKED answers
  `TM_WouldBlock`, and the executor passes over the row.
- So two sessions each taking the first ready row of a table,
  `ORDER BY ... LIMIT 1 FOR UPDATE SKIP LOCKED`, take different
  rows, and neither waits.
- **Not done**: `LOCK TABLE ... NOWAIT`, and `lock_timeout`.
- `server.sh` checks NOWAIT on a free row, a held one and a
  written one; a FOR SHARE NOWAIT passing a FOR SHARE; and SKIP
  LOCKED taking the next row, and leaving out a held row and a
  written one. Waiting all the same, and failing where it should
  skip, each fail it.

## Step 109: INSERT ... ON CONFLICT

It was refused. PostgreSQL does it by a speculative insertion: it
inserts the row, finds a conflict from the index it then writes,
and takes the row back. A row in sm cannot be taken back, short
of a savepoint a row.

- **sm finds the conflict before the row is written**
  (`sm_tbl_insert_arbiter`, `../sm/sm3/include/sm3.h`). It looks
  the key up in each arbiter index; finding none, it puts the
  row's entry into the arbiters first, where sm waits for another
  session's insert of the key and says at once if a row is there,
  and only then writes the row.
- **A conflict leaves nothing locked.** The first version locked
  the key to look, and kept the lock: a session waiting for the
  row then stood between the row's holder and a DELETE of it, and
  the two deadlocked. Now a lock taken to read the newest version
  is given back at once.
- **At READ COMMITTED a row the snapshot shows is confirmed**
  against the newest version before it is called a conflict. The
  version before that went on for ever finding a row another
  session had deleted since.
- **`ExecInsert()` has a branch for a table in sm**: on a conflict
  DO NOTHING returns, and DO UPDATE is PostgreSQL's
  `ExecOnConflictUpdate()`, which locks the row through
  `tuple_lock` and updates it through `tuple_update`; if the row
  is gone by then, the insert is tried again.
- **`ExecUpdateLockMode()` answers for a table in sm** without
  asking which columns an index has: sm has one lock to write a
  row. It read each index's `pg_index` row, which an index in sm
  has none of, and crashed.
- **`ExecCheckTupleVisible()` passes a row in sm**: above READ
  COMMITTED sm fails the insert, or the lock, on a key or a row
  changed since the snapshot.
- **A bug of step 106 is fixed**: `SELECT ... FOR UPDATE` above
  READ COMMITTED failed on a row no one had changed. sm reads a
  row's newest version only at READ COMMITTED; above it
  `smstore_lock_row()` now reads the snapshot's, which the lock
  has just shown to be the newest.
- **Unlike PostgreSQL**: two rows of one statement with the same
  key -- `VALUES (7, ...), (7, ...) ... DO UPDATE` -- insert the
  first and pass over the second, where PostgreSQL raises "cannot
  affect row a second time". sm tells a statement that it has
  changed a row already only when it writes it.
- **Found, not fixed here**: `ON CONFLICT ON CONSTRAINT name`
  opens `pg_constraint`.
- **Still refused**: MERGE.
- `server.sh` checks DO NOTHING and DO UPDATE, by a column list
  and by none, EXCLUDED, a WHERE, RETURNING, several rows; an
  upsert waiting for an insert that commits and for one that
  rolls back, for an updater, and for a delete; a row deleted
  once it was found; and both isolation levels above READ
  COMMITTED. Running PostgreSQL's visibility check, and not
  trying again, each fail it.

## Step 110: ALTER TABLE adds, drops and renames

Step 88 refused every ALTER TABLE. sm3 had `add_column`,
`drop_column` and `rename_table`; `sm.h` had none of them.

- **`sm.h` has them now**, with sm3's new `rename_column`:
  `sm_cat_add_column`, `sm_cat_drop_column`,
  `sm_cat_rename_column`, `sm_cat_rename_table`. An index
  names its columns by place, so a rename leaves it alone.
- **`utility.c` calls `smstore_alter_table()`** for a table in
  sm, as it calls `smstore_create_table()`: PostgreSQL's
  `AlterTable()` reads `pg_class` and `pg_attribute` at every
  turn. `AlterTableLookupRelation()` lets through ADD COLUMN
  and DROP COLUMN only, and looks the table up with no
  permission check, which would open `pg_class`.
  `RenameRelation()` and `renameatt()` call
  `smstore_rename_table()` and `smstore_rename_column()` for a
  table.
- **ADD COLUMN takes NOT NULL and a DEFAULT.** The default is
  cooked as CREATE TABLE's is, kept, and evaluated once: the
  rows already there read that value, as PostgreSQL's
  `attmissingval`. One that calls something volatile, and a
  serial, are refused: PostgreSQL rewrites the table for them,
  and sm has no rewrite (`../sm/NEXT.md` §2).
- **NOT NULL with no DEFAULT needs an empty table**, as in
  PostgreSQL, which fails `column ... contains null values`
  otherwise. sm3 takes the table's lock X only if it is free
  now, and keeps it only if the store holds no version at all;
  a savepoint lets it go otherwise. So a table emptied by
  DELETE counts as having rows until compaction; TRUNCATE
  empties it. A session using the table makes the ALTER fail
  at once, `could not obtain lock on relation`, 55P03, rather
  than wait.
- **DROP COLUMN of one an index uses is refused**, CASCADE or
  not: dropping the index with it is not done yet.
- **Each subcommand closes the transaction's handles of the
  table**, after `CheckTableNotInUse()`: a handle holds the
  definition it was opened with.
- **Not done**: the rest of ALTER TABLE -- SET SCHEMA, ALTER
  COLUMN TYPE, SET and DROP NOT NULL and DEFAULT, ADD
  CONSTRAINT, the identity forms -- and RENAME of an index or
  a constraint.
- sm3 `test_ddl`: a column renamed; `test_lock`: a NOT NULL
  column with no missing value. `server.sh`: each form, the
  refusals, a rollback, and another session reading the new
  column.

## Step 111: a statement waits out DDL before it reads the definition

Step 41 made PostgreSQL's relation locks no-ops for a table in
sm: sm locked the table when the executor opened it. So a
statement read the definition, planned, and only then waited in
sm for a DDL that held the table. Once the DDL committed it ran
the plan it had: an INSERT behind an uncommitted ADD COLUMN
failed with `sm_tbl_insert: n is not the attribute count`, and
one behind a DROP COLUMN wrote into the dropped column's place.

- **`LockRelationOid()`, `LockRelationId()` and
  `LockRelation()` open the table in sm**
  (`smstore_lock_relation()`), before they take the
  invalidation messages, as PostgreSQL's lock is taken before
  its relcache entry is used. sm's lock is only to exclude DDL,
  which takes X: AccessExclusive and Exclusive take X,
  RowExclusive IX, the rest IS. So CREATE INDEX's ShareLock and
  ANALYZE's ShareUpdateExclusive keep no writer out, as sm's
  index builds and analyzes do not. An index's or a sequence's
  oid is no table in sm, and takes nothing.
  `ConditionalLockRelationOid()` is still a no-op.
- **ALTER TABLE looks its table up with no lock**: sm's DDL
  takes the lock itself, and a NOT NULL column with no default
  asks for it without waiting (step 110), which X taken first
  would turn into a wait.
- **The invalidation messages are sent before sm commits, too**
  (`PreCommit_Inval()`). sm's commit lets go of the locks, and
  `AtEOXact_Inval()` sends them only after: a session the
  commit woke could read the messages waiting before they were
  there, and cache the old definition. One that reads them
  before the commit rebuilds nothing old, since a rebuild opens
  the table in sm and waits.
- `server.sh` gains helpers for held sessions and background
  statements, and checks, with sessions holding the table: ADD
  COLUMN and RENAME wait for a reader; an INSERT and a SELECT
  behind an uncommitted ADD COLUMN take its default and read
  it; an INSERT behind a DROP COLUMN fails as PostgreSQL's
  does. Without the lock the last three fail. The early send
  is a race no check holds to.

## Step 112: pg_namespace is read, and \dn runs

The first catalog relation (`SYSCATALOG.md` step 2): a catalog a
query reads, with nothing stored.

- **`gen_cat.py` writes each catalog relation's oid and
  columns** (`RELATIONS`, `cat_relation()`), from
  `postgres.bki`, and every catalog's rows to list
  (`cat_namespaces()` and the like).
- **`get_relname_relid()` finds its name in `pg_catalog`**, and
  `RelationBuildDesc()` builds its descriptor from the
  compiled-in columns (`RelationBuildCatalogDesc()`), its
  `relfilenode` its oid so it is not taken for a mapped one.
- **Its access method is `catrel.c`'s**: a scan begins by making
  the rows into a tuple store and reads them from it; the
  planner is told a page and a hundred rows. `pg_namespace` is
  the rows compiled in, then sm's schemas, owned by the
  superuser, with no ACL. `pg_publication` and
  `pg_publication_namespace`, which `\dn PATTERN` reads, are
  empty.
- **It is read by anyone and written by no one**: the ACL checks
  (`aclchk.c`) give SELECT alone, so INSERT, UPDATE, DELETE and
  FOR UPDATE are `permission denied for table`; TRUNCATE and
  DROP TABLE say it is a system catalog.
- **`pg_get_userbyid()` names the superuser** from
  `GetUserNameFromId()`, not `pg_authid`.
- sm's `pg_temp` is listed as it is, oid 8846, where PostgreSQL
  has a `pg_temp_N` a session.
- `server.sh`: `\dn`, `pg_namespace`'s rows, a schema made in an
  open transaction, and the writes refused.

## Step 113: pg_class and pg_am are read, and \dt and \ds run

`SYSCATALOG.md` step 3.

- **`pg_class` lists every relation**: the catalog relations, in
  `pg_catalog`, then sm's tables, their indexes and the
  sequences of each schema (`smstore_relations()`), and the
  session's temporary tables, in `pg_temp`. Each has its name,
  schema, kind, column count, whether it has an index, and its
  access method, heap or btree; the owner is the superuser, the
  rest as a relation never analyzed has it.
- **Listing takes no lock**: `sm_cat_table_def()`, new in
  `sm.h`, reads a table's definition as the transaction sees
  it, where `sm_tbl_open()` would wait for a DDL on it.
- **`pg_am` is the rows compiled in.**
- **`pg_class` is opened at startup** (`formrdesc()`), so the
  nailed descriptor takes the catalog relations' access method
  too.
- **`pg_table_is_visible()` of a sequence** reads its name and
  schema from sm (`RelationGetClassForm()`): a sequence has no
  descriptor.
- The temporary schema is `pg_temp`, as in `pg_namespace`, not
  PostgreSQL's `pg_temp_N`.
- `server.sh`: `\dt`, `\ds`, `\dt pg_temp.*`, `\dtS`, pg_class's
  rows, pg_am, and a write refused.

## Step 114: pg_index is read, \di runs, and an index's definition is printed

`SYSCATALOG.md` step 4.

- **`pg_index` lists sm's indexes** (`smstore_relations()`): each
  one's table, columns, key column count, uniqueness, whether
  it is valid, and each key column's operator class (its
  type's default btree one), collation, order and nulls. sm
  keeps no mark of a primary key, so `indisprimary` is false,
  and `\d` will call one UNIQUE.
- **`pg_get_indexdef()` prints an index of sm's** from its
  descriptor (`sm_indexdef()` in `ruleutils.c`), as PostgreSQL
  prints it: `CREATE [UNIQUE] INDEX name ON schema.table USING
  btree (...)`, with DESC, NULLS FIRST or LAST where they are
  not a btree's own, and INCLUDE; or one column of it.
- **An index can be opened by its oid alone** once a listing or
  a lookup by name has found it: both record its table
  (`remember_index()`), which `RelationBuildDesc()` asks.
  Before, only a table's descriptor recorded it. sm has no
  lookup of an index by oid.
- `server.sh`: `\di`, `pg_index`'s rows, and
  `pg_get_indexdef()` of three indexes and of one column.

## Step 115: \d describes a table and an index, and \du runs

`SYSCATALOG.md` step 6, for a table and an index.

- **`pg_attribute` lists the columns** of the catalog relations
  and of sm's tables and indexes (`smstore_relations()`, which
  now gives each one's names, types, typmods, NOT NULL,
  defaults and identity): storage as the type's, collation as
  the type's, a dropped one named as PostgreSQL names it. A
  sequence's columns and the system columns are not listed.
- **`pg_attrdef` has a column's DEFAULT**, the expression sm
  keeps, with oid 0: sm keeps none for it.
- **`pg_type` and `pg_collation` are the rows compiled in**, as
  text: `gen_cat.py` writes them out of `postgres.bki`
  (`TEXT_ROWS`), and each value is read by its column type's
  input function (`BuildTupleFromCStrings()`).
- **`pg_roles` is a catalog relation** though a view in
  PostgreSQL: its columns are written in `gen_cat.py` (`VIEWS`),
  its oid taken from initdb's range for views, 12100. One row,
  the superuser. So `\du` runs.
- **Read, and empty**: `pg_constraint`, `pg_inherits`,
  `pg_policy`, `pg_publication_rel`, `pg_rewrite`,
  `pg_statistic_ext` and `pg_trigger`.
- **`pg_relation_is_publishable()` is back**, answering false:
  step 61 removed it with publications, and psql's `\d` asks.
- **`try_relation_open()` of a relation of sm's or a catalog
  relation** builds its descriptor, or finds none, with no
  `pg_class` probe: `pg_get_expr()` opened its relation that
  way.
- **`hash_ok_operator()` reads the operator compiled in**: a
  correlated EXISTS the planner may hash, as `\d`'s, opened
  `pg_operator`.
- **Not done**: `\d` of a sequence, which reads `pg_sequence`;
  `\d+`, which needs `col_description()` and the size
  functions; a primary key is shown as UNIQUE (step 114).
- `server.sh`: `\d` of a table with a default, a typmod, a
  serial, a dropped column and two indexes, `\d` of an index,
  and `\du`.

## Step 116: a catalog scan for one relation makes its rows alone

Every scan of a catalog relation made every row: `\d t` reads
`pg_attribute` for one table, and listed every column of every
table, several times a command.

- **`nodeSeqscan.c` hands a catalog relation its keys**
  (`catrel_scan_keys()`, at the node's start): each qual of a
  filter column (`is_filter_column()` in `catrel.c`) equal to a
  value that reads nothing of the relation and is not volatile,
  a constant or a parameter, made a scan key when the scan
  begins (`catrel_beginscan()`). A rescan begins the scan again,
  so a correlated subquery's parameter narrows each one.
- **A generator given a key makes the rows of that relation**:
  `smstore_relations_of()`, which reads the one table's
  definition by its oid (`sm_cat_table_def_oid()`, new in
  `sm.h`, with no lock), with its indexes; for an index, its
  table's, found as step 114 records it; for a sequence, it
  alone. An index whose table the thread has not met falls back
  to every relation.
- The quals are still checked on each row, so a key need only
  narrow, never decide. A join clause narrows nothing: the
  planner gives a sequential scan none.
- `server.sh` checks, by `EXPLAIN ANALYZE`'s rows removed, that
  `pg_attribute` for one table makes its rows alone. By hand,
  on thirty tables, the filter removed one row, where it had
  removed 89.

## Step 117: a primary key is one

Steps 114 and 115 showed a primary key as UNIQUE: sm kept no
mark of one.

- **sm3 keeps the mark** (`index_rec_t::primary`, the index
  part's attribute 11, written only for one, so an index kept
  before reads as none). `create_index()` takes it, and refuses
  one not unique, not on columns alone, on a column that may be
  null, or a table's second.
- **`sm.h`**: `sm_cat_create_index()` takes `primary`, and
  `sm_index_def_t` has it. `smstore_create_index()` passes the
  statement's.
- **digres reads it**: the index's descriptor has
  `indisprimary`, the table's `rd_pkindex`, and `pg_index` the
  column. So `\d` says PRIMARY KEY, and `\d` of the index
  "primary key".
- A table made before this has its primary key unmarked.
- sm3 `test_ddl`: a primary key marked, and refused three ways;
  `server.sh`: `\d` of a table and of its primary key.

## Step 118: pg_database is read

`SYSCATALOG.md` step 5's last part.

- **`pg_database` has a row a database of sm's**
  (`datadir_database_names()`), its oid the one the server
  gives it, `FirstNormalObjectId` and sm's id: owned by the
  superuser, in UTF-8, with the C locale, in the default
  tablespace, with no ACL. It is a shared catalog opened at
  startup, so it takes the catalog relations' access method as
  `pg_class` does (step 113).
- **psql 16's `\l` fails** on `daticulocale`, which PostgreSQL
  17 renamed `datlocale`: as against PostgreSQL 18 itself. A
  psql of 17 or later sends `datlocale`.
- `server.sh` reads `pg_database`.

## Step 119: \d describes a sequence

- **sm3 reads a sequence's definition back**
  (`cat_get_sequence_def()`; `sm_cat_sequence_def()` in
  `sm.h`): its namespace, name, START, INCREMENT, and the column
  that owns it.
- **`pg_sequence` has a row a sequence**: bigint, from START by
  INCREMENT, up to bigint's bound, with no cycle and a cache of
  one, as sm's sequences are. A serial's is bigint, where
  PostgreSQL makes it integer: its range is the column's
  (step 104).
- **`pg_depend` has a row an owned sequence**, its dependence
  on the owning column, automatic for a serial's and internal
  for an identity column's, as PostgreSQL records them, so `\d`
  says "Owned by" or "Sequence for identity column". digres
  records no other dependency.
- Both are narrowed by the sequence's oid (`seqrelid`,
  `objid`), as step 116 narrows the others.
- sm3 `test_ddl`: a sequence's definition read back;
  `server.sh`: `\d` of three sequences.

## Step 120: \d+, \dt+ and \ds+ run

- **`obj_description()`, `col_description()` and
  `shobj_description()` are back**, with their PostgreSQL rows,
  in `sql_bodies.c`, answering null: digres keeps no comments,
  as it has no `pg_description` (step 87 left them out).
- **`pg_relation_size(regclass)` is back**, as
  `pg_relation_size($1, 'main')`.
- **`try_relation_open()` of a sequence gives none**: a
  sequence of sm's has no descriptor (step 102), and the size
  functions, which `\ds+` calls, read a null for one.
- A table's and an index's sizes are 0: the size functions
  count relation files, which a table in sm has none of. sm's
  sizes (`sm_tbl_size()`) are not reached yet.
- COMMENT ON still fails, on `pg_description`.
- `server.sh`: `\d+` of a table, `\dt+` and `\ds+`.

## Step 121: a table's size is sm's

Step 120 left the size functions at 0 for a table in sm: they
count relation files.

- **A relation of sm's main fork is its bytes in sm**
  (`relation_fork_size()` in `dbsize.c`,
  `smstore_relation_bytes()`): a table's heap or an index's
  entries, in sorted files and in memtables, as `sm_tbl_size()`
  and `sm_tbl_idx_size()` count them; its other forks, none.
  So `pg_relation_size()`, `pg_table_size()`,
  `pg_indexes_size()` and `pg_total_relation_size()` answer,
  and `\dt+` shows a size.
- Bytes in a memtable are counted as they are there,
  uncompressed, and a file's as written. They are RocksDB's
  estimates, which vary: a thousand rows read 69 kB to 198 kB
  over three runs.
- `server.sh`: a table's and its index's sizes grow with rows,
  and add up.

## Step 122: COMMENT is refused plainly

COMMENT ON failed opening `pg_description`, relation 2609.
digres keeps no comments, and step 120 made the functions that
read them answer null, so it is refused once the object is
looked up: "COMMENT is not supported yet". One not there, or of
a kind digres has none of, says that first. A column's is
refused before, as its lookup reads `pg_attribute`. A restore script
that comments its objects stops there, as it would on any
statement refused.

- `server.sh` checks the refusal.

## Step 123: \df, \do, \dC and \dx run

`SYSCATALOG.md` step 7, for functions, operators and casts.

- **`pg_proc`, `pg_operator` and `pg_cast` are the rows compiled
  in**, as text (`TEXT_ROWS`), as `pg_type` is (step 115). A
  `pg_node_tree`, which no input function reads, is taken as
  its text. `pg_extension` and `pg_description` are read, and
  empty.
- **What psql calls on them reads the rows compiled in**, not
  the syscache: `regprocout()`, `pg_get_function_result()`,
  `pg_get_function_arguments()` and
  `pg_get_function_identity_arguments()` (a function's argument
  names and modes are parsed from the row's text arrays), and
  `pg_function_is_visible()` and `pg_operator_is_visible()`.
- A built-in's description is PostgreSQL's in `pg_description`,
  which digres has not, so psql shows none.
- `pg_get_functiondef()` still reads the syscache, and fails.
- `server.sh`: `\df` of two, `\do`, `\dC`, `\dT` and `\dx`.

## Step 124: a syscache miss reads the compiled-in rows

`SYSCATALOG.md` §2. A syscache miss scanned the catalog's index,
which digres has not: `pg_get_functiondef()` failed opening
relation 2690, `pg_proc`'s.

- **A catalog of compiled-in rows alone is scanned without its
  index**: `pg_am`, `pg_cast`, `pg_collation`, `pg_language`,
  `pg_operator`, `pg_proc` and `pg_type`
  (`catrel_is_builtin()`, `systable_beginscan()`). Its rows are
  fixed: a cached one is never stale, and none needs an
  invalidation.
- **A catalog relation's scan applies its keys.** A text row's
  key columns are read first, and a row a key passes over is
  read no further, so a lookup by oid or name in `pg_proc`
  reads its three thousand rows' keys alone. Every other row a
  generator makes is tested as it is read.
- `systable_getnext()` frees the copy it makes of a catalog
  relation's row, a minimal tuple, when it reads the next.
- **`pg_language` is a catalog relation**, its rows compiled in:
  `c` and `internal`, as digres has no `sql`.
- A catalog with sm's rows in it is not looked up this way; a
  syscache lookup in one still fails.
- `server.sh`: `\sf abs(integer)` prints its definition, a
  function no oid names gives null, and `pg_language` lists two.
  The `pg_attribute` narrowing check now finds the scan gives
  dd1.t's five rows, with none left for the filter to remove.

## Step 125: sm's TOAST is on

`../sm/NEXT.md` §3. digres read a value straight out of sm's
tuple, so it left sm's TOAST off, and a row was held to 65535
bytes. sm's pointer is a PostgreSQL external varlena already,
`0x01` and then a vartag of its own, `0x40`
(`../sm/sm2/VARLENA.md` §2), so digres reads it as PostgreSQL
reads its own: late, where a value is read, and not where it
is only passed along.

- **`datadir_load()` turns it on** (`sm_set_toast`), after
  attaching and before any database is opened.
- **`varatt.h` has `VARTAG_SM`**, 20 bytes after the header,
  and `VARATT_IS_EXTERNAL_SM()`: a slot, `datumCopy()` and a
  heap tuple for a sort carry the pointer, 22 bytes, as they
  carry PostgreSQL's.
- **`detoast.c` reads one** through `smstore_fetch_toast()`,
  whole: a slice is cut from the whole value, since sm fetches
  no part of one. `toast_raw_datum_size()` and
  `toast_datum_size()` take its length from the pointer
  (`smstore_toast_size()`), so `octet_length()` and
  `pg_column_size()` fetch nothing.
- **A pointer is fetched by its store alone**
  (`sm_toast_fetch`), as PostgreSQL's names its TOAST
  relation: the pointer gives the store, and a store's values
  out of line are under its TOAST relid, `0xFFFF`. No table is
  looked up. The table the pointer was read from is open in
  the transaction, which keeps its store from being dropped.
- **A write is given values, but for the row's own pointers.**
  `row_to_sm()` fetches every pointer of an insert, an
  `ON CONFLICT` insert and a load; and of an update, every one
  but the pointer the replaced row holds for that column, which
  sm keeps without writing it again. sm refuses any other
  pointer, after the row is written. To tell them apart, an
  update whose row has a pointer reads the row it replaces:
  the kept one after a write found it changed, as
  `smstore_fetch_row()` does, else as the statement sees it.
  That is one point read more for such a row.
- **An index scan's key needs nothing**: the executor detoasts
  a runtime key of a varlena type before it keys the scan
  (`ExecIndexEvalRuntimeKeys()`), and sm fetches what its
  index entries are made of.
- **A pointer is to be fetched in the statement that read it**
  (`sm_tbl_fetch`): at READ COMMITTED a later one may find the
  value gone. PostgreSQL's detoasting of a holdable cursor's
  rows, before they outlive the transaction, stays.
- **Left**: compression, and a fetch of part of a value
  (`../sm/sm2/SM2_DESIGN.md` §4.2).
  `pg_column_toast_chunk_id()` and `pg_column_compression()`
  give null for sm's pointer.
- `server.sh`: a row of 100,000 bytes is read back whole and
  in part; updates keep a pointer and copy one to another
  column; inserts copy values into another table and into
  their own; an update rolled back; an index scan keyed by a
  value out of line; `DELETE ... RETURNING`.

## Step 126: the open tables are a hash, in the tcb

The tables a transaction opened in sm were a `List` in
`TopTransactionContext`, a `thread_local`, and
`open_table()` walked it on every insert, scan, update and
lock: a transaction using a thousand tables paid a thousand
comparisons a call.

- **`open_tables` is a `std::unordered_map` by oid**, of each
  oid's handles, one a lock mode taken: an IS one and then an
  IX one for a table read and then written. `open_table()`
  finds the oid, and looks at its few handles alone; so do
  `alter_table_rel()` and `smstore_truncate()`, which close
  the handles of one table.
- **It is `tcb->smstore`** (`smstore_state_t`), as
  `CODE.md` §3 asks of a global a change touches: made by
  `tcb_init()`, freed with the tcb. It is cleared as the
  transaction ends, by commit and abort alike
  (`close_tables()`), so its memory need not be the
  transaction's context.
- **A subtransaction's end still visits every handle**:
  `smstore_subcommit()` gives its handles to the parent and
  `close_subxact_tables()` closes them, by their level. A
  list of each level's handles would make that the
  subtransaction's own, which nothing has asked for yet.
- `open_table_t` loses its oid, which is the key.

## Step 127: asserts are on

`pg_config.h` left `USE_ASSERT_CHECKING` undefined, so no
`Assert()` had run since the fork; what the modules' removal
broke of them went unseen. Every build here is a debug one, so
`dg.mk` defines it (`ASSERT ?= -DUSE_ASSERT_CHECKING`); a
release build passes `ASSERT=`.

- **Assert-only code that no longer compiled**: casts C++ asks
  for (utils/mmgr `bump.c`, `generation.c`, tcop `postgres.c`),
  and asserts of what is gone: bootstrap mode, the heap's target
  block, `IsMVCCSnapshot()`, the walsender, parallel scans,
  `PG_WAIT_LWLOCK`, buffer locks, a multixact's updater. A
  pg_proc row's argument count is `count` in a compiled-in row.
- **Deleted, of modules removed**: utils/mmgr `freepage.c`,
  `freepage.h` and `relptr.h`, which only DSA used (step 29);
  utils/cache relcache's `AssertPendingSyncs_RelationCache()`,
  which checked WAL skipping, and had no caller.
- **Made `thread_local`**: the globals assert builds add, as
  `DEBUG_NODE_TESTS_ENABLED` turns on:
  `restore_location_fields`, three `Debug_*` settings,
  fd.c's `temporary_files_allowed` and inval.c's
  `recursion_depth`, all LOCAL.
- **What they caught**, each a fault in digres:
  - `AttachSharedMemoryStructs()` (storage/ipc `ipci.c`) worked
    out the fast-path lock groups again in each session thread,
    over the postmaster's SHARED value. It does not now.
  - `session_thread_end()` (utils/init) deletes the thread's
    `TopMemoryContext`, which no process does: it clears the
    global first.
  - An sm table's NOT NULL column was left of unknown
    nullability (utils/cache `relcache.c`), so the planner never
    took it as NOT NULL. sm enforces it: it is valid.
  - A catalog relation's descriptor
    (`RelationBuildCatalogDesc()`) never set `relisshared`: `pg_database`'s lock was taken
    under no database and looked for under this one, so
    `CheckRelationLockedByMe()` answered wrong.
  - A catalog relation's columns had no NOT NULL, which a
    syscache key must have. `gen_cat.py` now decides each as
    PostgreSQL's bootstrap does, by `FORCE NOT NULL`,
    `FORCE NULL`, or a fixed width after columns fixed and NOT
    NULL; `cat_column_t` has `notnull`.

## Step 128: the statement tests run in a server

`test/run.sh` ran each `test/*.sql` through `digres --parse
--run`, which plans and executes outside any transaction. With
asserts on, the planner refuses: it needs an active snapshot,
which needs a transaction, which needs a PGPROC and the lock
manager the tool does not set up.

- **`run.sh` starts a server**, as `server.sh` does, and runs
  each file in a session with psql, stopping at its first
  error, then in 8 sessions at once; each must print what is
  expected. A FATAL's check is 8 sessions with `exit_on_error`
  set, each printing the FATAL, then the server answering
  another.
- **The expected files are psql's** now, not DestDebug's: the
  types' details went, and every row and error was compared
  with the old files' before they were replaced.
- **Gone from `digres --parse`**: `--run`, `--threads` and
  `-c`, which only `run.sh` used; with them DestDebug's
  `debug_out` (access/common `printtup.c`) and
  `session_thread_start()`'s exit code, which only the tool's
  threads gave. `--plan` and `--analyze` stay, though no test
  uses them, and `--plan` fails the planner's assert as `--run`
  did.

## Step 129: a session's options, parsed without getopt()'s globals

A session's startup options, `PGOPTIONS` for one, are parsed
by `process_postgres_switches()` (tcop `postgres.c`) with
`getopt()`, whose state, `optind`, `optarg` and `opterr`, is
libc's: one for the process, and no `thread_local` reaches it.
Sessions starting at once raced on it; the thread sanitizer
found it when step 128's FATAL check started 8 with options.

- **`pg_getopt_ctx`** (port `pg_getopt_ctx.c`,
  `port/pg_getopt_ctx.h`) is PostgreSQL's re-entrant getopt,
  from its master branch, after the 18 release digres forked
  from: the state is a struct the caller keeps. The function
  keeps one on its stack, and no longer resets libc's
  `optind`. C++ wants one cast in it, for `EMSG`.
- `get_stats_option_name()` read the global `optarg` where it
  meant its argument, which every caller passed: it reads the
  argument.
- The postmaster's and initdb's `getopt()` run before any
  session thread, and keep libc's.

## Step 130: the sinval queue's flags are atomics, in the pcb

`SHMEM.md` §5. The thread sanitizer reported a race in
storage/ipc `sinvaladt.c`: `ProcState.hasMessages`, a plain
`bool`, was set by a writer under `SInvalWriteLock`, read by
its reader with no lock and cleared under `SInvalReadLock`.
PostgreSQL relied on a byte being written whole and on the
barriers of the locks around it; in C++ it is a data race.

- **`hasMessages` and `maxMsgNum` are `std::atomic`**, every
  load and store sequentially consistent. A writer stores
  `maxMsgNum` after the messages, then sets each reader's flag;
  a reader clears its flag, then loads `maxMsgNum`. One total
  order of the four means a reader that loads the old
  `maxMsgNum` finds its flag set again, and reads the new
  messages next time; and the load acquires the messages stored
  before it.
- **`msgnumLock` is gone**, the spinlock that made `maxMsgNum`'s
  barrier.
- **The segment is `pcb->sinval`**, `calloc`'d once by the
  postmaster and never freed, as `CODE.md` §3 asks of a global
  a change touches: the `thread_local` `shmInvalBuffer` and its
  `ShmemInitStruct()` are gone. A session thread's
  `SharedInvalShmemInit()` finds it made.
- `make tsan` passes.

## Step 131: the dead code of the shared-memory machinery

`SHMEM.md` §6: what is left of modules already removed, taken
out before the rest of that file's work changes what it
touches. Each went with nothing calling it.

- **Condition variables** (storage/lmgr
  `condition_variable.c`, `condition_variable.h`): nothing slept
  on one or signalled one since parallel query and the
  checkpointer went (steps 10, 14); only
  `ConditionVariableCancelSleep()` was called, by `ProcKill()`,
  `AuxiliaryProcKill()` and the two transaction aborts. PGPROC's
  `cvWaitLink` went with them.
- **Eleven named LWLocks no one took**: TablespaceCreate,
  BtreeVacuum, AddinShmemInit, SyncRep,
  DynamicSharedMemoryControl, ReplicationSlotAllocation,
  ReplicationSlotControl, ReplicationOrigin, LogicalRepWorker,
  WaitEventCustom and WALSummarizer, of tablespaces, vacuum,
  DSM, replication, pgstat and WAL. Each leaves its number in
  `lwlocklist.h` as a comment, as PostgreSQL's removed ones do,
  and goes from `wait_event_names.txt`. So do two tranches,
  ReplicationOriginState and ReplicationSlotIO, and
  `GetLWLockIdentifier()` (storage/lmgr `lwlock.c`), which
  nothing called. XactTruncation stays, `pg_xact_status()`'s,
  and InjectionPoint, a build option's.
- **Recovery conflicts**, of hot standby, which went with WAL
  (step 12): the seven `PROCSIG_RECOVERY_CONFLICT_*` reasons,
  which nothing sent, and their handling in tcop `postgres.c`
  (`HandleRecoveryConflictInterrupt()`,
  `ProcessRecoveryConflictInterrupts()`,
  `errdetail_recovery_conflict()`); PGPROC's
  `recoveryConflictPending`, never set, and
  `errdetail_abort()`, which only read it; and the proc header's
  `startupBufferPinWaitBufId` with its getter.
- **Auxiliary, walsender and prepared-transaction PGPROCs**,
  of processes gone in steps 14 to 17 and 23, and of two-phase
  commit (step 47). `NUM_AUXILIARY_PROCS` reserved six PGPROCs,
  sinval and ProcSignal slots for the background writer, the
  checkpointer and the rest; `InitAuxiliaryProcess()`,
  `AuxiliaryProcKill()` and `AuxiliaryPidGetProc()` served them,
  and nothing called the first. `MaxBackends` is
  `max_connections`, so the walsender free list was always
  empty, and the prepared-transaction PGPROCs began past the
  array's end. They go with `walwriterProc`, the walsender
  child flag (storage/ipc `pmsignal.c`) and the postmaster's
  test of it, and the auxiliary process's resource owner
  (utils/resowner). Every PGPROC is a session's now.
- **The proc array's functions no one called** (storage/ipc
  `procarray.c`), of vacuum's horizons, replication slots and
  the counting of connections: `TransactionIdIsActive()`,
  `GetOldestNonRemovableTransactionId()`,
  `GetOldestTransactionIdConsideredRunning()`,
  `GetReplicationHorizons()`, the `GlobalVisTestIs*` and
  `GlobalVisCheck*` tests, `ProcNumberGetTransactionIds()`,
  `BackendXidGetPid()`, `IsBackendPid()`, `CountDBBackends()`,
  `CountDBConnections()`, `CountUserBackends()` and the
  replication slots' xmin setter and getter. What still has a
  caller stays, though no xid is ever assigned (step 45): the
  group XID clearing, the horizons `GlobalVisTestFor()` and
  `GetSnapshotData()` compute, the slots' xmin fields they
  read. They go with the constant snapshot (`SHMEM.md` §7).
- **Left on purpose**: what still has a caller, though it does
  nothing in digres: lock groups, which the lock manager and
  the deadlock detector test throughout, and the lock tags the
  executor still takes, both to go with the lock manager; and
  the extension hooks `shmem_request_hook`, `shmem_startup_hook`
  and `RequestNamedLWLockTranche()`, which a library in
  `shared_preload_libraries` may use, and digres still loads
  libraries (`SHMEM.md` §6).

## Step 132: a lock or two of PostgreSQL's is a C++ mutex

`SHMEM.md` §3. Three locks of the five the survey named for
this are `std::mutex` now, in the pcb.

- **`held_lock_t`** (`storage/held_lock.h`) holds a
  `std::mutex` or `std::shared_mutex` as an LWLock is held:
  interrupts held off while it is, so no cancel taken at a
  `CHECK_FOR_INTERRUPTS()` inside the section, which
  `errfinish()` runs after a WARNING or a LOG, ends it midway;
  and let go, with them, as its scope ends. An error's unwinding
  lets the lock go and leaves the holdoff count, which
  `errfinish()` has already set to 0, as `LWLockReleaseAll()`
  does. A FATAL runs the exit callbacks before it unwinds, so a
  lock an exit callback takes must be let go before one.
- **AutoFile** is `pcb->auto_file_mutex`, held by a
  `held_lock_t` across `AlterSystemSetConfigFile()` (utils/misc
  `guc.c`). `server.sh` runs ALTER SYSTEM in 8 sessions at
  once: all 8 settings are kept. Without the lock it loses
  some, every time.
- **ProcStructLock**, a spinlock, is `pcb->proc_struct_mutex`,
  under `std::lock_guard`: its sections raise nothing.
  `InitProcess()` (storage/lmgr `proc.c`) takes a PGPROC under
  it and raises "too many clients" after.
- **ProcSignal's `pss_mutex`**, a spinlock a slot, is a
  `std::mutex` in each slot, each section a `std::unique_lock`.
  The slots are `pcb->procsignal`, `new`'d once by the
  postmaster, as `CODE.md` §3 asks of a global a change
  touches; the `thread_local` `ProcSignalSlots`, its
  `ShmemInitStruct()` and `ProcSignalShmemSize()` are gone.
- **Not converted**: XidGen and OidGen, as transaction ids and
  OIDs are sm's and PostgreSQL's counters of them are to go;
  RelationMapping and RelCacheInit, which guard the relation
  mapper's files and the relcache's init files, never read
  (`SHMEM.md` §3).

## Step 133: no spinlock is taken

Spinlocks were how processes sharing a segment guarded a few
words of it; threads of one process have `std::mutex`, whose
waiter sleeps rather than spins. Step 132 took ProcStructLock
and `pss_mutex`; these are the rest:

- **The fast path's strong-lock counts** (storage/lmgr
  `lock.c`) are a `std::atomic<uint32>` each, with no lock: the
  spinlock guarded only them, each on its own, and the fast
  path read them under another lock (`fpInfoLock`), which no
  mutex of theirs would have covered. Their structure is
  `pcb->fast_path_strong`, `new`'d once, and its
  `ShmemInitStruct()` is gone.
- **The latch's `owner_lock`** (storage/ipc `latch.c`) is a
  `std::mutex`, each section a `std::lock_guard`. A thread's
  `LocalLatchData` makes its own; `InitSharedLatch()` makes a
  PGPROC's in place, the PGPROCs being `calloc`'d. No signal
  handler takes it: one sets only its own thread's latch.
- **The dynahash free lists** of a partitioned table
  (utils/hash `dynahash.c`), the lock manager's, are a
  `std::mutex` each, made in place in the `calloc`'d header,
  and locked and unlocked where the spinlocks were: the
  locking depends on `IS_PARTITIONED()`, and no section raises.
- **Left**: port `atomics.c`'s emulation of 64-bit atomics by a
  spinlock, compiled out where a platform has them, as both of
  ours do; and `s_lock.c` with it.

## Step 134: the soft limit on open files is raised to the hard

Every session is a thread of one process, so the sessions'
files, RocksDB's and the clients' sockets count against one
`RLIMIT_NOFILE`. PostgreSQL gave each process its own; here a
service started with systemd's default soft limit, 1024, would
run out after a dozen busy sessions, and fail wherever an
`open()` or `accept()` landed rather than in fd.c's LRU.

- **At start, the soft limit is raised to the hard one**
  (storage/file `fd.c` `raise_open_files_limit()`, first in
  `set_max_safe_fds()`, which the postmaster and a standalone
  backend each run once). A failure is a WARNING; the server
  starts on what it has.
- **MySQL does the same**, one thread a connection too: it
  asks `setrlimit()` for what `open_files_limit`,
  `max_connections` and `table_open_cache` need, and shrinks
  the last two to what it got. Its open tables are one LRU for
  every thread, and each engine has a cap of its own
  (`innodb_open_files`, MyRocks's `rocksdb_max_open_files`).
- **`test/server.sh`** starts the server with the soft limit at
  1024 and checks `/proc/<pid>/limits` for soft equal to hard.
- **Left on purpose**: fd.c's budget is still per thread, each
  thinking it has `max_safe_fds`, and sm leaves RocksDB's
  `max_open_files` at -1, every SST file open. A budget for the
  process in the pcb, and a share for RocksDB, wait for a
  workload that comes near the raised limit.
- **Not reachable**: libpq `auth.c`'s RADIUS client calls
  `select()`, which a socket past `FD_SETSIZE` would overflow;
  authentication is peer or SCRAM, so it never runs.

## Step 135: fd.c's virtual fds are sm0's, under one ceiling

The sessions are threads of one process, but storage/file
`fd.c` gave each the budget a process had, `max_safe_fds`:
together they could open far more than the limit holds, and
fail wherever an `open()` landed. Its LRU of real fds is now
sm0's (`sm/sm0/VFD.md`, `sm_vfd.h`): each thread has a cache
of its own, `tcb->vfd`, and every real fd any cache holds
counts against one ceiling for the process.

- **The ceiling** is measured once, in `set_max_safe_fds()`:
  the raised limit, less the fds open, less
  `NUM_RESERVED_FDS`. sm0 lets it be set while fds are held,
  counting them inside it: the postmaster holds its wake fd
  before it gets there.
- **A thread's share**, `max_safe_fds`, is the ceiling over
  `max_connections` + 1 threads, at most
  `max_files_per_process`, at least `FD_MINFREE`: the ceiling,
  not the share, keeps them within the limit. A cache is made
  at its thread's first use; the postmaster's comes before
  `set_max_safe_fds()`, and keeps `FD_MINFREE`.
- **What fd.c keeps** is a table of `File`s over sm0's virtual
  fds: the resource owner and transaction that hold a file,
  temporary files deleted at close and counted against
  `temp_file_limit`, and how an error is reported. The LRU
  ring, `nfile`, `numExternalFDs` and `count_usable_fds()` go.
- **Real fds are counted too**: `OpenTransientFile()` is
  `sm_vfd_open_real()`; `AllocateFile()`, `AllocateDir()` and
  `OpenPipeStream()` reserve their fd first;
  `AcquireExternalFD()` reserves, and may be refused;
  `ReserveExternalFD()` counts an fd already open, past the
  share if it must. `BasicOpenFile()` goes: guc.c's ALTER
  SYSTEM writes its temporary file through
  `OpenTransientFile()`.
- **Lost on purpose**: an `fopen()` or `opendir()` that fails
  with `EMFILE` is not retried after closing a VFD (sm0's
  opens are); a close that fails on an eviction is not
  reported, where PostgreSQL might PANIC; macOS's `F_NOCACHE`
  for `PG_O_DIRECT` is an `#error`.
- **`test/server.sh`** starts the server with
  `max_files_per_process` at 64, and opens 100 cursors in one
  transaction, each over a function scan spilled to a
  temporary file, then reads from each again. With sm0's
  eviction disabled it fails at the 61st.
- **`sm/sm0/test/test_vfd.cc`**: a ceiling set while fds are
  held counts them, measured or given.

## Step 136: a latch is an interrupt bit

`SHMEM.md` §4. A latch was a flag in shared memory, with one
owner, that other processes set to wake it; storage/ipc
`latch.c` kept that shape over `sm_sys.h`'s wake: `is_set` and
`maybe_sleeping`, ordered by `pg_memory_barrier()`s placed by
hand, and the owner's pid and thread id. In one process a latch
is always a thread's, and `sm_sys.h` already wakes a thread
without losing the wake: a bit raised, then its wake fd written.

- **A latch is the owning thread's `INTERRUPT_LATCH`**
  (storage `procsignal.h`), outside `INTERRUPT_ALL`: no handler
  runs for it, `DeliverInterrupts()` does not take it, and no
  wait of sm's ends on it. `SetLatch()` raises it in the owner,
  `ResetLatch()` takes it, `LatchIsSet()` loads it, with
  acquire, so what a setter wrote before its set is seen after.
- **`SetLatch(MyLatch)` takes no lock**: it is
  `sm_interrupt_self()`, so a signal handler, which sets only
  `MyLatch`, may call it. Any other latch's owner is copied
  under its lock, as before, and interrupted.
- **`Latch` keeps only its owner**, for others to find the
  thread by: `InterruptProc()` (storage/ipc `procsignal.c`) as
  well. `is_set`, `maybe_sleeping`, `owner_pid`, `owner_tid`,
  `LATCH_GET()`/`LATCH_SET()` and the barriers are gone, and
  `WakeupMyProc()` and `WakeupOtherProc()`.
- **`WaitEventSetWait()`** (storage/ipc `waiteventset.c`) asks
  `LatchIsSet()` before it sleeps and after its wake fd is
  read: a set after the first look writes the fd, so the sleep
  ends. No announcement of the sleep is needed.
- **A wait is refused on another thread's latch.** The check
  compared the owner's pid with `MyProcPid`, which every thread
  of the process shares, so it refused nothing; `LatchIsMine()`
  compares the thread.
- `server.sh` catches a latch that does not wake: with
  `SetLatch()` on another thread's latch made a no-op, the
  postmaster, which a session's end sets, does not shut down.

## Step 137: PostgreSQL's xid and OID counters go, and the files

`SHMEM.md` §7, step 1. Transaction ids and oids are sm's; what
PostgreSQL kept of its own, and the files it read at start,
had no use left but their locks.

- **The relcache's init files**: utils/cache `relcache.c`'s
  `RelationCacheInitializePhase2()` and `Phase3()`, which load
  them, had no caller; they go, with `formrdesc()`,
  `load_critical_index()`, the readers, writers and removers of
  the files, inval.c's tracking of when to remove them, and
  RelCacheInit. `criticalRelcachesBuilt` and
  `criticalSharedRelcachesBuilt`, which only Phase3 set, were
  always false; they go, and each reader keeps its false branch:
  a catalog is scanned by heap. inval.c's
  `xactGetCommittedInvalidationMessages()` and
  `ProcessCommittedInvalidationMessages()`, for two-phase commit
  and a standby, had no caller either.
- **The relation mapper**: utils/cache `relmapper.c` goes, with
  RelationMapping, its sinval message and its wait events. No
  relation is mapped: every relcache entry has a relfilenode,
  and the maps it kept were empty.
- **The counters**: access/transam `varsup.c` keeps only the
  struct `GetSnapshotData()` reads, `oldestXid`,
  `latestCompletedXid` and `xactCompletionCount`, constant until
  the snapshot is (§7, step 3). `nextXid`, `nextOid`, the wrap
  limits, `ReadNextFullTransactionId()`, `GetNewObjectId()` and
  XidGen, OidGen and XactTruncation go. `ProcArrayAdd()` and
  `Remove()` hold ProcArrayLock alone.
- **Refused**: catalog `catalog.c` `GetNewOidWithIndex()` and
  `GetNewRelFileNumber()`, before the oid they would draw, as
  catalog `indexing.c` refuses the row; `pg_nextoid()` and
  `pg_stop_making_pinned_objects()` with them.
  `pg_current_snapshot()`, `pg_xact_status()` and `age(xid)`
  fail as `txid_current()` does: digres assigns no transaction
  ids. Left for the constant snapshot: `pg_export_snapshot()`
  and SET TRANSACTION SNAPSHOT.
- **`test/server.sh`** checks each refusal, and a statement
  after them. With `pg_xact_status()` returning NULL instead,
  it fails.

## Step 138: LOCK TABLE ... NOWAIT does not wait

`SHMEM.md` §5.1's first hole. `ConditionalLockRelationOid()` took
no lock in sm and said it had, and LOCK TABLE ... NOWAIT, meant
to be refused as not supported, waited instead: the permission
callback that `RangeVarGetRelidExtended()` runs before its lock
reads the table's definition, and building a descriptor opens
the table in sm IS (`smstore_table_desc()`), which waits out a
DDL's or a LOCK's X.

- **sm opens a table without waiting**: `sm_tbl_tryopen()`
  (`sm.h`), over sm3's `tbl_tryopen()` and `cat_trylock_table()`,
  which take the table-oid lock with `lock_table_now()`, as
  `add_column`'s NOT NULL case does (step 110): 0 with the table
  open, 1 with nothing taken if another transaction holds the
  lock in a mode that conflicts.
- **`ConditionalLockRelationOid()` takes sm's lock if it is free
  now** (`smstore_trylock_relation()`), in the mode
  `smstore_lock_relation()` would take.
- **A table in sm asked for with RVR_NOWAIT or RVR_SKIP_LOCKED
  is locked before the callback** (catalog `namespace.c`), so
  the callback's descriptor finds the table open and waits for
  nothing. The report of a lock not had is one function,
  `lock_not_available()`, for both places.
- **LOCK TABLE ... NOWAIT takes the LOCK's own mode without
  waiting** (`smstore_lock_table()`): SHARE is S, which another
  session's IS lets through and its IX does not. It fails as
  PostgreSQL's does: `could not obtain lock on relation`, 55P03.
- Not changed: ANALYZE on a table in sm goes to
  `smstore_analyze()` (commands `vacuum.c`), which reads no
  SKIP_LOCKED, so `ANALYZE (SKIP_LOCKED)` still waits.
- sm3 `test_lock.cc`: a try-open of a table held X, or held IS
  for X, returns 1 and takes nothing; once free it opens.
- `server.sh`: under SHARE, ACCESS SHARE NOWAIT is granted and
  ROW EXCLUSIVE NOWAIT fails, 55P03; under ACCESS EXCLUSIVE,
  ACCESS SHARE NOWAIT fails at once; free, NOWAIT is granted.

## Step 139: ANALYZE (SKIP_LOCKED) passes over a held table

Step 138 found it: `ExecVacuum()` (commands `vacuum.c`) read
SKIP_LOCKED and dropped it, as "nothing to skip: a reader's
lock". A reader's IS still waits out an X, a DDL's or a LOCK's,
so `ANALYZE (SKIP_LOCKED) t` waited for it.

- **SKIP_LOCKED reaches `analyze_table()`**, which opens the
  table without waiting (`try_open_table()`, step 138). A table
  it would wait for is passed over: with PostgreSQL's warning,
  `skipping analyze of "t" --- lock not available`, if the
  statement named it, and in silence in an ANALYZE of every
  table, as PostgreSQL's `vacuum_open_relation()` reports only
  a relation named.
- Not done: another session's ANALYZE of the same table is
  still waited for, on sm's lock of the table's statistics
  (sm3 `cat_lock_stats()`), which has no form that does not
  wait. In PostgreSQL the two would conflict on
  ShareUpdateExclusiveLock, and the second would skip.
- `server.sh`: with t2 held ACCESS EXCLUSIVE, `ANALYZE
  (SKIP_LOCKED) t2` warns and `ANALYZE (SKIP_LOCKED)` is silent,
  and neither waits.

## Step 140: ANALYZE (SKIP_LOCKED) passes over one in progress

Step 139's gap: a second session's `ANALYZE (SKIP_LOCKED) t`
waited for the first's transaction, on sm's lock of t's
statistics, which had no form that did not wait. In PostgreSQL
the two conflict on ShareUpdateExclusiveLock, and SKIP_LOCKED
skips.

- **sm analyzes without waiting**: `sm_tbl_tryanalyze()`
  (`sm.h`), over sm3's `tbl_tryanalyze()`; `analyze_table()`
  takes the statistics' lock with `cat_trylock_stats()`, over
  sm2's new `catkv_trylock()`, and returns 1 before it samples
  if another transaction holds it.
- **`analyze_table()` (access/sm `smstore.c`) uses it under
  SKIP_LOCKED**, and reports a table passed over as for a table
  held X: `skip_locked_table()`.
- **A plain ANALYZE still waits**, then analyzes again: the
  statistics are the transaction's (`STATISTICS.md` §2.1), so
  the first may abort, and rows committed after its sample
  belong in the second's. PostgreSQL does the same.
- **`waiting()` returns any answer but -1 at once.** It retried
  while the error was `SM_ERR_INTERRUPTED`, unless the call
  returned 0, so a call returning 1 after an earlier interrupt
  would have been made again without end.
- sm3 `test_lock.cc`: a try-analyze while another transaction's
  analyze is open returns 1 and records nothing; once that one
  has ended, it analyzes.
- `server.sh`: with t2 being analyzed in an open transaction,
  `ANALYZE (SKIP_LOCKED) t2` warns at once, and a plain ANALYZE
  t2 waits.

## Step 141: a lock on an index locks its table

`SHMEM.md` §5.1's second hole: a relation's descriptor used with
no lock held, which a version checked under the lock would never
see stale.

- **The audit**: `RelationIdGetRelation()` logged, for a while,
  each cached descriptor of sm's it handed out with no sm lock
  held (`smstore_relation_held()`), and digres's tests ran: 16
  uses at six call sites, each in a transaction. ALTER TABLE's
  lookup (`alter_table_rel()`) reads the relkind and whether the
  session has the table open; RENAME COLUMN's an attnum for the
  address it returns; the permission callbacks of CREATE INDEX
  and LOCK TABLE the relkind, persistence and owner. None reads
  what a DDL changes, or each reads it again under X.
- **The hole: `pg_get_indexdef()`** (adt `ruleutils.c`,
  `sm_indexdef()`) opened the index, then its table, and a lock
  on an index took nothing in sm (step 111). A session with the
  index's descriptor cached, behind a DROP INDEX in flight,
  printed the definition of an index that was gone.
- **A lock on an index locks its table** (`lock_target()`,
  access/sm `smstore.c`): an index's definition is its table's,
  in the table's record in sm3. One not yet known by its table
  (`smstore_index_table()`) takes nothing, as before.
- **DROP INDEX invalidates the index's descriptor**, as well as
  its table's, as PostgreSQL's `index_drop()` does. Through the
  descriptor: `CacheInvalidateRelcacheByRelid()` reads
  `pg_class`, which digres has not, and failed the drop.
- **`sm_indexdef()` finds an index gone** after its wait, and
  is null with `missing_ok`, as PostgreSQL's
  `pg_get_indexdef()` is.
- `server.sh`: a session with the index cached waits out a held
  DROP INDEX, then reads null.

## Step 142: an unlocked use of a descriptor of sm's is caught

Step 141's audit, made a rule: a descriptor of sm's is used
under sm's lock on its table, which a version checked under the
lock (`SHMEM.md` §5.1) needs.

- **`CheckRelationLockedByMe()` and
  `CheckRelationOidLockedByMe()`** (storage/lmgr `lmgr.c`)
  answer for a relation of sm's whether this transaction holds
  sm's lock on it, in a mode covering the one asked
  (`smstore_relation_held()`: the table's lock, an index's
  table's, and a sequence, which takes none, always). They said
  yes for any. So PostgreSQL's assertions that a relation opened
  with `NoLock` is locked (access/common `relation.c`) and that
  the executor's are (executor `execUtils.c`) hold for sm's.
  Asserts are on in every build (`dg.mk`).
- **ALTER TABLE's and RENAME COLUMN's lookups** have the
  descriptor from `RelationIdGetRelation()`, not
  `relation_open()`: they take no lock, as sm's DDL takes X
  itself and a NOT NULL column asks for it without waiting
  (step 110), and they read only the relkind, this session's
  use of the table, and an attnum for an address.
- **`pg_depend.c` asks too**, and on no lock takes one and looks
  the relation up in `pg_class`, which fails in digres: it comes
  only before a write to `pg_depend`, refused anyway, so only
  the error said differs. Not changed.
- digres's tests trip no assertion.

## Step 143: sm gives a table a version

`SHMEM.md` §5.1's version, the sm half; dg does not read it yet.

- **A table's version is a count of publishes**, which sm3
  already kept (`catalog_t::seq`) and dated an index's becoming
  valid by. `publish()` now also records the count at which each
  table's definition (`table_seq`) and statistics (`stats_seq`)
  were last published, and at which a namespace was last made or
  dropped (`nsp_seq`).
- **`cat_table_version()`** is the greatest of the table's
  definition's, statistics' and indexes' counts, so an ANALYZE
  or an index made valid, which today send a relcache message so
  plans are made again, move it too. It is `table_version_own`
  while the session's own transaction has changed the definition
  or the statistics. A commit timestamp would have needed sm1's
  plumbed into sm3, for nothing a count does not give; one count
  for every table would have made any DDL stale every session's
  every cache.
- **`sm_table_def_t::version`** carries it (`sm.h`), filled
  wherever sm builds one (`api_def()`), so the definition a
  session reads when it takes sm's lock has it at no extra call.
  `SM_VERSION_OWN` is `table_version_own`.
- **`sm_cat_namespace_epoch()`**, over `cat_namespace_epoch()`.
- Counts are in memory, as `seq` is: after a restart every table
  is at 0, and so is every host's cache.
- `../sm/DESIGN.md` §5 has it as a decision across levels: a
  host's caches are kept fresh by a version, not by a message.
- sm3 `test_table.cc`: a rename moves the version, own until
  committed; another table's DDL and inserts do not; an analyze
  does, own until committed; an index's creation does, and its
  becoming valid again; the epoch moves only with a namespace.

## Step 144: a relid's memtable bytes are counted

`server.sh`'s check that an index has bytes failed in about one
server run in six, on main as well: `pg_relation_size()` of a
primary key over 1,000 fresh rows read 0. The same rows read from
0 to 198,000 bytes from run to run while unflushed, and 23,618
after a restart, which flushed them.

- **The cause is RocksDB's estimate of a range within a
  memtable.** A table and its indexes are one store, one column
  family, each under a relid; `GetApproximateMemTableStats()`
  samples the skip list, whose shape is random per process, so a
  relid's range among others reads anything from 0 to the whole
  memtable. The planner sizes a table by it at every plan
  (`smstore_estimate_size()`), and ANALYZE records it.
- **sm1 counts what it writes** (`sm1/src/memcount.h`): each
  write's version key and value, by the key's first two bytes, at
  the four places a store is written (a version's put, a
  rollback's delete, a load's batch, GC's range delete). A count
  is kept for the active memtable and each sealed one; a RocksDB
  listener on each instance, the temporary one included, starts a
  count when a memtable is sealed and drops those a flush wrote
  out. A read counts only as many sealed memtables as RocksDB
  says are unflushed, since a waiting `Flush()` returns before
  the listener hears of it.
- **`sm1::approxsize()` answers a range of whole 2-byte prefixes
  from the counts**, which is every range sm2 asks for; any other
  range keeps RocksDB's estimate. The bytes in files are still
  estimated.
- Counts start at 0 when a database opens: recovery flushes what
  the WAL held.
- 1,000 rows of `(int primary key, text of 100)` now read 140,000
  bytes for the heap and 39,000 for the index in every run.
- sm1 `test_stats.cc`: each prefix's memtable bytes are exactly
  its rows' versions, 0 after a flush, and only the new rows'
  after more. `../sm/sm1/API.md`, `sm1.h`, `../sm/STATISTICS.md`
  §3.1.

## Step 145: another session's DDL is learned from sm, not sent

`SHMEM.md` §5.1, built: no session sends an invalidation message
to another, or reads one. sinval's code stays one step more.

- **A descriptor of sm's records sm's version** of the table's
  definition it was built from (`rd_smversion`, utils `rel.h`;
  `smstore_table_t::version`), a table's and its indexes' alike.
- **sm's lock checks it** (`smstore_lock_relation()` and its try
  form, access/sm `smstore.c`): under the lock, the table's
  version is the handle's, and `RelationCheckSmVersion()` (utils
  `relcache.c`) invalidates, as a message received would, each
  cached entry of the table or its indexes built from another.
  Their invalidations bump `SharedInvalidMessageCounter`, so
  `RangeVarGetRelidExtended()` looks the name up again after a
  wait, as it did after messages.
- **A schema is checked by sm's namespaces' epoch**
  (`smstore_namespace_epoch_moved()`):
  `AcceptInvalidationMessages()` compares it with the one the
  session last saw, kept in its `smstore_state_t`, in place of
  reading the queue, and on a change flushes `pg_namespace`'s
  cache, which drops the search path.
- **`SendSharedInvalidMessages()` sends nothing.** A session's
  own DDL still invalidates its own caches as it goes (inval.c's
  transaction lists).
- `server.sh`: a held session reads a table, another adds a
  column; then a schema made by another session after the held
  one set its search path to it. With the version check made to
  do nothing, these and four checks already there fail ("ALTER
  TABLE seen by another session" among them); with the epoch's,
  the schema's and "search path finds a schema made later".

## Step 146: sinval is gone

`SHMEM.md` §7 step 2, done. Since step 145 no session sent or
read a message; the queue that carried them goes.

- **Deleted**: storage/ipc `sinval.c` and `sinvaladt.c`, and
  `storage/sinvaladt.h`: the ring, the per-session read state,
  the reset and catchup logic, `pcb->sinval`.
- **The catchup interrupt goes**: `PROCSIG_CATCHUP_INTERRUPT`
  (storage/ipc `procsignal.c`), its handler, and its processing
  at a client read (tcop `postgres.c`). ProcSignal has one
  reason left.
- **The SInvalRead and SInvalWrite LWLocks go**, their slots
  left as gaps (`storage/lwlocklist.h`), with their wait events.
- **inval.c sends nothing**: `PreCommit_Inval()`, step 111's
  early send, goes with its call in xact.c; `AtEOXact_Inval()`
  at commit and `AtInplace_Inval()` have nothing to do;
  `CacheInvalidateSmgr()`, which only sent and had no caller,
  goes. `SharedInvalidMessageCounter` is defined in inval.c.
  `storage/sinval.h` keeps the messages, which a session still
  processes for itself.
- **A local transaction id comes from one counter for the
  process** (`pcb_t::next_lxid`, xact.c): PostgreSQL kept each
  PGPROC slot's next one in the queue's state, so a slot's next
  user went on from it and reused no virtual xid soon. One
  counter reuses none until it wraps.

## Step 147: ProcSignal is gone

`SHMEM.md` §5, its ProcSignal: a slot a session, by ProcNumber,
holding a pid, a cancel key and a flag for each reason, read by
one interrupt bit's handler. With sinval's catchup gone (step
146) one reason was left.

- **`pg_log_backend_memory_contexts()` raises an interrupt bit
  of its own**, `INTERRUPT_LOG_MEMORY`, the bit that was
  `INTERRUPT_PROCSIG`, on the session's PGPROC (adt
  `mcxtfuncs.c`); its handler is
  `HandleLogMemoryContextInterrupt()`, which sets the latch as
  the multiplexer did. SIGUSR1, which nothing sends a session,
  maps to no bit.
- **A session's cancel key is in its PGPROC**
  (`cancelKey`, `cancelKeyLength`), set by `ProcSetCancelKey()`
  from `InitPostgres()` and cleared with the pid by `ProcKill()`,
  both under `pcb->cancel_key_mutex`; `SendCancelRequest()`
  matches a cancel request's pid and key there.
- **The slots go**: `ProcSignalSlot`, `pcb->procsignal`,
  `ProcSignalShmemInit()`, `ProcSignalInit()`,
  `SendProcSignal()`, `procsignal_sigusr1_handler()` and the
  `ProcSignalReason` enum.
- `server.sh`: a session asked by another to log its memory
  contexts logs them; the cancel request's check was there.

## Step 148: PMSignalState is gone

`SHMEM.md` §5, its PMSignalState: a quit reason, and a flag for
each child slot, set by the child once it took a PGPROC and
cleared by its exit callback, which the postmaster read when the
child ended to tell one that had not cleaned up after itself.

- **The quit reason is `pcb->quit_reason`**, an atomic, set by
  the postmaster before it sends SIGQUIT and read by a session
  quitting (`SetQuitSignalReason()`, `GetQuitSignalReason()`),
  and cleared when the postmaster rebuilds shared state after a
  crash, as PMSignalState's rebuild cleared it.
- **The child flags go, with nothing in their place.** In
  PostgreSQL a backend could die without its exit callbacks, to
  SIGKILL, the OOM killer or a segfault, and leave shared memory
  half changed. A session thread cannot: whatever skips its
  callbacks (a segfault, `abort()`, an uncaught exception,
  `exit()`, a signal to the process) ends the process, the
  postmaster with it. `CleanupBackend()` tells a crash by the
  exit status alone. `RegisterPostmasterChildActive()`,
  `MarkPostmasterChildSlotAssigned()` and `Unassigned()` and
  `PMSignalShmemInit()` go; `ReleasePostmasterChildSlot()`
  reports nothing. Two SHARED globals go with them.
- **Not changed, and noted**: an immediate shutdown (SIGQUIT)
  ends the process before a session tells its client why, so
  "terminating connection due to immediate shutdown command"
  reaches no client; it did not before this step either.

## Step 149: a snapshot is neither exported nor imported

Found by the audit before `SHMEM.md` §7 step 3, the constant
snapshot. `pg_export_snapshot()` wrote out PostgreSQL's snapshot,
and `SET TRANSACTION SNAPSHOT` read one in, so a second session
would see what the first saw. What a session sees in digres is
sm's snapshot, which neither carries: the importing session read
at its own, and `pg_dump -j`, which dumps in parallel at one
exported snapshot, would have dumped each table at a moment of
its own, with no error.

- **Both are refused**, "not supported yet" (utils/time
  `snapmgr.c`). Sharing a snapshot needs sm to hand one of its
  own to another session.
- `server.sh` checks both.

## Step 150: a snapshot is a constant

`SHMEM.md` §7 step 3, its first half. `GetSnapshotData()` took
`ProcArrayLock` and scanned every session's PGPROC for the xid
it was running, for every snapshot, though no PostgreSQL xid is
ever assigned (steps 45, 137): the list was always empty and the
snapshot always the same.

- **The audit**: nothing live reads a snapshot's xids. sm's
  table access method keeps the snapshot it is given and reads
  at sm's; the catalog relations make their rows with no
  snapshot. The readers of `xip` and `xmin` are CREATE INDEX
  CONCURRENTLY and DETACH PARTITION CONCURRENTLY, refused or
  with no partition to detach; `pg_current_snapshot()`, which
  fails (step 137); partitions, enums and logical decoding,
  which digres has none of; and `XactLockTableWait()`, given
  only a heap's xids. The planner's `get_actual_variable_range()`
  takes a horizon, and skips every index of sm's. What is read:
  the command id each snapshot carries, and `RecentXmin`, which
  `index_getnext_tid()` asserts. Step 149 came of it.
- **`GetSnapshotData()` is a constant** (storage/ipc
  `procarray.c`): xmin and xmax the xid after the last completed,
  which `TransamVariables` fixes before the first session, an
  empty list, the caller's command id, and `TransactionXmin`,
  `RecentXmin` and `MyProc->xmin` as before. No lock, no scan, no
  horizon advanced. `GetSnapshotDataReuse()`, its one helper,
  goes.
- **`TransactionIdIsInProgress()` asks only of this session's own
  transaction**, as no other holds an xid.
- Still there, as nothing feeds them now: the horizons, group
  xid clearing, the replication slots' xmins and the `XidCache`
  (`SHMEM.md` §6). `ProcArrayLock` is taken still at a
  transaction's start and end and by the lookups by pid.

## Step 151: what fed the snapshot's scan goes

With the snapshot a constant (step 150), what only the scan
used, or computed for heap pruning and VACUUM, which do not run,
goes from storage/ipc `procarray.c`, 1,721 lines to 903.

- **Group XID clearing**: `ProcArrayGroupClearXid()`,
  `ProcArrayEndTransactionInternal()`, and the PGPROC and
  ProcGlobal fields for them. `ProcArrayEndTransaction()`
  asserts the transaction had no xid, as none is assigned.
- **The horizons**: `ComputeXidHorizons()`, `GlobalVisTestFor()`
  and `GlobalVisHorizonKindForRel()`, the `GlobalVisUpdate`
  functions, the `GlobalVis*Rels` state and its types. The
  planner's `get_actual_variable_range()` gives its
  non-vacuumable snapshot no horizon: it is reached only for a
  catalog's index, and there is none.
- **The replication slots' xmins**, which nothing set.
- **`CountOtherDBBackends()`**, which had no caller.
- Left for the session registry: the `XidCache`, its debug
  counters, and the dense arrays `ProcArrayAdd()` and
  `ProcArrayRemove()` keep.

## Step 152: a session is found by its record

`SHMEM.md` §5.2. The proc array found a session by scanning
PGPROC slots under `ProcArrayLock`, and a reader rechecked the
slot, as a slot and a pid are both reused.

- **A session has a record** (`storage/session_registry.h`),
  made as it registers and owned by `std::shared_ptr`: an id
  never reused, its pid, its PGPROC, its thread, and as atomics
  its database and role, set as `InitPostgres()` learns them,
  and `ended`. The registry is the pcb's: a mutex, and the
  records by pid. A session registers where it joined the proc
  array (`ProcArrayAdd()`) and leaves where it left it; its tcb
  keeps its own record.
- **A lookup keeps the record**: `pg_signal_backend()`,
  `pg_log_backend_memory_contexts()` and DROP DATABASE FORCE
  read the role and database there and interrupt the session's
  own thread (`session_interrupt()`), not its PGPROC's latch
  owner, which is the next session's once the slot is reused.
- **`pg_blocking_pids()`** keeps the record while it reads the
  session's lock state from its PGPROC under the partition
  locks. A session releases its locks, which takes those locks,
  before it leaves the registry, and its PGPROC is another's
  only after: so a record not ended names its own PGPROC, and
  one ended holds and awaits nothing.
- **`ProcArrayLock` goes**, a gap left in `lwlocklist.h`, and
  its wait event; with it the array of PGPROC numbers, the
  dense arrays `ProcGlobal->xids`, `subxidStates` and
  `statusFlags`, each PGPROC's `pgxactoff`, the `XidCache` and
  its debug counters, and `BackendPidGetProcWithLock()`.
  `MaintainLatestCompletedXid()` goes, as no xid completes.
  DROP DATABASE FORCE no longer counts prepared transactions,
  which digres does not have.
- `server.sh`: a session waiting on another's advisory lock is
  named blocked by it in `pg_blocking_pids()`, and
  `pg_terminate_backend()` ends it.

## Step 153: advisory locks are digres's own

`SHMEM.md` §3, §7 step 4. Advisory locks were the lock
manager's user lock method. They guard nothing stored, so
they do not go to sm (`PLAN.md` §7): sm1's locks end with
their transaction, and session scope, explicit unlock and
counted holds are PostgreSQL's to keep.

- **A table of digres's own** (`storage/advisory_lock.h`,
  storage/lmgr `advisory_lock.c`), the pcb's: a mutex, the
  holders of each key, what each waiting session waits for,
  and a condition variable the waiters sleep on in
  `sm_wait()`, so an interrupt ends the wait. A key is the
  locktag's fields: the database, two words, and the form.
  A holder is a session's record, with its session-scope
  holds counted by mode and the nest level of each
  transaction-scope one. A session's tcb lists the keys it
  holds, so a transaction's end that holds none takes no
  mutex.
- **What PostgreSQL did is kept**: a session's own holds
  never conflict; share conflicts with exclusive; an unlock of
  a hold not had is false with PostgreSQL's warning;
  `pg_advisory_unlock_all()` keeps transaction-scope holds; a
  savepoint rolled back drops the holds taken in it, and one
  released hands them to its parent; a session's end and
  DISCARD ALL drop all. `lock_timeout` is the wait's deadline.
- **A deadlock among advisory waits** is found as a session
  starts to wait, by following the holders it waits on and
  what they wait on; it fails with `deadlock detected`. One
  through an sm lock is not seen, and `lock_timeout` or a
  cancel ends it: `../sm/NEXT.md` §3 says how sm1 could find it.
- **Not kept**: the wait queue. A request waits only for
  conflicting holders, so a stream of share holds can keep an
  exclusive request waiting. Advisory locks are not in
  `pg_locks`; `pg_blocking_pids()` reads their waits.
- **Gone**: `LockReleaseSession()` and `SET_LOCKTAG_ADVISORY`,
  which nothing else used. The user lock method's table in
  `lock.c` is left, to go with the lock manager.
- `server.sh`: counted holds and the warning; the scopes and
  savepoints; a wait ended by `lock_timeout` and by a cancel;
  a deadlock between two sessions, of which the second to
  wait fails.
- `server.sh`'s check of step 152 raced: the waiting session
  could take advisory lock 42 before the blocker, and the check
  then passed with no pids and terminated the holder, an idle
  session whose psql never noticed. The second session now asks
  only once the blocker says it holds the lock.

## Step 154: a catalog takes no lock

`SHMEM.md` §3, §7 step 4. A relation not sm's is a catalog,
compiled in, and every write to one is refused
(`catalog/indexing.c`): no lock on it guards anything. Step 48
made the object locks no-ops for the same reason; this does
the rest.

- **A catalog relation's lock is nothing** (`lmgr.c`):
  `LockRelationOid()`, `LockRelationId()`, `LockRelation()`
  and the conditional form take sm's lock for a table of sm's
  and none for a catalog; each still takes the invalidation
  messages waiting, now on every call rather than the first
  in a transaction, which costs an atomic load of sm's
  namespace epoch. An unlock does nothing, and
  `CheckRelationLockedByMe()` is true of a catalog, so the
  asserts that a relation is locked hold.
- **A tuple lock is nothing**: `LockTuple()` and its forms,
  which only an inplace update of a catalog row took, and
  `SearchSysCacheLocked1()`'s lock, so it is one search.
- **A relation's session lock is nothing**:
  `LockRelationIdForSession()`, for a catalog as for sm's
  tables, which sm cannot lock for a session.
- **Gone**: `SetLocktagRelationOid()`, `MarkLockClear()` and
  `SET_LOCKTAG_TUPLE`, which nothing uses now. A local lock's
  `lockCleared` is never set, and is left to go with
  `lock.c`. `LockApplyTransactionForSession()`, with no
  caller before this step, is left.
- `server.sh`: reading `pg_class` and `pg_namespace` leaves
  no relation lock in `pg_lock_status()`.

## Step 155: nothing takes a lock of the lock manager

`SHMEM.md` §3, §7 step 4. After steps 153 and 154 the lock
manager's last lock was the virtual xid's, which each
transaction took at its start for a session that would wait
for it to end. What waits for one, and the other waits on the
lock manager, waited for locks no session holds any more.

- **A transaction takes no virtual xid lock**
  (`StartTransaction()`): `VirtualXactLockTableInsert()` and
  `VirtualXactLockTableCleanup()` go.
- **The waits wait for nothing** (`lmgr.c`):
  `WaitForLockers()` and `WaitForLockersMultiple()`, as no
  session holds a lock of this lock manager on a relation;
  `XactLockTableWait()`, as no transaction id is locked and no
  transaction but the session's own is in progress to it; the
  speculative insertion lock, which only a catalog's insert
  takes, and it is refused. What called them is left: the
  concurrent forms of CREATE INDEX and DROP INDEX on a
  catalog, DETACH PARTITION CONCURRENTLY, which no sm table
  reaches, and a catalog's unique check.
- **Gone**: `GetLockConflicts()` and
  `ConditionalXactLockTableWait()`, which nothing calls now,
  and `XactLockTableWait()`'s error context. `VirtualXactLock()`
  is left to go with `lock.c`: with no virtual xid lock, it
  waits for nothing.
- `server.sh`: a transaction that reads catalogs holds no lock
  in `pg_lock_status()`, its virtual xid's among them.

## Step 156: the lock manager is gone

`SHMEM.md` §3, §7 step 4. After step 155 no transaction took a
lock of PostgreSQL's lock manager, and nothing waited on one;
what was left of it read it, released it, and kept its
structures.

- **Gone**: storage/lmgr `lock.c` and `deadlock.c`; `proc.c`'s
  wait queue (`JoinWaitQueue()`, `ProcSleep()`,
  `ProcWakeup()`, `ProcLockWakeup()`, `CheckDeadLock()`,
  `GetLockHoldersAndWaiters()`) and `LockErrorCleanup()`,
  with its calls at an abort and in `ProcessInterrupts()`;
  the lock manager's shared tables and their init; the
  sixteen partition LWLocks and the LockManager and
  LockFastPath tranches; each PGPROC's fast-path arrays,
  `fpInfoLock`, wait fields, PROCLOCK lists and lock-group
  fields, and `pcb->fast_path_strong`; a resource owner's
  cache of its locks, and the retail release of a
  subtransaction's.
- **Settings gone**: `deadlock_timeout`,
  `max_locks_per_transaction`, `log_lock_waits`,
  `log_lock_failures`, and `LOCK_DEBUG`'s `trace_locks`,
  `trace_userlocks`, `debug_deadlocks`, `trace_lock_oidmin`
  and `trace_lock_table`; `trace_lwlocks` stays with the
  LWLocks. The DEADLOCK_TIMEOUT and LOCK_TIMEOUT timeouts go,
  as no wait arms them: an advisory lock's wait has
  `lock_timeout` as its deadline. The Lock class of wait
  events goes.
- **Kept**: `lmgr.c`'s entry points, as sm's calls or no-ops,
  and `DoLockModesConflict()` there, with PostgreSQL's
  conflict table, for `reloptions.c`; `lock.h` keeps the lock
  modes, the virtual xid and a relation's locktag, which the
  callers still name.
- **SQL**: `pg_lock_status()` returns no row: a table's locks
  are sm's and an advisory lock is digres's own, and neither
  is listed yet. `pg_blocking_pids()` names the holders of an
  advisory lock waited for, and not yet a lock of sm's.
- **Also gone, unused now**: `GetCurrentVirtualXIDs()`,
  whose one caller, `WaitForOlderSnapshots()`, waits for
  nothing; `VirtualXactLock()`; lmgr's extension, page and
  apply-transaction locks and `DescribeLockTag()`, which
  nothing called; `InitializeFastPathLocks()`;
  `pg_depend.c`'s test of a lock held, false since step 48;
  `elog.c`'s `%P` names no lock group leader.
- **Left**: storage/lmgr `README`, PostgreSQL's account of
  the lock manager, as `README-SSI` was left.

## Step 157: the LWLocks are gone

`SHMEM.md` §3, §7 step 4. After step 156 one LWLock was left,
InjectionPoint, taken only by a build option's code
(`USE_INJECTION_POINTS`, compiled out); and the LWLocks'
waiters were all a semaphore was for.

- **InjectionPoint is a `std::mutex`**, `pcb_t::
  injection_point_mutex`, held by `held_mutex_t`. The code
  stays, compiled out, and compiles with the option on.
- **Gone**: storage/lmgr `lwlock.c` and
  `generate-lwlocknames.pl`, `storage/lwlock.h`,
  `lwlocklist.h` and the generated `lwlocknames.h`, with the
  pcb's LWLock array and tranche counter; the LWLock class of
  wait events; `LWLockReleaseAll()` at an abort and at exit;
  `trace_lwlocks`; PGPROC's LWLock wait fields, and
  `storage/proclist.h` and `proclist_types.h`, its wait list.
  `RequestNamedLWLockTranche()` goes with them, which a library
  in `shared_preload_libraries` could use; none in the tree did.
- **Semaphores gone**: port `posix_sema.c` and `sysv_sema.c`
  (skipped already, for another platform), `storage/pg_sema.h`,
  each PGPROC's semaphore, `pcb->shared_semas`,
  `ProcGlobalSemas()`, and the runtime-computed
  `num_os_semaphores` with `InitializeShmemGUCs()`, which only
  set it.
- **Also gone**: `src/include/storage/lwlocknames.h.tmp3049571`,
  a generator's temporary file committed by mistake with
  "dg: there is no shared memory" (`d4856b6`).
- `postmaster/backend_thread.c` includes `port/atomics.h`
  itself, which it had through `lwlock.h`.
- Left: `held_lock.h`'s comments, which explain it by the
  LWLock it stands for; storage/lmgr `README`'s account of
  LWLocks, with the rest of it (step 156).

## Step 158: a session is reached through its record

`SHMEM.md` §5, §7 step 4. A session was interrupted through its
PGPROC's latch, whose owner was its thread, and a cancel
request was matched against each PGPROC's pid and key. The
session's record (step 152) holds its thread already.

- **The cancel key is the record's**
  (`session_set_cancel_key()`), set and read under
  `pcb->cancel_key_mutex`. `SendCancelRequest()` finds the
  session with `session_by_pid()`, not by a scan of PGPROC
  slots, and interrupts it with `session_interrupt()`.
- **The postmaster signals a backend through its record**
  (`signal_child()`), by `session_by_pid()`.
- **Gone**: `InterruptProc()`, `PostmasterPidGetProc()`,
  `ProcSetCancelKey()`, PGPROC's cancel key and latch, and the
  shared latch with it: `InitSharedLatch()`, `OwnLatch()`,
  `DisownLatch()`, `SwitchToSharedLatch()`,
  `SwitchBackToLocalLatch()` and `Latch::is_shared`. A
  thread's latch is its local latch, which other threads set.
  `ProcSendSignal()`, which set a PGPROC's latch, and
  `ProcWaitForSignal()` go: nothing called either.
- `server.sh` checks it already: a cancel request from psql
  ends a lock wait, and the postmaster's shutdown ends an idle
  session.

## Step 159: PGPROC is gone

`SHMEM.md` §5, §7 step 4. After step 158 a session's PGPROC held
nothing another session read: its virtual xid, its xmin, its
database and role, its flags, and its slot on the free list.

- **A session's number is its thread's id**: `InitProcess()`
  sets `MyProcNumber` to `MyProcPid`, which a session thread
  takes from `gettid()`. A virtual xid is that and the
  transaction's local id, `tcb_t::lxid`, so a log line's `%v`
  shows the thread's id.
- **The session's xmin is `tcb_t::xmin`**, set and cleared where
  PGPROC's was. `TransactionXmin` stays apart from it, as
  before: a transaction's end clears the one and not the other.
- **Dropped**: `xid`, never assigned; `databaseId` and
  `roleId`, which the record holds; `statusFlags`, with the
  `PROC_*` flags and `set_indexsafe_procflags()`'s flag, which
  nothing read; `delayChkptFlags`, `isRegularBackend`,
  `tempNamespaceId`, which nothing read.
- **Gone**: `PGPROC`, `PROC_HDR`, `ProcGlobal`,
  `InitProcGlobal()`, `MyProc`, the free list and
  `pcb->proc_struct_mutex`, `GetPGProcByNumber()`,
  `ProcNumberGetProc()`, `BackendPidGetProc()`,
  `ProcArrayInstallRestoredXmin()` and the PGPROC argument of
  `SetTransactionSnapshot()`, whose one caller passed none;
  `session_record_t::proc`; the shared estimate of
  `spins_per_delay` with `set_spins_per_delay()` and
  `update_spins_per_delay()`.
- **`max_connections` is the registry's**: `session_register()`
  refuses a session past `MaxBackends`, as the empty free list
  did, and `session_have_free()` counts the slots left for the
  reserved connections, as `HaveNFreeProcs()` did.
- `server.sh`: with `max_connections = 3`, three sessions
  connect, a fourth is refused with "too many clients", and one
  connects once the three end. The shutdown check's comment
  names the record, not the PGPROC.

## Step 160: SQL tests say what they expect

`test/slt/README.md`. `run.sh` compares what psql printed with
a file of it, and most of what SQL is checked is in
`server.sh`, a check a `psql` call. A test in the
sqllogictest format, SQLite's, as DuckDB and CockroachDB use
it, says in each record what it must give, and its answers
are PostgreSQL 18's.

- **`build/slt`** (`test/slt/slt.cc`): a client of the
  system's libpq that runs each file in a database of its
  own. `statement ok`, `statement error PATTERN`, `query
  TYPES SORT LABEL` with its results or their MD5,
  `skipif`, `onlyif`, `hash-threshold` and `halt`.
  `--rewrite` writes a file's results from what the server
  gave.
- **`test/slt.sh`** starts digres, or with `--pg18` or
  `--rewrite` the PostgreSQL 18 in `$PG18` (`~/pg18`), and
  runs the files on it. `make slt` runs it on digres.
- **SQLite's corpus**, `select1.test` to `select5.test`,
  unchanged: 10,706 records. PostgreSQL 18 passes them all,
  in 51 seconds; digres fails 103 of `select4`'s, joins over
  two bitmap scans (`../sm/NEXT.md` §2), and takes 3 minutes
  55.
- **Each record is timed**: `--times FILE` writes every
  record's time, the slowest first, and a file's tally line
  has its time. What it showed: digres took 195 seconds to
  PostgreSQL 18's 41, 184 of them in `select5`, planning
  joins of up to 64 tables. The cause is the build: digres's
  is `-O0` with asserts, so with `MEMORY_CONTEXT_CHECKING`
  and `CLOBBER_FREED_MEMORY`, and its planning's time went to
  `bms_is_valid_set()`, `AllocSetCheck()` and
  `check_list_invariants()`. Built `-O2` without asserts,
  digres took 46 seconds, and 39 for `select5` to
  PostgreSQL's 37. Short statements still take about twice
  PostgreSQL's time; that is not looked into.
- **Not in `make test`** until those pass, and then
  `select1` to `select4` only, about 11 seconds: `select5`
  stays in `make slt`.
- **Left for later**: named connections in a file, PostgreSQL's
  own regression suite as a measure of what digres runs,
  isolation specs, and moving `server.sh`'s SQL checks over.

## Step 161: a row read by rowno is the slot's own

`../sm/NEXT.md` §2. SQLite's corpus (step 160) found 103
joins in `select4.test` giving wrong rows: a nested loop over
two bitmap scans returned the inner table's text in the outer
table's column too.

- **The cause**: a point read, `sm_tbl_read()`, hands out
  bytes that last only until the session's next call to sm
  (`../sm/include/sm.h`). `read_row()` in `access/sm`
  `smstore.c` left the slot's values by reference pointing
  into them; only a table with a numeric column was copied,
  by `slot_from_sm()`. The inner side's next read then
  overwrote the outer row's text. A cursor's row lasts until
  that cursor's next call, so a seq scan was never affected.
- **The fix**: `read_row()` materializes the slot, copying the
  values passed by reference, as `smstore_lock_latest()`
  already did. It serves the bitmap scan, the index scan and
  `smstore_fetch_row()`. An index entry read back from its
  row (`entry_from_row()`) is copied the same way.
- **`NEXT.md` said digres copied** the row an index scan or a
  bitmap scan fetches. It did not; it does now.
- **`test/slt/point_read.test`**: nested loops of bitmap
  scans, of index scans, and of an index scan inside, over
  text columns; three of its four queries failed before the
  fix. Its indexes are made before its rows: an index built
  over rows is not planned with for some statements after
  `CREATE INDEX` returns (`../sm/NEXT.md` §2).
- **`make test` runs the sqllogictest files** but
  `select5.test`, about 15 seconds; `make slt` runs them all.
  All 10,723 records pass.

## Step 162: CREATE INDEX waits for its build

`../DECISION.md`, for review. On a table with rows sm builds a
new index in the background, after the commit
(`sm_cat_create_index`), and digres never waited: `CREATE
UNIQUE INDEX` over duplicates returned `CREATE INDEX`, its
build failed unseen, and duplicates went on being taken; and
the statements after planned without the index until its build
ended.

- **`smstore_wait_indexes()`** (access/sm `smstore.c`): a
  transaction's commit hands the indexes it made to a list,
  and `finish_xact_command()` (tcop `postgres.c`) waits for
  each with `sm_cat_index_wait()` once no transaction is open,
  as `CREATE INDEX CONCURRENTLY` waits between its
  transactions. A cancel ends the wait; the build goes on.
- **A build that failed is dropped**, in a transaction of its
  own, and its error raised: `could not create unique index`
  with sm's detail for a duplicate. PostgreSQL leaves no index
  after a failed `CREATE INDEX`, and a failed one in sm is good
  only for dropping.
- **In a transaction block** the error comes at `COMMIT`,
  after the block's other work committed; PostgreSQL fails the
  `CREATE INDEX` itself. Within the block the index is not
  planned with until the commit.
- **Left**: after one failed build the others the transaction
  made are not waited for; the detail is sm's, `two rows of u
  with one key in uk`, not PostgreSQL's `Key (k)=(1) is
  duplicated.` (`../sm/NEXT.md` §2); a session that ends right
  after its commit waits for nothing.
- `test/slt/create_index.test`: a unique index over duplicates
  fails and leaves no index; a plain one over rows is planned
  with by the next statement. Four of its records failed
  before.

## Step 163: an index's name taken is refused as PostgreSQL does

`../sm/NEXT.md` §2. sm refuses a name taken in a namespace with
`SM_ERR_DUPKEY`, as it refuses a duplicated key, and digres
said `could not create unique index`, `the name nk is taken`,
for both; `CREATE INDEX IF NOT EXISTS` failed the same way.

- **`smstore_create_index()` looks the name up first**, with
  `smstore_relation_oid()`, as `CREATE SEQUENCE` does: a taken
  one is `relation "nk" already exists`, or with `IF NOT
  EXISTS` a NOTICE and nothing made.
- **`SM_ERR_DUPKEY` for an index not unique** is a name taken
  since the look-up, and says the same.
- `test/slt/create_index.test`: a name an index or the table
  holds, unique or not, and `IF NOT EXISTS`.

## Step 164: SQLite's whole corpus, on demand

`../DECISION.md`, for review; `test/slt/README.md`. The rest of
SQLite's corpus, `index/`, `random/` and `evidence/`, is 1.2
GB, too much to keep, and the part most likely to find what
`select4` found.

- **`test/slt_corpus.sh`** (`make slt-corpus`) clones the
  mirror `github.com/gregrahn/sqllogictest` at a pinned
  commit, with no file contents, into `build/slt-corpus`,
  checks out the files its sparse-checkout patterns name, and
  runs them with `test/slt.sh` on PostgreSQL 18 and on digres.
  What digres fails and PostgreSQL 18 passes is reported, by
  file and line. A record PostgreSQL 18 fails is another
  dialect's.
- **The sample**: the first set of files in each directory of
  `index/` and `random/`, and `evidence/`: 1,495,198 records,
  about 5 minutes on PostgreSQL 18 and 17 on digres.
  PostgreSQL 18 fails 7,328, digres 31,734.
- **What digres alone fails is views**: 24,408 records, all
  of them a `CREATE VIEW` digres refuses, or a query of the
  view it did not make (`../sm/NEXT.md` §2). The rest of the
  sample digres answers as PostgreSQL 18 does.
- Not in `make test`: it needs the network once, and long.

## Step 165: a sqllogictest file runs in more than one session

`test/slt/README.md`. `server.sh` checks what one session sees
of another's work with held psql sessions on fifos; a
sqllogictest file can now say it in records.

- **`connection NAME`**, on the line before a record, runs it
  in the session NAME, made at its first use in the file's
  database; a record with none runs in `default`. Every
  session ends before the database is dropped.
- **One record at a time**: a record that waits for another
  session's lock waits forever. Blocking needs an isolation
  format, as PostgreSQL's isolationtester has, and is not
  built.
- `test/slt/sessions.test`: a row inserted in an open
  transaction is not seen by another session until the
  commit; a REPEATABLE READ transaction keeps its snapshot
  past another's commit; another session's ADD COLUMN and
  DROP TABLE are seen by the next statement. Run in one
  session, two of its records fail.

## Step 166: pg_blocking_pids() sees waits in sm

`../DECISION.md`, for review. A session waiting in sm, for a
row's writer or a table's lock, was blocked by no one as far as
`pg_blocking_pids()` knew, so PostgreSQL's `isolationtester`,
which asks `pg_isolation_test_session_is_blocked()`, waited out
each blocked step for 300 seconds and failed it.

- **sm says whom a session waits for**: `sm_session_tag()`
  gives an sm session the host's number for it, and
  `sm_waits_for()` returns the numbers of the sessions it
  waits for, from sm1's waits-for edges (`sm1::set_tag`,
  `sm1::waits_for`, `../sm/sm1/API.md`). It reads them under
  the env's mutex and the database's `lock_mu`, and passes over
  an edge to a session no longer registered.
- **digres numbers each sm session by its record's id**
  (`datadir_open_session()`), and `pg_blocking_pids()` adds the
  pids of the sessions `datadir_sm_blockers()` finds.
- `../sm/sm1/test/test_lock.cc`: a writer waiting behind a
  version and a locker waiting on a lock name their holder;
  with the edges left out, it fails. `server.sh`: a row's
  writer and a table lock's holder are named, and the waiter
  is blocked by no one once it goes through.
- **Fixed on the way, in `test/slt_corpus.sh`**: a run of the
  whole corpus reported PASSED with nothing run on digres, as
  a `make test` beside it had cleared `build/test/slt`, the
  directory both used. `test/slt.sh` now takes its directory
  from `SLT_WORK`, the corpus gives each engine its own, and a
  run that could not be made or ran no record fails. The whole
  corpus is 622 files, 5,675,180 records; PostgreSQL 18 runs it
  in about 45 minutes and fails 11,626.

## Step 167: a deadlock in sm says "deadlock detected"

PostgreSQL's isolation specs (step 169). A transaction sm1 chose
to end a cycle of waits failed with `could not open a table:
chosen as a deadlock victim; the transaction was aborted`;
its SQLSTATE was PostgreSQL's, 40P01, but clients and tests
look for the words.

- **`sm_error()`** (access/sm `smstore.c`) raises
  `SM_ERR_DEADLOCK` as `deadlock detected`, with what was being
  done in the detail, wherever an sm call ends in one.
- **Which transaction is chosen is sm1's**: the youngest on the
  cycle (`../sm/sm1/SM1_DESIGN.md` §5.1), not, as PostgreSQL,
  the one whose check found it. `deadlock-simple` fails on that.
- `server.sh`: two sessions locking two tables in opposite
  orders; one fails with `40P01: deadlock detected`. It failed
  before, on the words.

## Step 168: lock_timeout ends a wait in sm

`../sm/NEXT.md` §2 asked for it. `lock_timeout` ended a wait for
an advisory lock only; a wait in sm, for a row's writer or a
table's lock, went on past it.

- **`LOCK_TIMEOUT` is back** (utils `timeout.h`), with its
  handler, which raises a cancel (utils/init `postinit.c`), and
  `ProcessInterrupts()`'s choice between it and
  `statement_timeout` (tcop `postgres.c`), as PostgreSQL has
  them; step 156 removed them when no wait armed them.
- **`waiting()`** (access/sm `smstore.c`) arms it around each
  sm call that may wait, and disarms it as the call returns. sm
  does not say when a call begins to wait, so the time counts
  from the call; a row's or a lock's call reaches its wait in
  microseconds. The cancel ends sm's wait, as any interrupt
  does, and is reported as `canceling statement due to lock
  timeout`, 55P03.
- `server.sh`: a wait for a row's writer and for a table's lock
  each end with 55P03 within the timeout, and a 100,000-row
  insert that waits for nothing is not cancelled with
  `lock_timeout` at 1ms. The isolation spec `timeouts` now
  differs only in printing `600.000` for `600`.

## Step 169: PostgreSQL's isolation specs run on digres

`test/isolation.sh` (`make isolation`), with step 166's
`pg_blocking_pids()`.

- **37 of PostgreSQL 18's 124 specs** use nothing digres leaves
  out outright. Each runs in a database of its own, with
  PostgreSQL 18's `pg_isolation_regress` and `isolationtester`
  from the tree built in `$PG18_SRC`; a step stuck 20 seconds
  is cancelled. `test/isolation_specs.txt` lists each with
  whether it is to pass, and why not.
- **16 pass**: NOWAIT, SKIP LOCKED, `INSERT ... ON CONFLICT`,
  savepoints, `lock-nowait`, `aborted-keyrevoke`.
- **21 fail, for reasons known** (`../sm/NEXT.md` §2): no `FOR
  KEY SHARE` or `FOR NO KEY UPDATE` (six); sm1's deadlock
  victim and its unordered wait queue (two); `deadlock_timeout`
  gone (two); `REFERENCES` in `CREATE TABLE` and `char(n)` not
  supported (four); an unconstrained numeric printed with three
  decimals (two); catalog tables, `MERGE`, an index on an
  expression (five).
- Not in `make test`: it needs the PostgreSQL 18 build, and ten
  minutes.

## Step 170: deadlock_timeout is back, for sm's waits

Step 156 removed `deadlock_timeout` with the lock manager, which
was its only reader; PostgreSQL's isolation specs set it, per
session, to say which session finds a cycle first.

- **sm1 waits per session**: `sm1::set_deadlock_ms()` sets how
  long a session waits before it looks for a cycle, 0 for its
  database's `deadlock_ms`; `sm_set_deadlock_ms()` in `sm.h`.
  Only the session's own waits read it. The transaction ended
  is still the cycle's youngest.
- **`deadlock_timeout`** (utils/misc `guc_tables.c`), as
  PostgreSQL's, superuser-set, in ms; its assign hook and
  `datadir_open_session()` hand it to the session's sm session
  (utils/init `datadir.c`). Its default is 50ms, sm1's, not
  PostgreSQL's 1s: sm1's check walks only the waits, and is
  cheap. `postgresql.conf.sample` has it again.
- `../sm/sm1/test/test_lock.cc`: a cycle between two sessions
  with 3000ms each is found no sooner than 2.5 seconds, and with
  the default well before; with the setting ignored it fails.
  The isolation spec `deadlock-hard` passes now: 17 of 37.

## Step 171: a wait looks for a cycle as it begins

The isolation spec `deadlock-simple` failed on timing alone:
PostgreSQL finds a cycle at once when a waiter's place in a
lock's queue closes it, and sm1 looked only after a waiter had
slept `deadlock_ms`. The victim was the same.

- **sm1 searches as a wait begins** (`../sm/sm1/src/lockmgr.cc`,
  `../sm/sm1/SM1_DESIGN.md` §5.1): `lock_wait` and
  `lock_wait_xact` call `check_deadlock()` once their edges
  are set, before they
  first sleep; a cycle the wait closes is found at once. One
  that forms while a session sleeps, its holders changing under
  it, is still found after `deadlock_ms`, the session's
  `deadlock_timeout`. The victim is still the cycle's youngest.
- **So `deadlock_timeout` delays little now**: every edge is
  set by a session as it waits, so a cycle is nearly always
  closed by a wait's beginning. Each wait pays for one walk of
  the edges under `lock_mu`.
- `../sm/sm1/test/test_lock.cc`: step 170's test now checks that
  a cycle is found within a second with `deadlock_ms` at 3000;
  without the search it took 3 seconds. The 109 scenarios of
  `../sm/sm1/mvcc` match. `deadlock-simple` passes: 18 of 37.
## Step 172: sm knows a session by its thread's id

`../DECISION.md`, wip-slt3, as decided on review. Step 166
numbered each sm session by its session record's id, and
`datadir_sm_blockers()` looked each number up in the registry
to find its pid.

- **An sm session's number is its pid**
  (`datadir_open_session()`), its thread's id, as a session's
  number is since step 159.
  `sm_waits_for()` so gives pids, and `datadir_sm_blockers()`
  returns them as they are, with no scan of the registry. A pid
  is distinct among the sessions running, as sm asks, and 0,
  which sm takes for none, is no pid.
- `server.sh`'s check of step 166 checks it: a row's writer and
  a table lock's holder are named by their pids.

## Step 173: the registry of shared blocks is gone

`SHMEM.md` §2, §7 step 5. storage/ipc `shmem.c` had been a
registry of named blocks since step 26, found by name by every
session thread's `AttachSharedMemoryStructs()`. Two blocks were
left in it: `TransamVariables`, and the injection points',
under a build option.

- **`TransamVariables` is gone**, with access/transam
  `varsup.c`: what was left of it never changed.
  `GetSnapshotData()` takes `FirstNormalTransactionId` as every
  snapshot's xmin and xmax, the xid after the last completed,
  and 1 as its completion count, as they were.
- **The injection points' control struct is the pcb's**
  (`pcb_t::injection_points`), `calloc()`'d before any thread
  starts. The code stays behind `USE_INJECTION_POINTS`.
- **Gone**: the registry, `ShmemInitStruct()`,
  `ShmemInitHash()`, `ShmemAlloc()`, `InitShmemIndex()`,
  `pcb->shmem_index` and `storage/shmem.h`;
  `AttachSharedMemoryStructs()` and the init list a session
  thread ran, as it has nothing to find; `shmem_request_hook`
  with `process_shmem_requests()`, and `shmem_startup_hook`, as
  decided: a library had no registry to use them with, and
  none in the tree did. `shmem.c` keeps `pg_numa_available()`.
- `parse_oper.c`, `array_typanalyze.c`, `dynahash.c` and
  `guc.c` include `utils/hsearch.h` themselves, which they had
  through `storage/shmem.h`.
- **Left**: dynahash's shared mode, which no table uses now.

## Step 174: dynahash's shared mode is gone

`SHMEM.md` §2, §7 step 5. dynahash made PostgreSQL's hash tables
in shared memory: a header a process attached to by name, a
directory fixed in size, a size not to grow past, and free lists
partitioned among locks, each under a mutex of its own. Since
step 173 no table is made so, and no caller passed the flags.

- **Gone** (utils/hash `dynahash.c`, `utils/hsearch.h`):
  `HASH_SHARED_MEM`, `HASH_ATTACH`, `HASH_PARTITION`,
  `HASH_DIRSIZE` and `HASH_FIXED_SIZE`, with `HASHCTL`'s
  `num_partitions`, `dsize`, `max_dsize` and `hctl` and
  `NO_MAX_DSIZE`; `hash_select_dirsize()` and
  `hash_get_shared_size()`; the 32 free lists and their mutexes,
  for one, a count of entries and a chain, in the header; the
  borrowing of an entry from another free list; `HTAB`'s
  `isshared` and `isfixed`; `next_pow2_long()`, which only
  `hash_select_dirsize()` called. A table never in shared memory
  reports "out of memory" and FATALs on corruption, as a local
  one did.
- **Kept**: the flags a local table uses, at their values, and
  `HASH_ALLOC` and `HASH_SEGMENT`, which a local table may pass.

## Step 175: a relation's name by oid is sm's now

`get_rel_name()` and its siblings (utils/cache `lsyscache.c`),
and `regclassout`, read a relation's pg_class form with no lock
(`RelationGetClassForm()`, utils/cache `relcache.c`), as
PostgreSQL reads the syscache. digres answered from the
session's cached descriptor, which since step 145 is checked
against sm's version only after sm's lock: another session's
rename left the old name, and its drop left the name of a table
that was gone, until the session next locked the table.

- **sm** (`sm.h`, `src/sm.cc`): `sm_cat_table_version_oid()`, a
  table's version by oid with no lock and no definition copied;
  1 if there is no such table.
- **dg** (access/sm `smstore.c`): `smstore_relation_name()`.
  The version alone decides the common case: equal to the one
  the cached entry was built at, the entry is current. Else, or
  while the transaction has changed the table itself
  (`SM_VERSION_OWN`), the name and schema come from sm's
  definition, read with no lock (`sm_cat_table_def_oid()`); an
  index's from its table's. No such table, or an index its
  table no longer has: false.
- `RelationGetClassForm()` asks it for every relation of sm's,
  and overwrites the cached form's name and schema when they
  have moved; the other fields cannot change for an oid. The
  cached entry itself is left: it is rebuilt under the next
  lock, as before. Nothing is read from disk: sm3's catalog is
  in memory.
- `test/server.sh`: a held session caches a table and its
  index, then sees another's rename, the index's drop and the
  table's drop by oid as regclass.

## Step 176: lmgr.c's no-ops are gone

`SHMEM.md` §3. Since steps 154 to 156, much of storage/lmgr
`lmgr.c` did nothing: a lock is held to the transaction's end,
none is held for a session, a catalog's row is never locked,
and no one waits for a transaction or a locker.

- **Gone**, with their calls (about 60, in 20 files):
  `UnlockRelationId()`, `UnlockRelationOid()`,
  `LockRelationIdForSession()`, `UnlockRelationIdForSession()`,
  `LockTuple()`, `ConditionalLockTuple()`, `UnlockTuple()`,
  `XactLockTableWait()` with `XLTW_Oper`,
  `SpeculativeInsertionLockRelease()`,
  `SpeculativeInsertionWait()`, `WaitForLockers()`,
  `WaitForLockersMultiple()`, `UnlockDatabaseObject()`,
  `UnlockSharedObject()`, `LockSharedObjectForSession()` and
  `UnlockSharedObjectForSession()`; catalog `dependency.c`'s
  `ReleaseDeletionLock()`, which only called them.
- **What their callers lost**: the lock tags and session
  `LockRelId`s of the concurrent index build and drop (commands
  `indexcmds.c`, catalog `index.c`) and of DETACH PARTITION
  CONCURRENTLY; MERGE's and UPDATE's `lockedtid` and `tuplock`
  (executor `nodeModifyTable.c`); the release of a lock taken on
  the wrong relation in the retry loops of catalog `namespace.c`,
  `objectaddress.c`, `tablecmds.c`'s callbacks and
  `stat_utils.c`; `relation_close()`'s and `index_close()`'s
  release. Where PostgreSQL waited, a comment says why nothing
  does.
- **Kept**: the entry points that take sm's lock
  (`LockRelationOid()` and the rest) or the invalidations
  waiting (`LockDatabaseObject()`, `LockSharedObject()`,
  `ConditionalLockSharedObject()`), and
  `SpeculativeInsertionLockAcquire()`, which hands out the
  token `heap_insert()` stores. `SearchSysCacheLocked1()` and
  `ri_needLockTagTuple` are left: they choose a path, not a
  lock.
- executor `execIndexing.c`'s wait for a conflicting inserter
  is not reached, as no other transaction is in progress to it
  and only a catalog's index is checked there; it restarts the
  scan, as it did.
- storage/lmgr `README` is rewritten for what locks now, and
  `README-SSI` is gone (commit before this step).

## Step 177: foreign keys

`../sm/FOREIGNKEY.md`. sm3 keeps a foreign key in its catalog
(`sm_cat_create_fkey` and the rest); digres checks the rows, with
PostgreSQL's own RI triggers.

- **DDL** (access/sm `smstore.c`): `REFERENCES` and `FOREIGN KEY`
  in `CREATE TABLE`, which parse analysis makes an `ALTER TABLE ...
  ADD CONSTRAINT`, and `ALTER TABLE`'s `ADD`, `DROP`, `VALIDATE`
  and `RENAME CONSTRAINT`. `add_fkey()` chooses the referenced
  columns, the parent's primary key if none are named, their
  unique index, and the operators, as
  `ATAddForeignKeyConstraint()` does; sm's index keys have no
  operator class, so each referenced type's default btree class
  stands for one. The operators and `SET NULL (cols)` are sm's
  host bytes. The rows already there are checked by
  `RI_Initial_Check()`, unless the table is new or the key is
  `NOT VALID`. PostgreSQL's messages throughout.
- **The relation cache** (utils/cache `relcache.c`) makes the
  RI triggers from the table's foreign keys and those that
  reference it, as `createForeignKeyCheckTriggers()` and
  `createForeignKeyActionTriggers()` would have stored them,
  named by constraint, numbered by name; `SetTriggerFlags()` is
  exported for it. A rebuild forgets what `ri_triggers.c` cached
  of them (`RI_ForgetConstraint()`).
- **`ri_LoadConstraintInfo()`** reads the constraint from sm
  (`smstore_get_fkey()`), not `pg_constraint`;
  `get_ri_constraint_root()` went, unused.
- **A row trigger on a table in sm is given its rows from a
  tuple store**, as a foreign table's is (commands/trigger.c):
  sm cannot read again a row its statement deleted. So
  `smtable.c`'s `tuple_satisfies_snapshot` is true, the store's
  rows having no tid, and `RI_FKey_fk_upd_check_required()` asks
  no xmin of an sm row: its insert's check runs whatever came
  after.
- **So a DEFERRABLE foreign key is refused**: a deferred check
  fires at commit, past the statement whose store holds its
  rows. NOT ENFORCED, MATCH PARTIAL and PERIOD are refused too.
- **`DROP TABLE`** of a referenced table is PostgreSQL's error,
  or with `CASCADE` drops the constraints with its notice; one of
  a table the statement drops too goes with it. **`TRUNCATE`** of
  one is refused unless its referencing tables are truncated with
  it, or `CASCADE` adds them.
- **A definition is read afresh** (`table_def_now()`): a table
  handle keeps the definition it was opened with, so one opened
  earlier in the transaction, before the parent's primary key
  was made, had none. And after each change to a foreign key both
  tables' descriptors are forgotten and their handles closed
  (`forget_table()`, the half of `alter_table_rel()` that did
  it), or a key made in a transaction was checked only after it.
- `test/slt/foreign_key.test`, PostgreSQL 18's answers: every
  action, a two-column key MATCH SIMPLE and FULL, a self
  reference, NOT VALID and VALIDATE, rename, drop, DROP TABLE and
  TRUNCATE, and a parent and child made in one transaction. The
  isolation specs `fk-contention`, `fk-deadlock` and
  `update-locked-tuple` now run, and fail on `FOR KEY SHARE`
  being `FOR SHARE`. `server.sh`'s check of what ALTER TABLE refuses uses
  ALTER COLUMN now.
- **Left**: `pg_constraint` lists no foreign key, so `\d` shows
  none; DEFERRABLE; `rel` verbs for them.

## Step 178: a row lock's check asks the server whether it waits

`test/server.sh`'s `against()` and `upsert_against()` run a
statement in one session while another holds a row, and say
whether it "waited" or "passed". They slept a second and called
it waiting if it was still running: under the thread sanitizer
a statement that does not wait can take longer, and "skip
locked takes the next row" failed so once.

- `wait_state()`: the other session prints its pid first; it
  "waited" once `pg_blocking_pids()` names a blocker of it,
  "passed" if it ends first, and is "stuck", which no check
  expects, if neither within ten seconds.
- Shown: with every other session slowed by 1.5 s
  (`pg_sleep()`), the old test failed eight checks and the new
  one passes; with `pg_blocking_pids()` blinded, the new one
  fails every check that expects a wait.
- Left: the checks that only want a statement still running a
  second later (`bg_waiting()` and four others), which a slow
  build can pass but not fail.

## Step 179: an immediate shutdown tells every client why

On SIGQUIT the postmaster raises `INTERRUPT_QUIT` in every
session (`backend_thread_signal()`), and each session's
`quickdie()` (tcop `postgres.c`) sends its client "terminating
connection due to immediate shutdown command", then called
`_exit(2)`. A session is a thread: the first to exit ended the
process, and with it the sessions that had not yet written.
Four busy clients over ten stops were told 17 times of 40, and
the server's status was the session's 2 (step 148 noted it).

- `quickdie()` and `SignalHandlerForCrashExit()` (postmaster
  `interrupt.c`, a session still in its startup) end a session
  thread with `proc_exit(2)`, as `process_startup_packet_die()`
  ends one with `proc_exit(1)`: the thread's exit callbacks
  run, and the postmaster stops once every session has ended.
  PostgreSQL skipped them as shared memory might be corrupt; a
  crash in digres ends the process, so a SIGQUIT is a stop.
- 40 of 40 told; the server exits 0, "database system is shut
  down", as PostgreSQL's does.
- **The postmaster's last resort** (postmaster `postmaster.c`):
  five seconds after its SIGQUIT, it sent SIGKILL to the
  children still running, which a thread drops. Now, if a
  session thread remains, it logs how many did not end and
  ends the process with `_exit(2)` (`CountThreadChildren()`).
  Shown with every session made to sleep ten seconds first: it
  stopped at five, status 2.
- `test/server.sh`: three busy sessions and two idle are each
  told, and the server stops within ten seconds with status 0.
  With `_exit(2)` put back it failed three runs of three.

## Step 180: char(n)

`../sm/NEXT.md` §2, `../sm/sm2/TYPE.md` §3.5. PostgreSQL's
`char(n)`, `bpchar`, was refused: "type character is not
supported yet". Its values are padded with spaces to n, and the
padding counts for nothing when they are compared, so `'a'` and
`'a  '` are one value.

- **sm2 has a type for it**, `bpchar`, code 19: a `text`'s
  bytes, stored and printed as given, the padding and all. Its
  `cmp` and its key leave the trailing spaces out, so a lookup
  finds a value padded or not, and a unique index takes one of
  `'a'` and `'a  '`. Its key so gives no value back: it has no
  `key_decode`, and an index-only scan reads the row, as under a
  non-deterministic collation. sm3 lets a key of it have a
  collation, as a `text` key may.
- **digres maps `char(n)` to it and back** (access/sm
  `smstore.c`), and does nothing else: PostgreSQL pads a value
  to n as it is cast to the column's type, and reads it back as
  stored. A `bpchar` of no length keeps a value's spaces, as
  PostgreSQL's does.
- `test/slt/char.test`, PostgreSQL 18's answers: padding,
  length, equality, order and grouping without the padding, a
  unique index and its lookups and index-only scan, the length
  limit, `character` as `char(1)`, `bpchar`, and a foreign key
  between char columns. `../sm/sm2/test/test_type.cc` checks the
  type, and failed with the key's trim taken out. The isolation
  spec `propagate-lock-delete` gets past its setup now, and
  fails on `FOR KEY SHARE`.

## Step 181: pg_atomic_* is gone

`SHMEM.md` §3. PostgreSQL's atomics, port `atomics.h` and the
headers under `port/atomics/`, had two users left; an atomic is
a `std::atomic`, its order explicit.

- postmaster `backend_thread.c`: a launch record's `tid` is a
  `std::atomic<pid_t>`, stored with release after `self`, and
  loaded with acquire, so the postmaster that reads the tid
  sees the thread's handle too (`backend_thread_signal()`).
- utils/misc `injection_point.c`, built only with
  `USE_INJECTION_POINTS`: each entry's `generation` and
  `max_inuse` are `std::atomic`, loaded and stored relaxed, and
  `pg_write_barrier()` and `pg_read_barrier()` are release and
  acquire fences, so the generation protocol is as it was. A
  reader still copies an entry a writer may be changing, which
  the C++ memory model calls a race. Checked by compiling with
  the option: the file already failed to, on a `void *`
  converted to `InjectionPointCallback`, which is left.
- **Gone**: `port/atomics.h` and its eight headers, port
  `atomics.c`'s emulation of 64-bit atomics (compiled out on
  both platforms), and storage/lmgr `README.barrier`, on the
  barriers. Five files included `atomics.h` and used nothing
  of it.

## Step 182: spinlocks are gone

`SHMEM.md` §3. No spinlock had been taken since steps 132 and
133; the last user of `slock_t`, port `atomics.c`'s emulation
of 64-bit atomics, went with step 181.

- **Gone**: storage `s_lock.h` and `spin.h`, storage/lmgr
  `s_lock.c` with its spin delay, the `SpinDelay` wait event,
  and backend `port/`, which held only the test-and-set
  assembly of Sun Studio's compilers (`tas/`).
- access `relscan.h` included `spin.h` and used nothing of it.

## Step 183: Solaris is gone

digres builds on Linux and macOS. What was left for Solaris and
Sun's compilers goes; steps 181 and 182 took its atomics and
test-and-set assembly.

- **Gone**: `port/solaris.h`; `__SUNPRO_C` in `c.h`'s attribute
  macros; the Sun C++ `restrict` workaround and the Solaris
  `HAVE_ATOMIC_H`, `HAVE_MBARRIER_H`, `HAVE_GETPEERUCRED` and
  `HAVE_UCRED_H` in `pg_config.h` and `pg_config.h.in`; port
  `getpeereid.c`'s `getpeerucred()`; libpq `pqcomm.c`'s
  `TCP_KEEPALIVE_THRESHOLD`; `SHM_SHARE_MMU` in portability
  `mem.h`; `__sun` in utils/misc `ps_status.c`.
- libpq `auth.c`, built only with `USE_PAM`: the conversation
  callback's `msg` is `const`, as Linux-PAM and OpenPAM declare
  it, and it takes the password from its `appdata_ptr` alone;
  the static copy kept for Solaris 2.6's PAM, which did not
  pass it, is gone. Checked by compiling with the option
  against a stub of `pam_appl.h`: the file already failed to,
  on `calloc()`'s `void *` assigned to a `pam_response *`,
  which is left.
- Comments that only recall Solaris (timezone `strftime.c`,
  utils/error `elog.c`, `pqcomm.c`'s macOS case) and
  utils/cache `typcache.c`'s guard of `bsearch()` over no items
  are left.

## Step 184: the postmaster-death watch is gone

`SHMEM.md` §5. A child process watched for its postmaster's
death so as not to run on without it: the read end of a pipe
the postmaster held open, or on macOS the postmaster's pid
under kqueue, and a parent-death signal (`SIGPWR` by
`prctl()`). A session is a thread of the postmaster's process,
and ends with it.

- **Gone**: the pipe, `postmaster_alive_fds` and the postmaster's
  `InitPostmasterDeathWatchHandle()`; storage/ipc `pmsignal.c`'s
  `PostmasterIsAlive()`, `PostmasterDeathSignalInit()` (each
  session installed a process-wide `SIGPWR` handler) and its
  flag; the wait events `WL_POSTMASTER_DEATH` and
  `WL_EXIT_ON_PM_DEATH`, in storage/ipc `waiteventset.c`'s four
  implementations and `latch.c`'s waits, which no longer assert
  that a caller names one.
- **Their callers**: libpq `be-secure.c`'s FATAL "unexpected
  postmaster exit" on a read or write wait, the event in
  `pqcomm.c`'s client wait set (two events, not three),
  executor `nodeAppend.c`'s async wait set (an empty one counts
  0), the flag in `pg_sleep()`, `pg_terminate_backend()`'s wait,
  and the SSL and GSSAPI handshakes, and commands `vacuum.c`'s
  check after a cost delay, which called `exit(1)`.
- `pmsignal.c` keeps the quit reason.
- The poll and kqueue implementations, not built on Linux, are
  checked by compiling with each forced, kqueue's against a stub
  of `sys/event.h`.

## Step 185: the postmaster signals only threads

Every child of the postmaster is a backend thread
(`BackendStartup()`, `backend_thread_launch()`), and leaves
`ActiveChildList` as it ends (`CleanupBackend()` releases the
slot before a crash signals the rest), so postmaster
`postmaster.c`'s `signal_child()` took its first branch,
`backend_thread_signal()`, always.

- **Gone** from `signal_child()`: the lookup by pid in the
  session registry (step 158), and `kill()` of the pid and of
  its process group, for a child process. A thread's id given
  to `kill()` names the whole process on Linux, so the
  fallback, had it run, would have signalled the server.
- An assertion says the child is a thread; asserts are on in
  every build, and the suite's shutdowns, cancels and
  terminations pass through it.

## Step 186: a thread sets its own signal mask

`sigprocmask()` is unspecified in a process with more than one
thread; digres has one per session, and sm's. Each call
(postmaster `postmaster.c`'s two, utils/init `miscinit.c`'s
two, tcop `postgres.c`'s and `backend_startup.c`'s two each,
access/transam `xact.c`'s two) is `pthread_sigmask()`, as
utils/init `session_thread.c` and `datadir.c` already were.

- On Linux and macOS `sigprocmask()` set the calling thread's
  mask, so nothing changes: in the postmaster and a standalone
  backend the masks are PostgreSQL's (libpq `pqsignal.c`), and
  in a session thread `BlockSig`, `UnBlockSig` and
  `StartupBlockSig` are one set, every signal but a fault's
  (`session_thread_start()`), so its calls keep it as it is.
- Windows's `pqsigprocmask()` emulation, compiled out, is left.

## Step 187: the pending flags are plain bools

A session thread takes no asynchronous signal: it blocks all
but a fault's (`session_thread_start()`), and its handlers run
where its interrupts are delivered (storage/ipc `procsignal.c`'s
`DeliverInterrupts()`, at `CHECK_FOR_INTERRUPTS()` and in a
wait), so what they set needs neither `volatile` nor
`sig_atomic_t`.

- `InterruptPending`, `QueryCancelPending`, `ProcDiePending`,
  `CheckClientConnectionPending`, `ClientConnectionLost`, the
  three timeout flags and `LogMemoryContextPending` (utils/init
  `globals.c`), and postmaster `interrupt.c`'s
  `ConfigReloadPending`, are `bool`. `ShutdownRequestPending`,
  never set or read, is gone.
- utils/misc `timeout.c`: the alarm's handler,
  `handle_sig_alarm()`, runs on `INTERRUPT_ALARM` in its own
  thread, so the timeouts' state is not `volatile` either.
- **Left**: the postmaster's `pending_pm_*`, set by its real
  handlers of `SIGHUP`, `SIGINT`, `SIGQUIT` and `SIGTERM`;
  storage/ipc `waiteventset.c`'s `waiting`, read by the
  self-pipe's `SIGURG` handler (the poll implementation,
  compiled out); the frontend's `common/string.h`. The holdoff
  counts (`InterruptHoldoffCount` and the rest) are `volatile`
  still.

## Step 188: DDL on a cached descriptor takes sm's lock first

`SHMEM.md` §5.1, step 142. PostgreSQL 18's regression suite
(`test/regress/README.md`) stopped the server in five files on
`relation_open()`'s assertion that a relation opened with
`NoLock` is locked. Five call sites of digres's own read a
descriptor before sm's lock: when the descriptor was built in
the statement, building it took IS (`smstore_table_desc()`),
and the assertion held by chance; when it was cached from an
earlier transaction, nothing was held. Step 141's audit ran
over digres's tests, which never cached one first. Asserts are
on in every build, so each stopped any server:

```
create table t (k int4);
create index i on t (k);
insert into t values (1);
drop index i;
```

- **Each takes its lock as it opens the relation**, in the
  mode PostgreSQL's code takes, so `LockRelationOid()` checks
  the version (§5.1) before the descriptor is read, and the
  lock is held to the transaction's end:
  - DROP INDEX (`smstore_drop_indexes()`): the table, X, as
    `index_drop()` locks it.
  - A table's drop (`smstore_drop_table()`): X. DROP TABLE had
    it already; DISCARD TEMP and ON COMMIT DROP did not.
  - ON COMMIT DELETE ROWS (commands `tablecmds.c`,
    `PreCommit_on_commit_actions()`): X, as `heap_truncate()`.
  - ALTER SEQUENCE ... OWNED BY (`smstore_own_sequence()`): the
    table, IS, as `process_owned_by()` locks it.
  - `pg_stats()` (`put_stats_rows()`): the table, IS.
- `test/server.sh` runs each after caching the descriptor, and
  failed at each with its lock taken out. The remaining `NoLock`
  opens in `smstore.c` follow a lock: DROP TABLE's,
  `smstore_create_index()`'s, ANALYZE's and the planner's
  statistics' were tried after caching, and hold.

## Step 189: several keys cascading from one row

`test/regress/README.md`'s crash in `foreign_key`:
`!TTS_EMPTY(slot)`, on an `UPDATE` of a key that three columns
of one table reference `ON UPDATE CASCADE`.

- A table in sm gives its row triggers their tuples from a
  tuple store, as a foreign table does (`commands/trigger.c`):
  a row's first event fetches them, the rest reuse them.
  `AfterTriggerSaveEvent()` chose which before it asked whether
  a foreign key's check could be skipped, so when the row's
  first check was, the next event reused another row's tuples,
  or an empty slot. PostgreSQL never meets this: a foreign table
  has no foreign keys. It chooses now as the event is queued.
- `test/slt/foreign_key.test`: three keys cascading from one
  parent, in a transaction and out of one, PostgreSQL 18's
  answers. Before the change it failed with "Key (b)=(3) is not
  present", a check reading the row of the cascade before. The
  `foreign_key` file runs to `MATCH FULL` now.

## Step 190: WHERE CURRENT OF

`test/regress/README.md`'s crash in `portals` and
`rowsecurity`: `DatumGetObjectId(ldatum) == table_oid`, in
`execCurrent.c`, on `DELETE` or `UPDATE ... WHERE CURRENT OF`.

- **A row sm reads has its table's oid now.** sm gave a slot
  its row's tid, its rowno, but not `tts_tableOid`, which a heap
  table's slot has from its tuple; `execCurrent.c` checks the
  cursor's row is of the table written. A scan of a table in sm
  keeps the table's oid (access/sm `smstore.c`), and each read
  sets it: a scan, an index scan, an index-only scan, a bitmap
  scan's fetch, `smstore_fetch_row()` and `smstore_lock_row()`.
- **A TID scan runs on a table in sm**, the plan of `WHERE
  CURRENT OF` (`smtable.c`). Any tid is valid, as fetching it
  says whether the statement sees a row; a row's newest version
  has the tid it had, as sm updates a row at its rowno.
- **An index-only scan's cursor** gives the row's tid from its
  slot (`execCurrent.c`): sm's index-only scan has no scan
  descriptor, and sets the slot's tid as a table scan does.
- `test/slt/current_of.test`, PostgreSQL 18's answers: delete
  and update through a cursor, the cursor not moving, an update
  twice and a delete twice, and cursors over a bitmap scan, an
  index-only scan and an index scan. Without the change the
  server crashed on its first `WHERE CURRENT OF`. `portals`
  runs past its `uctest` cases now; where it differs, an
  updated row keeps its place in a scan, and a scan runs
  forwards only.

## Step 191: a snapshot's xid arrays are not allocated

`SHMEM.md` §2. Each session malloc'd, for each of its three
static snapshots (current, secondary and catalog), an array of
`MaxBackends` xids and one of 65 per backend, as PostgreSQL
sized them to hold every running xid and each PGPROC's cache of
subtransaction xids. No xid is ever in progress (step 150), so
nothing was put in them: about 26 kB a snapshot at 100
connections, 78 kB a session.

- storage/ipc `procarray.c`'s `GetSnapshotData()` allocates
  nothing; `GetMaxSnapshotXidCount()` and
  `GetMaxSnapshotSubxidCount()` are 0, so utils/time
  `snapmgr.c`'s checks of a snapshot it copies or reads refuse
  any xid.
- **Gone**: `PROCARRAY_MAXPROCS`, `TOTAL_MAX_CACHED_SUBXIDS`,
  storage `proc.h`'s `PGPROC_MAX_CACHED_SUBXIDS`, and
  `FreeStaticSnapshots()`, which freed the arrays as a session
  thread ended.

## Step 192: a latch is a token

`SHMEM.md` §4. Since step 158 a `Latch` held only its owner: a
handle on the thread, new'd by `InitLatch()`, and a mutex that
`SetLatch()` held to copy it, so that another thread could find
the one to wake. One did: a backend thread, as it ended, set
the postmaster's latch (postmaster `backend_thread.c`'s exit
queue). Every other `SetLatch()` named `MyLatch`.

- **`Latch` holds nothing**: it is `MyLatch`, a thread's
  `LocalLatchData`, naming the calling thread's
  `INTERRUPT_LATCH` to PostgreSQL's API, which is unchanged.
  `SetLatch()` raises the caller's own bit, and asserts that
  the latch is its own; `LatchIsMine()` is `latch == MyLatch`.
- **The exit queue keeps the postmaster's thread**
  (`sm_thread_ref_t`) and raises its bit with `sm_interrupt()`.
- **Gone**: the owner, its mutex, `LatchOwner()`, and
  `ShutdownProcessLocalLatch()`, which deleted the handle as a
  thread ended.

## Step 193: pg_lock_status() lists sm's locks and advisory locks

`SHMEM.md` §3, §7 step 4. Since the lock manager went (step
156), `pg_lock_status()` returned no row.

- **sm1 lists its lock table** (`sm1::lock_status()`,
  `../sm/sm1/API.md`): a row for each holder of each lock, in
  its mode, and one for each session waiting in `sm1::lock`, in
  the mode it asks; a waiter's key and mode are kept while it
  waits (`lock_wait()`). A writer waiting behind an uncommitted
  version holds and asks no lock, and is not listed:
  `pg_blocking_pids()` names whom it waits for.
- **The C wrapper decodes each** (`sm_lock_status()`): an oid's
  lock gives the oid; a row's, in a table's store, gives the
  rowno and the table, the committed one with that store
  (`sm3::cat_get_table_by_store()`); another key, such as a
  catalog key DDL locks, gives neither.
- **digres maps them to pg_locks' columns** (utils/adt
  `lockfuncs.c`, utils/init `datadir.c`'s `datadir_sm_locks()`):
  an oid's lock is a `relation`, or an `object` of
  `pg_namespace` for a schema of this database; a row's is a
  `tuple` at the row's tid (`smstore_rowno_to_tid()`), its
  `relation` NULL for a store no committed table has, as a
  truncate not yet committed makes; another key an `object`.
  The database is sm's id above the built-in oids, the pid the
  session's tag (step 172).
- **Modes are sm's names**, IS, IX, S, SIX and X, and an
  advisory lock's S or X, not PostgreSQL's eight.
- **Advisory locks** come from digres's own table
  (`advisory_status()`): each mode a holder holds, and each
  wait, keyed as PostgreSQL keys them (`classid`, `objid`,
  `objsubid` 1 or 2).
- `virtualxid`, `transactionid`, `virtualtransaction` and
  `waitstart` are NULL, `fastpath` false; `pg_locks` itself is
  not a view of digres's.
- Tests: sm1's `lock_status lists each lock held, and each
  waited for in lock`, sm3's `cat_get_table_by_store finds a
  committed table by its store`, and `test/server.sh`'s check
  of three sessions' table, row and advisory locks, held and
  waited for. sm1's and `server.sh`'s failed with the waits
  left out.
- Found on the way, left: `select ctid from t` fails, "relation
  2659 cannot be opened: digres has no catalog tables".

## Step 194: the proc array is gone

`SHMEM.md` §5. Storage/ipc `procarray.c` was left with
forwarders to the session registry and what had no other home.
Each piece is with its callers, and the file and its header are
gone.

- **Inlined**: `ProcArrayShmemInit()` (storage/ipc `ipci.c`
  makes the registry and the advisory table),
  `ProcArrayAdd()` and `ProcArrayRemove()` (storage/lmgr
  `proc.c` registers and unregisters), and
  `ProcArrayEndTransaction()` (access/transam `xact.c` clears
  the session's local id and xmin, at commit and at abort).
- **Moved**: `TransactionIdIsInProgress()` to access/transam
  `transam.c`, beside `TransactionIdDidCommit()`;
  `GetSnapshotData()` and the snapshot's xid limits (step 191)
  to utils/time `snapmgr.c`, static, their one user;
  `TerminateOtherDBBackends()` to commands `dbcommands.c`,
  static, DROP DATABASE FORCE's.
- **Gone**: `ProcArrayInstallImportedXmin()`, which always said
  no, so `snapmgr.c`'s `SetTransactionSnapshot()` reports the
  import failed, as it did, reached by nothing as
  `ImportSnapshot()` refuses first; and `CountOtherDBBackends()`,
  declared and never defined.

## Step 195: a system column is found by name

Found in step 193: `select ctid from t` failed, "relation 2659
cannot be opened: digres has no catalog tables". So did
`tableoid`, `xmin` and the rest, on a table in sm and on a
catalog relation.

- **The parser asked pg_attribute.** parser
  `parse_relation.c`'s `scanRTEForColumn()` looked up a system
  column's name in the `ATTNUM` syscache, to ask whether the
  relation has it; digres has no `pg_attribute` rows to find
  (`SYSCATALOG.md` §2). Every relation but a view has them
  (catalog `heap.c`'s `AddNewAttributeTuples()`), so the
  relkind answers instead.
- **EXPLAIN VERBOSE asked it too.** `get_rte_attribute_name()`
  names a system column from its compiled-in definition
  (`SystemAttributeDefinition()`).
- **What each gives.** `ctid` is the row's tid, its rowno
  (`smstore_rowno_to_tid()`), and `tableoid` the table's oid,
  which each read sets (step 190). `WHERE ctid =` runs a TID
  scan, and `UPDATE` and `DELETE` by ctid work. `xmin`, `xmax`,
  `cmin` and `cmax` are found now, and refused as the row is
  read: "cannot retrieve a system column in this context", as
  sm keeps no transaction ids in a row.
- `test/slt/system_columns.test`: ctid read plain and
  qualified, a row found, updated and deleted by its ctid,
  tableoid, and EXPLAIN's output naming ctid. Its answers
  are written by hand, as PostgreSQL 18 gives them: no
  PostgreSQL 18 here to fill them. 8 of its 10 records failed
  without the change.
- Left: a catalog relation's row has no tid, so its `ctid` is
  `(4294967295,0)`.

## Step 196: SUBSTRING ... SIMILAR with no match is null

`test/regress/README.md`'s bug in `strings`: `SUBSTRING('abcdefg'
SIMILAR '#"(b_d)#"%' ESCAPE '#')`, null on PostgreSQL, failed
with "function 0x... returned NULL".

- **The body called through DirectFunctionCall.** PostgreSQL
  writes `substring(text, text, text)` in SQL; digres has no
  SQL language (step 6), and its body, utils/adt
  `sql_bodies.c`'s `substring_similar()`, called
  `textregexsubstr()` with `DirectFunctionCall2Coll()`, which
  refuses a null result. `textregexsubstr()` is null when the
  pattern does not match. It is called with a call
  info of its own now, and its null passed on.
- Of `sql_bodies.c`'s other direct calls, none reaches a
  function that returns null.
- `test/select_text.sql` step 6: the PostgreSQL documentation's
  `oob`, and its null where the pattern does not match. It
  failed without the change.

## Step 197: name

`test/regress/README.md`'s second item: `name` was refused,
"type name is not supported yet", and `test_setup`'s `onek` and
`tenk1` have `name` columns, so 17 files lacked them.

- **sm2 has a type for it**, `name`, code 20
  (`../sm/sm2/TYPE.md` §3.5): PostgreSQL's `NameData`, 64 bytes,
  the string and then NULs, stored as they are, a fixed-width
  datum by reference as `uuid` is, so the tuple codec and digres
  need nothing new. Its `cmp` and key are a `text`'s of the
  bytes before the NULs: bytewise, as under collation `C`,
  which a `name` column has. The key decodes, padded again, so
  an index-only scan gives the value. 64 bytes a value is what
  PostgreSQL stores too; the storage's compression takes most
  of the padding back.
- **digres maps `name` to it and back** (access/sm
  `smstore.c`). PostgreSQL's `namein` clips a value to 63
  bytes, at a character's start, before sm sees it.
- `test/slt/name.test`, PostgreSQL 18's answers: lengths, the
  clip in one- and two-byte characters, order, equality with a
  name and with text, `LIKE`, grouping, an update, a unique
  index and its lookup, range and index-only scan, and a
  foreign key between name columns. 25 of its 27 records failed
  without the change. `../sm/sm2/test/test_type.cc` checks the
  type's bytes, limits, order and key.
- `test_setup` makes `onek` and `tenk1` now; `select` stops next
  at `onek2`, which is `CREATE TABLE AS`.

## Step 198: the partition functions answer for a table of sm's

`test/server.sh`'s `\d` checks failed on psql 18 with "relation
2662 cannot be opened": psql 18's `\d` asks which foreign keys
reference the table, through `pg_partition_ancestors()`, for
every table, where psql 16 asked only for one with triggers.
`check_rel_can_be_partition()` (utils/adt `partitionfuncs.c`)
asked `pg_class`'s syscache whether the relation exists; for a
table of sm's that misses the compiled-in rows and scans
`pg_class` through its index, `pg_class_oid_index`, which
digres has not. Whether `\d` failed was the plan's: a hash
semi join called the function, a nested loop, at nearly the
same cost, did not.

- **It says no for every relation**, as digres has no
  partitions: `CREATE TABLE` refuses `PARTITION BY` and
  `PARTITION OF`. `pg_partition_tree()`,
  `pg_partition_ancestors()` and `pg_partition_root()` give
  what PostgreSQL gives for a table not partitioned: no rows,
  and null.
- `test/server.sh` calls each on a table of sm's and on oid 0,
  so no plan can keep the call away; it failed before the
  change.
- Left as it was: any other `RELOID` syscache lookup of a
  table of sm's still opens `pg_class`'s index, and fails.

## Step 199: the time zone abbreviations, compiled in

`test/regress/README.md`'s first item: `timezone_abbreviations`
was empty, so `EDT`, `zulu` and `MET DST` were not read, and
`time`, `timestamp`, `timestamptz` and `horology` failed on
them.

- **Not loaded since step 9.** PostgreSQL reads a set from its
  installation's `share/timezonesets`, and digres has no
  installation; the start left `timezone_abbreviations` unset.
- **The sets are compiled in.** `src/tools/gen_tznames.py`
  writes utils/misc `tznames_builtin.c` from
  `src/timezone/tznames`: `Default`, `Australia` and `India`,
  the sets PostgreSQL installs. The `.txt` lists beside them
  are to copy from, and no set's name. `tzparser.c`'s
  `ParseTzFile()` reads a set's text a line at a time, as it
  read the file, with each check kept; a name that is no set's
  is a file that does not exist. A set of one's own needs a
  rebuild.
- **The start installs `Default`** (utils/misc `guc.c`'s
  `SelectConfigFiles()`), as PostgreSQL's does when the
  configuration file sets none.
- `test/slt/timezone_abbrevs.test`: the setting at start,
  `Default`'s abbreviations, `Australia`'s `EST` and `India`'s
  `IST`, `RESET`, and a name that is no set refused. Its
  answers are written by hand from the sets' offsets: no
  PostgreSQL 18 here to fill them. 5 of its 13 records failed
  with the start not installing `Default`.
- Left: `pg_timezone_abbrevs` is a view, which digres has not.

## Step 200: a row is named by its rowno; ctid and xmin are gone

Decided 2026-10-16, `../sm/PART4.md` §3: digres is a digression
from PostgreSQL, and says what it has not. sm keeps no
transaction ids in a row, and names a row by its `rowno`, not a
tid. Step 195 had made every system column found by name;
`xmin`, `xmax`, `cmin` and `cmax` were then refused only as a
row was read, and `ctid` was the rowno packed into a tid,
`(0,2)` for rowno 1.

- **`rowno` is a system column**, an `int8` (access
  `sysattr.h`'s `RownoAttributeNumber`, -7, and catalog
  `heap.c`'s `SysAtt`; `FirstLowInvalidHeapAttributeNumber` is
  -8). `slot_getsysattr()` gives the rowno the slot's tid
  carries (executor `execTuples.c`'s `ExecSlotRowno()`), as
  every read of sm sets the tid (step 190). A table in sm has
  it; a catalog relation, whose rows have no tid, has not.
- **The parser finds tableoid and rowno alone** (parser
  `parse_relation.c`'s `system_column_is_kept()`). `ctid`,
  `xmin`, `xmax`, `cmin` and `cmax` are no relation's column:
  `column "xmin" does not exist` (42703), as for any column a
  relation has not, with a hint saying why. A subquery's own
  column of that name is found, as it is in PostgreSQL.
- **The executor still keeps `ctid`**, the junk column
  `UPDATE`, `DELETE` and `WHERE CURRENT OF` carry; `EXPLAIN
  VERBOSE` of an `UPDATE` prints it.
- `test/slt/system_columns.test` is rewritten: rowno read,
  found, and returned; UPDATE and DELETE by it; tableoid; and
  ctid, xmin, xmax, cmin and cmax each read on PostgreSQL 18
  (`skipif digres`) beside its refusal on digres (`onlyif
  digres`). Without the parser's change it failed, and the
  server stopped on `select rowno from pg_namespace`, whose
  rows have no tid.
- `../sm/PART4.md` §3 and §6, `../sm/sm2/SM2_OBJECTIVE.md` and
  `../sm/sm2/SM2_DESIGN.md` §5 and §7 say so; `../sm/NEXT.md`
  items 3 and 4 are done, and two take their place.
- Left (`../sm/NEXT.md`): `WHERE rowno = n` reads the whole
  table, as the planner makes a TID scan of `ctid = tid` alone;
  and a missing qualified column of a table in sm, `t.nosuch`,
  fails with "does not have a composite type".
- CREATE TABLE of sm's (access/sm `smstore.c`) takes a column
  named as a system column is, `xmin` or `rowno`, which ALTER
  TABLE ADD COLUMN refuses; the user's column hides the system
  one, as in PostgreSQL. Left as it is.

## Step 201: snapshot export and import are gone

`../sm/NEXT.md` item 5. Step 149 made `pg_export_snapshot()`
and `SET TRANSACTION SNAPSHOT` refuse, and left behind them
what wrote a snapshot to a file in `pg_snapshots` and read one
back, reached by nothing.

- **Gone from utils/time `snapmgr.c`**: `ExportSnapshot()`,
  which nothing called; `ImportSnapshot()`'s body after its
  refusal, with its line parsers `parseIntFromText()`,
  `parseXidFromText()` and `parseVxidFromText()`, and
  `SetTransactionSnapshot()`, which installed what it read;
  `DeleteAllExportedSnapshotFiles()`, never called; the list
  of a transaction's exported snapshots, `exportedSnapshots`,
  and its cleanup in `AtEOXact_Snapshot()`; and
  `SNAPSHOT_EXPORT_DIR`.
- **Orphaned by it, and gone**: `GetMaxSnapshotXidCount()` and
  `GetMaxSnapshotSubxidCount()`, which only the export and the
  import asked, and access/transam `xact.c`'s
  `xactGetCommittedChildren()`, which only the export called.
- **The refusals stay**: `pg_export_snapshot()` is in
  `pg_proc`, and utils/misc `guc_funcs.c` calls
  `ImportSnapshot()` for `SET TRANSACTION SNAPSHOT`.
  `test/server.sh` checks both, as it did.
- Left: the postmaster still makes `pg_snapshots`, among the
  directories nothing opens (`../sm/NEXT.md` item 5, was 6).

## Step 202: DROP INDEX finds a temporary index first

`test/regress/README.md`'s bug in `temp`: with a table and a
temporary table both named `temptest`, each with an index
`i_temptest`, the second `DROP INDEX i_temptest` found none.

- **The first had dropped the wrong one.** access/sm
  `smstore.c`'s `find_index()` searched
  `fetch_search_path(false)`, which leaves out the implicit
  namespaces, the session's temporary one among them. So it
  dropped the table's index, and the temporary index went with
  its table. It searches the whole path now, as PostgreSQL's
  `RangeVarGetRelid()` does, and a temporary index hides one of
  the same name.
- **`pg_temp.` is found too.** A schema named in `DROP INDEX`
  was looked up by `get_namespace_oid()`, which knows no
  `pg_temp`: `schema "pg_temp" does not exist`. It is
  `LookupExplicitNamespace()` now, which knows it and checks
  `USAGE`, as PostgreSQL's lookup does.
- `test/slt/temp_shadow.test`, PostgreSQL 18's answers: the
  names hidden and found again as `temp` has them, and a drop
  named in `pg_temp`. 2 of its 18 records failed without the
  change. `temp` stops next at an expression index.

## Step 203: CREATE TABLE AS, into sm's bulk load

`test/regress/README.md`'s cheapest unblocking: `CREATE TABLE
AS` and `SELECT INTO` failed with `a relation's storage cannot
be made`, as `create_ctas_internal()` still made a heap and its
TOAST table.

- **The table is sm's.** commands `createas.c` makes it with
  access/sm `smstore_create_table()`, as `CREATE TABLE` does
  (`utility.c`): no TOAST table. `CREATE MATERIALIZED VIEW`
  still takes the old path, and fails there, as views do.
- **Its rows go to sm's bulk load.** `intorel_receive()` hands
  each to `table_multi_insert()`, which is `smstore_load()`
  (access/sm `smtable.c`), and `intorel_shutdown()`'s
  `table_finish_bulk_insert()` ends the load. The table is new
  and empty, so its rows go to sorted files the store takes
  whole. PostgreSQL inserts them a row at a time. A row's
  conversion allocates, so each is handed over in a memory
  context of its own, reset after it.
- **A savepoint set before it can be rolled back to**: since
  `a656446` a rollback past a load empties the table
  (`../sm/BULKLOAD.md` §3.5).
- **sm's statement advances after the table is made**, as
  PostgreSQL's command counter does at the same place:
  access/sm `smstore_statement_advance()`, over
  `sm_stmt_advance()` (`9cb37a0`). The statement's mark hid
  what it made, the table's store and its rowno sequence, so
  no row could be loaded, nor inserted once a value stored out
  of line ends the load. The snapshot stays the statement's,
  and sm's visibility rule is unchanged.
- **Left**: a `CREATE TABLE AS` that fails inside a savepoint
  after its first row ends the transaction, not the savepoint
  alone: its load is still open, sm refuses the rollback, and
  the next call into sm or the `COMMIT` fails, as after a
  `COPY` (`../sm/NEXT.md` §1). Nothing of it is committed.
- `test/slt/create_table_as.test`: the forms, a column list,
  `SELECT INTO`, `WITH NO DATA`, `IF NOT EXISTS`, 100,000 rows,
  values out of line, a temporary table, `EXECUTE`, and a
  rollback to a savepoint before it. Its answers are written by
  hand, as no PostgreSQL 18 was at hand; `make regress` could
  not be run for the same reason.

## Step 204: a cursor reads on after a rollback to a savepoint

`test/regress/README.md`'s crash in `transactions`: a cursor
declared before a savepoint, fetched from inside it, used
after `ROLLBACK TO SAVEPOINT` read freed memory. Its sm scan
began at its first `FETCH`, inside the savepoint, so sm1's
rollback closed and freed it, and dg closed the table handle
the scan read by, opened in the subtransaction.

- **A scan begins with its plan.** executor `nodeSeqscan.c`,
  `nodeIndexscan.c` and `nodeIndexonlyscan.c` begin the sm
  scan in `ExecInit*`, at `DECLARE`, not at the first row:
  the scan, its table handle and its lock all come before a
  savepoint set after. A cursor also reads the rows there
  were when it was declared, as PostgreSQL's does; it read
  the transaction's writes between `DECLARE` and its first
  `FETCH` before. An index scan with runtime keys still
  begins when they are known, and a scan begun again (a
  nested loop's inner side) at its next row.
- **sm1 closes a cursor without freeing it.** A cursor opened
  after the mark is closed by the rollback as before, but
  stays the session's: each read and seek fails with
  `SM_ERR_INVALID`, until `sm1::scanend()` or the
  transaction's end frees it (`../sm/sm1/SM1_DESIGN.md`
  §6.5). A cursor whose nested loop began its inner scan
  again inside
  the savepoint fails at its next `FETCH`, `could not scan a
  table: the cursor was closed by a rollback ...`, where
  PostgreSQL reads on.
- **Locks are released at the rollback, as before.** Keeping
  a table's lock past it is not needed: a cursor's locks are
  all taken at `DECLARE` now. And sm3 relies on the release:
  `need_empty()` (sm3 `ddl.cc`) gives back a table's X by a
  rollback to a savepoint set before it.
- `test/server.sh`: a cursor by seq scan and by index across
  a rollback to a savepoint, a cursor that does not see a row
  inserted after `DECLARE`, and the nested loop's error; the
  first three failed before, the first stopping the server.
  sm1 `test_savepoint.cc`: the closed cursor's errors, and
  its free at commit.

## Step 205: a rollback sm refuses dooms the transaction

`../sm/NEXT.md`'s bug: access/sm `smstore_subabort()`, which
`AbortSubTransaction()` calls, discarded `sm_rollback()`'s
error, taking a savepoint sm could not undo for a transaction
sm had ended. But a rollback sm refuses changes nothing and
leaves its transaction running, with the writes it was to
undo: past `savepoint_undo_bytes` (`SM_ERR_UNDOLIMIT`), or
with a bulk load open (`SM_ERR_INVALID`).

- **An open load is ended first.** A `COPY` or a `CREATE
  TABLE AS` that fails after its load began leaves it open;
  `end_load_quietly()` ends it before the rollback, which then
  empties the table, as since `a656446`. So the failed
  statement rolls back to the savepoint and the transaction
  goes on, as in PostgreSQL. Its rows are written and then
  deleted, on a path that is rare.
- **A rollback still refused dooms the transaction.** In an
  abort an error has nowhere to go, so sm's message is kept in
  `tcb->smstore`. The next call into sm (`xact_session()`)
  fails with `the transaction cannot go on`, and `COMMIT`
  (`smstore_commit()`) with `could not commit`, aborting sm's
  transaction: what the rollback was to undo never commits.
  The transaction's end clears it.
- **Where digres still differs**: a load whose end fails, on a
  duplicate key, stays open in sm, so its rollback is refused
  and the transaction doomed, where PostgreSQL goes on.
- `test/slt/savepoint_load.test`: a `COPY` failing after 1,000
  rows, and a `CREATE TABLE AS` failing mid-load, each in a
  savepoint, rolled back to and committed; a duplicate key
  failing a load's end, then the next statement, and the
  commit, refused (`onlyif digres`, beside PostgreSQL's
  answers). 5 of its 49 records failed before. The cap's path
  is untested (`../sm/NEXT.md` §2).

## Step 206: a load whose end fails rolls back to its savepoint

Step 205's last difference from PostgreSQL. A `COPY` into an
empty table with a primary key, two equal keys in its rows,
fails at its load's end with `SM_ERR_DUPKEY`. sm1's
`load_close()` kept its loader open on a failure, for the
abort it asked for, though sm3 had freed its own handle; so
nothing could close the load, the rollback to the savepoint was
refused, and the transaction was doomed.

- **sm1: a close that fails ends the load.** It frees the
  loader and its files, as an abort would. The store holds
  nothing of the load: its write batch, or its ingest, applies
  whole or not at all (`../sm/BULKLOAD.md` §3.5). The rollback
  is let in and finds the store empty, and the transaction goes
  on, as in PostgreSQL. digres is unchanged.
- **The doom stays**, for a rollback past
  `savepoint_undo_bytes`; it is now untested
  (`../sm/NEXT.md` §2).
- **A rollback sm refuses is logged**: `smstore_subabort()`
  writes a `WARNING` with sm's reason, which does not throw in
  an abort, before dooming the transaction.
- `../sm/sm1/test/test_load.cc`: a close failing on a
  duplicate key, in either of a load's forms, then the
  rollback, a write and the commit. It failed before.
- `test/slt/savepoint_load.test`: the duplicate key's case now
  has PostgreSQL's answers on digres: rolled back to, the table
  read empty, a row inserted and committed.

## Step 207: sm dooms a transaction whose rollback failed

Step 205 doomed the transaction in digres: `smstore_subabort()`
kept the message of a rollback sm refused, and digres failed the
next statement and the commit itself. The doom now lives in sm,
where every caller gets it.

- **sm1: a rollback that fails dooms its transaction.** At a
  savepoint lost past `savepoint_undo_bytes`, or on an I/O
  error, it sets the session's `doomed` to the new
  `SM_ERR_DOOMED` and keeps its own error as the reason. Every
  later call on the transaction fails with `SM_ERR_DOOMED`:
  those that check `doomed` on entry, as for a deadlock victim,
  and an open cursor's read, and a sequence draw. Only the
  abort is let through. Unlike a deadlock victim's, the
  transaction is not aborted at once
  (`../sm/sm1/SM1_DESIGN.md` §6.5).
- **A rollback refused by its checks**, at a depth with no
  savepoint or with a bulk load open, changes nothing and dooms
  nothing.
- **digres drops its own doom.** `smstore_subabort()` only logs
  the failed rollback as a `WARNING`; sm's `SM_ERR_DOOMED` is
  raised by the next statement and the commit, as SQLSTATE
  `25P02`, in failed SQL transaction.
- `rel`: `db-open` takes `:savepoint-undo-bytes`.
  `test/test_doomed.rel` sets it to 1 byte, fails a rollback,
  and checks that 15 calls fail with `ERR_DOOMED`, through
  sm3's tables, indexes and catalog, and that the abort leaves
  nothing. This closes `../sm/NEXT.md`'s loose end: the cap's
  path is tested, in sm.
- `../sm/sm1/test/test_savepoint.cc`: the cap's three tests no
  longer go on after `SM_ERR_UNDOLIMIT`, and a new one checks
  each kind of call in a doomed transaction.

## Step 208: CREATE TABLE takes WITH (fillfactor, autovacuum_enabled)

`test/regress/README.md`'s third item: 5 files stopped at
`CREATE TABLE ... WITH (...)`, which access/sm
`check_create_table()` refused whole.

- **Two storage parameters are taken, and ignored.** sm has no
  pages to fill and no autovacuum (step 11), so `fillfactor`
  and `autovacuum_enabled` change nothing and are kept nowhere.
  `check_storage_options()` checks their values as PostgreSQL
  does: `fillfactor` through `heap_reloptions()`, its range of
  10 to 100 and given once; `autovacuum_enabled`, whose
  reloption went with autovacuum, with `parse_bool()` and
  PostgreSQL's words. `CREATE TABLE AS` takes them the same way.
- **Any other is refused**, `toast.` ones included: `storage
  parameter "x" is not supported`.
- `test/slt/create_table_with.test`: each taken alone, together
  and on `CREATE TABLE AS`; the bad values; the others refused
  (`onlyif digres`, beside PostgreSQL's answers, written by
  hand: no PostgreSQL 18 here).

## Step 209: the postmaster makes no legacy directories

`../sm/NEXT.md` §1's fifth item. postmaster
`make_legacy_dirs()` made eight directories at start that
nothing opened any more: `pg_dynshmem` (step 29), `pg_stat`
and `pg_stat_tmp` (step 28), `pg_replslot` and `pg_logical`'s
three (step 2), and `pg_snapshots` (step 201).

- **The function is gone**, its call and its prototype with it.
  A data directory made before keeps the empty directories;
  nothing reads them.

## Step 210: no std::optional in sm.h

`../sm/NEXT.md` §1's second item: `../sm/CODE.md` §2 lets no
`std::optional` cross to sm's users, and
`sm_column_def_t::default_expr` was one.

- **`bool has_default` beside a plain `std::string`** replaces
  it; the string is empty without a default. sm `api_def()`
  fills both from sm3's column, which keeps its
  `std::optional`: sm3's header is not the interface.
- access/sm `add_table()` and `smstore_table_desc()` read
  `has_default`.

## Step 211: shmem_t, what the threads share and write

The pcb held two kinds of field under one rule, "set before
the first thread starts and only read after, or guards
itself", and every field it had was of the second kind. Now
each kind has a block of its own (`utils/control_block.h`,
`CODE.md` section 3):

- **`shmem_t`, what the threads share and write**: the pcb's
  fields, all of them, moved as they were. Each is an atomic,
  a mutex whose comment names what it guards, or an object
  that guards itself: the stderr, ALTER SYSTEM and cancel-key
  mutexes, the injection points' mutex and table, `next_lxid`,
  `quit_reason`, the session registry and the advisory table.
  `shmem->` for `pcb->` in their eight files.
- **`pcb_t`, what the main thread fixes before the first
  thread starts**, read through `const pcb_t *pcb`, so a write
  from a session does not compile. It is empty: a SHARED
  variable no thread writes after the start comes here as a
  change touches it.
- **`tcb_t`, the thread's alone**, as before; what it inherits
  from the thread that started it is copied in at its start,
  and is its own after. A SHARED variable a session changes,
  a setting, comes here, not to `pcb_t`: the postmaster's
  settings change at a reload, after threads have started.
- `pcb_init()` makes `pcb` and `shmem`, and
  `src/tools/process_wide.txt` allows both outside
  `thread_local`. A guard type for a mutex and its data was
  weighed and not made: no field pairs a mutex with data that
  is reached only under it. The injection points' table is
  read without the lock, by its generation counters.

## Step 212: t.nosuch on a table in sm is a missing column

`../sm/NEXT.md` §1's third item. On a table in sm, `select
t.nosuch from t` failed with `relation "t" does not have a
composite type`. The parser, finding no column `nosuch`, tries
`nosuch(t)` as a function on the row (parser
`transformColumnRef()`), and a table in sm has no row type to
make the whole-row reference with (nodes `makeWholeRowVar()`).

- **The function is not tried on a relation without a row
  type**, parser `has_row_type()`: no function can take a row
  that has no type, so nothing is lost. The name then fails as
  PostgreSQL's does, `column t.nosuch does not exist`, with its
  position. Qualified by schema and table, or by an alias, the
  same.
- **A whole-row reference still fails** as before: `select t
  from t`, `(t).a`, `row_to_json(t)`. A table in sm would need
  a row type for those.
- `test/slt/missing_column.test`: the missing column named
  with the table, its schema, an alias, in `WHERE` and in a
  subquery, and with a catalog relation. Without the change 4
  of its 8 records failed.

## Step 213: WHERE rowno = n is a TID scan

`../sm/NEXT.md` §1's second item. A query names a row by its
`rowno` (step 200), and `WHERE rowno = n` read the whole table
with a filter: the planner made a TID scan of `ctid = tid`
alone. The row's tid carries its rowno
(`smstore_rowno_to_tid()`), and sm's table access method
fetches a row by its tid already, for `WHERE CURRENT OF`.

- **The planner takes `rowno = n` as it takes `ctid = tid`**
  (optimizer `tidpath.c`): `IsRownoVar()` and
  `IsRownoEqualOperator()`, the equality of `integer_ops`, so
  `n` may be an `int2`, `int4` or `int8`, on either side. With
  it come `rowno IN (...)`, `rowno = ANY (array)`, an `OR` of
  them, and a join on `rowno`, whose inner side is a TID scan
  for each outer row.
- **The executor makes the tid of each rowno** (executor
  `nodeTidscan.c`'s `RownoTid()`). A rowno no tid holds,
  negative or too large, fetches nothing, through access/sm
  `smstore_rowno_fits_tid()`, which `smstore_rowno_to_tid()`
  now calls; it still fails on one.
- **A TID scan is rescanned** (access/sm
  `smtable_scan_rescan()`), as a nested loop does for each outer
  row: it has nothing to start again, as it never reads its
  cursor. Any other rescan through the access method is still
  refused.
- **The cost is PostgreSQL's**: a fetch by tid costs a random
  page. On a table of one page a sequential scan costs less,
  and is chosen.
- **A range, `rowno > n`, is still a sequential scan**: sm's
  access method has no TID range scan.
- `test/slt/rowno_scan.test`: rowno = n, IN, ANY and OR, each
  a TID scan; rownos no row has; another condition beside it;
  update and delete by rowno; a join, plain and left; a range.
  Without the change its 5 plans were sequential scans.

## Step 214: the replication catalogs are purged

`../sm/NEXT.md` §1's last cleanup: step 2's known leftovers,
the catalogs of replication, which nothing has written since
step 2 cut replication.

- **`pg_subscription`, `pg_subscription_rel` and
  `pg_replication_origin` are gone**, headers and all, and with
  them what `genbki.pl` made of them: their `schemapg.h` entry,
  their syscaches, their foreign keys in `system_fk_info.h`.
  catalog `IsSharedRelation()` and
  `IsCatalogTextUniqueIndexOid()` no longer list them. `select
  from pg_subscription` already failed: digres never served it.
- **`pg_publication` and its two relations stay, empty**
  (`SYSCATALOG.md` §2): psql's `\d name` reads them for a
  table's publications. Their headers keep the catalog and its
  indexes, for `cat_builtin.c`; their syscaches go, and so does
  the rest of PostgreSQL's header, `Publication`,
  `PublicationDesc` and the like.
- **What read them as objects is gone**: lsyscache
  `get_publication_oid()`, `get_publication_name()`,
  `get_subscription_oid()` and `get_subscription_name()`;
  relcache `RelationBuildPublicationDesc()`, which nothing
  called; their cases in objectaddress (the object property
  table, a description, an identity, a type) and
  `getPublicationSchemaInfo()`; alter's duplicate-name and
  subscription checks; dependency's, and tablecmds' column
  of a publication's `WHERE`. The prototypes
  `CheckCmdReplicaIdentity()` and `CheckSubscriptionRelkind()`
  had no body.
- **The statements still parse and are refused**, as step 2
  left them: `publications are not supported`, `subscriptions
  are not supported`. Their nodes and `OBJECT_PUBLICATION`,
  `OBJECT_SUBSCRIPTION` and the like stay.
- **`pg_describe_object()` of a publication's class** fails
  with `unsupported object class`, where it failed before on
  `relation 6110 cannot be opened`.
- access `tableam.h` includes `nodes/lockoptions.h` for
  `LockTupleMode`: it had it through utils `rel.h`, which
  included `pg_publication.h` for nothing.
- Left: the replication wait events in
  `utils/activity/wait_event_names.txt`, slots, WAL senders and
  receivers, logical apply, which nothing waits on.

## Step 215: replication's dead declarations

Step 2 cut replication, and steps since took its xlog and
proc-array parts (steps 18, 151). What was left had no use:
names, declarations and catalog text of a subsystem that is
gone.

- **The walsender backend type**, `B_WAL_SENDER`, and
  `AmWalSenderProcess()`: no connection becomes one, as a
  replication connection is refused at its startup packet. The
  postmaster's masks, its child pool and the backend names no
  longer list it.
- **`nodes/replnodes.h`**, the replication commands' nodes, out
  of `NODE_HEADERS` and nodes `gen_node_support.pl`'s lists.
- **Bodiless or uncalled**: tcop `EndReplicationCommand()`;
  executor `execReplication.c`'s six prototypes; relcache
  `RelationGetIdentityKeyBitmap()`, for logical decoding; inval
  `CacheInvalidateRelcacheAll()`, for ALTER PUBLICATION.
- **Constants nothing uses**: the GUC group
  `REPLICATION_SENDING`, the timeouts `STANDBY_*`,
  `PM_STATUS_STANDBY`, `RepOriginId`,
  `LOCKTAG_APPLY_TRANSACTION`, and two empty section comments
  in `pg_proc.dat`.
- **35 wait events**: logical apply and sync, WAL sender and
  receiver, synchronous replication, replication slots and
  origins, reorder buffer, snapshot builder, logical rewrite.
- **Kept, as live**: `session_replication_role`, which fires
  triggers, rules and event triggers by it; the `REPLICATION`
  role attribute, `rolreplication`, which psql reads; replica
  identity in `pg_class` and `pg_index`; and
  `pg_relation_is_publishable()`, which psql's `\d` calls
  (step 214). `postgresql.conf.sample` is not maintained (step
  11), and still lists replication's settings.
- Left for the next step: logical decoding's hooks in live
  code, `CheckXidAlive` and `bsysscan` in the table access
  method and genam, historic snapshots in snapmgr and the
  relcache, and the relsync invalidation messages.

## Step 216: logical decoding's hooks in live code

Step 215's last item. Logical decoding went with step 2, but
three of its hooks stayed in code every statement runs, each
waiting on a state nothing sets now.

- **`CheckXidAlive` and `bsysscan`** (access/transam
  `xact.c`): the transaction being decoded, and whether a
  catalog scan was under way, which logical decoding set to
  catch a decoded transaction's abort. Nothing sets either, so
  the six tests in access `tableam.h`'s inline wrappers and the
  one in `tableam.c` were always false, and access/index
  `genam.c` set and cleared `bsysscan` and called
  `HandleConcurrentAbort()` for nothing. All of it is gone.
- **Historic snapshots** (utils/time `snapmgr.c`):
  `SetupHistoricSnapshot()`, with no caller, so
  `HistoricSnapshotActive()` was always false in
  `GetTransactionSnapshot()`, `GetLatestSnapshot()`,
  `GetCatalogSnapshot()` and the relcache's
  `RelationRebuildRelation()`. `SNAPSHOT_HISTORIC_MVCC` goes, and
  `IsMVCCSnapshot()` asks only for `SNAPSHOT_MVCC`.
  `GetNonHistoricCatalogSnapshot()` keeps its name.
- **The relsync invalidation message** (utils/cache `inval.c`,
  `storage/sinval.h`): `SHAREDINVALRELSYNC_ID`, for pgoutput's
  cache, with `CacheInvalidateRelSync()`, its callbacks and
  their registry. Nothing sent one.

## Step 217: a COPY PROGRAM program starts with its own signals

storage/file `fd.c`'s `OpenPipeStream()` ran `popen()`, as
PostgreSQL does, around `pqsignal(SIGPIPE, SIG_DFL)` and back
to `SIG_IGN`, so that the program would take SIGPIPE at its
default. In a session thread that `pqsignal()` does nothing
(port `pqsignal.c`: a session's handlers are its own, and the
process's stay the postmaster's), and `popen()` hands the
program the calling thread's state:

- **Every signal blocked but the faults**, the session thread's
  mask (utils/init `session_thread.c`). The shell runs a
  script's last simple command by `exec`, so `COPY ... TO
  PROGRAM 'gzip > f'` ran gzip so: a `kill` did not stop it,
  only SIGKILL.
- **SIGPIPE, SIGALRM, SIGUSR1 and SIGUSR2 ignored**, as the
  postmaster ignores them, inherited through every fork and
  exec. In `seq 1 100000 | head -2`, `seq` failed with `write
  error: Broken pipe` in the server's log, where in PostgreSQL
  it ends quietly on SIGPIPE.

- **`spawn_pipe()` runs `sh -c` by `posix_spawn()`** with the
  signal state a PostgreSQL backend's program has: nothing
  blocked (`POSIX_SPAWN_SETSIGMASK`), and every signal at its
  default but SIGTTIN, SIGTTOU and SIGXFSZ
  (`POSIX_SPAWN_SETSIGDEF`). Nothing of the process changes,
  so other threads see no signal handled otherwise meanwhile.
  The pipe is made close-on-exec, and `dup2()` gives the
  program its end as stdin or stdout.
- **`close_pipe()` closes our end, then waits** for the
  program's pid, kept in its `AllocateDesc`: `pclose()`'s
  wait status, so `ClosePipeStream()`'s callers read it as
  before. The `fflush(NULL)` before `popen()` goes: nothing is
  forked with our stdio buffers.
- `test/slt/copy_program_signals.test`: the program's blocked
  and ignored signals, read from `/proc/self/status`, and a
  pipeline whose writer outlives its reader. Without the change
  2 of its 9 records failed: `SigBlk` was `fffffffe7ffbfa17`,
  and SIGPIPE ignored.

## Step 218: postmaster_tcb_t, the postmaster's own state

`../sm/NEXT.md` §1's second item, its first batch, under the
rule this step states (`CODE.md` §3): `pcb_t` holds what the
main thread owns and other threads read; what it owns and no
other thread reads is in a block of its own,
`postmaster_tcb_t`. 18 `/*SHARED*/` globals were the
postmaster's alone, copied into every session as it started
and never read there: a session's end is queued and the
postmaster cleans up after it (`CleanupBackend()`), signals
reach the postmaster's thread alone, and the exit callbacks
that remove lock and socket files are the main thread's.

- **`postmaster_tcb_t`** (`utils/control_block.h`), reached
  through `thread_local /*LOCAL*/ postmaster_tcb_t
  *postmaster_tcb`: `main()` makes it for the main thread
  after its tcb (`postmaster_tcb_init()`), whether it is to be
  the postmaster or a standalone backend, and never frees it.
  It is NULL in every other thread, so a session cannot reach
  the postmaster's state.
- **postmaster `postmaster_state_t`**,
  `postmaster_tcb->postmaster`, made as `PostmasterMain()`
  starts: `NumListenSockets`, `ListenSockets`, `Shutdown`,
  `FatalError`, `pmState`, `connsAllowed`, `AbortStartTime`,
  `ReachedNormalRunning`, the four `pending_pm_*` signal flags
  and `pm_wait_set`, read through `pms()`.
- **pmchild `pmchild_state_t`**, `postmaster_tcb->pmchild`,
  made by `InitPostmasterChildSlots()`, the postmaster's or a
  standalone backend's: `pmchild_pools`, `num_pmchild_slots`
  and `ActiveChildList`, which postmaster reads through
  `pmchild_active_list()`.
- **`postmaster_tcb->lock_files`** (miscinit) and
  **`postmaster_tcb->sock_paths`** (libpq `pqcomm.c`), lists
  in `TopMemoryContext`.
- 71 `/*SHARED*/` globals are left.

## Step 219: pcb_t's first fields

`../sm/NEXT.md` §1's second item, its second batch: 15
`/*SHARED*/` globals the main thread sets as the server or a
standalone backend starts, and sessions read, are `pcb_t`'s
(`utils/control_block.h`).

- **globals**: `DataDir`, `OutputFileName`, `my_exec_path`,
  `pkglib_path`, `PostmasterPid`, `IsPostmasterEnvironment`,
  `IsBinaryUpgrade`, `MaxBackends`. **common `file_perm.c`**:
  `pg_dir_create_mode`, `pg_file_create_mode`,
  `pg_mode_mask`. **fd**: `max_safe_fds_per_thread`, whose start,
  `FD_MINFREE`, a `static_assert` in `fd.c` checks.
  **timestamp**: `PgStartTime`. **miscinit**:
  `process_shared_preload_libraries_in_progress` and `_done`.
- **Every thread reads them through `const pcb_t *pcb`**
  (`pcb->DataDir`); a write elsewhere does not compile. **The
  main thread writes through `pcb_main()`**, which asserts it
  is the main thread (`postmaster_tcb` is set). Each write is
  made before the first session starts: `SetDataDir()`,
  `SetDataDirectoryCreatePerm()`, `set_max_safe_fds()`,
  `InitializeMaxBackends()`,
  `process_shared_preload_libraries()`, and the postmaster's
  and a standalone backend's options and start. `-b` and `-r` in
  `process_postgres_switches()`, which a session runs too, are
  taken only from the command line.
- **`max_safe_fds` is `max_safe_fds_per_thread`**: a thread's
  share of the process's fds (step 135's ceiling over the
  threads), which PostgreSQL's name, from when a backend was a
  process, did not say.
- postmaster `CreateOptsFile()` takes its path `const`.
- **Left SHARED, on purpose**: `progname`, whose address is
  the copy table's anchor (`session_thread.c`'s `tls_block()`),
  until the table goes; `data_directory_mode`, a setting, and
  settings are each thread's (`PLAN.md` §7).
- 56 `/*SHARED*/` globals are left.

## Step 220: the modules' own state in pcb_t

`../sm/NEXT.md` §1's second item, its third batch: 6
module-private `/*SHARED*/` statics the main thread sets as it
starts, and sessions read.

- **Each module's state is a struct it defines** (`CODE.md`
  §3), made by `pcb_init()` through the module's `*_new()`,
  and held by a plain pointer in `pcb_t`, never freed:
  datadir `datadir_state_t`, `pcb->datadir` (the superuser's
  name and SCRAM secret, and sm's home directory, `sm_env`);
  ps_status `ps_status_state_t`, `pcb->ps_status` (`save_argc`
  and `save_argv`); auth-scram `auth_scram_state_t`,
  `pcb->auth_scram` (`mock_auth_nonce`). A `std::unique_ptr`
  would make `pcb_t` no literal type, which fd's
  `static_assert` on its `max_safe_fds_per_thread` needs.
- **const does not reach through those pointers**, so a module
  reads its state through a const reference (datadir `dds()`,
  ps_status `pss()`), and writes through `pcb_main()`
  (datadir `dds_main()`): `load_superuser()`,
  `datadir_load()`'s `sm_attach()`, `detach_sm()` at exit,
  `save_ps_display_args()`, `scram_init_mock_nonce()`.
- **main `reached_main` is gone**: it told the sanitizer's
  `__ubsan_default_options()` that `main()` had run, which
  `postmaster_tcb` being set now says.
- 49 `/*SHARED*/` globals are left.

## Step 221: three constants are const

`../sm/NEXT.md` §1's second item, its fourth batch: three
`/*SHARED*/` globals were constants nothing writes, made
`thread_local` only because they were not `const` all the way
down.

- common `md5_common.c`'s `hex`, in `bytesToHex()`, is a
  `static const char[]`, in `.rodata`.
- common `encnames.c`'s `pg_enc2gettext_tbl` is `const char
  *const[]`, and tcop `dest.c`'s `None_Receiver` is
  `DestReceiver *const`: both in `.data.rel.ro`, which
  `src/tools/cxx_thread_local.py` counts read-only. Their
  `extern` declarations (`mb/pg_wchar.h`, `tcop/dest.h`) say
  `const` too, which also gives the definitions external
  linkage.
- 46 `/*SHARED*/` globals are left.

## Step 222: the hooks are the thread's, copied at its start

`../sm/NEXT.md` §1's second item, its fifth batch: the 30 hooks
a loaded library sets, each a `/*SHARED*/` global, so that a
session started with the postmaster's: what
`shared_preload_libraries` set there reached every session,
and a library a session `LOAD`s set that session's alone.

- **`hooks_t`** (`utils/hooks.h`) holds them, each NULL until a
  library sets it, and **`tcb->hooks`** is the thread's,
  made by `tcb_init()`. A module calls its hook as
  `tcb->hooks->planner_hook`; `fmgr.h`'s `FmgrHookIsNeeded()`
  and `catalog/objectaccess.h`'s `Invoke...Hook()` macros read
  it so, and a file that uses one includes `utils/hooks.h`.
- **A session starts with its parent's**:
  `session_thread_start()` copies the parent's `hooks_t` into
  what it hands the new thread, which takes it after its
  `tcb_init()`. The behaviour is the one the SHARED copy gave.
- **An extension sets `tcb->hooks->X`** where it set the global
  `X`: the hooks' names are now fields. No library in the tree
  sets one, so no test calls a hook.
- elog's `emit_log_hook` is asked only where the thread has a
  block: a thread may report before `tcb_init()`.
- **Left, and not safe as copied**: dfmgr's list of loaded
  libraries (`file_list`, `file_tail`) and the explain
  extension registry (`ExplainExtensionNameArray` and the rest)
  are still SHARED, a pointer copy each. A session's `LOAD`
  appends to the list through the parent's last node, and a
  session's registration reallocates the parent's array, both
  from another thread: they need copies of their own, not the
  parent's pointers.
- 16 `/*SHARED*/` globals are left.


## Step 223: the loaded libraries and EXPLAIN registry are the thread's

`../sm/NEXT.md` §1's second item, its sixth batch, and the
hazard step 222 left: dfmgr's list of loaded libraries and the
EXPLAIN extension registry were SHARED, a pointer copy each,
so a session's copy was the postmaster's memory. With
`shared_preload_libraries` set, a session's `LOAD` linked its
file through the postmaster's last node, and a session's
`GetExplainExtensionId()` wrote the postmaster's array, or
`repalloc`ed it from another thread.

- **`tcb->dfmgr`** (`dfmgr_state_t`, dfmgr.c) is the session's
  list, a `std::vector` of its files, oldest first, in place of
  `file_list`, `file_tail` and the files' `next` links. A file
  once loaded is never written or freed, so a session's list
  holds its parent's files themselves; `get_next_loaded_module()`
  finds the file in the list.
- **`tcb->explain`** (`explain_registry_t`, explain_state.c) is
  the session's extension names, by ID, and options, two
  `std::vector`s in place of the six arrays and counts. The
  names are the extensions' own, never freed, and shared.
- **A session starts with its parent's**:
  `session_thread_start()` copies both, as it copies the hooks,
  and the new thread takes them after its `tcb_init()`. Each is
  the session's alone after: a `LOAD` or a registration changes
  no other's.
- No library is built in the tree, so no test loads one.
- A library's own statics are still the process's: a second
  session that `LOAD`s it runs its `_PG_init()` again on them.
  `ISSUE.md` issue 1 records it, not to be fixed soon, and
  `internal_load_library()` asserts that no session thread
  loads a library.
- 10 `/*SHARED*/` globals are left; step 222's 16 counted the
  three signal masks, declared together, as one.

## Step 224: the postmaster reads its signals from a signalfd

`../sm/NEXT.md` §1's second item, its seventh batch. Only the
main thread took a signal, and only to set a flag and wake its
own loop; sessions blocked every signal but their faults and
are interrupted by bits. Each thread still carried three masks
(`BlockSig`, `UnBlockSig`, `StartupBlockSig`) and a table of
handlers (`pqsignal_handlers`), all SHARED, and a session
switched among masks that were the same.

- **`PostmasterMain()` blocks SIGHUP, SIGINT, SIGQUIT and
  SIGTERM before any thread starts**, so every thread inherits
  the mask, and opens a signalfd for them, which
  `ConfigurePostmasterWaitSet()` adds to the postmaster's wait
  set. `read_pm_signals()` reads it in `ServerLoop()` and sets
  the pending flags the handlers set, now plain `bool`s; no
  handler runs, and no thread's system call is interrupted.
  The other signals stay ignored, and faults keep their
  default action.
- On Linux `WL_SOCKET_ACCEPT` is `WL_SOCKET_READABLE`:
  `ServerLoop()` must not `accept()` on the signalfd.
- **Gone**: `libpq/pqsignal.c` (`pqinitmask()` and the masks),
  every `pthread_sigmask()` a backend made (`xact.c`,
  `postgres.c`, `backend_startup.c`, `miscinit.c`,
  `session_thread.c`), and the mask `datadir_load()` set around
  `sm_attach()`. `port/pqsignal.c` keeps `pqsignal()`, which
  installs the function itself: `wrapper_handler` and its
  table are gone. A session's `pqsignal()` still sets nothing.
- **A standalone backend blocks no signal**, as before its
  `PostgresMain()` unblocked them all: SIGINT or SIGTERM ends
  it, having no handler.
- `server.sh`: the mask check now wants every thread, the
  postmaster's too, to block the four. New checks: a
  `pg_reload_conf()` reaches the postmaster, which takes the
  signalfd for no listen socket, and a smart shutdown
  (SIGTERM) refuses new sessions and stops when the last ends.
  Interrupting a blocked query still works: a client's cancel
  request, `statement_timeout` and `lock_timeout` were checked
  already, and `pg_cancel_backend()` of a session waiting for
  an advisory lock now is too, which leaves the session. A
  reload in the test's data directory applies nothing: it has
  no `postgresql.conf` (`PLAN.md` §9).
- 6 `/*SHARED*/` globals are left.

## Step 225: PgReloadTime is the main thread's, in pcb_t

`../sm/NEXT.md` §1's second item, its eighth batch.
`PgReloadTime`, what `pg_conf_load_time()` reports, was SHARED:
a session started with the postmaster's, then wrote its own when
it re-read the file on `INTERRUPT_RELOAD`, a moment after the
postmaster.

- **`pcb->PgReloadTime`**, a `std::atomic<TimestampTz>`: the
  main thread writes it after threads start. `pcb_t` stays a
  literal type (fd.c's `static_assert`).
- **Only the main thread writes it**:
  `ProcessConfigFileInternal()` stores it when it applies the
  file and is not in a session thread, so the postmaster or a
  standalone backend. A session still re-reads the file, to
  apply it to its own settings, but leaves the time be.
- Re-reading the file once, in the main thread, and having
  sessions apply its result was weighed and not taken: a
  session must still apply the settings to its own GUCs, keep
  what it `SET` and reset what was removed, which is most of a
  reload; the parse saved is small and rare.
- Without a configuration file, as `initdb` leaves the data
  directory, nothing is applied and `pg_conf_load_time()` is
  2000-01-01, as before.
- `server.sh`: after `pg_reload_conf()` with a configuration
  file, a held session and a new one report the same load
  time, later than before.
- 5 `/*SHARED*/` globals are left.

## Step 226: the CPU dispatch pointers are plain globals

`../sm/NEXT.md` §1's second item, its ninth batch.
`pg_comp_crc32c`, `pg_popcount_optimized` and
`pg_popcount_masked_optimized` were marked SHARED, but none is
compiled: `pg_config.h` chooses slicing-by-8 and no runtime
check, and `src/port/Makefile` skips the processor's files
(bc6b8c1). The copy table held none of them.

- Each is a plain global again, as `pg_popcount32` and
  `pg_popcount64` beside them are, and as the definitions in
  `pg_bitutils.c` and `pg_crc32c_sse42_choose.c` already were:
  a build with the option would not have compiled. The first
  call chooses, and every thread chooses the same.
- Not `pcb_t`: `src/port` is linked into the frontend programs
  too, which have no `pcb`.
- A build that turns an option on lists the pointers in
  `src/tools/process_wide.txt`, for
  `cxx_thread_local.py --check`.
- 2 `/*SHARED*/` globals are left: `progname` and
  `data_directory_mode`.

## Step 227: server.sh fails when the server does not start

A `make -j4 tsan` run failed `server.sh`'s max_connections check:
the server took thirteen seconds to log that it was ready, the
wait gave up after ten without a word, and the three sessions
connected before the socket was there. Started alone, the tsan
server is ready in about a second: the machine was loaded.

- **`server_ready()`**, in `server.sh`: waits up to thirty
  seconds for "ready to accept connections", stops at once if
  the server has exited, and returns false on either. The four
  starts use it, and each fails its check by name ("start",
  "max_connections: start", ...) rather than going on.
- `isolation.sh`, `run.sh`, `slt.sh` and `regress.sh` already
  fail when the server does not start, after ten seconds.

## Step 228: data_directory_mode is gone

`../sm/NEXT.md` §1's second item, its tenth batch.
`data_directory_mode` was SHARED: `checkDataDir()` assigned it
directly, not through GUC, and a session was given the
postmaster's copy. It was the variable of a read-only setting,
which `pg_basebackup` and `pg_receivewal` read over a
replication connection to give their files the server's mode.
digres refuses a replication connection and has no WAL.

- The variable, its setting, `show_data_directory_mode()` and
  the assignment in `checkDataDir()` are removed, and
  `globals.c`'s include of `common/file_perm.h`, which only it
  used. `SHOW data_directory_mode` is now an unrecognized
  parameter.
- The data directory's mode still sets every file and
  directory digres makes: `pcb->pg_dir_create_mode`,
  `pg_file_create_mode` and `pg_mode_mask`, as before.
- 1 `/*SHARED*/` global is left: `progname`.

## Step 229: progname is pcb's, and the copy table is gone

`../sm/NEXT.md` §1's second item, its last batch. `progname` was
the last SHARED global, and its address was the copy table's
anchor (`session_thread.c`'s `tls_block()`), so it moved with
the table.

- **`pcb->progname`**: `main()` sets it from `argv[0]`, through
  `pcb_main()`, before any thread starts; `main.c`,
  `postmaster.c` and `postgres.c` read it. A session reads it in
  one place, the hint for a bad option in its startup packet.
  `main.c`'s static functions keep their `progname` parameter.
  `server.sh`: a session refusing `PGOPTIONS="-Q"` names the
  program in its hint, which no test checked before.
- **The copy table is gone**: `src/tools/shared_tls.py`, the
  table and its anchor in `session_thread.h`, `tls_block()`,
  and the copy in `session_thread_start()` and in the new
  thread. A thread is still given the settings not at their
  defaults, its parent's hooks, loaded libraries and EXPLAIN
  registry.
- **One link**: the Makefile's `link_shared`, which linked
  twice and read the table between, then checked a third read
  matched, is `link_server`, one link with no map.
- **The `/*LOCAL*/` marks stay**: each marks a thread's own
  variable, to move into `tcb_t` when a change touches it.
  `c.h` and `CODE.md` §3 say a thread shares only through
  `pcb` and `shmem`. `cxx_thread_local.py --check` still fails
  the build on a writable global not `thread_local`.
- No `/*SHARED*/` global is left.

## Step 230: the control blocks' fields in groups

`utils/control_block.h`'s fields had been added a batch at a
time, each where the last one ended. They are in groups now,
each under a `/* -- name -- */` heading:

- **`pcb_t`**: the program and where it runs (`progname`,
  `DataDir`, the paths, `PostmasterPid`, `PgStartTime`); how
  the process runs; limits; file permissions; configuration and
  libraries (`PgReloadTime`, the preload flags); module state.
- **`tcb_t`**: the session (its record, sequences, advisory
  keys); its transaction (`lxid`, `xmin`, `smstore`); the
  thread's resources (timeouts, the vfd cache); loaded
  libraries (hooks, dfmgr, EXPLAIN registry).
- **`postmaster_tcb_t`**: the postmaster's state; what to
  remove at exit.
- The forward declarations at the top follow the same order.
  No field is added or removed, and `shmem_t` is unchanged.
- `tcb_t`'s members are now freed in another order. Each
  deleter is a plain `delete`, and `tcb_free()` clears `tcb`
  first, so none reaches another member.

## Step 231: what replication left is gone

`../sm/NEXT.md` §1's replication item. Replication's modules
went in step 2; what they left was refused or inert. It is
removed now, but for what psql reads.

- **Kept, for psql**: `pg_publication` and its two relations,
  empty, and `pg_relation_is_publishable()`, which `\d` reads;
  `rolreplication`, which `\du` reads; `relreplident` and
  `indisreplident`, which `\d+` reads. A replication
  connection is still refused at the startup packet.
- **Publications and subscriptions are refused by sq's
  converter**, before digres has a node for them: their five
  statements, and any statement naming one as an object (DROP,
  COMMENT, ALTER ... RENAME or OWNER, SECURITY LABEL), by
  `REFUSED` and `REFUSED_OBJECTS` in `../sq/gen/gen_nodes.py`.
  A string of statements is converted whole, so one of these
  stops the statements before it from running too, as a syntax
  error does. Gone from digres: the seven nodes and three enums
  in `nodes/parsenodes.h`, their cases in `utility.c`, six
  command tags, and the object-type cases in `alter.c`,
  `dropcmds.c`, `seclabel.c`, `extension.c`, `event_trigger.c`,
  `aclchk.c` and `objectaddress.c`, whose names map no longer
  takes "publication" or "subscription".
- **`ObjectType` keeps its four members**: the converter copies
  an enum by value, so a member removed would shift every later
  one; they are commented as kept for that.
- **REPLICATION**: `CREATE ROLE` and `ALTER ROLE` refuse it;
  `NOREPLICATION`, which `pg_dumpall` writes, passes. Gone:
  `has_rolreplication()` and `user.c`'s checks of it. A new
  role's `rolreplication` is false, and so is the superuser's,
  in `pg_roles` and `pg_authid.dat`: `\du` no longer says
  Replication.
- **REPLICA IDENTITY**: `ALTER TABLE` refused it already, before
  its executor. Gone: `ATExecReplicaIdentity()`,
  `relation_mark_replica_identity()`, the rebuild of a replica
  identity index after ALTER COLUMN TYPE, and
  `get_index_isreplident()`.
- **Recovery**: `PreventCommandDuringRecovery()`, which did
  nothing, and `COMMAND_OK_IN_RECOVERY`, which only it read.
- **walsender and walreceiver**: `printsimple.c` and
  `DestRemoteSimple`, which only a walsender used; three wait
  events of theirs and two of streaming recovery; comments in
  `acl.c` and `postinit.c`.
- Left: `session_replication_role` and `ENABLE REPLICA
  TRIGGER`, which are about triggers, which digres has none of;
  and `system_views.sql`, which nothing loads.
- `server.sh`: RENAME and OWNER of a publication or subscription,
  and `ALTER SUBSCRIPTION`, are refused; REPLICATION is refused
  and NOREPLICATION passes; `REPLICA IDENTITY` is refused; `\du`
  shows no Replication.

## Step 232: xact.c's dead API is gone

The first step of `../sm/NEXT.md` §1's `xact.c` item: what
nothing needs, as digres assigns no transaction id and has no
prepared transactions.

- **The xid functions**: `GetTopTransactionId()`,
  `GetCurrentTransactionId()`, their `IfAny` and `Full`
  forms, `GetStableLatestTransactionId()` and
  `NoTransactionId()`. Their callers answer directly:
  `log_line_prefix`'s `%x` prints 0; `pg_current_xact_id()`,
  `txid_current()` and `age(xid)` refuse, as they did;
  `pg_current_xact_id_if_assigned()` and
  `txid_current_if_assigned()` are null; DROP INDEX
  CONCURRENTLY no longer checks for an xid.
- **ON CONFLICT on a heap**: the speculative insertion that asked
  for an xid is gone, as only a table in sm takes a row and its
  ON CONFLICT is sm's; the branch is an internal error. With it
  went what only it used: `ExecCheckIndexConstraints()`,
  `ExecCheckTIDVisible()`, `SpeculativeInsertionLockAcquire()`
  and its counter, and `tableam.h`'s two speculative wrappers.
  An exclusion check's livelock-preventing wait, which ordered
  this transaction's xid against another's, waits as NOWAIT
  does; it was not reached.
- **PREPARE TRANSACTION**: `utility.c` refuses it, and
  `PrepareTransactionBlock()` and `TRANS_PREPARE` are gone.
- **No callers**: `SubTransactionIsActive()`,
  `IsInTransactionBlock()`, `GetCurrentTransactionStopTimestamp()`
  and its timestamp, `BeginInternalSubTransaction()`,
  `ReleaseCurrentSubTransaction()` and
  `RollbackAndReleaseCurrentSubTransaction()`.
- Kept, as they are live: `MyXactFlags` (pipelining, the
  extended protocol's immediate commit) and `xact_is_sampled`
  (`log_transaction_sample_rate`).
  `TransactionIdIsCurrentTransactionId()`, always false, goes
  with the snapshots, next.
- `server.sh`: `pg_current_xact_id()` and `txid_current()` are
  refused; the `if_assigned` forms are null.

## Step 233: combo cids, the current xid and xmin are gone

The second step of `../sm/NEXT.md` §1's `xact.c` item, its
first part. digres has no heap, assigns no xid, and every
snapshot is the same, so what told one transaction's tuples
from another's had nothing to tell.

- **`combocid.c`**: its functions had no caller left;
  `xact.c`'s `AtEOXact_ComboCid()` went with it.
- **`TransactionIdIsCurrentTransactionId()`**, always false.
  `TransactionIdIsInProgress()` and the heap slot's
  `is_current_xact_tuple` say false themselves; a serialization
  failure above READ COMMITTED on a relation not in sm no
  longer asks whose the tuple is.
- **"cannot affect row a second time"**: ON CONFLICT DO UPDATE
  and MERGE told the case by the row's xmin being their own
  transaction's, which never held. The checks are gone, and the
  gap they leave is `ISSUE.md` issue 2: digres keeps the first
  row and drops the second's update. MERGE does not write yet.
  `test/slt/on_conflict_twice.test` holds PostgreSQL's answer
  and digres's, so the gap is not forgotten.
- **xmin**: `tcb_t`'s `xmin`, `TransactionXmin`, `RecentXmin`,
  `SnapshotResetXmin()` and `AtEOXact_Snapshot()`'s `resetXmin`,
  and the asserts on them in `indexcmds.c` and `indexam.c`.
- **Transient plans**: a plan was transient when it skipped an
  index with `indcheckxmin` not yet past `TransactionXmin`;
  `indcheckxmin` is always false in digres. `plancat.c`'s check,
  `transientPlan` in `PlannerGlobal` and `PlannedStmt`, and the
  plan cache's `saved_xmin` are gone.

## Step 234: the command id is gone

The second step's second part. A command id told one command's
tuples from another's in a heap; sm's statement does that now
(`CommandCounterIncrement()` starts sm's next), and digres has
no heap.

- **`xact.c`**: `currentCommandId`, `GetCurrentCommandId()` and
  the "used" flag. `CommandCounterIncrement()` starts sm's next
  statement and processes the queued invalidations after every
  command, there being no command id to tell a read-only one by;
  with none queued that does nothing. slt's time is unchanged.
  `inval.c`'s hack to mark the id used goes with it.
- **The table AM**: `tuple_insert`, `multi_insert`,
  `tuple_delete`, `tuple_update` and `tuple_lock`, and their
  `table_*` wrappers, take no `CommandId`; nor do `smtable.c`'s,
  `smstore_update()` and `smstore_delete()`.
  `TM_FailureData` has no `cmax`.
- **The executor**: `EState`'s `es_output_cid`, and the
  `output_cid` and `mycid` of CREATE TABLE AS, REFRESH
  MATERIALIZED VIEW, COPY FROM and ALTER TABLE's rewrite.
  `TM_SelfModified` is a change by this statement, as sm reports
  only that (`write_result()`): the six checks in
  `nodeModifyTable.c` and one in `trigger.c` that told it from a
  later command's, for the "triggered by the current command"
  error, never fired, and are gone.
- **Snapshots**: `curcid`, `SnapshotSetCommandId()` and
  `UpdateActiveSnapshotCommandId()`, and its five callers.
- Kept: the `CommandId` type, of the `cid` SQL type and the
  system columns `cmin` and `cmax`, and `trigger.c`'s firing
  counter, which only borrows the type.

## Step 235: snapmgr.c hands out one snapshot

The second step's last part. Every MVCC snapshot was the same
(steps 233 and 234): what a session sees is sm's snapshot. So
`snapmgr.c`, 987 lines, is 313, and its callers are untouched.

- **One snapshot**: `GetTransactionSnapshot()`,
  `GetLatestSnapshot()` and the catalog snapshot functions
  return the one MVCC snapshot, filled at each call. Gone:
  copies, `CopySnapshot()` and `FreeSnapshot()`, the pairing
  heap of registered snapshots ordered by xmin, the transaction
  snapshot of REPEATABLE READ, the secondary and catalog
  snapshots, and `GetSnapshotData()`.
  `InvalidateCatalogSnapshot()` does nothing, there being no
  catalog snapshot.
- **Kept, as callers ask**: the active snapshot stack, a
  `std::vector` of each snapshot pushed and the nesting level
  that owns it, so a subtransaction's abort pops what it pushed;
  a count of registrations, each tracked by a resource owner,
  for `ThereAreNoPriorRegisteredSnapshots()` (COPY FREEZE) and
  `HaveRegisteredOrActiveSnapshot()` (TOAST); and
  `FirstSnapshotSet`, for SET TRANSACTION ISOLATION LEVEL.
- **`SnapshotData`** loses `copied`, `active_count`,
  `regd_count`, `ph_node` and `snapXactCompletionCount`.
- Callers that compared snapshots, all in asserts, compare the
  same pointer now; `PushCopiedSnapshot()` pushes without a
  copy, as no snapshot changes.

## Step 236: xact.c is stripped, and stays until it is empty

What `xact.c` still does is digres's own: the transaction
block's state (BEGIN, COMMIT, SAVEPOINT, the aborted block, the
protocol's status byte), per-transaction and per-savepoint
memory and resource owners, and the hooks that end each, sm's
among them. It stays until it is empty: it is not moved to a
module of another name just so the file goes, and what it no
longer needs is deleted, as here:

- **`savepointLevel`**: only `BeginInternalSubTransaction()`
  changed it (step 232), so it was always 0, and the checks
  against crossing a savepoint level never fired.
- **Comments of what is gone**: NOTIFY's queue and signals, the
  relation map, clog, logical decoding's exported snapshot, a
  subtransaction's XID, `PrepareTransaction`; and the reason
  for `reschedule_timeouts()`, which no longer longjmps out of a
  signal handler.
- **`xact.h`**: `GIDSIZE`, `IsolationIsSerializable()`,
  `XACT_FLAGS_ACQUIREDACCESSEXCLUSIVELOCK`, `xl_xact_stats_item`,
  and the includes the WAL records needed; `namespace.c` and
  `relcache.c` include `storage/sinval.h` themselves.
- **Kept**: SPI, nonatomic contexts included, as a scripting
  language is to use it: `SPI_inside_nonatomic_context()` and the
  advance of `transaction_timestamp()` for a transaction a
  procedure starts. Also kept, as live: `MyXactFlags`,
  `xact_is_sampled`, the after-trigger hooks (foreign keys run
  as RI triggers), `AtEOXact_Enum()`.
- `xact.c` is 2874 lines, from 3,284 before step 232.

## Step 237: ON CONFLICT DO UPDATE refuses a row twice

`ISSUE.md` issue 2, from step 233. An `INSERT ... ON CONFLICT DO
UPDATE` whose rows conflict with each other, as `values (1, 0),
(1, 1)` on a key, kept the first row and dropped the second's
update, where PostgreSQL refuses the statement: "ON CONFLICT DO
UPDATE command cannot affect row a second time". PostgreSQL
told the case by the row's xmin; digres assigns none.

- **sm tells it**: the conflicting row was written by this
  statement, so ON CONFLICT's update of it fails with
  `SM_ERR_SELFMODIFIED`, `TM_SelfModified` to the executor, as
  sm's contract for `sm_stmt_begin()` says ON CONFLICT should
  take it. `ExecUpdate()` passed over the row, as an UPDATE
  does; it now calls `refuse_on_conflict_twice()`, which raises
  the error when the ModifyTable is an INSERT, so ON CONFLICT's.
- A join UPDATE that reaches one row twice still passes over
  the second change, as in PostgreSQL.
- `test/slt/on_conflict_twice.test` holds PostgreSQL's answers
  alone now: both cases fail and leave the table as it was, and
  the join UPDATE passes. Issue 2 is removed from `ISSUE.md`.

## Step 238: views are sm's

`../sm/VIEW.md` is the design; this is its second step, on sm3's
views (`../sm/sm3/test/test_view.cc`).

- **CREATE [OR REPLACE] VIEW** keeps PostgreSQL's parse analysis
  and checks, and `DefineVirtualRelation()` hands the query to
  `smstore_create_view()` or `smstore_replace_view()` in place
  of `DefineRelation()` and its rule. sm keeps the query as
  `pg_rewrite`'s `ev_action` would hold it, `nodeToString()` of
  a list of the one Query, and what it reads, found by
  `find_expr_references()`, the walker `recordDependencyOnExpr()`
  uses, its relations and a table's columns kept.
- **The relation cache builds a view's entry** from sm's record
  (`RelationBuildSmViewDesc()`): its columns its query's select
  list, its rule `_RETURN`, `ON SELECT DO INSTEAD` the query, the
  view's oid its own. The rewriter, planner and executor are
  PostgreSQL's: a simple view's INSERT, UPDATE and DELETE reach
  its table.
- **A view has a lock in sm**: `smstore_lock_relation()` takes it
  where an oid opens no table, and the version it reads under
  the lock keeps the entry current, so another session's replace
  is seen, a prepared query over the view planned again. The
  views a transaction locked are kept with its open tables, and
  go with them.
- **DROP VIEW is `smstore_drop_views()`.** What reads a view, a
  table, or a column of one is found through sm, transitively:
  RESTRICT is PostgreSQL's error, naming each view; CASCADE its
  notice, and the readers go first. DROP TABLE, ALTER TABLE ...
  DROP COLUMN and DROP SCHEMA cascade to views; a schema's own
  views go before its tables.
- **`get_rte_attribute_is_dropped()`** reads a relation's
  descriptor, not its `pg_attribute` row, which a table in sm
  does not have: the rewriter asks it of a view's join.
- **Refused, not supported yet**: a temporary view, and a view's
  options, `WITH CHECK OPTION` among them.
- **Not done**: a view's rows in the catalog relations
  (`pg_class`, `pg_attribute`, `pg_rewrite`), so `\d` and
  `pg_get_viewdef()`; ALTER VIEW.
- **sm3 keeps a view's query in pieces** of 60,000 bytes, a part
  each: a part is an sm2 tuple, at most 65,535 bytes, and one of
  SQLite's views serializes to 100 KB.
- `test/slt/view.test`, its answers PostgreSQL 18's; `server.sh`
  checks a view's lock and what it reads across sessions.
- **SQLite's sample** (`make slt-corpus`, step 164): digres
  fails nothing PostgreSQL 18 passes, of 1,495,198 records; it
  failed 24,408 more before, every one a view's.

## Step 239: a view in the catalog relations

`../sm/VIEW.md` §10, its third step; `SYSCATALOG.md` §3.

- **`pg_class` lists a view**, `relkind` `v`, `relhasrules`
  true, no `relfilenode`; **`pg_attribute`** its columns, its
  query's select list, as the relation cache makes them
  (`smstore_relations()`, `add_view()`).
- **`pg_rewrite` has a row a view**, its `_RETURN` rule, its oid
  the view's, its `ev_action` the query sm keeps; a scan by
  `ev_class` or `oid` makes that view's alone.
  `pg_get_viewdef()` reads it over SPI, unchanged.
- **`pg_views`** is a catalog relation, oid 12101, as `pg_roles`
  is: a view of sm's, a row, its definition `pg_get_viewdef()`'s.
- **ruleutils.c names a relation from its descriptor**:
  `generate_relation_name()` and
  `generate_qualified_relation_name()` call
  `RelationGetClassForm()`, as `get_rel_name()` does, in place
  of a `pg_class` row a table in sm has none of.
- psql's `\dv`, `\d` and `\d+` of a view give PostgreSQL 18's
  output. `test/slt/view_catalog.test`; `server.sh` runs `\dv`
  and `\d+`.

## Step 240: ALTER VIEW, and a view's options

`../sm/VIEW.md` §10, its fourth step.

- **ALTER VIEW ... RENAME TO** is `sm_cat_rename_view`, and so is
  ALTER TABLE ... RENAME TO of a view, as PostgreSQL takes it;
  ALTER VIEW of a table is refused, `"t" is not a view`.
  `rename_target()` says which it named.
- **RENAME COLUMN of a view**, by ALTER VIEW or ALTER TABLE,
  renames the column in its query's select list, which sm keeps
  (`rename_view_column()`): PostgreSQL renames its
  `pg_attribute` row, and the view's rule names it no more.
  What reads the view reads the column by place.
- **A view's options are kept**: `WITH (...)` and `WITH CHECK
  OPTION`, checked by `view_reloptions()`, kept in sm as
  `untransformRelOptions()`'s list, `nodeToString()`'s. The
  relation cache makes the view's `rd_options` of them, and
  `pg_class.reloptions` lists them, so a check option is
  checked by the executor and `security_barrier` and
  `security_invoker` are PostgreSQL's. CREATE OR REPLACE
  replaces them; `ALTER VIEW ... SET` and `RESET` change them,
  refusing a check option on a view not automatically
  updatable, as `ATExecSetRelOptions()` does.
- **`AlterTableLookupRelation()`** takes ALTER VIEW, and of a
  view SET and RESET alone; `smstore_alter_table()` hands a
  view's to `alter_view()`.
- `test/slt/view_alter.test`. PostgreSQL's `create_view` still
  fails, first on CREATE FUNCTION; its diff goes from 934 lines
  to 873. What is left of views there is a temporary view, 15
  statements and those after them, and SET SCHEMA.

## Step 241: storage/ipc stripped

A pass over `storage/ipc/*.c` for what digres no longer needs.
Most of it is digres's own and live: the session registry,
cancel requests and interrupt delivery (`procsignal.c`), the
quit reason (`pmsignal.c`), the exit callbacks (`ipc.c`),
latches, and `pg_cancel_backend()` and its kin
(`signalfuncs.c`).

- **Wait sets are epoll's alone**: digres runs on Linux alone
  (`../sm/PART4.md` section 10). `waiteventset.c`, 1,600 lines,
  is 590: PostgreSQL's poll(), kqueue() and Windows
  implementations and the self-pipe are gone, none of which
  could wake on sm's wake fd, the latch of a digres thread.
  Gone with them: `FreeWaitEventSetAfterFork()`, the `waiting`
  flag only a signal handler read, and the WIN32 branches of
  `latch.c`, `latch.h` and `waiteventset.h`.
  `WaitEventSetCanReportClosed()` says true, as epoll does.
- **`shmem.c` is gone**: it held `pg_numa_available()` alone,
  whether NUMA serves PostgreSQL's shared memory, which digres
  has not. Its `pg_proc` entry and `src/port/pg_numa.c` went
  with it.
- **`ipc.c`**: `on_exit_reset()`, which dropped the callbacks a
  forked child inherited; a thread's start empty, being
  `thread_local`. Kept: `cancel_before_shmem_exit()`, which
  `PG_ENSURE_ERROR_CLEANUP` uses, a facility for code to come.
- **`ipci.c`**: the reset of the quit reason "after a crash",
  which runs once, at startup, where `shmem_t` already starts
  at `PMQUIT_NOT_SENT`.
- Stale comments: the "dead man switch" of `atexit_callback()`,
  the SysV keys at `CreateSharedMemoryAndSemaphores()`'s caller,
  and `kill()` and PID reuse in `pg_signal_backend()`.

## Step 242: fd.c stripped

`fd.c` stays: sm0's `sm_vfd.h` keeps the pool of virtual fds and
the process's ceiling, and `fd.c` keeps what sm0 knows nothing
of -- a file's transaction or resource owner, temporary files
(sorts' and hashes' spills, through `buffile.c`), the wrappers
of fopen, opendir, popen and open that close on abort, each
thread's share of fds, and the fsync helpers. What digres no
longer needs is gone, 2,715 lines to 2,215:

- **PostgreSQL's relation-file calls**, which md.c made:
  `FilePrefetch()`, `FileWriteback()`, `FileSync()`,
  `FileZero()`, `FileFallocate()`, `FileTruncate()` and the
  `FileGetRaw*()` three; and `pg_fdatasync()`, `pg_truncate()`,
  `pg_ftruncate()` and `durable_unlink()`. Nothing called them.
- **Temporary relation files**: `RemovePgTempRelationFiles()`,
  its per-database half and `looks_like_temp_rel_name()`.
  `RemovePgTempFiles()` empties `base/pgsql_tmp` alone, digres
  having no other tablespace; it no longer logs that it could
  not open `pg_tblspc` at every start.
- **Two settings that drove nothing**: `file_extend_method`,
  md.c's way to extend a relation file, and `debug_io_direct`,
  whose flags no open read; their hooks, `io_direct_flags` and
  the `IO_DIRECT_*` and `FILE_EXTEND_METHOD_*` constants, and
  the sample configuration's entry.
- `server.sh`: a temporary file a prior run left is removed at
  startup, and the log names no `pg_tblspc`.

## Step 243: heap pages, data checksums and heap TOAST are gone

A pass over what PostgreSQL's heap left in `storage/page`,
`lib` and `access/common`.

- **`storage/page`**: `bufpage.c`, PostgreSQL's slotted page,
  none of whose functions had a caller; `checksum.c`,
  `checksum.h`, `checksum_impl.h` and the `ignore_checksum_failure`
  setting, as digres writes no page to check; and the README of
  checksums. `bufpage.h` keeps the page layout's types and
  macros, by which `htup_details.h` and `itup.h` size a tuple.
  `itemptr.c` stays: a row's tid is an item pointer.
- **`lib/integerset.c`**, which VACUUM's GiST pass used.
- **Heap TOAST**: a value sm stores out of line has its own
  pointer, `VARATT_IS_EXTERNAL_SM`, and nothing makes
  PostgreSQL's on-disk one. `detoast.c` loses its five branches
  for it and `toast_fetch_datum()` and `toast_fetch_datum_slice()`,
  589 lines to 386; the table AM, `relation_fetch_toast_slice`
  and its wrapper, which sm's and the catalog's AMs set to a
  refusal; `toast_internals.c`, the TOAST table's index helpers
  and `get_toast_snapshot()`, with `SnapshotToastData`, 219 lines
  to 88. `toast_compress_datum()` stays, for index tuples.
- Left: `toasting.c`, which makes a TOAST table, with the
  catalog DDL kept to be ported (`PLAN.md` §7).

## Step 244: TABLESAMPLE on a table in sm

`TABLESAMPLE` was parsed, planned and run by PostgreSQL's sample
scan, but sm's table access method refused the two callbacks
it calls. A sampling method picks blocks and, in each, row
offsets; sm has no pages, so a block is a tid's.

- **A tid's block is 256 rownos** (`SMSTORE_ROWS_PER_BLOCK`,
  `access/smstore.h`): block rowno / 256, offset one above the
  remainder, as an offset of 0 is no tid. It was 65535 rownos a
  block, which would make a table under 65535 rows one block,
  and `SYSTEM` take all or none. A tid now holds a rowno up to
  2^40 or so. No test showed a tid: digres has no `ctid` column.
- **`scan_sample_next_block`**: the method's pick, as `SYSTEM`
  makes one, or the next block, as `BERNOULLI` reads them all;
  the blocks are as many as hold the table's highest rowno
  (`smstore_rowno_limit()`). The cursor moves to the block's
  first rowno (`smstore_scan_seek()`, forward only).
- **`scan_sample_next_tuple`**: the method's next offset that has
  a row (`smstore_scan_row()`), which reads forward and keeps a
  row read past the rowno asked for, the offsets coming in
  increasing order. A rowno with no row, deleted or never used,
  is passed over, as an unused line pointer is.
- **Rescan** starts the cursor and the blocks again, as under
  `LATERAL`.
- **Cost**: the planner's pages for a table in sm are its bytes
  over 8 KB (`STATISTICS.md` section 2.2), not its blocks of
  rownos, so a `SYSTEM` scan's I/O cost is an estimate of its
  own; its rows, the table's times the fraction, are right.
- `test/slt/tablesample.test`: both methods at 0 and 100, a
  fraction near BERNOULLI's, REPEATABLE taking the same rows,
  deleted rows not sampled, a rescan, and no SYSTEM_ROWS.

## Step 245: temporary views

`../sm/VIEW.md` §12, its fourth step, on sm3's temporary view,
its register of temporary readers and its cascade across
sessions (steps 1 to 3 there).

- **CREATE TEMP VIEW** is made in sm's namespace 0, as a
  temporary table is, and so is a view that reads a temporary
  relation, with PostgreSQL's notice. `DefineVirtualRelation()`
  refuses it no more.
- **In `pg_temp`**: the relation cache makes its entry
  `relpersistence` `t` and `rd_islocaltemp`, and `pg_class`
  lists the session's temporary views (`smstore_relations()`);
  `add_view()`, `smstore_view_desc()` and `view_name()` give
  `pg_temp` for sm's namespace 0.
- **What reads or is read across sessions**: another session's
  DROP of a table or view a temporary view reads is refused,
  naming it `pg_temp.tv`, as digres has one temporary schema
  where PostgreSQL names `pg_temp_1`; with CASCADE it drops the
  view. `find_readers()` takes the reader's name and reads from
  `sm_cat_view_readers()`, which gives them now, as
  `sm_cat_view_def()` finds no other session's view.
- **The session whose view another dropped** sees it no more at
  once, in a transaction or out of one, as PostgreSQL's does.
  Kept to its next sm transaction, a view the name lookup
  found, outside one, read a view gone, and the rewriter opened
  that unlocked: an assertion failed.
- **DISCARD TEMP** drops the temporary views, readers first,
  before the temporary tables (`drop_temp_views()`).
- `test/slt/view_temp.test`, which PostgreSQL 18 passes too, and
  `view.test` refuses no temporary view;
  `server.sh`, another session's drop refused and cascaded, in
  a transaction of the view's session and out of one.
  PostgreSQL's `create_view` diff goes from 1,324 lines to
  1,208 (from 1,486 to 1,370 with `test_setup`'s; step 240's
  873 was measured before `test_setup` changed). Its temporary
  views are PostgreSQL's but for one that reads a temporary
  sequence, which digres refuses.

## Step 246: pg_constraint lists a foreign key

digres enforced a foreign key, but `pg_constraint` was read
empty, so psql's `\d` showed none.

- **`pg_constraint`'s rows** (`catalog/catrel.c`,
  `gen_pg_constraint()`): one of type `f` for each foreign key of
  a table in sm, from what sm keeps of it (`smstore_fkey_t`):
  its columns, the referenced table, its unique index and
  columns, the three operator arrays, the actions, the match
  type, and `ON DELETE SET NULL (...)`'s columns. A scan keyed by
  `conrelid` makes that table's rows alone; the lookup by oid
  and \d's Referenced by read them all, opening only the tables
  that have a foreign key. A primary key or unique constraint
  has no row yet: it is its index, `pg_index`'s, and \d lists it
  so.
- **`relhastriggers`** is true in `pg_class` for a table with a
  foreign key or that one references, as the relation cache
  gives each RI triggers: \d asks it before it lists them.
  `smstore_relation_t` has the table's count of foreign keys and
  whether one references it, from the definition `add_table()`
  reads already.
- **`pg_get_constraintdef()`** scans `pg_constraint` with no
  index, as digres makes its rows, and names a column of a table
  in sm from the table's descriptor (`column_name()`), as
  `sm_indexdef()` does: there is no `pg_attribute` row to look it
  up by.
- `test/slt/foreign_key_catalog.test`: the rows, their
  definitions and columns, `relhastriggers`, and the rows gone
  with their table. `server.sh`: \d of a table with a foreign key
  and of the one it references.

## Step 247: base backup's leftovers in src/common are gone

A scan of the backend, `src/common` and `src/port` for exported
functions nothing references found 185. Most are kept on
purpose: an extension's API (custom settings, scan methods,
label providers, resource owner callbacks, `regexport.c`), SPI
(a scripting language is to use it), general utilities
(`list.c`, `pg_prng.c`, the multibyte and Unicode helpers), and
the catalog DDL kept to be ported (`PLAN.md` section 7). What
belonged to modules digres removed is gone:

- **`src/common`**: `blkreftable.c`, the block reference tables
  of incremental backup's WAL summaries; `parse_manifest.c`, a
  base backup's manifest; `checksum_helper.c`, its checksums;
  and `compression.c`, `pg_basebackup`'s compression options;
  with their headers, 2,743 lines. Nothing included any of
  them but each other.
- **`LockRelationId()`** in `lmgr.c`, which nothing called. The
  rest of `lmgr.c`, `proc.c`, `launch_backend.c` and
  `interrupt.c` is digres's own and live: `lmgr.c` takes sm's
  table locks.
- Left for a pass of its own: `spccache.c`, the planner's costs
  per tablespace, and `tablespace_reloptions()`, digres having
  no tablespace but the default.

## Step 248: pg_constraint lists a primary key and a unique constraint

Step 246 gave `pg_constraint` a row for each foreign key; a
primary key and a unique constraint had none, so a tool reading
`pg_constraint` missed them, and `\d` called a `UNIQUE`
constraint's index plain `UNIQUE`, where PostgreSQL says `UNIQUE
CONSTRAINT`. PostgreSQL gives a unique index a row only when a
constraint made it, not `CREATE UNIQUE INDEX`, and sm did not
keep which.

- **sm marks a constraint's index**
  (`sm_index_def_t::constraint`, `sm_cat_create_index()`'s and
  `sm3::create_index()`'s `constraint`,
  `../sm/sm3/CATALOG_ONDISK.md`): kept in the index's part of the
  catalog blob as a thirteenth attribute, null unless set, so an
  index written before reads false. It must be unique and on
  columns alone. `sm3/test/test_ddl.cc` checks it, and that it
  survives a restart.
- **digres sets it** from `IndexStmt.isconstraint`, true for an
  index a `UNIQUE` or `PRIMARY KEY` constraint makes, at `CREATE
  TABLE`; `smstore_relation_t` carries it to `catrel.c`.
- **`pg_constraint`'s rows** (`key_row()`): one of type `p` or `u`
  for each such index, named as it, its oid, on its key
  columns, its `conindid` itself. `pg_get_constraintdef()` writes
  `PRIMARY KEY (a)` and `UNIQUE (b, c) INCLUDE (d)`; `\d` says
  `UNIQUE CONSTRAINT` for the one and `UNIQUE` for an index
  `CREATE UNIQUE INDEX` made, as PostgreSQL does.
- A table made before this keeps its unique constraints unmarked:
  `\d` shows them `UNIQUE`, with no row.
- `test/slt/key_constraint_catalog.test`.

## Step 249: the planner's page costs are the settings

`spccache.c` cached each tablespace's `seq_page_cost` and
`random_page_cost`, which `ALTER TABLESPACE ... SET` gave it; digres
has only the default tablespace, no `pg_tablespace` and no `ALTER
TABLESPACE`, so every lookup gave the settings.

- **`spccache.c` and `spccache.h` are gone**; the planner's six
  calls in `costsize.c` and the index cost's in `selfuncs.c`
  take `seq_page_cost` and `random_page_cost` themselves. Costs
  are what they were.
- **The tablespace options** are gone with it:
  `tablespace_reloptions()`, `TableSpaceOpts`, and the
  `seq_page_cost` and `random_page_cost` entries of
  `reloptions.c`'s table, of kind `RELOPT_KIND_TABLESPACE`,
  whose name the enum keeps with PostgreSQL's other kinds.

## Step 250: what removed modules left uncalled

A second scan for exported functions nothing references (step
247's), for those whose module digres has removed, or which a
strip since left alone. Kept, as before: an extension's or a
procedural language's API (`get_transform_fromsql()` and
`_tosql()` among them), general utilities, the catalog DDL kept
to be ported (`toasting.c`, `EventTriggerTableRewrite()`), and
`varsize_any()`, a debugger's.

- **nbtree's and the index access methods'**: `skipsupport.c`, its
  skip scan's support; `amvalidate.c` and its header, the
  helpers an index AM's validator called;
  `PrepareSortSupportFromIndexRel()`, an index build's sort;
  `index_truncate_tuple()`, its suffix truncation;
  `hashcostestimate()`, the hash index's cost.
- **The heap's**: `heap_expand_tuple()`, `minimal_expand_tuple()`
  and their `expand_tuple()`, a tuple of fewer columns than its
  table; `table_index_fetch_tuple_check()`, heapam's unique check.
- **The catalog snapshot's**: `RelationHasSysCache()` and
  `RelationSupportsSysCache()`, with the sorted OID lists
  `InitCatalogCache()` built for them, which step 235's
  `snapmgr.c` left uncalled.
- **Compiled out already** (`#ifdef NOT_USED`): `FileInvalidate()`
  and `equalTriggerDescs()`.

## Step 251: no library is loaded

A session that loaded a library, by `LOAD` or a C function's
first call, failed `internal_load_library()`'s assertion that
no session thread loads one, and took the server down:
PostgreSQL's `create_function_c`, on its first statement.
Dynamic loading is to be redesigned and redone (`ISSUE.md`
issue 1); until then it is disabled.

- **`internal_load_library()` refuses every library**, with
  `loading library "..." is not supported yet`: `LOAD`, a C
  function, `shared_preload_libraries` and the session's
  preload settings alike. What only a load used goes:
  `incompatible_module_error()`, the ABI block it checked,
  `PG_init_t` and `SAME_INODE`. The list of loaded modules
  stays, empty.
- `test/slt/load.test`.

## Step 252: a table scan reads the columns it needs

A sequential scan of a table in sm deformed every column of
each row, and sm checked the whole row, though its
projection and quals used only some: `select a from t` on a
wide table cost what `select *` did.

- **`scan_natts()` in `nodeSeqscan.c`** counts the columns a
  scan reads, from the first: up to the last its target list
  and quals name, or every one for a whole-row reference. A
  scan with no projection names every column. It is passed to
  `smstore_beginscan()`, which deforms that many
  (`sm_tbl_deform()` takes a prefix now, and sm checks only
  what it reads, `../sm/sm2/TUPLE.md` §8).
- **The slot stays virtual.** The columns left out are null
  to a materializing, and not valid: the executor reads a
  virtual slot with no deform, so a count too few trips an
  assertion, or reads null.
- **Left as it was**: the table AM's scans (sample, TID,
  ANALYZE) and the index and bitmap scans read every column
  (`../sm/NEXT.md` §2).
- `test/slt/scan_columns.test`.

## Step 253: an index scan reads the columns it needs

Step 252, for an index scan: `smstore_index_getnext()` read
each row with every column deformed.

- **`smstore_scan_natts()`**, step 252's `scan_natts()` moved
  to `smstore.c` for both scans, counts the columns from the
  plan, kept in `IndexScanState`'s `iss_SmNatts` for the scan
  that begins at the first row, as one with runtime keys does.
- **The keys' columns too.** sm checks each row against every
  scan key (`keys_met()`), on the key's column in the table,
  which the projection and quals need not name:
  `smstore_index_beginscan()` reads up to the last of them.
- **One deform for every row read**: `tuple_into_slot()` takes
  the columns to read, and step 252's `scan_row_into_slot()`
  is gone into it. A fetch by tid, a row lock and the newest
  version of a row read every column, as they did.
- **Left as it was**: a bitmap index scan and an index-only
  scan read no row through it, and pass every column.
- `test/slt/scan_columns.test`, by index scans.

## Step 254: PostgreSQL 18's planner fixes since 18.6

PostgreSQL's regression suite stopped the server twice in the
planner, in `subselect` and `join`, on bugs REL_18_STABLE has
fixed since 18.6, which digres forked from; past them `join`
found two more. Every commit to REL_18_STABLE after 18.6
(`724edf9bde9`) that touches `src/backend/optimizer`,
`src/backend/parser` or `src/backend/rewrite`, 31 to
`d7938ff3085`, is taken, each whole but for its tests.

- **How**: each commit's change to a file was merged into
  digres's copy (`git merge-file`), the two upstream versions
  formatted as digres's file is. Where that conflicted,
  digres's own edits to the file were reapplied to upstream's
  new version instead.
- **By hand**: `create_append_path()` keeps digres's single
  child with no parallel test, and takes the commit's
  `disabled_nodes`; two casts for C++ in `prepjointree.c`.
  A cast `analyzejoins.c` had went with the code that
  `9f25197bf27` removed.
- **Not taken**: `src/tools/pgindent/typedefs.list`, which
  digres does not keep; the tests, which `make regress` reads
  from PostgreSQL 18's tree.
- **The regression suite**: no crash; `subselect` and `join`
  fail on what digres does not support (`road`, `b_star`).
  No other file's outcome changed.
- `test/slt/planner_upstream.test`: the queries that crashed
  or failed.

## Step 255: two of PostgreSQL 18's function fixes since 18.6

PostgreSQL's regression suite failed `int4` and `text` on two
bugs REL_18_STABLE fixed after 18.6, in commits outside step
254's planner, parser and rewriter. Both are taken, merged as
step 254's were, cleanly.

- **`ec434db1f49`**: `int4_to_char()` scales a value for the
  `V` format with `int4mul()`, so one that overflows fails with
  `integer out of range` instead of wrapping.
- **`bfb08d0946e`**: `text_right()` clamps a count of
  `PG_INT32_MIN`, whose negation overflowed, so it skips the
  whole string as any count past its length does.
- `text` passes now; `int4` fails only on a numeric of no
  typmod printed at scale 3, as `int2` and `int8` do
  (`test/regress/README.md`).
- `test/slt/adt_upstream.test`.

## Step 256: a bare numeric keeps its value's scale

A bare `numeric` was `decimal64(17, 3)` everywhere, a cast
included (`DG_DECIMAL_PLAN.md` §3.1), so `-2.5::numeric` was
`-2.500` and `'1.23456'::numeric` `1.235`, where PostgreSQL
keeps the scale written: its `int2`, `int4` and `int8` failed
on it. A value carries its scale (`DECIMAL.md` §2.2), so a
bare `numeric` needs none of its own.

- **`parse_type.c`** gives a bare `numeric` no typmod: a
  `decimal64` whose value keeps its scale, at most 17 digits.
- **A column of it is still `numeric(17, 3)`**: `smstore.c`
  gave an unconstrained column that typmod already, as sm2
  needs a scale to store at, and a `CREATE TABLE AS` from a
  bare `numeric` makes one.
- **Expected output**: `select_decimal64`, `select_numeric`,
  `select_text` and `select_jsonb_numbers`, and two checks in
  `server.sh`, print the scale written, as PostgreSQL does.
- PostgreSQL's `int2`, `int4` and `int8` pass.
- `test/slt/numeric_bare.test`.

## Step 257: a bitmap heap scan reads the columns it needs

A bitmap heap scan read every column of each row it fetched by
rowno, as `smstore_fetch()` deformed the whole tuple. It now
reads the first columns only, up to the last its plan names,
as a sequential and an index scan do (steps 252 and 253).

- **`smstore_scan_natts()`** counts a bitmap heap scan's
  recheck too, `bitmapqualorig`, which is a field of its own
  and not among the plan's quals: the recheck runs on every
  row.
- **`smstore_beginfetch(rel, natts)`**: `nodeBitmapHeapscan.c`
  counts the columns when it initializes, and
  `smstore_fetch()` passes them to `read_row()`.
- **A point read by tid stays whole**: `smstore_fetch_row()`
  gives an update its old row, whose unchanged columns it
  copies, and a trigger and EvalPlanQual their rows.
- `test/slt/scan_columns.test`, step 9; without the recheck's
  columns, or with one column too few, it trips the slot's
  assertion.

## Step 258: a read by tid takes the columns it needs

`smstore_fetch_row()`, the read of a row by its tid, deformed
every column. It now takes the count, and the two callers that
know a smaller one pass it; the others need the whole row.

- **A TID scan** (`rowno = n`) counts its columns from its plan
  with `smstore_scan_natts()`, and `nodeTidscan.c` calls
  `smstore_fetch_row()` for a table in sm, not
  `table_tuple_fetch_row_version()`.
- **DELETE RETURNING** reads the deleted row up to the last
  column RETURNING names of the table, counted once in
  `ExecInitModifyTable()` (`ri_SmReturningNatts`, by the new
  `smstore_expr_natts()`); all of it for a row moving to
  another partition, which `ExecInsert()` is given, and for a
  result relation whose count was not made.
- **The tableam's callback passes every column**: an update's
  old row, whose unchanged columns it copies, the newest
  version of a locked row, a trigger's rows and EvalPlanQual's.
- `test/slt/scan_columns.test`, step 10; with one column too
  few the TID scan trips the slot's assertion and DELETE
  RETURNING fails in its projection.

## Step 259: a sample scan reads the columns it needs

A `TABLESAMPLE` scan deformed every column of each row it
sampled, as `smtable_scan_begin()` opened its cursor so. It now
reads the first columns only, up to the last its plan names, as
the other scans do (steps 252, 253, 257 and 258).

- **`nodeSamplescan.c`** counts the columns with
  `smstore_scan_natts()` when it initializes, and hands them to
  the scan the access method begins, by
  `smtable_scan_set_natts()`, which begins its cursor again
  before any row is read.
- **A rescan keeps the count**: `smtable_scan_t` holds it, and
  `smtable_scan_rescan()` begins the cursor with it.
- The access method's other scans, a utility's of the whole
  table, read every column.
- `test/slt/scan_columns.test`, step 11; with one column too
  few at the begin, or at the rescan under a nested loop, it
  trips the slot's assertion.

## Step 260: a failed unique build says the duplicated key

`CREATE UNIQUE INDEX` over a duplicated key said sm's
`index build: two rows of u with one key in uk`, where
PostgreSQL says `Key (k)=(1) is duplicated.`: sm's error named
no row, and a build on a table with rows fails after the
commit, in the background, where no row is at hand.

- **sm keeps the two rows.** sm0's error state carries a
  duplicate's record, the index and up to two rownos
  (`sm1::errdup()`, `sm_errdup()`); sm3's build sets it, a
  failed index's catalog state keeps it, and `builder_wait`
  raises it again (`../sm/sm3/SM3_DESIGN.md` §3).
- **digres reads one of them** and says its key as PostgreSQL
  does, the columns quoted as `pg_get_indexdef_columns()`
  quotes them (`dup_key()` in `smstore.c`); the rows go in a
  hint, `Rows 1 and 3 have it.`, so PostgreSQL's detail stays
  as its tests expect. For a build after the commit the key is
  read in the transaction that drops the failed index. A row
  neither of which the transaction sees leaves sm's message.
- **Column keys only**: digres makes no expression index, so
  a key is the row's columns, and needs no evaluator.
- PostgreSQL's checks of the user's right to see the columns,
  row security and SELECT, are not made: digres's roles are
  compiled in (`test/regress/README.md`).
- `test/server.sh`, "a duplicated key's build": a build after
  the commit, a key of two columns, and one in the creating
  transaction.

## Step 261: a write of a taken key says PostgreSQL's error

An insert, an update, ON CONFLICT on another index, or a COPY
of a key a unique index held said `duplicate key value
violates a unique index on "t"` and sm's `key already
exists`. PostgreSQL names the index and the key.

- **sm3 names the index**: `add_entry()`, which every write's
  entry goes through, and a load's `idx_load`, put its oid in
  the duplicate's record (`sm1::errdup()`, step 260).
- **digres says the key** from the row it wrote, with
  PostgreSQL's own `BuildIndexValueDescription()`:
  `duplicate key value violates unique constraint "t_pkey"`,
  `Key (a)=(1) already exists.`, and the constraint's name in
  the error's fields (`duplicate_write()` in `smstore.c`).
- **No index named, sm's words**: a load's duplicate found at
  its close, in sm1's sort, names none (`../sm/NEXT.md` §2).
- **A COPY's CONTEXT line was wrong**, before and after: it
  named the line after the batch COPY handed sm, not the
  row's. Step 262 mends it.
- `test/server.sh`, "a write of a taken key"; five older
  checks now look for PostgreSQL's words.

## Step 262: a COPY's error names its row's line

A COPY into a table in sm whose row sm refused, for a
duplicate key, said `CONTEXT: COPY t, line 3` for a row on
line 2: the line after the batch.
PostgreSQL's COPY buffers rows and inserts a batch with one
`table_multi_insert()`, then sets each row's line before its
index entries and triggers, where heap's errors come from. sm
writes a row's entries, and checks its keys, as it takes the
row, inside `table_multi_insert()`.

- **`copyfrom.c` hands a table in sm its rows one call each**,
  at each row's own line (`CopyMultiInsertBufferFlush()`). The
  load goes on as before: `smstore_load()` took the rows one at
  a time already.
- A million-row COPY takes as long, into an empty table and a
  table with rows, within a few percent.
- `test/server.sh`, "a write of a taken key", checks the line.

## Step 263: a row read by rowno is pinned, not copied

A row an index scan, an index-only scan or a bitmap heap scan
read by its rowno was copied twice: sm copied it into the
session's buffer, and `read_row()` copied its columns passed
by reference into the slot (step 161), as the buffer lasted
only until sm's next call. sm now reads a row into a pin,
which keeps it in memory where RocksDB has it
(`../sm/PIN.md`).

- **Each of those scans holds a pin ref** in its
  `smstore_scan_t`, which every row it reads by rowno is read
  into: `read_row()` with a ref, and `entry_from_row()`. The
  slot's values point into the pinned row until the scan
  reads its next, as a cursor's row lasts, and
  `smstore_endscan()` unpins it. Neither copy is left.
- **A read with no scan** (a TID scan, the fetch of a write's
  target, a recheck, the rows of a duplicate's message) still
  reads lent and copies into the slot: sm's own copy is gone
  from it, as a lent read is now a pin the session keeps.
- **Nothing on an error**: a scan an error frees leaves its
  ref behind, and sm releases the pin at the end of the
  transaction. sm releases none at a rollback to a savepoint.
- Left for later: the old row of an update or a delete, which
  sm2, sm3 and `keep_row()` still copy, and releasing every
  pin at the end of a statement (`../sm/PIN.md` §8).
- `test/slt/point_read.test` fetches from a cursor over a
  nested loop of index scans across a rollback to a
  savepoint, its outer row held from one FETCH to the next.

## Step 264: a scan a rollback closed says its view is lost

A rollback to a savepoint closes every sm cursor opened after
it (`../sm/sm1/SM1_DESIGN.md` §6.5). A SQL cursor declared
before the savepoint and fetched inside it may begin its
inner scan again there, as a nested loop does for each outer
row; after `ROLLBACK TO`, its next FETCH read on from that
cursor and failed with an internal error naming sm's rule.
PostgreSQL goes on.

- **sm1 gives it a code of its own**, `SM_ERR_SCANCLOSED`, in
  place of `SM_ERR_INVALID`, so a host can tell it from a
  misuse.
- **`sm_error()` reports it** as "cannot recreate the old
  view of a scan closed by a rollback to a savepoint", with
  SQLSTATE 0A000: sm does not reopen the cursor at the
  savepoint's view.
- **It is documented** as a known incompatibility, in a new
  section of `README.md`.
- `test/slt/point_read.test` fetches row 6 inside a
  savepoint, so its inner scan begins there, rolls back, and
  checks that the next FETCH fails on digres and finds no row
  on PostgreSQL. Below digres, `../sm/sm3/test/test_table.cc`
  checks the code on an index cursor, and
  `../sm/rel/test/test_cursor.rel` with rel's new cursor
  commands, `idx-open`, `cursor-next` and `cursor-close`.

## Step 265: a decimal128 column is sm's decimal128

A decimal column with no typmod was given numeric(17, 3)
whatever its type, and `type_code()` chose sm's type by the
precision, not the type. So a `decimal128` column of 17
digits or fewer, or of none, was stored as sm's `decimal64`:
each write packed its value (`numeric_pack()`), and each read
converted it back into memory reset at the next row and
materialized the slot (`slot_from_sm()`), a copy of the whole
row (`../sm/PIN.md` §2, D). And a `CREATE TABLE AS` from a
`decimal128` expression, such as a literal of 22 digits, made
a numeric(17, 3) column and failed with a numeric field
overflow, where PostgreSQL stores the value.

- **`type_code()` takes the type alone**: `decimal64` is sm's
  17, `decimal128` its 18, whatever the precision; sm2 takes
  a `decimal128` of 1 to 36 digits.
- **A `decimal128` column with no typmod is numeric(36, 3)**,
  the widest of its type at the three places a bare numeric
  column has (`numeric_typmod()`, `DECIMAL.md` §2.3).
- **Nothing is converted**: every decimal datum is sm's as it
  is, a word or a pointer into sm's tuple. Gone with it:
  `numeric_from_sm()`, `converted_decimal()`, `has_numeric()`,
  `slot_from_sm()` and its `numeric_scratch`. A scan bound
  is packed at its column's type (`numeric_bound()`).
- `test/slt/decimal128_column.test`: the CREATE TABLE AS of
  a 22-digit literal, which failed before, a `decimal128(10,
  2)` column's rounding and overflow, and a nested loop of
  index scans and an index-only scan on such columns.

## Step 266: a write's old row is pinned, not copied

The row an update or a delete replaces was copied at every
level: sm1 into the session's `readbuf`, sm2 again for a row
with datums out of line, sm3 always, before its index
maintenance, and digres's `keep_row()` into
`TopTransactionContext` for RETURNING. A DELETE paid three
copies of each row, four with TOAST. Each level now hands the
old row back pinned into a ref (`../sm/PIN.md` §8 step 4).

- **sm1's `update` and `erase`** take a ref and pin the row
  their existence check found. The form without one pins into
  the session's own ref, which a lent read shares, so
  `readbuf` is gone. The row a ref held is released only when
  the write succeeds, as the new row may view it.
- **sm2's and sm3's writes** take the caller's ref, or use one
  of their own for the call: the erasing of the old row's
  datums out of line and the index maintenance read it there.
- **`sm_tbl_update()` and `sm_tbl_delete()`** take an
  `sm_pin_ref_t` and hand back a view, in place of a
  `std::string`.
- **`keep_row()` keeps `kept_ref`**: `smstore_delete()` deletes
  into it, and `smstore_lock_row()` reads the newest version
  into it after a write found the row changed. A new
  statement unpins it, and the transaction's end releases it.
- **`smstore_update()`** reads the old row, to tell its own
  TOAST pointers from others, into a ref of its own, not a
  `std::string`.
- `test/slt/write_old_row.test`: DELETE ... RETURNING by an
  index and a sequential scan, and an UPDATE and a DELETE of a
  row stored out of line.

## Step 267: a cursor's row is lent, not copied

sm1's `scan_next` copied every row it handed out into the
cursor, and its key twice, because it stepped past the key's
older versions before it returned. It now returns on the
visible version, the row a view into RocksDB's block, and
steps past the rest at the next call (`../sm/PIN.md` §8 step
7). A sequential, a sample and every index scan read rows and
entries through it, so none copies a row in sm now.

- **digres changes nothing**: a cursor's row lasted until the
  cursor's next call before, and still does.
- **SSI sees no change**: the versions newer than the visible
  one are judged before it returns, as before, the older ones
  were only ever stepped over, and `last_examined` is set when
  the key's run begins. The 109 isolation scenarios match.
- **The key is copied once**, into `last_examined`, which is
  also the key handed out.
- `../sm/sm1/test/test_access.cc` checks a cursor's row
  through other calls, the key's older versions passed over,
  and a seek after a row handed out.

## Step 268: an index-only scan's key column is decoded once

sm2's `idx_scan_key`, which gives an index-only scan its key
columns, unescaped a text column's key into a `std::string`,
then copied it into a varlena with `make_varlena`. It now
decodes it behind four bytes of room and writes the varlena's
header there (`varlena_in_room`), so the key is copied once,
as PostgreSQL's btree copies an entry into `currTuples`
(`../sm/PIN.md` §8 step 6). digres changes nothing.

- **A variable-width type's `key_decode` appends** to the
  string it is given, which may hold the room; a fixed-width
  one's still sets it.
- `../sm/sm2/test/test_index.cc` gives back a text past the
  1-byte header's 126 bytes, with a NUL in it, as the varlena
  `make_varlena` makes.

## Step 269: a statement's pins go at its end

A pin is released by its ref's next read, an unpin, or the end
of the transaction. A scan an error frees leaves its ref
behind, and its pin, an iterator and the blocks it holds,
lasted until the transaction's end: in a transaction that
loops on a savepoint, a failing statement and `ROLLBACK TO`, a
pin a turn (`../sm/PIN.md` §7).

- **`smstore_statement_end()`**, called by
  `finish_xact_command()` at the end of each client statement,
  releases every pin the session holds (`sm_release_all_pins`),
  and forgets the kept row, whose pin goes with them. A
  statement SPI runs is no client statement, and ends nothing.
- **Not while a cursor is open**: a FETCH leaves its plan's
  slots holding rows that the next FETCH may read, so with
  any portal ready (`ThereAreNoReadyPortals()`) nothing is
  released, and the pins wait for the cursor's close and the
  next statement's end, or the transaction's. A suspended
  portal of the extended protocol and a holdable cursor count
  too, which only keeps pins longer.
- **`ThereAreNoReadyPortals()` ends its scan** of the portals
  before it returns false. PostgreSQL's left it open, which
  COPY FREEZE, its one caller, could afford; called at every
  statement's end with a cursor open, it leaked a scan each
  time, until "too many active hash_seq_search scans" and a
  PANIC. `test/server.sh`'s cursors over temporary files met
  it, and are its test.
- `test/slt/statement_pins.test`: a cursor over a nested loop
  fetched across other statements, and a read after a
  savepoint undid a failed statement. The release itself is
  not seen from SQL: a released row in a memtable still reads
  right, so neither case fails without a sanitizer if the
  check were wrong.

## Step 270: a join's re-check checked against PostgreSQL

`../sm/sm3/SM3_DESIGN.md` §3.1 said, from memory, that under
READ COMMITTED the re-check of an UPDATE whose target row
changed uses the rows of the other tables it joined as they
were, not their newest versions. PostgreSQL 18's source
agrees: a table read and not locked gets `ROW_MARK_REFERENCE`
(`select_rowmark_type()`), and `EvalPlanQualFetchRowMark()`
fetches its row by the joined ctid with `SnapshotAny`.
`smtable_fetch_row_version()` reads a row as the statement
sees it, which is that version, so digres agrees too.

- **One case differs**: a table joined to itself. Its copy
  has the target's rowno, and `smstore_fetch_row()` gives the
  newest version the write kept, where PostgreSQL gives the
  one joined, by a ctid digres has no version in. Recorded in
  `README.md`'s known incompatibilities.
- **`test/server.sh`** joins the target of a held update to
  `tj`: on a column the first session changed, the second
  updates none; on one it did not, it updates the first's row,
  though the first also moved the `tj` row out of the WHERE.
  And a self-join updates none, where PostgreSQL would update
  one. `after_update` is now `after_held` with the update it
  held.

## Step 271: ADD COLUMN with a volatile default

ALTER TABLE ... ADD COLUMN with a default that calls something
volatile, `random()`, `clock_timestamp()` or `nextval()`, was
refused: sm3's `add_column` gives the rows already there one
value, evaluated once. PostgreSQL rewrites the table, so each
row gets a value of its own.

- **`add_column()`** makes such a column without NOT NULL and
  with no value for the rows there, then `fill_column()`, in a
  statement of its own that sees the column, scans the table
  and updates each row with the default evaluated for it. The
  cursor does not see the rows the loop writes, so each is
  filled once; the column is in no index, so no index changes.
  It costs a version a row, which compaction removes.
- **NOT NULL is set after**, by sm3's new `set_not_null`
  (`sm_cat_set_not_null()`), in another statement, which opens
  the table exclusive and refuses if a row holds a null:
  PostgreSQL's "column ... contains null values".
- **A new statement is begun by the session's next fetch**,
  `xact_session()`, not by `CommandCounterIncrement()` alone,
  so the check fetches the session again after it.
- **`value_to_sm()`** is `default_value()`'s conversion of a
  value to sm's datum, which both now use.
- A `serial` or an identity column is still refused.
- `test/slt/add_column_volatile.test`; `../sm/sm3/test/
  test_table.cc` tests `set_not_null`. `test/server.sh`'s
  ALTER TABLE, which expected the refusal, now adds such a
  column to al1, counts three values, and drops it.

## Step 272: ADD COLUMN of a serial or an identity column

ALTER TABLE ... ADD COLUMN of a `serial`, `bigserial` or
`smallserial` column, or of one `GENERATED ... AS IDENTITY`,
was refused. Each needs a sequence of its own, and every row
already there its next value, as PostgreSQL's rewrite of the
table gives one.

- **`add_generated_column()`** runs such an ADD COLUMN through
  PostgreSQL's parse analysis, `transformAlterTableStmt()`, on
  a statement of that one subcommand, as CREATE TABLE's goes
  through `transformCreateStmt()`. It makes the sequence its
  CREATE SEQUENCE describes, adds the column it rewrote, and
  then runs the OWNED BY that gives a serial's sequence its
  column; a statement of its own between each, so each sees
  what the last made. The NOT NULL constraint it adds as a
  subcommand is the column's own already.
- **The table is locked first** (AccessShareLock): parse
  analysis opens it assuming so, and digres's ALTER TABLE
  locks in sm only.
- **The sequence has no role owner**: parse analysis gives it
  the table's, which digres's CREATE SEQUENCE refuses, and
  the one CREATE TABLE makes for a serial has none either.
- **`add_column()`** takes the column as parse analysis made
  it: its default in `raw_default`, `nextval()` for a serial,
  so the fill of step 271 numbers the rows. An identity
  column is filled from its sequence by a `NextValueExpr`,
  then made NOT NULL, then identity by sm3's new
  `set_identity` (`sm_cat_set_identity()`), which makes the
  column own the sequence for good, as `create_table` does.
- **A draw is not a flush**: each row's `nextval()` takes the
  next value of sm1's lease, a million values a synced write
  (`../sm/sm1/SM1_DESIGN.md` §4.2), and reads the sequence's
  definition. No batch of values is taken for the fill.
- `test/slt/add_column_serial.test`; `../sm/sm3/test/
  test_table.cc` tests `set_identity`. `test/server.sh`'s
  ALTER TABLE, which expected a serial refused, now adds one,
  lists 1 to 3, and drops it.

## Step 273: a release build, and digres --build

The benchmark of `../bench/` measures digres against
PostgreSQL 18, so only builds optimized and without
assertions (`../bench/CODE.md`). digres's build is the tests':
`-O0 -g` with `Assert()`; sq's is `-O0 -g`; and the system's
RocksDB, which it links, Ubuntu's, keeps its `assert()`s.

- **`make release`** builds `build/digres-release`, every
  object again as NAME.rel.o at `-O2 -DNDEBUG` without
  `USE_ASSERT_CHECKING`, as `make tsan` has its own objects.
  It links sq's new `libsq-release.a` (`-O2 -DNDEBUG`), sm
  built into `sm/build/release` (`-O3 -DNDEBUG`), and RocksDB
  8.9.1 built from source at `-O3 -DNDEBUG`, statically. `dg.mk`
  keeps the `.rel.o` objects from its stale list and from the
  debug link; `cxx_thread_local.py` checks the debug objects
  alone, as the release ones are of the same sources.
- **`test/rocksdb_tsan.sh` is `test/rocksdb.sh tsan|release`**,
  and writes the flags it builds with beside the library.
- **`digres --build`** prints the flags of the server's
  objects, of sq (`sq_build_flags()`) and of sm
  (`sm_build_flags()`, from each level's `build_flags()` and
  the wrapper's), and the RocksDB it links, which the link
  step compiles in (`rocksdb_build.cc`): its version, and the
  system's or a static one, with its flags.

## Step 274: an index key names its default operator class

`test/regress/README.md`: 5 files stopped at `an index key is
a column, with no collation or operator class, yet`, which
access/sm `check_create_index()` raised for any key that named
either. An sm index is keyed by its type's default btree
operator class and its column's collation, as the relcache
describes it (`RelationInitIndexAccessInfo()`): naming those
asks for nothing else.

- **`check_index_key()`** takes an operator class that
  `ResolveOpClass()` finds is the type's default, and a
  collation that is the column's. It refuses another
  (`text_pattern_ops`, `collate "C"` on a column of the
  default), and raises PostgreSQL's errors for an operator
  class that does not exist or does not take the type, for
  options (no core btree operator class has any), and for a
  collation on a type without.
- **An expression key is refused alone**: `an index key is a
  column, not an expression, yet`.
- **Opclasses are read from the compiled-in catalog**:
  `OpclassnameGetOpcid()`, `OpclassIsVisibleExt()`,
  `ResolveOpClass()` and ruleutils `get_opclass_name()` read
  `cat_opclass()` and `cat_opclasses()` for the syscache of a
  `pg_opclass` digres has not.
- `test/slt/index_key.test`: default operator classes named,
  descending and with the default collation, used by a plan
  and by UNIQUE; PostgreSQL's errors; the refusals.
- The regression suite: `create_index` (81 to 77 hunks),
  `btree_index` (27 to 26) and `plpgsql` (223 to 218) pass
  their index keys; `insert_conflict` and `temp` stop at
  `text_pattern_ops` and an expression key.

## Step 275: an index scan keyed by another integer type is bounded

`smstore_beginscan()` made an index bound only of a key of
the column's own type. `WHERE id = 500000` on an `int8` column
compares with an `int4` constant, by a cross-type operator, so
the scan had no bound: it read every entry of the index, and
for an index scan every row, and kept the one `keys_met()`
passed. The benchmark of `../bench/` met it: a point read of
a million rows took 5 seconds where PostgreSQL takes 2 ms.

- **`plain_key()` takes an integer key of another integer
  type**, `int2`, `int4` or `int8`, whose value the column's
  type holds, and the bound is the value as a datum of the
  column's type (`int_key_datum()`). A value it does not hold
  is no bound, as before; every key is still checked of each
  row, so a bound only narrows the scan.
- `test/server.sh`: two SERIALIZABLE transactions read one key
  each of an `int8` table by its index, with an `int4`
  constant, and insert one far from the other's; both commit.
  Without the bound each read's range was the whole index,
  and the second failed with SQLSTATE 40001.

## Step 276: a row is deformed as far as it is read

A table in sm's slot was virtual: the executor compiles no
fetch steps for one, so `tuple_into_slot()` deformed every
column a plan might read before any was asked for, and a
plan's physical target list asked for all of them. `count(*)`
of a table of 50 columns spent a quarter of its time
deforming columns nobody read (`../bench/`). sm's pin now
deforms its row as far as it is read (`../sm/ROW.md`), and
digres's slot holds the pin's ref.

- **`TTSOpsSmRow`** (`smstore.c`): a virtual slot with the ref
  of its row. `getsomeattrs()` deforms from `tts_nvalid` by
  `sm_pin_deform()`; `materialize()`, `copyslot()` and the
  tuple copies deform every column first, then do as a
  virtual slot does. With no ref it is a virtual slot.
- **The sequential, index, bitmap heap and sample scans store
  the ref** (`ref_into_slot()`): a cursor's row by
  `sm_rel_scannxt()`'s ref form, a row by rowno by
  `pin_row()`. A lent copy, read by tid for a TID scan, an
  update, a lock or RETURNING, is deformed as before
  (`read_row()`, `tuple_into_slot()`).
- **`keys_met()` asks the slot for each key's column** before
  it reads it: it read `tts_values` as an eager deform left
  them.
- **The column counts go** from the scans that store a ref:
  `smstore_beginscan()`, `smstore_index_beginscan()` and
  `smstore_beginfetch()` take none, `smtable_scan_set_natts()`
  and the executor states' `sm_natts` are gone, and the scans
  take their slot type from `table_slot_callbacks()`.
- `test/slt/row_deform.test`: a filter on a late column with
  every column read, sorts and hash joins that copy rows,
  nested loops keeping the outer row, cursors with and without
  hold, UPDATE and DELETE RETURNING. It failed at the index
  scans before `keys_met()` asked the slot, and crashes the
  server when the copies do not deform first.

## Step 277: shared_buffers sizes sm's block cache

sm gave each store's column family RocksDB's default block
cache, 32 MB of its own, so a scan of a table larger than that
read every block from the OS each time and put it in the cache
again. sm1 now makes one block cache an environment's every
instance and column family reads through, sized at attach
(`../sm/sm1/SM1_DESIGN.md` §12.2).

- **`shared_buffers` is back**, `PGC_POSTMASTER`, in 8 kB
  blocks, 128 MB by default, as PostgreSQL's: `datadir_load()`
  hands it to `sm_attach()` as `block_cache_bytes`. It was
  deleted with the buffer pool (step 68).
- `postgresql.conf.sample` has it again; `test/set_guc.sql`
  shows its default.
- `../bench/run.sh` sets it to 2 GB on digres as on
  PostgreSQL.

## Step 278: the planner reads sm3's statistics, not a copy

Q7 of `../bench`, one row by its key, spent about half the
backend's time rebuilding a table's statistics: the wrapper
copied sm3's into a form of its own on every `sm_tbl_stats()`,
and kept the copy only while a caller held it, so each of the
planner's several asks a query copied every column's values
again. sm3 keeps them in memory already; `sm_stat_t`,
`sm_stats_index_t` and `sm_stats_t` are now sm3's own types, in
`../sm/include/sm_stats.h`, and `sm_tbl_stats()` hands out
sm3's object (`../sm/OBJECTIVE.md`, `../sm/DESIGN.md`).

- **`has_index` and `has_correlation` go**: a statistic's
  `index` and `correlation` are `std::optional`, as sm3's
  were; `smstore_statistic_tuple()` and `put_stats_rows()`
  test them as such.

## Step 279: an index-only scan decodes only the index's columns

`../sm/NEXT.md`'s scan item: an index-only scan reads the row
when sm says the entry's key does not give its columns back --
the key was cut, or a text column is under a non-deterministic
collation -- and `entry_from_row()` then deformed every column
of the row, to take the few the index holds.

- **Each index column is decoded alone**, with
  `sm_tbl_decode()`, straight into the slot: sm2 finds an
  attribute by its offset, less the widths of the nulls before
  it (`../sm/sm2/TUPLE.md` §6), so the cost is the index's
  column count, not the row's. A dropped column, a null and a
  column added after the row read as `sm_tbl_deform()` reads
  them.
- **The scan's room for a row is gone**: `row_values`,
  `row_isnull` and `row_natts`.
- `test/slt/index_only_cut.test`: an index on a table's fifth
  and third columns, INCLUDE the second, whose 5,000-byte keys
  sm cuts, read by an index-only scan beside short keys and
  nulls. A log line put in `entry_from_row()` for the run
  showed the three cut keys read through it.

## Step 280: a session keeps the statistics rows it makes

After step 278, Q7 of `../bench` still spent 9% of the server's
time making `pg_statistic` rows: three a plan, for the key's
condition, its index column and `c1`'s width, each converting
every common value and histogram bound to a datum and building
its arrays again (`STATISTICS.md` §2.3).

- **`smstore_statistic_tuple()` keeps each row** in
  `smstore_state_t`, by relation and column, as its bytes, with
  the `sm_stats_t` it was made from and the column's type and
  collation. A later call with the same statistics, type and
  collation returns a copy (`heap_copytuple`); callers free it
  as before.
- **An ANALYZE is seen by its new statistics**: sm3 publishes a
  new object, so the kept row no longer matches. The entry
  holds the old object, so its address cannot be reused while
  the entry is there.
- **A row is kept only once made**: an error midway leaves no
  entry, which would read as no statistics. A column with none
  is kept as empty bytes. 1,024 rows at most, then start over.
- `test/server.sh`: in one session, an estimate follows a second
  ANALYZE (1,000, then 5,000); it fails if the kept row is used
  whatever the statistics.

## Step 281: locking a table copies no definition

After step 280, Q7 of `../bench` spent 6% of the server's time
copying a table's definition: every `sm_tbl_open()`, from the
parser's lock, built `sm_table_def_t` -- 50 columns with their
names and defaults, the schema, the indexes -- and freed it at
the transaction's end, while the lock read only its version.

- **sm builds the definition at the first `sm_tbl_def()`**,
  and `sm_tbl_version()` gives the version alone
  (`../sm/include/sm.h`). `smstore_lock_relation()` and
  `smstore_trylock_relation()` check the relcache with it.
- **`sm_tbl_def()` can now fail**, out of memory: its callers,
  `smstore_table_desc()`, `smstore_identity_sequence()` and
  `fill_column()`, raise sm's error.
- No test of its own: what changed is when the copy is made.
  `test/server.sh` reads the version through the lock
  throughout: with `sm_tbl_version()` made to return 0, it
  fails from its first index on.

## Step 282: an index's estimate does not size its table again

After step 281, Q7 of `../bench` spent 3.8% of the server's time
sizing its table and index for the plan: three
`sm2::relid_size()` calls, the heap twice, as
`smstore_estimate_index_size()` took the table's growth since
the analyze from `growth()` again.

- **`smstore_estimate_index_size()` takes the table's rows**,
  `rel->tuples`, which `get_relation_info()` estimated just
  before (`plancat.c`), and scales the index's entries by them
  over the analyze's live rows (`STATISTICS.md` §2.2).
- **A table analyzed whose heap now holds no bytes** is planned
  at ten pages, as before; its indexes now scale by that guess,
  where they read as empty.
- **sm1 asks RocksDB less** (`../sm/sm1/API.md`,
  `sm1::approxsize`): its bound above every key is made only
  when there is no bound, and the count of sealed memtables not
  yet flushed is kept under the tree's version, as the files'
  bytes are.
- No test of the index's scaling, before or now: it shows only
  in an index scan's cost.

## Step 283: a catalog relation is found by name by a binary search

An unqualified name is looked for in `pg_catalog` first, so
`get_relname_relid()` asked `cat_relation_named()` about every
user table a query names, twice, and each time it compared the
name with all 28 catalog relations: 0.4% of Q7 of `../bench`.

- **`gen_cat.py` writes `relations_by_name`**, the relations in
  name order, and `cat_relation_named()` searches it, as the
  generator's other lookups by name do.
- **`get_relname_relid()` asks once.**
- With the order reversed, `test/server.sh` fails 20 checks,
  from the lock status views on: they are not found.

## Step 284: VACUUM compacts a table's store

VACUUM did nothing, and VACUUM ANALYZE not even the ANALYZE. A
store loaded through its memtable kept the last level-0 files
of the load for good: RocksDB compacts level 0 at four files.
A point read of `../bench`'s `wide_mixed` probed four sorted
runs where it needs two, 8 to 17% of its time.

- **`smstore_vacuum()` compacts each table's store**
  (`sm_tbl_compact()`, `../sm/sm1/API.md` `sm1::compact`): its
  memtables flushed and every level merged into the bottom
  one. Each table is opened as a reader, so writers go on and
  DDL waits; a bare VACUUM takes every table, as a bare
  ANALYZE does (`every_table()`, which both share now).
- **VACUUM ANALYZE analyzes after**, so the sizes it records
  are the compacted store's. FULL, FREEZE and the rest have
  nothing more to do in sm and are accepted.
- **A table that is not there is an error**, as in
  PostgreSQL; `test/server.sh` checked that VACUUM did nothing,
  and now checks this.
- **The versions no reader needs go with the compaction**;
  the tombstones among them go after, by sm1's purge.
- `../bench/run.sh` runs `VACUUM (FREEZE, ANALYZE)` on both
  systems after the load (`../bench/DESIGN.md` §3).
- `test/server.sh`: VACUUM ANALYZE estimates a table's 20,000
  rows exactly; with every row deleted, VACUUM takes it under
  a third of its size; a view is passed over with a warning.
  It fails with the compaction left out, and with the ANALYZE.

## Step 285: a table with a primary key is keyed by it in sm

sm keys a table's rows by its primary key now, and every row
keeps an `_id`, the number digres still calls `rowno` and packs
into the tid (`../sm/CLUSTER.md`, its §11 step 1). digres reads
and writes every row by that number, as before, and sm finds
the row's key through the table's `_id` index. Two things in
digres had to change with it:

- **A primary key's row lock names no tid.** sm locks such a
  row by its key, not its `_id`, and does not read the row to
  learn the one from the other (`../sm/CLUSTER.md` §10,
  question 5): `sm_lock_status()` gives the row `rowno` 0, and
  `pg_lock_status()` shows it as a tuple of its table with page
  and tuple NULL. `test/server.sh`'s lock status checks lock a
  row of such a table, and expect that now.
- **Adding or dropping a primary key forgets the table's
  handles** (`forget_table()`), as an ALTER TABLE that changes
  the table does. sm keys the rows anew, in a new store when
  the table has rows (`../sm/CLUSTER.md` §8), and a handle the
  transaction opened before reads the rows as they were laid
  out: pgregress's `join` made temporary tables with primary
  keys and scanned them through such a handle.

Reading by key, the primary key's index scan as a range of the
rows, and the system column's new name are the next step
(`../sm/CLUSTER.md` §11).

## Step 286: digres reads a table with a primary key by its key

The second step of `../sm/CLUSTER.md` §11, its first part. A
table keyed by its primary key in sm was read through its `_id`,
two seeks a row, as digres asked for every row by its tid:

- **A scan of the primary key's index reads the rows**, in key
  order, from one cursor (`sm_tbl_idx_scan_rows()`): the rows
  are the index's entries. `smstore_index_beginscan()` opens
  one for an index with `indisprimary`; an index-only scan of it
  decodes its columns from the row. `../bench`'s Q6, 10,000 rows
  by a range of the key, went from 9.0 ms to 4.2.
- **A secondary index's row is read by its key**
  (`sm_idx_scan_row()`, `sm_tbl_read_key()`), which its entry
  carries: one seek, as before the pivot.
- **A sequential scan reads the rows in key order**
  (`sm_tbl_scan_rows()`), one sorted run. A sample or TID scan
  still reads by `_id` (`smstore_beginscan()`'s `by_rowno`).
- **Unordered results compare as multisets** in pgregress, its
  test and its trim (`test/rowset.py`): PostgreSQL returns a
  table's rows in the order they went in, digres in the key's.
  `foreign_key` is trimmed again for nine statements: `UPDATE pp
  SET f1 = f1 + 1` over keys 12 and 11 meets its own duplicate
  in key order (`../sm/CLUSTER.md` §7), and the foreign key
  checks after it go with it.

## Step 287: no bitmap scan of a table in sm

A bitmap scan read its rows by `_id`, in `_id` order: a seek a
row, two in a table with a primary key, where PostgreSQL's
sorted TIDs read pages in order. A bitmap of sorted primary
keys, fetched through one cursor, was planned
(`../sm/CLUSTER.md` §10, question 6); it was dropped at review
for planning no bitmap scan at all, as InnoDB and CockroachDB
do without one:

- **An index in sm has no `amgetbitmap`** (`smindex.c`), nor a
  table in sm `scan_bitmap_next_tuple` (`smtable.c`), so the
  planner builds no bitmap path. A secondary index scan reads
  its rows by key already; BitmapAnd and BitmapOr are what is
  lost: an OR over two indexes is a sequential scan.
- **`RowBitmap` is gone** (`nodes/rowbitmap.c`, step 56's), and
  the sm branches of the bitmap nodes with it, which are
  PostgreSQL's again; `smstore_beginfetch()` and
  `smstore_fetch()` went with their one caller.
- **`server.sh` checks the planner plans no bitmap** for an OR
  over two indexes, in place of its bitmap scan checks;
  `slt/scan_columns.test` loses its bitmap heap scan step.
- **pgregress does not compare a plan where PostgreSQL's has a
  bitmap scan** (`pgregress.py`'s `bitmap_plans()`): the
  statement still runs, and the results after it are compared.
  Twelve plans in five files differ so, `create_index`'s six
  among them; two are dearer than PostgreSQL's. `join`'s `id IN
  (2, 3)` is a sequential scan, as an index in sm takes no
  array of values yet (`amsearcharray`), and `groupingsets`'s
  subplan by its primary key scans its table for each outer row.

## Step 288: no TID scan of a table with a primary key

A tid carries a row's `_id`, and a table with a primary key
reaches its row by `_id` only through the `_id` index, two
seeks. `_id` is to go from such a table (`../sm/CLUSTER.md`
§10, question 6), so the planner no longer plans a row's
fetch through it:

- **`create_tidscan_paths()` makes no TID scan of a table with
  a primary key** (`optimizer/path/tidpath.c`'s
  `has_primary_key()`), but `WHERE CURRENT OF`'s, the only
  correct plan for it: `rowno = n`, `ctid = tid` and a join on
  `rowno` are filters there. A keyless table's TID scans are
  as they were: its `_id` is its key, one seek.
- **The executor still fetches by tid** in a table with a
  primary key: the target of an UPDATE or a DELETE, a lock,
  EvalPlanQual and `WHERE CURRENT OF`, through `_id`.
- **`slt/rowno_scan.test` checks a table with a primary key**
  plans no TID scan for `rowno = n` and a join on `rowno`, and
  `slt/current_of.test` updates and deletes through a cursor
  on one.

## Step 289: a write takes its row's key from the scan on it

In a table with a primary key, a row's tid reaches the row
through the `_id` index: an UPDATE by the key read the `_id`
entry and the row to fetch it, then the `_id` entry again to
write it. The scan that handed the row out has both, the key
in its cursor and the row in its pin (`../sm/CLUSTER.md` §6):

- **Each open scan records the row it handed out last**
  (`smstore_scan_t`'s `cur`), and `open_scans` lists the
  scans, forgotten at a statement's start, a subtransaction's
  end and the transaction's, so one an error freed is never
  looked at. `scan_at()` finds the scan on a rowno.
- **`smstore_fetch_row()` copies the row from that scan's
  pin**; `smstore_update()`, `smstore_delete()` and
  `smstore_lock_row()` pass sm its key, from the cursor
  (`sm_rel_scan_key()`, new, or `sm_idx_scan_row()`).
- **sm's `sm_tbl_update()`, `sm_tbl_delete()`, `sm_tbl_lock()`
  and `sm_tbl_trylock()` take the key**, `NULL` to find it by
  `_id` as before. sm3 trusts it; a debug build checks it is
  what the snapshot's `_id` entry says (`key_given()`).
- **A sort or a hash between the scan and the write** loses the
  scan's row, and the `_id` is read as before.
- Fifty UPDATEs by the primary key a transaction, of a table of
  100,000 rows, one client: PostgreSQL 112 transactions a
  second, digres 92 to 110.

## Step 290: TABLESAMPLE of a table with a primary key, by key

A table with a primary key is to lose its `_id`
(`../sm/ROWKEY.md`), and with it the rownos a sample scan's
blocks were made of. Its blocks are now in key order:

- **SYSTEM picks slots of the table's bytes**, as many as its
  bytes in sorted files and memtables fill 8 KB blocks
  (`smstore_sample_slots()`), and reads only those: each
  slot's rows lie between the keys at its two ends, which sm
  finds from its size estimates with no row read
  (`sm_tbl_keys_at()`), and the scan, in key order, seeks
  forward to each (`smstore_sample_slot()`,
  `sm_rel_scan_seek_key()`), a row read past a slot kept for
  the next (`smstore_sample_row()`). A slot holds as many rows
  as it holds, every row of it, as SYSTEM takes whole pages:
  the method is asked for no offsets, as a slot may hold more
  rows than a page has. The
  slot after the last one read starts where that one ended,
  its start not estimated again: a memtable's estimate is
  sampled anew each call, and two of one boundary disagreed,
  losing the rows between (SYSTEM (100) read 9,552 rows of
  10,000).
- **BERNOULLI reads every row**, as it does in PostgreSQL: its
  blocks are 256 rows each of the scan in key order, a row
  taken by its place in it (`smstore_scan_nth()`).
- **A keyless table samples as before**, by rowno.
- **`slt/tablesample.test`** runs on a table with a primary key
  and on a keyless one.

## Step 291: a duplicate key's row named by its key

A unique index whose build met a duplicated key was reported
with the two rows' `_id`s: digres read one to say the key, and
named both in a hint, "Rows 1 and 3 have it". A table with a
primary key is to have no `_id` (`../sm/ROWKEY.md` §2.5):

- **sm names one row by its key** (`sm_errdup()`'s `row`, a
  key past the relid), as the index's failed state keeps it;
  digres reads the row by it (`read_row_at()`,
  `sm_tbl_read_key()`) to say "Key (a)=(1) is duplicated.",
  as before. One row is enough: both hold the key.
- **The hint goes.** PostgreSQL has none, and it named rows by
  rowno, which a table with a primary key will not have; it
  also said "Rows 0 and 2" of a table made in the
  transaction.
- **`test/server.sh`'s duplicated-key case** expects no hint.

## Step 292: a row is named by its key, not its TID

A table with a primary key is to have no `_id`, so no rowno to
pack into a TID (`../sm/ROWKEY.md` §3). The executor now names a
row of a table in sm by its key: the values of its primary key's
columns, or a keyless table's rowno alone (`access/rowkey.h`'s
`RowKey`, built in `smstore.c` from sm's table definition).

- **The table AM takes a key**: `tuple_fetch_row_version`,
  `tuple_delete`, `tuple_update` and `tuple_lock` take a
  `const RowKey *` where they took an `ItemPointer`, and
  `smstore_fetch_row()`, `smstore_update()`, `smstore_delete()`
  and `smstore_lock_row()` pass it to sm (`sm_row_key_t`). The
  row sm keeps for EvalPlanQual's re-read, and the scan on the
  row (`scan_at()`), are matched by the key's encoding
  (`sm_tbl_row_key()`), not by rowno.
- **The plan carries the key's columns** where it carried
  `ctid`: as row identity columns of an UPDATE or DELETE
  (`add_row_identity_columns()`), "rowno" in a keyless table and
  `rowkey<relid>_<n>` in one with a key; and as a locking row
  mark's junk, `key<rowmark>_<n>` (`add_row_key_junk()`).
  digres has no table inheritance (`PLAN.md` §7), so no row
  mark reaches a child through its parent.
- **A row mark that locks nothing copies the row**
  (`select_rowmark_type()`): PostgreSQL's re-fetch by reference
  fetched the very version again, which a copy is. A table in sm
  has no composite type, so the copy is a RECORD whole-row Var
  (`make_rowmark_wholerow()`). A table joined to itself is now
  re-checked as PostgreSQL re-checks it, on the version it
  joined: the known incompatibility of step 270 is gone from
  `README.md`, and `test/server.sh` expects PostgreSQL's row.
- **A table whose rows the plan names**, an UPDATE's or a
  DELETE's target or a locking row mark's, gets no index-only
  scan (`indxpath.c`'s `rows_named()`): EvalPlanQual cannot
  re-check one. Its `ctid`, which no index has, kept it off one;
  its key's columns, a primary key index's, do not.
- **A catalog relation** (`SYSCATALOG.md`) is named by its rowno
  in a plan that would write it: the permission check refuses it
  before it runs, as it did.
- **ModifyTable, LockRows, triggers, EvalPlanQual,
  `WHERE CURRENT OF` and the TID scan** take the key:
  `ExecUpdate()` and the rest take a `RowKey *`, LockRows keeps
  the locked row's key for CURRENT OF (`ExecRowMark`'s
  `curKey`), `execCurrentOf()` reads the cursor's row's key from
  its scan's slot, and the TID scan visits keys: a keyless
  table's `rowno = n`, or CURRENT OF's row.
- **`ON CONFLICT DO UPDATE`** takes the key from the row the
  arbiter found, which sm now hands back.
- **What is left of the TID** is in slots' `tts_tid`, AFTER
  trigger events, foreign tables' row identity, and the `ctid`
  system column; they go next.

## Step 293: no TID range scan, no currtid2()

No row has a TID (`../sm/ROWKEY.md` §3.1), so nothing is left
for a scan by a range of TIDs or a function that follows one:

- **The TID range scan is deleted**: `nodeTidrangescan.c` and
  its header, `TidRangePath`, `TidRangeScan` and
  `TidRangeScanState`, `create_tidrangescan_path()`,
  `cost_tidrangescan()`, the planner's range quals in
  `tidpath.c`, EXPLAIN's "Tid Range Scan", and the table access
  method's `scan_set_tidrange` and `scan_getnextslot_tidrange`.
  `RelOptInfo`'s `amflags` went with its one flag,
  `AMFLAG_HAS_TID_RANGE`. Nothing planned one since step 213:
  its quals name `ctid`, which no query can.
- **`currtid2()` is deleted** (`tid.c`), with the table access
  method's `tuple_tid_valid` and `tuple_get_latest_tid` and
  `table_tuple_get_latest_tid()`, which only it called.
- **pgregress `tid`**: its three `currtid2()` calls that ran,
  each an error on PostgreSQL, are taken out.
- The `tid` type stays, as a built-in type; no column may be
  one.

## Step 294: no index check by TID

PostgreSQL checks an exclusion constraint, and a deferred
unique constraint at commit, by the TIDs of the rows an index
holds. digres has neither, `check_create_table()` and CREATE
INDEX refusing both, and no row has a TID
(`../sm/ROWKEY.md`), so the code that did is deleted:

- **`ExecInsertIndexTuples()` inserts nothing**: sm writes a
  table's indexes with its row, and only a table in sm is
  written. The rest of `execIndexing.c` but opening and
  closing a result relation's indexes is gone:
  `check_exclusion_constraint()`,
  `check_exclusion_or_unique_constraint()`,
  `index_unchanged_by_update()` and the WITHOUT OVERLAPS
  check.
- **`unique_key_recheck()` is deleted** (`constraint.c`), the
  trigger function of a deferred unique constraint, with its
  `pg_proc` row; `index_constraint_create()` makes no trigger,
  and `AfterTriggerSaveEvent()` no longer looks for one.
- **`IndexCheckExclusion()` is deleted** (`index.c`), the scan
  that checked a new exclusion constraint's rows.
- **Left on purpose**: the index access method's TID calls
  (`index_insert()`, `xs_heaptid`, `table_index_fetch_tuple()`),
  which an index in sm refuses, and the trigger calls'
  `recheckIndexes`, NIL now.

## Step 295: no ctid

The last of the TID (`../sm/ROWKEY.md` §3.1): a row of a table
in sm is named by its key (step 292), and nothing in digres
carries a TID now.

- **`ctid` is no system column**: `SelfItemPointerAttributeNumber`
  is gone, and `heap.c`'s `SysAtt` starts at `xmin`'s -2, -1
  being no attribute's. A table may have a column named `ctid`,
  and naming one it lacks is "column does not exist", with no
  hint.
- **A slot has no `tts_tid`**: `tts_rowno` holds its row's
  rowno in sm, 0 if none, for the system column `rowno`
  (`slot_getsysattr()`). `smstore_rowno_to_tid()`,
  `smstore_rowno_fits_tid()`, `smstore_tid_to_rowno()` and
  `ExecSlotRowno()` went with it.
- **An AFTER trigger event has no ctid**: a row-level trigger's
  tuples are always kept in a tuple store (step 177's
  `trigger_tuples_stored()`), so `ate_ctid1` and `ate_ctid2` are
  gone, and with them a cross-partition update's partition
  OIDs, which only said where to fetch them by ctid. A
  statement's event is `AFTER_TRIGGER_NO_TUPLE`.
- **A TID scan is of rownos alone**: the planner's and the
  executor's `ctid = tid` cases are gone (`tidpath.c`,
  `nodeTidscan.c`), as are the selectivity of a `ctid`
  comparison and its distinctness (`selfuncs.c`) and
  `heap_getsysattr()`'s `ctid`.
- **A foreign table's row marks are copies**: its `ctid` junk
  column (`ctidAttNo`), the FDW's `RefetchForeignRow()` calls
  in LockRows and EvalPlanQual, and `GetForeignRowMarkType()`
  are no longer used. digres makes no foreign table.
- **pg_locks** reckons a keyless row's page and tuple from its
  rowno, as the TID did (`lockfuncs.c`).
- **`TM_FailureData` has no `ctid`**: nothing set or read it.
- **Left on purpose**: `HeapTupleData`'s `t_self`, which
  catalog code and the heap tuple format still use, the `tid`
  type, and the index access method's TID calls (step 294).

## Step 296: a row moved meanwhile is a serialization failure

Since step 292 a write names its row by its key, and sm3's
follow of a moved row went by `_id`, which no write of
digres's takes. A writer at READ COMMITTED whose row a
concurrent update had moved to another primary key re-read it
at the old key, found it gone, and skipped it as deleted:
`UPDATE t SET v = 'x' WHERE v = 'a'` said `UPDATE 0` where
PostgreSQL updates the row. `../sm/ROWKEY.md` §2.3's moved
tombstone, from its step 4, is built now:

- **A move erases the old key with sm1's moved tombstone**
  (sm2 `move_row()`, `sm1::erase_moved`): the newest read of
  the old key, after `SM_ERR_UPDATED`, fails with
  `SM_ERR_MOVED`. A delete's tombstone is as it was.
- **sm3's follow is gone** (`moved_to()`, `write_followed()`,
  and `read_latest()`'s follow): no row is followed by its
  `_id`. Its test, `test_cluster.cc`'s "a writer at READ
  COMMITTED follows a row moved since", is now one of
  `SM_ERR_MOVED`, by key.
- **digres raises `SM_ERR_MOVED` as a serialization failure**
  (`smstore.c`'s `sm_error()`), "could not serialize access
  due to concurrent update", as PostgreSQL refuses a row moved
  to another partition. Above READ COMMITTED sm fails the
  write itself, as before.
- **`test/server.sh`**: an UPDATE and a FOR UPDATE of a row
  whose key changed meanwhile fail with 40001.

## Step 297: a row of a table with a primary key is named by its key alone

`../sm/ROWKEY.md`'s step 4 begins: a table with a primary key
is to have no `_id`, so nothing may name its row by one.

- **sm3's and the C API's calls by `_id`** — `tbl_update`,
  `tbl_delete`, `tbl_read`, `tbl_read_latest`, `tbl_lock`,
  `tbl_trylock` and their `sm_tbl_*` — **are a keyless
  table's**, whose key the `_id` is (`id_key()`); on a table
  with a primary key they fail with `SM_ERR_INVALID`. Their
  optional key, the encoding a scan gave, is gone with
  `key_given()`, `key_to_write()`, `key_of()` and the C API's
  `key_in()`: a write by key needs none.
- **digres's last write by rowno** names the row by its key:
  ADD COLUMN's volatile default (`fill_column()`), from the
  primary key's columns of the row the scan gave, or its
  rowno. `read_row()`, which nothing called since step 292,
  is deleted.
- **The `rel` tool** keeps its verbs by rowno: it makes no
  primary key, so its rows are keyless.
- **Tests**: sm3's keyed tests name rows by key (`at_t`,
  `get_at()`), and check a read by `_id` is refused; the test
  of the scan's key is deleted. `slt/add_column_volatile.test`
  adds a column drawn from a sequence to a table with a
  primary key.

## Step 298: a table with a primary key has no _id

`../sm/ROWKEY.md`'s step 4: a row of a table with a primary key
has no `_id`, and nothing keeps one for it.

- **A row's value is its tuple alone** (sm2 `rel.cc`), in
  either kind of table: a keyless row's `_id` is read from its
  key, `be64(_id)`. A keyed row draws no `_id` from the table's
  sequence, and `rel_insert`, `rel_load` and the scans give 0.
- **The `_id` index is gone**: `id_entry()`, `set_id_entry()`,
  `erase_id_entry()`, `rel_find()`, `rel_find_latest()` and
  `rel_lock_id()`; nothing is written under relid 0xFFFE, which
  no index takes still. A move erases and inserts the row, and
  a delete erases it, with no entry beside. A keyed rel's rows
  are scanned in key order only: `rel_scan` by `_id` refuses
  one, as `rel_reserve` does.
- **An index entry's value is the cut flag, the INCLUDE
  columns and the row's key**, for both kinds of table, with no
  `_id`; `idx_insert`, `idx_load` and `idx_delete` take none.
- **sm3 reads no row to write it by key** (`named()`): the
  interim read of step 1 is gone. The arbiter reserves no `_id`
  for a keyed row, a load opens no `_id` stream, and a rewrite
  that drops a primary key draws the rows' `_id`s anew, in key
  order. A primary key adds no bytes of its own
  (`tbl_idx_size`), its entries being the rows, so
  `pg_relation_size()` of one is 0. The table's sequence is
  still made with it, and a keyed table draws nothing from it.
- **digres**: a sample of a keyed table is by its bytes from
  the start (`sample_table()`), as it has no `_id` limit; its
  growth since ANALYZE is its bytes' (`growth()`); the table
  AM scans a keyed table in key order always
  (`smtable_scan_begin()`), a TID scan's too; an index-only
  scan reads INCLUDE columns from the value's second
  attribute.
- **SQL's `rowno` of a keyed row is 0** until it is refused
  there (`ROWKEY.md` §3.1): `slt/rowno_scan.test`'s keyed
  table finds no row by it.
- **Tests**: sm2's keyed rel tests have no `_id`, and its
  follow test is one of `SM_ERR_MOVED`; sm3's keyed tests
  expect 0 for every `_id`, and a new one adds a primary key to
  a keyless table and finds its rows' `_id`s gone, with nothing
  under 0xFFFE; `server.sh` sizes a secondary index, the
  primary key's being 0.

## Step 299: rowno is _id, on a keyless table alone

`../sm/ROWKEY.md` §3.1, the last of its step 3: the system
column SQL reads by the name sm gives it, and only where a row
has one.

- **The system column is `_id`** (`heap.c`), `rowno` renamed;
  the junk column a keyless table's UPDATE and DELETE carry is
  `_id` too. The C names, `RownoAttributeNumber` and
  `tts_rowno`, stay.
- **A table with a primary key has none**: naming `_id` there
  is "column does not exist" (`parse_relation.c`'s
  `system_column_is_kept()`), as its rows have no `_id` since
  step 298. The planner's check for a primary key before a TID
  scan (`tidpath.c`'s `has_primary_key()`) went with it: no
  `_id` Var can name such a table's row.
- **No table may have a column named `_id`**: CREATE TABLE now
  refuses a system column's name (`check_column_name()`), as
  ADD COLUMN and RENAME did. `rowno` is a name like any other.
- **Tests**: `slt/rowno_scan.test` is `slt/id_scan.test`, its
  table with a primary key now an error by `_id`;
  `slt/system_columns.test` adds a keyed table without `_id`, a
  column named `rowno`, and one named `_id` refused;
  `scan_columns.test` and `point_read.test` name `_id`.

## Step 300: a numrange aligns as its decimal128 bounds do

`ISSUE.md` issue 3: a numeric range in an array or a
multirange lost a bound, `ARRAY[numrange(1, 2), numrange(3,
4), numrange(5, 6)]` giving `[3,2475880078570760549798248448)`
for its second. numrange's bounds are decimal128s, aligned to
8 (`typalign 'd'`), where PostgreSQL's numeric aligns to 4;
numrange and nummultirange kept PostgreSQL's `'i'`. An array
or a multirange puts each range at the range type's alignment,
4, but `range_deserialize()` aligns a bound by its own address:
a range put 4 bytes off 8 had its upper bound read from the
wrong place. The first range, at 8, and a range copied out
alone were right.

- **numrange and nummultirange are `typalign 'd'`**
  (`pg_type.dat`), as PostgreSQL's rule for a range type is its
  subtype's alignment when that is `'d'`; their arrays follow
  (`Catalog.pm`). No table holds a numrange yet, so no stored
  value is read otherwise.
- **Tests**: `test/select_numrange.sql`, an array of three
  numranges, one of two, and a multirange given its ranges out
  of order, and the four types' alignment; pgregress's
  `rangetypes` and `multirangetypes` ported again, their two
  statements back.

## Step 301: a decimal64 is an element of a numrange

`numrange(1.5, 2.5) @> 2.0` was `operator does not exist:
numrange @> numeric`, as were `<@`, `range_contains_elem()`
and `elem_contained_by_range()`: they take `anyrange` and
`anyelement`, and PostgreSQL's resolver wants the element's
type to be the range's subtype exactly. In PostgreSQL every
numeric is the one type; in digres `2.0`, and a `numeric(10,2)`
column, is a decimal64, and numrange's subtype is decimal128.

- **A decimal64 fits a decimal128 subtype**
  (`parse_coerce.c`'s `elem_fits_range()`), in
  `check_generic_type_consistency()` and
  `enforce_generic_type_consistency()`, which resolve the
  element to the subtype. Nothing else is relaxed: two
  `anyelement`s, or an array's element, still match exactly.
- **The element is declared the subtype** for its caller to
  cast: PostgreSQL rewrites a polymorphic argument's declared
  type for an unknown input alone, so a decimal64 left
  `anyelement` was passed as it was, and
  `range_contains_elem()` read its by-value datum as a
  decimal128's pointer and crashed.
- **Tests**: `test/select_numrange.sql` adds a numrange of
  `numeric(10,2)` values, alone, in an array and a multirange,
  with the plan's cast to decimal128, and a decimal64 found or
  not in a numrange by all four; from a VALUES list, not a
  table, as `run.sh`'s 8 sessions share one database. The seven
  pgregress files that use numrange, ported again, are as
  they were: their statements with a decimal64 element read a
  table of numranges, which sm does not store yet.

## Step 302: an index is its table's at once in its transaction

A duplicate key in a table made in the open transaction, with
a UNIQUE column, said `could not open relation with OID N`
(`ISSUE.md` issue 6): the savepoint of the issue's repro had
nothing to do with it. And a unique index made on a table the
transaction had already written was not kept: the duplicate
went in, `INSERT 0 1`.

- **The cause**: a handle of a table holds the definition it
  was opened with (`sm.h`'s `sm_tbl_def()`), and
  `smstore_create_index()` closed the transaction's handles of
  the table for a primary key alone. For any other index the
  descriptor, rebuilt, read the handle's definition, with no
  index: the index was never known as the table's, so
  `index_open()` in `duplicate_write()` looked for a table of
  its oid, and the planner never chose it. A handle opened
  before the index, as an earlier INSERT's, wrote no entry to
  it, so did not check it.
- **The fix**: `smstore_create_index()` calls
  `forget_table()` for every index, as `smstore_drop_indexes()`
  does: the next use opens a handle with the index.
- **Tests**: `test/server.sh` checks both, a UNIQUE column in
  a savepoint and a unique index made after an INSERT, for
  PostgreSQL's error and key. pgregress's `transactions`,
  ported again, has its two statements back.

## Step 303: ON COMMIT DELETE ROWS keeps a foreign key whole

A temporary table that empties at commit, referenced by a
foreign key of a table that keeps its rows, committed and
emptied, leaving the child's rows referencing none
(`ISSUE.md` issue 7). PostgreSQL's `heap_truncate()` first
calls `heap_truncate_check_FKs(rels, true)`, which refuses
the commit; digres's `PreCommit_on_commit_actions()` truncated
each table by `smstore_truncate()` and skipped the check.

- **The check**, in `PreCommit_on_commit_actions()`: each
  table to empty is locked first, as `heap_truncate()` locks
  them, then any table referencing one
  (`smstore_referencing_tables()`, as TRUNCATE asks) that is
  not to be emptied too is PostgreSQL's error, `unsupported ON
  COMMIT and foreign key combination`, and the commit rolls
  back. A table referencing itself, or one emptied with it,
  passes.
- **Tests**: `test/server.sh` checks the error, its detail and
  the rollback, and that tables both DELETE ROWS, and one
  referencing itself, commit empty. pgregress's `temp`,
  ported again, has its COMMIT back, and two statements after
  it that had been taken out as its cascade.

## Step 304: a cursor reads as its DECLARE did on each FETCH

A cursor saw its own transaction's writes made after its
DECLARE (`ISSUE.md` issue 4): a row deleted after it was
gone once the cursor was rewound, `MOVE BACKWARD ALL`, and a
row inserted after it was there. Not rewinds alone: a nested
loop's inner scan, begun again for each outer row in a later
FETCH, missed an inner row deleted since, and an index scan
missed one with no rewind at all, its row read by rowno. A
scan open since the DECLARE kept its view; anything the plan
read anew in a FETCH read in the FETCH's statement, which
sees the transaction's writes so far. PostgreSQL's FETCH runs
the executor at the query's snapshot, with the DECLARE's
command id.

- **sm** (`sm.h`): `sm_stmt_save()` copies the running
  statement's view, its snapshot and mark, and
  `sm_stmt_resume()` runs a saved view inside the running
  statement, until `sm_stmt_end()` puts the running one back;
  `sm_stmt_free()` frees a view. Both are sm3's
  `stmt_begin(outer)` and `stmt_end()`, a trigger's nesting:
  sm1 is unchanged. No pin is needed: a READ COMMITTED
  transaction keeps its begin snapshot registered, at or
  below any statement's (`sm1/SM1_DESIGN.md` §6.2). A view
  saved before its statement read takes its snapshot at its
  first resumed read, and `sm_stmt_end()` keeps it in the
  view for every later resume.
- **dg**: `PortalStart()` saves the view of a named portal, a
  cursor (`PortalData`'s `smView`); `PortalRunInView()`
  (`tcop/pquery.h`) resumes it around each step of its
  executor, `ExecutorRun()` in `PortalRunSelect()`,
  `ExecutorRewind()` in `DoPortalRewind()`, and both in
  `PersistHoldablePortal()`, putting the running statement
  back by `PG_FINALLY` if the step raises; `PortalDrop()`
  frees it. An unnamed portal runs to its end in its
  statement, and saves none.
- **Tests**: `test/select_cursor.sql` (run.sh, 10 files now):
  a rewind after a DELETE and after an INSERT, a nested loop's
  inner row deleted, an index scan's row deleted and updated,
  and a SCROLL cursor WITH HOLD read again at its commit; each
  case fails without the fix.
  `../sm/rel/test/test_stmt_view.rel`: a resumed view sees
  the writes before its save and none since, keeps its
  snapshot under READ COMMITTED, and one saved before a read
  keeps the snapshot its first resume took.
  pgregress's `portals`, ported again, has its `FETCH ALL`
  after `MOVE BACKWARD ALL` back, and five `SELECT * FROM
  uctest` whose markers dated from the first port: digres
  gives their rows today with or without this fix.
- **Not fixed**: issue 5, a FOR UPDATE cursor under
  SERIALIZABLE returning a row updated after its DECLARE, is
  as it was.

## Step 305: FOR UPDATE passes over a row changed since

A FOR UPDATE cursor returned a row its transaction updated or
deleted after the DECLARE (`ISSUE.md` issue 5), where
PostgreSQL's row lock answers `TM_SelfModified` and
`ExecLockRows()` passes over the row. It was not SERIALIZABLE
alone: at REPEATABLE READ and SERIALIZABLE the old row came
back, read at the cursor's view (step 304), and at READ
COMMITTED an updated row came back new, read as the newest
version; a deleted one there was passed over as gone.

- **sm1**: `sm1::lock`, `trylock`, `chain_lock` and
  `chain_trylock` take `as_read`, a lock of a row the running
  statement read: a key whose newest version is the
  transaction's own, written after the statement's mark, fails
  with `SM_ERR_SELFMODIFIED` and takes no lock, as a write of
  it does (`SM1_DESIGN.md` §6.2), checked under the key's
  latch, a temporary store's too (`lock_self_modified()`,
  `lockmgr.cc`). Without it nothing changes: sm2's and sm3's
  own locks, taken to read a key's newest version, the
  transaction's own included, need it so. A first try that
  refused every such lock broke INSERT ... ON CONFLICT, whose
  arbiter locks a key its statement inserted.
- **sm2, sm3**: `rel_lock` and `rel_trylock` pass `as_read`;
  `tbl_lock` and `tbl_trylock`, SQL's row lock behind
  `sm_tbl_lock()`, set it.
- **dg**: `smstore_lock_row()` answers `TM_SelfModified` on
  `SM_ERR_SELFMODIFIED`. `ExecOnConflictUpdate()`'s
  `TM_SelfModified` is PostgreSQL's `TM_Invisible` case there,
  a row its statement inserted: `refuse_on_conflict_twice()`.
- **Tests**: scenario `a18` (`../sm/sm1/mvcc`, its driver's new
  `lockread` verb and `selfmodified` error), at each level, on
  a row updated and one deleted; `test_arbiter.cc`, an
  arbiter's find of a row its statement inserted and SQL's
  lock of it; `../sm/rel/test/test_stmt_view.rel` step 4, a
  lock from a resumed view; `test/select_cursor.sql` step 6, a
  FOR UPDATE cursor after an UPDATE and a DELETE at each
  level, on a temporary table. Each fails with the check
  disabled. pgregress's `portals`, ported again, has its FETCH
  back. Above READ COMMITTED an arbiter's own read still fails
  (issue 9), so `test_arbiter.cc` asks it at READ COMMITTED
  alone.

## Step 306: ON CONFLICT above READ COMMITTED meets its own row

`INSERT ... ON CONFLICT` at REPEATABLE READ or SERIALIZABLE
failed with sm1's `sm1_read_latest requires READ COMMITTED`
when two of its rows had one key (`ISSUE.md` issue 9). Not the
message alone: DO NOTHING, which PostgreSQL runs, inserting
the first row and passing over the second, failed with it;
DO UPDATE, which PostgreSQL refuses, refused with sm1's words.
A conflict with a committed row, or with one an earlier
statement inserted, was right, the statement's view showing
it.

- **The cause**: sm3's arbiter looks with the statement's
  view, which does not show a key its own statement took; the
  insert of the entry then fails with `SM_ERR_DUPKEY`, and
  `find_now()` names the row by `sm1::read_latest`, which sm1
  allowed at READ COMMITTED alone. Above it a key taken since
  the snapshot by another fails the insert with
  `SM_ERR_CONFLICT` first, so the version found is the
  transaction's own.
- **The fix** (sm1): `read_latest` and `chain_read_latest`
  read, above READ COMMITTED, a newest version that is the
  transaction's own, which the snapshot does not hide and no
  other writer changes; any other is `SM_ERR_INVALID` as
  before (`SM1_DESIGN.md` §5.4). DO UPDATE then locks the row
  as read, which is refused (step 305), and dg says
  PostgreSQL's "cannot affect row a second time".
- **Tests**: scenario `a19` (`../sm/sm1/mvcc`), at each level,
  both layouts; `test_read_latest.cc`, an own version read
  above READ COMMITTED and another refused; `test_arbiter.cc`'s
  case of step 305 at every level now; `test/server.sh`'s
  ON CONFLICT step 9, DO NOTHING and DO UPDATE on two rows of
  one id at REPEATABLE READ and SERIALIZABLE. rel has no insert
  with arbiters. pgregress's `insert_conflict`, ported again,
  has its five `selfconflict` statements back, and an
  `excluded.ctid` statement whose marker dated from the first
  port.

## Step 307: a column's update default

A column may have an update default, `DEFAULT x ON UPDATE`: the
value an UPDATE writes in it when it does not assign it, beside
an optional plain `DEFAULT y` for INSERT (`ONUPDATE.md`). It
keeps a modification time, or a state an edit moves a row to,
which PostgreSQL keeps with a trigger, and digres has none. It
is digres's own: PostgreSQL refuses it (`UNSUPPORTED.md` §10).

- **sq**: one rule beyond PostgreSQL's grammar, `DEFAULT b_expr
  ON UPDATE`, making the DEFAULT's `Constraint` marked by
  `generated_when = 'u'` (`../sq/DESIGN.md` §2). It needs one
  token of lookahead; a clause of its own, `ON UPDATE x` or
  `ON ROW UPDATE x`, conflicted with a foreign key's `ON
  UPDATE` after `REFERENCES t`, and was dropped for it.
- **sm**: a column's record keeps the update default as the
  host's text, opaque, as it keeps the insert one
  (`on_update_expr`); an identity column may not have one.
- **dg**: parse analysis takes one plain and one update default
  on a column, the plain one the column's default, or the
  update one without it (`transformColumnDefinition()`).
  `smstore.c` checks both are constants, cast or not, or
  `CURRENT_TIMESTAMP`, `LOCALTIMESTAMP` or `CURRENT_DATE`, in
  CREATE TABLE and ADD COLUMN, and keeps the update default
  cooked to the column's type. The rewriter gives an UPDATE a
  target entry with it for each column the UPDATE leaves out
  (`rewriteTargetListIU()`, `smstore_update_defaults()`), as
  it gives `SET c = DEFAULT` the insert default, so every
  UPDATE path takes it: ON CONFLICT DO UPDATE, `WHERE CURRENT
  OF`, a view, a foreign key's cascade.
- **Left on purpose**: no ALTER COLUMN form sets or drops an
  update default; psql's `\d` and `pg_dump` show the insert
  default alone, PostgreSQL's catalog having a place for one.
  ADD COLUMN, which digres analyses only for a serial or an
  identity column, still takes the last of two plain defaults
  where PostgreSQL refuses them; two update defaults it
  refuses.
- **Tests**: `test/slt/on_update.test`, every case of
  `ONUPDATE.md` §8 and what it refuses, failing with the
  rewrite, the check, the update default's keeping or the
  analysis's choice of default each disabled;
  `../sm/rel/test/test_on_update.rel` and an sm3 test of
  `add_column` and of an identity column, failing with the
  field's write, read, check or display disabled;
  `../sq/test/on_update.sql` and two files of what it refuses.
  sq's corpus is unchanged against PostgreSQL 18.6's scripts.


## Step 307: nothing is made in pg_catalog

`CREATE TABLE pg_catalog.t ()` made a table (`ISSUE.md` issue
8), as did CREATE VIEW and CREATE SEQUENCE there. PostgreSQL
refuses one in `heap_create()`, which a relation made in sm
does not reach. In digres no catalog is in sm: the 28 in
pg_catalog are compiled in, and a database fresh from initdb
has no relation of sm's in any schema. But pg_catalog is
still the schema every search path looks in first, so a table
made there hid one of its name in any schema, in every session:
`public.hid`, read as `hid`, read the empty `pg_catalog.hid`.
And a catalog's own name was free to sm, whose check of a
taken name looks in sm alone: `CREATE TABLE pg_catalog.pg_class
(x int)` made a second `pg_class`, which no name reached.

- **The check**, `check_system_namespace()` in `smstore.c`,
  from `smstore_create_table()`, `_view()` and `_sequence()`:
  in pg_catalog or pg_toast, unless `allow_system_table_mods`,
  a name a relation has there is PostgreSQL's "already exists",
  as `heap_create_with_catalog()` finds it first, and any other
  is "permission denied to create", as `heap_create()` says.
  An index follows its table, as PostgreSQL lets it; ALTER
  TABLE ... SET SCHEMA, which could move one there, is not
  supported yet.
- **Tests**: `test/server.sh`: a table, a view and a sequence
  refused in pg_catalog, `pg_class` there already a relation,
  a public table of the refused name still read by it, and
  with `allow_system_table_mods` a table made and dropped
  there. sm has no system schema, so no test of sm's reaches
  it. pgregress's `alter_table`, ported again, has its
  `CREATE TABLE pg_catalog.new_system_table()` back; and, from
  work since it was last ported, a `select * from t1`, and two
  `vacuum analyze` markers with ANALYZE's reason now.

## Step 308: a SET DEFAULT column named twice is kept once

A foreign key's `ON DELETE SET DEFAULT (d, d)` or `SET NULL
(d, d, d)` kept every column named (`ISSUE.md` issue 11), where
PostgreSQL's `validateFkOnDeleteSetColumns()` drops a repeat.
Not the definition alone: the action's UPDATE set `d` twice,
`multiple assignments to same column "d"`, so the parent's row
could not be deleted at all.

- **The fix**: the loop that reads the column list, in
  `smstore.c`'s foreign key, drops a column already in it. The
  list is dg's alone, kept in the key's host bytes, which sm
  stores and never reads: no test of sm's reaches it.
- **Tests**: `test/fkey_delete_set.sql` (run.sh, 11 files
  now): SET DEFAULT (d, d) in CREATE TABLE and SET NULL (d, d,
  d) in ALTER TABLE ... ADD FOREIGN KEY, each defined with d
  once and acting on a parent's delete, as PostgreSQL's; each
  part fails without the fix. pgregress's `foreign_key`,
  ported again for the first time since its port, has 30
  statements back: the SET DEFAULT block's definition, DELETE
  and SELECT by this fix, the only ones a build without it
  fails; the rest by the foreign key work since.

## Step 309: a column named twice is PostgreSQL's error

`CREATE TABLE t (LIKE a, LIKE a)` failed with sm's `could not
create a table: create table: a column without a name, or two
with one: xx` (`ISSUE.md` issue 10). Not LIKE alone: any column
named twice, `(a int, a int)` or `(LIKE a, xx text)`, did; the
table was refused, as PostgreSQL refuses it, with sm's words.
PostgreSQL's `MergeAttributes()`, which says `column "xx"
specified more than once`, is not on a table in sm's path.

- **The check**: `smstore_create_table()`'s loop over the
  columns, LIKE's among them after parse analysis, refuses a
  name an earlier column has, with PostgreSQL's error and code
  (`ERRCODE_DUPLICATE_COLUMN`), before sm is asked. `"A"` and
  `a` are two names, as in PostgreSQL.
- **Tests**: `test/server.sh`, a name twice in the list, by
  two LIKEs and by a LIKE and the list, each PostgreSQL's
  error, and no table made; the step fails without the check.
  sm3's own refusal, which the check stands in front of, had
  no test: `test_ddl.cc` (two columns of one name, one of
  none, and `A` and `a` taken) and rel's `test_basic.rel`
  (`table-create` of two columns `a`). sm1 knows no columns,
  so no scenario reaches it. pgregress's `create_table_like`,
  ported again, has its `(LIKE inhx, LIKE inhx)` back.

## Step 310: the hash access method cannot exclude, rightly

`pg_indexam_has_property(hash, 'can_exclude')` is false in
digres and true in PostgreSQL (`ISSUE.md` issue 12). Closed as
digres's answer, not a bug. `amutils.c` computes the property
from the method's `amgettuple`, a scan an exclusion constraint
checks its rows by; digres's hash method is one in name only
(`hashfunc.c`), there for the planner and executor to find a
type's hash function and equality through its operator
classes, with no callback to build or scan an index. So CREATE
INDEX USING hash is refused, and true would promise a tool what
digres cannot do. Every other property of it is PostgreSQL's.

- **Tests**: `test/server.sh` asks `can_exclude` of hash and
  btree (false and true) and has CREATE INDEX USING hash
  refused. No source changed; sm has no access methods.
- **pgregress**: `amutils` keeps its `--dg-differs: the
  result`; the marker's words are pgregress's own, made again
  at each port, so `TRIAGE.md` says why it differs.
- **ISSUE.md**: issue 12 is closed in the preamble. Every issue
  the triage found (3 to 12) is closed, and its introduction
  goes with them.

## Step 311: partial and expression indexes

An index may have a predicate, `WHERE`, and key columns that
are expressions (`EXPRINDEX.md`). sm3 kept and maintained both
already, asking its host's evaluator whether a row belongs and
what its expression keys are; digres refused them in
`check_create_index()`, and registered no evaluator.

- **The evaluator** (`smstore_index_eval()`, `smstore.c`) runs
  PostgreSQL's executor on the expressions and the predicate,
  made ready once a thread and index, over sm's row as it is.
  An error is caught and kept, never thrown through sm, and
  `sm_error()` raises it again as PostgreSQL's when sm's call
  fails with it.
- **sm's builders**, threads digres did not start, run it too:
  each is adopted at its first call (`session_thread_adopt()`),
  set up as a session thread is, without a session, and freed
  as it ends. It reads the compiled-in catalogs in a
  transaction of its own, which touches nothing of sm's.
- **CREATE INDEX** passes sm the expressions, the predicate and
  the expression columns' names, `nodeToString()`'s, and the
  columns they read; it refuses what PostgreSQL refuses, and a
  numeric expression with no scale, which sm would round.
- **The catalog**: the relcache gives an index in sm its
  `rd_indexprs` and `rd_indpred`, so the planner uses it as
  PostgreSQL's; `pg_index`, `pg_attribute` and
  `pg_get_indexdef()` show them.
- **Scans**: a key on an expression is checked by the executor
  against the original quals, as a lossy index's, as digres's
  own check reads a row's column; an index-only scan's against
  the quals on the index's columns.
- **A duplicate** names the expression and its value as
  PostgreSQL does, in a write and in a build.
- **Left on purpose**: an error in a build on sm's builders
  says `could not create index "x":` before PostgreSQL's
  message; settings on a builder are their defaults
  (`EXPRINDEX.md` §8); what ANALYZE gives an expression's
  statistics, and the planner reads of them, was not looked at
  (§10 step 6).
- **Tests**: `test/slt/expr_index.test`, its results
  PostgreSQL 18's, and two `server.sh` checks, of PostgreSQL's
  DETAIL in each path and of a build a crash cut short,
  resumed on an adopted builder; each shown to fail with one
  part disabled (`EXPRINDEX.md` §10). A first run of those
  checks restored each file older than its object, so `make`
  kept the disabled part built in; they were run again, each
  file touched as it was restored.

## Step 312: the roles live in pg_global

The first step of roles (`../sm/ROLE.md` §9). digres had one
role, the superuser initdb named, whose name and SCRAM secret
were the data directory's `superuser` file. A role is the
whole server's, and no sm transaction spans two databases, so
the roles go to a database of their own, `pg_global`, which
holds all role data and nothing else holds any (ROLE.md §3,
the invariant).

- **`pg_global`** (`utils/init/pg_global.c`) is an sm
  database like any other, with a table of digres's,
  `pg_authid`: a role a row, its primary key the oid, a unique
  index on the name. sm learns nothing of roles. initdb makes
  it after `postgres`, which keeps the first database id, with
  the superuser in it, oid 10; the `superuser` file and
  `SUPERUSER_FILE` are gone. The superuser has PostgreSQL's
  attributes but REPLICATION, which digres has none of, as
  `pg_roles` said before.
- **The snapshot**: the postmaster reads every role at start
  into one `pg_global_roles_t`, in `shmem->pg_global`, which
  sessions copy a pointer to under its mutex. No session opens
  `pg_global`. Role DDL, step 2, replaces it whole after its
  commit.
- **Who reads it**: `InitializeSessionUserId()` (the role by
  name or oid, and its superuser flag), `get_role_password()`
  (the secret, and PostgreSQL's check of `rolvaliduntil`,
  which no role has yet), `GetUserNameFromId()`, and the
  catalog relations `pg_authid`, new, and `pg_roles`, which
  the syscache's `AUTHOID` and `AUTHNAME` scan.
- **Hidden**: `pg_global` is not in `pg_database`, cannot be
  connected to or dropped (`database does not exist`), and
  `CREATE DATABASE pg_global` finds it there.
- **A data directory made before** has no `pg_global`, and
  the server refuses it at start, `data directory has no
  database "pg_global"`, with a hint to run initdb (ROLE.md §8
  question 5). No upgrade in place.
- **Left on purpose**: single-user mode (`--single`) loads no
  data directory and finds no role but the bootstrap one,
  as before; ROLE.md keeps no single-user mode.
- **Tests**: `initdb.sh` makes no `superuser` file, reads
  `-U` and `--pwfile`'s role from `pg_authid` through a server
  started on it, and has a directory whose `pg_global` sm's
  `rel` dropped refused (so `make test` builds `../sm/rel`);
  `server.sh` reads `pg_authid` and `pg_roles`, finds a role by
  name and oid (`regrole`), and has `pg_global` hidden. Each
  was shown to fail with its part disabled. sm and its `rel`
  and mvcc tests have nothing to check: sm is unchanged.

## Step 313: role DDL writes pg_global

The second step of roles (`../sm/ROLE.md` §9): CREATE, ALTER
and DROP ROLE, GRANT and REVOKE of a membership, SET ROLE and
SET SESSION AUTHORIZATION. `user.c` is PostgreSQL's still, its
checks and messages too; what it wrote to `pg_authid` and
`pg_auth_members` goes to `pg_global` instead.

- **`pg_auth_members`** is a table of `pg_global`'s beside
  `pg_authid`, keyed by oid, with a unique index on (role,
  member, grantor), and a catalog relation; the oids of both
  come from `pg_global`'s sequence `role_oid`, from 16384.
  A data directory from step 312 has none, and is refused at
  start, to be made again.
- **A session's changes** (`pg_global_put_role()` and the
  rest, in place of `CatalogTupleInsert()`, `Update()`,
  `Delete()` and `GetNewOidWithIndex()`) go to a copy of the
  snapshot the session alone reads, and its caches of the two
  catalogs are invalidated at its next command, as a catalog
  tuple's write would.
- **At commit**, before the database's (`CommitTransaction()`),
  what differs from the snapshot the changes began from is
  written to `pg_global` in one transaction, each row checked
  first to be as the session read it: one another session
  changed meanwhile fails the commit, `could not serialize
  access due to concurrent update`, rather than lose that
  change. Then the snapshot is read again and replaced, under
  its mutex, and its epoch moves. An abort forgets the copy.
- **Other sessions** drop their caches of the roles at their
  next transaction, when the epoch has moved
  (`AcceptInvalidationMessages()`), as they do a schema's.
- **No pg_shdepend**: a membership's grantor was recorded
  there. DROP ROLE now reads it from `pg_global`
  (`pg_global_role_dependents()`), with PostgreSQL's DETAIL;
  the cleanup of a role's comments, security labels and
  settings is gone, as digres keeps none.
- **The session's role is set as PostgreSQL sets it**, through
  the `session_authorization` setting, now that a role can be
  looked up by name: digres set the user ids alone, and RESET
  SESSION AUTHORIZATION then left the session as the role SET
  had made it, superuser or not.
- **In a transaction block** CREATE, ALTER, DROP ROLE, a
  role's GRANT and REVOKE, and ALTER ROLE RENAME are refused
  (ROLE.md §8 question 1).
- **System-table scans** of the two catalogs read their rows,
  as of a catalog of compiled-in rows: they have no index
  (`catrel_is_global()`).
- **pgregress**: between files, the `regress%` roles are
  dropped in one DROP ROLE, which drops a grantor with what it
  granted whatever their order. The 55 files with statements
  taken out at `pg_authid` were ported again, and 1,115 of
  their statements are back: `password` keeps all 44 it took
  out, `roleattributes` 73 of 80, `privileges` 461 of 1,361,
  `rowsecurity` 53 of 907, `create_role` 53 of 139. Statements
  that ran as no role before now run as one that is not a
  superuser, whose privilege checks read `pg_namespace`'s ACL
  by the syscache, which opens an index a catalog relation
  does not have: 1,020 now stop at `relation pg_namespace
  cannot be opened`, until owners and GRANT (steps 4 and 5).
- **Left on purpose**: ALTER ROLE ... SET (`pg_db_role_setting`)
  still fails; REASSIGN OWNED and DROP OWNED are step 6. Two
  sessions changing one role at once is not tested: role DDL
  is one statement, with nothing to hold one between its read
  and its commit.
- **Tests**: `server.sh` makes, grants, alters and drops roles,
  and checks what a session already open sees, the refusal in
  a transaction block, the grantor's DETAIL, SET ROLE and what
  a role without CREATEROLE may not do, and the roles after a
  restart. sm is unchanged, so mvcc and `rel` have nothing to
  check.

## Step 314: how a client gets in

The third step of roles (`../sm/ROLE.md` §5, §9): the fixed
rule of step 92, peer on the socket and SCRAM over TCP, gives
way to ROLE.md's four checks, with no `pg_hba.conf`.

- **How it proves who it is** (`hba_getauthmethod()`): on the
  local socket, peer if the OS user at its other end is the
  role's name; else the role's password, by SCRAM-SHA-256, as
  over TCP. A role with no password has only peer.
- **Locked out**: on the local socket, the data directory's
  owner, the OS user the server runs as, asking for the
  bootstrap superuser by its name, is let in by peer, whatever
  that role's LOGIN, VALID UNTIL or CONNECTION LIMIT say
  (`Port.datadir_owner`, ROLE.md §8 question 3). Another role
  it asks for is any role's.
- **The role may log in** (`InitializeSessionUserId()`): LOGIN,
  not past VALID UNTIL, under its CONNECTION LIMIT, counted in
  the session registry (`sessions_of_role()`); a superuser has
  no limit, as in PostgreSQL. VALID UNTIL refuses peer too,
  where PostgreSQL's only refuses the password.
- **CONNECT on the database** (`CheckMyDatabase()`), which a
  superuser need not have. No database has an ACL yet (step
  5), so `PUBLIC` has it everywhere.
- **What the client is told**: `password authentication failed
  for user`, whatever failed, as PostgreSQL tells it of a
  wrong password; the log's DETAIL says which check, as
  PostgreSQL's does for its own.
- **Left on purpose**: a session of a role that is not a
  superuser gets in, but a statement that looks a name up
  fails, at `pg_namespace`'s ACL (step 313): step 4.
- **Tests**: `server.sh` has a role get in by password on the
  socket and over TCP, and refuses one with no password, one
  NOLOGIN, one past VALID UNTIL and one at its CONNECTION
  LIMIT, the log naming each; `initdb.sh` lets the owner in as
  a bootstrap superuser made NOLOGIN, CONNECTION LIMIT 0 and
  past VALID UNTIL, under another name, and not as another
  role. Each was shown to fail with its check disabled. sm is
  unchanged.

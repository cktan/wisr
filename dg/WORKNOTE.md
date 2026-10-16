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
  `pg_subscription_rel` and `pg_publication*`.

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
  data directory is not left holding them.
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
sys (`../sys/INTERRUPT.md`) has one already: each thread's wake
fd, an eventfd that `sys_interrupt()` writes.

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
thread's wait mask (`../sys/INTERRUPT.md` §8), and fail with
`SM_ERR_INTERRUPTED` when a bit in it is raised.

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

- **A sequence is an object in sm's catalog**, in a schema,
  with an sm counter behind it (`sm3/CATALOG.md` kind 11;
  `REVIEW.md` §5 has why). CREATE SEQUENCE is
  `smstore_create_sequence()`, in the transaction: START and
  INCREMENT, each at least 1, and AS bigint. Every other
  option, TEMPORARY, an owner and an identity column are
  refused, as are ALTER SEQUENCE and `setval()`: sm's counters
  only go forward.
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
  never runs (`../sm/sm3/CATALOG.md`). A part written before it
  reads as null, so the catalog's format is unchanged.
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
  fields of the sequence part (`../sm/sm3/CATALOG.md`).
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
  nullable fields of the column part (`../sm/sm3/CATALOG.md`).
  `create_table` makes the column own the sequence; it must be
  a NOT NULL smallint, integer or bigint with no default, and
  not in a temporary table. `own_sequence` on that sequence is
  `SM_ERR_INUSE`, and `add_column` refuses an identity column.
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
  after more. `sm1/API.md`, `sm1.h`, `STATISTICS.md` §3.1.

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
  `SM1_DESIGN.md` §5.1): `lock_wait` and `lock_wait_xact` call
  `check_deadlock()` once their edges are set, before they
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

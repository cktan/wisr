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

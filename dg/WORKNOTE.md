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

# digres: the design of record

What digres is built as, and the decisions that shape it. It
holds what stands, not the steps that got there: `WORKNOTE.md`
has each step and why, `CODE.md` the rules for changing the
code, and `README.md` what runs. `../sm/PART4.md` §10 is how
digres was to be made, and §2 why the server must be threaded.
The overriding objective, a database is its directory, is in
`CODE.md`.

---

## 1. One process, a thread a session

digres is one process, with a thread for each connection, and
sm attached once by that process. sm keeps its transaction
table, lock manager and SSI graph in process memory, so forked
backends could not share a database: threads are what lets
more than one connection use a table.

As built (`CODE.md` §3 to §5 have the rules):

- **A connection is a thread** of the postmaster's process,
  started through `session_thread_start()`
  (`utils/init/session_thread.c`). Nothing forks but
  `popen()`, which execs at once.
- **A FATAL ends the thread, not the process**: `proc_exit()`
  throws to the thread's top, which queues the exit code for
  the postmaster and sets its latch; `process_pm_child_exit()`
  takes it as it took a child from `waitpid()`.
- **Signals between threads are interrupt bits**: a session
  is interrupted by `session_interrupt()`, and runs the
  handler at its next `CHECK_FOR_INTERRUPTS()` or wait. No
  thread takes a signal; the postmaster reads them from a
  signalfd.
- **A latch wakes its owning thread**, and a thread's timeouts
  are one timer of its own (`sm_sys.h`).
- **A thread starts with its parent's settings**: the ones not
  at their defaults, written to it as PostgreSQL wrote them
  for an exec'd child (`write_nondefault_variables_to()`). It
  makes its own memory contexts and copies none of the
  parent's memory.
- **What a process's exit freed, a thread's end frees**: the
  client socket, the wait sets, open files, the static
  snapshots' arrays, the memory contexts and the allocator's
  freelists. 300 connections leave the server's descriptors
  and heap where they were.
- **sm is attached once**, by the postmaster at startup
  (`utils/init/datadir.c`). A session opens its database
  (`sm_opendb()`) and closes it as it ends. A database's oid
  is `FirstNormalObjectId` plus the `dbid` sm gave it
  (`sm_dbid()`, never reused).
- **TSan runs the server** (`make tsan`). Its reports were all
  of PostgreSQL's lock-free handoffs done with plain loads and
  barriers, which are relaxed atomics now, release and acquire
  for `lwWaiting`.
- **`test/run.sh` runs each file alone and in 8 sessions at
  once**, each a thread of one server, and each must print the
  same.

## 2. How it got there

The order was agreed with the owner, each step on its own, and
all of it is done:

1. **The `thread_local` sweep** (§3).
2. **N sessions in one process**, under TSan, with no server,
   by an option of `digres --parse` since gone. TSan found one
   race, the `lc_messages` hook calling `setlocale()`, which is
   the process's.
3. **Connections as threads** (§1; `WIP_THREADS.md` has the
   detail and what is left).
4. **Signals became flags, and the process-only subsystems
   went**: crash restart and `EXEC_BACKEND` (`WORKNOTE.md`
   steps 23 and 24); shared memory, each structure the threads
   share now an ordinary allocation (steps 25 and 26); dynamic
   shared memory (step 29); the auxiliary processes, from
   autovacuum to the startup process (steps 11 to 17);
   `xlog.c` (step 18).

Before the threads, the first recommendation, written when
every file first compiled as strict C++ and nothing linked:

- **Drop the client and use stock `psql`.** `libpq`, `psql`
  and `fe_utils`, 69k lines, are not in the tree: the fork
  changes storage and the process model, not the wire
  protocol, so PostgreSQL 18's own client works unchanged.
- **Link by deleting, not stubbing.** The callers of
  replication, the JIT and the WAL summarizer were removed,
  and the built-ins `pg_proc.dat` listed with no source, with
  their operator classes: stubs would have kept code that was
  going away.
- **Carve out the parser.** It is `../sq`, the parser alone,
  with its own nodes and no base under it, which digres links
  (`WORKNOTE.md` step 5, `../sq/PLAN.md`). The base stayed in
  digres.
- **Defer the rest.** `src/timezone` and `regex` stay in
  digres: carving them out is tidy and unblocks nothing. The
  planner is not separated: it would need a catalog-provider
  interface first, 357 symbols from 29 components, led by the
  catalog cache. TLS stays compiled out until something needs
  an encrypted connection.

The replacement of GUC was to follow the threads
(`../sm/NEXT.md`, low priority); it is not designed yet.

## 3. The globals, and how the build keeps them

Every writable global and file-scope static is `thread_local`,
marked `/*LOCAL*/`; a thread shares only through `pcb` and
`shmem`, the control blocks (`CODE.md` §3).

- **The sweep was a script**, `src/tools/cxx_thread_local.py`.
  It takes the variables from the objects, not from a search
  of the source: a writable one lies in `.bss`, `.data`,
  `.data.rel` or `.data.rel.local` (1,480 of the 1,712
  writable symbols then; the other 232 were `const` tables in
  `.data.rel.ro`). `--apply` wrote the mark at each
  definition and every `extern` of it.
- **`--check` runs before `build/digres` is linked.** It fails
  on a writable variable not `thread_local`, so a global added
  later is marked or the build fails, and on any static
  initialiser (`.init_array`): in C++, a shared table whose
  initialiser holds the address of a `thread_local` compiles,
  and is filled once, at startup, with the first thread's
  address. `src/tools/process_wide.txt` lists the exceptions,
  `pcb` and `shmem` alone.
- **What could not be `thread_local` is `const`**: the 13
  `JSON_PROD_*` tables in `common/jsonapi.c`, whose addresses
  a `constexpr` table takes; flex's `yy_start_state_list` in
  `jsonpath_scan.c`, which the Makefile makes `const`.
- **The GUC variables and tables are per thread**, by the
  owner's choice, rather than shared until GUC is replaced.
  The 340 variables and the five tables (`ConfigureNamesBool`
  and the rest) are `thread_local`; a table's initialiser runs
  in each thread on first use and takes that thread's own
  variables' addresses. `guc.c`'s own state is per thread too,
  so `SET` stays per session.
- **No C++ object with a destructor lives in a global**:
  `thread_local` objects run their destructors at thread exit,
  in an order hard to reason about. What a block owns is freed
  by `session_thread_end()` and the tcb's destructors.

## 4. Decisions of record

Do not reopen these without the owner.

- **A database is its directory, and this overrides the
  rest.** `base/X` can be copied to another machine and
  imported, whole, into a new digres instance (`CODE.md`, the
  overriding objective). So `global/` holds only the list of
  databases, a database uses nothing outside its directory and
  nothing of another database, and nothing inside it names
  where it is. Decided by the owner on 2026-10-11. The import
  is not built, and is low priority: sm has no call that takes
  a directory in under a name and gives it a `dbid`. What
  matters now is that no change makes it impossible later.
- **Memory contexts and resource owners stay.** Contexts are
  arenas and are what makes an exception cheap to recover
  from. RAII is for new code and for guards around them.
- **No object with a destructor in `palloc`'d memory.**
- **`ereport(ERROR)` throws `pg_error_t`**; `PG_TRY` is a `try`.
- **sm's callbacks are `noexcept`**, and four C++ types cross
  `sm.h`: `std::string`, `std::vector`, `std::string_view` and
  `std::shared_ptr`; `sm_sys.h` adds what a wait is made with
  (`../sm/CODE.md` §2).
- **SPI stays.** SQL-language functions are cut.
- **One encoding, UTF8; default collation C;** ICU for a column
  or a query that asks.
- **Authentication is not configured**: no `pg_hba.conf`, and
  its parser is gone (`WORKNOTE.md` step 92). The rule is
  `../aa/ROLE.md` §5's, built in `WORKNOTE.md` steps 320 and
  327.
- **Every writable global is `thread_local`**, marked
  `/*LOCAL*/`, and the build fails otherwise (§3). A thread
  shares only through `pcb` and `shmem` (`src/include/c.h`).
- **The globals move into four control blocks**: the SHARED ones
  did, as work of their own, and a LOCAL one does as a
  change touches it (`utils/control_block.h`, `WORKNOTE.md` steps
  22 and 211): `pcb_t`, what the main thread owns and the others
  read; `shmem_t`, what the threads share and write, each field
  guarding itself; one `tcb_t` for each thread; and
  `postmaster_tcb_t`, what the main thread owns and no other
  reads. A field is a value, or a `std::unique_ptr` to what the
  block owns; a module's state is a struct only it defines. New
  state goes into a block at once. The last SHARED variable
  moved in `WORKNOTE.md` step 229, with the copy table and the
  two-pass link: the only writable globals outside
  `thread_local` are `pcb` and `shmem`.
- **The GUC variables and tables are per thread** (§3).
- **No client in the tree**: stock `psql` (§2).
- **No parallel workers.** Parallel query, index build and
  vacuum are removed, planner included; sorting and temporary
  files stay serial.
- **No VACUUM.** sm collects its own garbage. `VACUUM`
  compacts each table's store and no more (`WORKNOTE.md` step
  284); `ANALYZE` stays. `CLUSTER` and
  `REINDEX` succeed too, doing nothing, with a warning: a table
  in sm is kept in its primary key's order, or by its `_id`
  without one (rowno once; `../sm/ROWKEY.md`), and its indexes
  with its rows.
- **No WAL.** sm keeps what is durable. There is no recovery,
  archiving or hot standby, and `PREPARE TRANSACTION` is
  refused.
- **No buffer manager, smgr or free-space map.** Every
  relation with storage is sm's, and sm reads and writes its
  own; there are no pages in shared buffers, no relation files,
  and no PostgreSQL sequences: a sequence is sm's, a counter
  in its catalog (`commands/sequence.c`).
- **No multixacts.** A heap row named its lockers in its
  header, and several needed a MultiXactId. A row in sm names
  none: sm's lock manager holds row locks, shared by as many
  as take them, and they end with their transactions. What
  sm lacks is PostgreSQL's two weaker row lock modes, a
  concurrency gap (PART4.md gap 3), not a reason for
  multixacts.
- **SERIALIZABLE is sm's.** sm runs serializable
  transactions itself, and fails one of a write skew with a
  retryable error, SQLSTATE 40001. PostgreSQL's predicate
  locks, their SLRU and its `shared_buffers` are gone.
- **No asynchronous I/O.** Disk I/O is to go through sm; the
  rest is the client socket, which waits on the latch. A buffer
  read is synchronous.
- **No checkpoints.** sm keeps what is durable, and RocksDB
  bounds its own recovery. `CHECKPOINT` succeeds and does
  nothing, as `VACUUM` does.
- **sm's lock manager is the only one, in the end.** sm locks
  a table as it is opened and a record as it is read or
  written; the parser and executor lock nothing of their own,
  and only a special operation (a DROP, a `LOCK TABLE`, a
  `SELECT ... FOR UPDATE`) asks sm for a lock explicitly. A
  table is opened where PostgreSQL locks it, before its
  definition is read, so a statement waits out DDL before it
  plans, not after (`WORKNOTE.md` step 111). One lock manager
  means one deadlock detector. Tables in sm moved
  first; PostgreSQL's lock manager went when nothing else used
  it (`WORKNOTE.md` step 156).
- **Advisory locks are not sm's.** They name a number the
  application picks and guard nothing stored, and their
  session scope, explicit unlock and counted holds are
  PostgreSQL's, which sm1's locks need not learn. They are in
  a table of digres's own (`storage/advisory_lock.h`,
  `WORKNOTE.md` step 153), with a deadlock detector of its own
  for cycles among advisory waits. A cycle through an advisory
  lock and an sm lock is not detected, and `lock_timeout` or a
  cancel ends it; sm1's wait-for graph could take edges from
  digres to find it (`../sm/NEXT.md` §3).
- **sm's transactions are the only ones.** sm keeps the
  transaction, its statements, savepoints, isolation,
  visibility and durability; `xact.c` keeps only what a
  session needs around it -- its block state, memory and
  resource cleanup, and the hooks that end a statement. It
  stays until it is empty: what it no longer needs is deleted,
  and nothing is moved out just so the file goes
  (`WORKNOTE.md` steps 232 to 236).
  PostgreSQL's transaction ids, command ids, combo cids and
  two-phase commit are gone, and every snapshot is the same
  one (`utils/time/snapmgr.c`).
- **A table in sm has no bitmap scans.** A row in sm has no
  page or block, and a table with a primary key is read by its
  key: a bitmap of row keys would read a row a seek, in no order
  the store keeps (`../sm/CLUSTER.md` §6). An index in sm has
  no `amgetbitmap`, so the planner plans none.
- **Storage and replication features go.** Tablespaces,
  publications, subscriptions and large objects are tied to
  PostgreSQL's files, WAL and catalog tables, and are deleted,
  not ported: their statements are refused. Databases are not
  among them: CREATE and DROP DATABASE are sm's (`sm_mkdb`,
  `sm_rmdb`); ALTER DATABASE is refused until sm keeps a
  database's name changes and attributes. The rest of the DDL
  that writes catalog tables is kept, refusing, to be ported onto
  sm's catalog.
- **The temporary namespace is compiled in.** `pg_temp` has
  one oid in every session and database
  (`PG_TEMP_NAMESPACE`), and no row anywhere: sm keeps each
  session's temporary tables apart, in no namespace
  (`../sm/TEMPTABLE.md`). PostgreSQL's `pg_temp_N` per backend
  would be a row in the database naming a slot of this
  instance, against "a database is its directory"
  (`CODE.md`).
- **No geometric types** (decided 2026-10-17). PostgreSQL's
  built-in `point`, `line`, `lseg`, `box`, `path`, `polygon`
  and `circle` are not supported: sm has no type for them, and
  a column of one is refused. They are planar, with no
  coordinate system, and few applications use them; spatial
  work in PostgreSQL goes through PostGIS, whose model, the
  OpenGIS one MySQL has too (`geometry`, well-known binary, a
  coordinate system, an R-tree or GiST index), is a different
  family. Supporting that would be a decision of its own. The
  regression files that test the built-in types, and those
  that read `test_setup`'s `POINT_TBL`, `person` and `road`,
  fail by design.
- **No table inheritance** (decided 2026-10-23). `CREATE TABLE
  ... INHERITS` and `ALTER TABLE ... INHERIT` are not
  supported, as partitioning is not. A row is named by its key,
  the values of its table's primary key's columns
  (`../sm/ROWKEY.md`), and PostgreSQL gives a child no primary
  key of its parent's, so the tables of a tree name their rows
  by different columns: a row mark, an UPDATE or a DELETE
  through the parent would need a key for each child.
  `CREATE TABLE` refuses `INHERITS` and `ALTER TABLE` refuses
  `INHERIT` already, so no table in sm has a parent; nothing is
  to be built for inheritance trees, and the planner's and
  executor's code for them is dead for tables in sm.
- **`CREATE INDEX` on a table with rows waits for its build,
  after the commit** (decided on review, `WORKNOTE.md` step
  162). sm3 builds such an index in the background once the
  transaction that made it commits (`../sm/sm3/SM3_DESIGN.md`
  §3). digres waits for each build before it reports the
  command complete, outside any transaction, as PostgreSQL's
  `CREATE INDEX CONCURRENTLY` waits between its transactions;
  a build that failed, a unique index over duplicates, is
  dropped in a transaction of its own and its error reported.
  In autocommit this is PostgreSQL's behaviour: an error and
  no index, or an index usable at once. Inside a transaction
  block the error comes at `COMMIT`, after the block's other
  work has committed, and the index is not used before the
  commit. Refusing such a `CREATE INDEX` in a block, as
  PostgreSQL refuses `CONCURRENTLY` there, was the
  alternative: it would fail scripts that wrap DDL in `BEGIN
  ... COMMIT`.

## 5. Known gaps

What the threads left is in `WIP_THREADS.md`, "Left". Beside
it:

- **`initdb` cleans up on an error, not on a signal**: an
  interrupted `initdb` leaves its data directory behind
  (`main/initdb.c`, `cleanup()`).
- **A reload with no configuration file logs a complaint**, as
  `initdb` leaves a data directory with none (not re-checked
  since the threads).

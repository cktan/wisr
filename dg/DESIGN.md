# digres: the design of record

This document says what digres is built as, and the decisions
that shape it. It holds what stands now, not the steps that got
there. Other documents hold the rest:

- [`WORKNOTE.md`](WORKNOTE.md) has each step, and why it was
  taken.
- [`CODE.md`](CODE.md) has the rules for changing the code, and
  the overriding objective: a database is its directory.
- [`README.md`](README.md) says what runs.
- [`../sm/PGFORK.md`](../sm/PGFORK.md#10-digres-how-the-fork-is-to-be-made)
  §10 says how digres was to be made, and §2 says why the server
  must be threaded.

---

## 1. One process, a thread a session

digres is one process, with a thread for each connection. The
process attaches sm once.

Threads are needed because sm keeps its transaction table, lock
manager and SSI graph in process memory. Forked backends could
not share them, so they could not share a database. Threads let
more than one connection use a table.

As built ([`CODE.md`](CODE.md#3-globals) §3 to §5 have the
rules):

- **A connection is a thread** of the postmaster's process. It
  starts through `session_thread_start()`
  (`utils/init/session_thread.c`). Nothing forks except
  `popen()`, which execs at once.
- **A FATAL error ends the thread, not the process.**
  1. `proc_exit()` throws to the top of the thread.
  2. The top of the thread queues the exit code for the
     postmaster, and sets the postmaster's latch.
  3. `process_pm_child_exit()` takes the exit code, as it took a
     child's from `waitpid()`.
- **Signals between threads are interrupt bits.**
  `session_interrupt()` interrupts a session. The session runs
  the handler at its next `CHECK_FOR_INTERRUPTS()` or wait. No
  thread takes a signal: the postmaster reads signals from a
  signalfd.
- **A latch wakes the thread that owns it.** Each thread has one
  timer of its own for all its timeouts (`sm_sys.h`).
- **A thread starts with its parent's settings.** The parent
  writes the settings that are not at their defaults, as
  PostgreSQL wrote them for an exec'd child
  (`write_nondefault_variables_to()`). The thread makes its own
  memory contexts, and copies none of the parent's memory.
- **A thread's end frees what a process's exit freed**: the
  client socket, the wait sets, open files, the arrays of the
  static snapshots, the memory contexts and the allocator's
  free lists. After 300 connections, the server's descriptors
  and heap are where they started.
- **sm is attached once**, by the postmaster at startup
  (`utils/init/datadir.c`). A session opens its database with
  `sm_opendb()`, and closes it as the session ends. A database's
  oid is `FirstNormalObjectId` plus the `dbid` that sm gave it
  (`sm_dbid()`). sm never reuses a `dbid`.
- **TSan runs the server** (`make tsan`). All its reports were
  PostgreSQL's lock-free handoffs, done with plain loads and
  barriers. They are relaxed atomics now, and release and
  acquire for `lwWaiting`.
- **`test/run.sh` runs each file alone, and in 8 sessions at
  once.** Each session is a thread of one server, and each must
  print the same output.

## 2. How it got there

The order was agreed with the owner, one step at a time. All of
it is done:

1. **The `thread_local` sweep** (§3).
2. **N sessions in one process**, under TSan, with no server.
   This used an option of `digres --parse` that is now gone. TSan
   found one race: the `lc_messages` hook called `setlocale()`,
   which is process-wide.
3. **Connections as threads** (§1; §5 has what is left).
4. **Signals became flags, and the process-only subsystems were
   removed** ([`WORKNOTE.md`](WORKNOTE.md) has each step):
   - crash restart and `EXEC_BACKEND` (steps 23 and 24);
   - shared memory: each structure the threads share is now an
     ordinary allocation (steps 25 and 26);
   - dynamic shared memory (step 29);
   - the auxiliary processes, from autovacuum to the startup
     process (steps 11 to 17);
   - `xlog.c` (step 18).

Before the threads came the first recommendation. It was written
when every file first compiled as strict C++, and nothing linked
yet:

- **Drop the client, and use stock `psql`.** `libpq`, `psql` and
  `fe_utils`, 69k lines, are not in the tree. The fork changes
  storage and the process model, not the wire protocol, so
  PostgreSQL 18's own client works unchanged.
- **Link by deleting, not by stubbing.** The callers of
  replication, the JIT and the WAL summarizer were removed. So
  were the built-ins that `pg_proc.dat` listed with no source,
  with their operator classes. Stubs would have kept code that
  was going away.
- **Carve out the parser.** It is now `../sq`: the parser alone,
  with its own nodes and no base library under it. digres links
  it ([`WORKNOTE.md`](WORKNOTE.md) step 5,
  [`../sq/DESIGN.md`](../sq/DESIGN.md)). The base library stayed
  in digres.
- **Defer the rest.**
  - `src/timezone` and `regex` stay in digres. Carving them out
    would be tidy, but unblocks nothing.
  - The planner is not separated. It would first need an
    interface to a catalog provider: 357 symbols from 29
    components, led by the catalog cache.
  - TLS was to stay compiled out until something needed an
    encrypted connection. It is built now, by one setting
    ([`../aa/ROLE.md`](../aa/ROLE.md#8-decisions) §8).

The replacement of GUC was to follow the threads
([`../sm/NEXT.md`](../sm/NEXT.md), low priority). It is not
designed yet.

## 3. The globals, and how the build keeps them

Every writable global and file-scope static is `thread_local`,
marked `/*LOCAL*/`. Threads share only through `pcb` and
`shmem`, the control blocks ([`CODE.md`](CODE.md#3-globals) §3).

- **The sweep was a script**, `src/tools/cxx_thread_local.py`.
  It finds the variables in the object files, not by a search of
  the source. A writable variable lies in `.bss`, `.data`,
  `.data.rel` or `.data.rel.local`. At the time, that was 1,480
  of the 1,712 writable symbols; the other 232 were `const`
  tables in `.data.rel.ro`. `--apply` wrote the mark at each
  definition and at every `extern` of it.
- **`--check` runs before `build/digres` is linked.** It fails
  the build on two things:
  - a writable variable that is not `thread_local`. So a global
    added later must be marked.
  - any static initializer (`.init_array`). In C++, a shared
    table whose initializer holds the address of a
    `thread_local` compiles. But it is filled once, at startup,
    with the first thread's address.

  `src/tools/process_wide.txt` lists the exceptions: only `pcb`
  and `shmem`.
- **What could not be `thread_local` is `const`**:
  - the 13 `JSON_PROD_*` tables in `common/jsonapi.c`, whose
    addresses a `constexpr` table takes;
  - flex's `yy_start_state_list` in `jsonpath_scan.c`, which the
    Makefile makes `const`.
- **The GUC variables and tables are per thread.** The owner
  chose this over sharing them until GUC is replaced. The 340
  variables and the five tables (`ConfigureNamesBool` and the
  rest) are `thread_local`. A table's initializer runs in each
  thread on first use, and takes the addresses of that thread's
  own variables. `guc.c`'s own state is per thread too, so `SET`
  stays per session.
- **No C++ object with a destructor lives in a global.** A
  `thread_local` object runs its destructor at thread exit, in
  an order that is hard to reason about. Instead,
  `session_thread_end()` and the tcb's destructors free what a
  block owns.

## 4. Decisions of record

Do not reopen these without the owner.

- **A database is its directory, and this overrides everything
  else.** `base/X` can be copied to another machine and
  imported, whole, into a new digres instance
  ([`CODE.md`](CODE.md), the overriding objective). So:
  - `global/` holds only the list of databases;
  - a database uses nothing outside its directory, and nothing
    of another database;
  - nothing inside the directory names where it is.

  The owner decided this on 2026-10-11. The import is not built,
  and is low priority: sm has no call that takes in a directory
  under a name and gives it a `dbid`. What matters now is that no
  change makes the import impossible later.
- **Memory contexts and resource owners stay.** Contexts are
  arenas, and they make an exception cheap to recover from. RAII
  is for new code, and for guards around contexts.
- **No object with a destructor in `palloc`'d memory.**
- **`ereport(ERROR)` throws `pg_error_t`**, and `PG_TRY` is a
  `try`.
- **sm's callbacks are `noexcept`.** Four C++ types cross
  `sm.h`: `std::string`, `std::vector`, `std::string_view` and
  `std::shared_ptr`. `sm_sys.h` adds the types that a wait is
  made with
  ([`../sm/CODE.md`](../sm/CODE.md#2-the-interface-to-sms-users)
  §2).
- **SPI stays.** SQL-language functions are cut.
- **One encoding, UTF8, and the default collation C.** ICU is
  used for a column or a query that asks for it.
- **Authentication is not configured.** There is no
  `pg_hba.conf`, and its parser is gone
  ([`WORKNOTE.md`](WORKNOTE.md) step 92). The rule is
  [`../aa/ROLE.md`](../aa/ROLE.md#5-how-a-client-gets-in) §5's,
  built in [`WORKNOTE.md`](WORKNOTE.md) steps 320 and 327.
- **Every writable global is `thread_local`**, marked
  `/*LOCAL*/`, or the build fails (§3). A thread shares only
  through `pcb` and `shmem` (`src/include/c.h`).
- **The globals move into four control blocks**
  (`utils/control_block.h`):

  | Block | What it holds |
  | --- | --- |
  | `pcb_t` | what the main thread owns and the other threads read |
  | `shmem_t` | what the threads share and write; each field guards itself |
  | `tcb_t` | what one thread owns; one for each thread |
  | `postmaster_tcb_t` | what the main thread owns and no other thread reads |

  A field is a value, or a `std::unique_ptr` to what the block
  owns. A module's state is a struct that only the module
  defines. New state goes into a block at once.

  The SHARED globals moved as work of their own
  ([`WORKNOTE.md`](WORKNOTE.md) steps 22 and 211). The last one
  moved in step 229, with the copy table and the two-pass link.
  Now the only writable globals outside `thread_local` are `pcb`
  and `shmem`. A LOCAL global moves when a change touches it.
- **The GUC variables and tables are per thread** (§3).
- **No client in the tree**: use stock `psql` (§2).
- **No parallel workers.** Parallel query, parallel index build
  and parallel vacuum are removed, from the planner too. Sorting
  and temporary files stay serial.
- **No VACUUM.** sm collects its own garbage.
  - `VACUUM` only compacts each table's store
    ([`WORKNOTE.md`](WORKNOTE.md) step 284).
  - `ANALYZE` stays.
  - `CLUSTER` and `REINDEX` succeed, do nothing, and give a
    warning. A table in sm is always kept in its primary key's
    order, or by its `_id` without one
    ([`../sm/ROWSTORE.md`](../sm/ROWSTORE.md); this was rowno
    once). Its indexes are kept with its rows.
- **No WAL.** sm keeps what is durable. There is no recovery,
  archiving or hot standby, and `PREPARE TRANSACTION` is
  refused.
- **No buffer manager, smgr or free-space map.** Every relation
  with storage is sm's, and sm reads and writes it. There are no
  pages in shared buffers and no relation files. There are no
  PostgreSQL sequences either: a sequence is a counter in sm's
  catalog (`commands/sequence.c`).
- **No multixacts.** A heap row named its lockers in its header,
  and needed a MultiXactId for several lockers. A row in sm names
  none. sm's lock manager holds row locks, which any number of
  transactions can share, and which end with their transactions.
  sm lacks PostgreSQL's two weaker row lock modes. That is a
  concurrency gap (PGFORK.md gap 3), not a reason for multixacts.
- **SERIALIZABLE is sm's.** sm runs serializable transactions
  itself. In a write skew, it fails one transaction with a
  retryable error, SQLSTATE 40001. PostgreSQL's predicate locks,
  their SLRU and its `shared_buffers` are gone.
- **No asynchronous I/O.** Disk I/O is to go through sm. The
  other I/O is the client socket, which waits on the latch. A
  buffer read is synchronous.
- **No checkpoints.** sm keeps what is durable, and RocksDB
  limits its own recovery time. `CHECKPOINT` succeeds and does
  nothing, as `CLUSTER` and `REINDEX` do.
- **sm's lock manager is the only one.**
  - sm locks a table as it is opened, and a record as it is read
    or written.
  - The parser and executor take no locks of their own. Only a
    special operation asks sm for a lock explicitly: a DROP, a
    `LOCK TABLE`, or a `SELECT ... FOR UPDATE`.
  - A table is opened where PostgreSQL locks it, before its
    definition is read. So a statement waits for DDL before it
    plans, not after ([`WORKNOTE.md`](WORKNOTE.md) step 111).
  - One lock manager means one deadlock detector.

  Tables in sm moved first. PostgreSQL's lock manager was removed
  when nothing else used it ([`WORKNOTE.md`](WORKNOTE.md) step
  156).
- **Advisory locks are not sm's.** An advisory lock names a
  number that the application picks, and guards nothing stored.
  Its session scope, explicit unlock and counted holds are
  PostgreSQL's ideas, which sm1's locks need not learn. Advisory
  locks are in a table of digres's own
  (`storage/advisory_lock.h`, [`WORKNOTE.md`](WORKNOTE.md) step
  153). It has its own deadlock detector, for cycles among
  advisory waits.

  A cycle through both an advisory lock and an sm lock is not
  detected. `lock_timeout` or a cancel ends it. sm1's wait-for
  graph could take edges from digres to find such a cycle
  ([`../sm/NEXT.md`](../sm/NEXT.md#3-not-blocking) §3).
- **sm's transactions are the only ones.** sm keeps the
  transaction: its statements, savepoints, isolation, visibility
  and durability. `xact.c` keeps only what a session needs
  around the transaction: its block state, memory and resource
  cleanup, and the hooks that end a statement.

  `xact.c` stays until it is empty. What it no longer needs is
  deleted, but nothing is moved out only to remove the file
  ([`WORKNOTE.md`](WORKNOTE.md) steps 232 to 236). PostgreSQL's
  transaction ids, command ids, combo cids and two-phase commit
  are gone. Every snapshot is the same one
  (`utils/time/snapmgr.c`).
- **A table in sm has no bitmap scans.** A row in sm has no page
  or block, and a table with a primary key is read by its key. A
  bitmap of row keys would read one row for each seek, in an
  order the store does not keep
  ([`../sm/CLUSTER.md`](../sm/CLUSTER.md#6-digres) §6). An index
  in sm has no `amgetbitmap`, so the planner plans no bitmap
  scan.
- **Storage and replication features are removed.**
  Tablespaces, publications, subscriptions and large objects are
  tied to PostgreSQL's files, WAL and catalog tables. They are
  deleted, not ported, and their statements are refused.

  Databases are not among them. CREATE and DROP DATABASE are
  sm's (`sm_mkdb`, `sm_rmdb`). ALTER DATABASE is refused until sm
  keeps a database's name changes and attributes. The rest of the
  DDL that writes catalog tables is kept, and refuses, until it
  is ported onto sm's catalog.
- **The temporary namespace is compiled in.** `pg_temp` has one
  oid in every session and database (`PG_TEMP_NAMESPACE`), and
  no row anywhere. sm keeps each session's temporary tables
  apart, in no namespace
  ([`../sm/TEMPTABLE.md`](../sm/TEMPTABLE.md)). PostgreSQL's
  `pg_temp_N` for each backend would be a row in the database
  that names a slot of this instance. That would break "a
  database is its directory" ([`CODE.md`](CODE.md)).
- **No geometric types** (decided 2026-10-17). digres does not
  support PostgreSQL's built-in `point`, `line`, `lseg`, `box`,
  `path`, `polygon` and `circle`. sm has no type for them, and a
  column of one is refused. The reasons:
  - They are planar, with no coordinate system, and few
    applications use them.
  - Spatial work in PostgreSQL goes through PostGIS. Its model is
    the OpenGIS one, which MySQL has too: `geometry`, well-known
    binary, a coordinate system, and an R-tree or GiST index.
    That is a different family of types, and supporting it would
    be a decision of its own.

  Some regression files fail by design: those that test the
  built-in types, and those that read `test_setup`'s
  `POINT_TBL`, `person` and `road`.
- **No table inheritance** (decided 2026-10-23). digres does not
  support `CREATE TABLE ... INHERITS` or `ALTER TABLE ...
  INHERIT`, as it does not support partitioning. The reason:
  - A row is named by its key, the values of its table's primary
    key columns ([`../sm/ROWSTORE.md`](../sm/ROWSTORE.md)).
  - PostgreSQL does not give a child its parent's primary key. So
    the tables of a tree name their rows by different columns.
  - A row mark, an UPDATE or a DELETE through the parent would
    then need a key for each child.

  `CREATE TABLE` already refuses `INHERITS`, and `ALTER TABLE`
  refuses `INHERIT`. So no table in sm has a parent, and nothing
  is to be built for inheritance trees. The planner's and
  executor's code for them is dead for tables in sm.
- **`CREATE INDEX` on a table with rows waits for its build,
  after the commit** (decided on review,
  [`WORKNOTE.md`](WORKNOTE.md) step 162).
  1. sm3 builds such an index in the background, after the
     transaction that made it commits
     ([`../sm/INDEXBUILD.md`](../sm/INDEXBUILD.md#2-the-protocol)
     §2).
  2. digres waits for each build before it reports the command
     complete, outside any transaction. PostgreSQL's `CREATE
     INDEX CONCURRENTLY` waits the same way, between its
     transactions.
  3. If a build fails, for example a unique index over
     duplicates, digres drops the index in a transaction of its
     own and reports the error.

  In autocommit, this is PostgreSQL's behavior: an error and no
  index, or an index usable at once. Inside a transaction block,
  the error comes at `COMMIT`, after the block's other work has
  committed. The index is not used before the commit.

  The alternative was to refuse such a `CREATE INDEX` in a block,
  as PostgreSQL refuses `CONCURRENTLY` there. That would fail
  scripts that wrap DDL in `BEGIN ... COMMIT`.

## 5. Known gaps

What making each session a thread left:

- **Crash and immediate shutdown.** `SIGQUIT` reaches a thread
  as a flag, and `quickdie()` calls `_exit(2)`, so the whole
  server ends. A thread that ends abnormally also stops the
  server: there is no crash restart
  ([`WORKNOTE.md`](WORKNOTE.md) step 23).
- **Nothing forks except `popen()`**
  ([`WORKNOTE.md`](WORKNOTE.md) steps 17 and 20): `COPY ...
  PROGRAM` and the like, which exec at once.
- **A session thread has no `SIGFPE` handler.** The process's
  disposition, which is the postmaster's, applies.
- **Nothing counts RocksDB's files.** The sessions' files are
  counted, under one limit for the process
  ([`WORKNOTE.md`](WORKNOTE.md) step 135). But RocksDB's
  `max_open_files` is still -1, so every SST file stays open.
  `datadir_load()` could set it to a share of the limit, as
  MyRocks ties `rocksdb_max_open_files` to `open_files_limit`.

Other gaps:

- **`digres --init` cleans up on an error, not on a signal.**
  An interrupted `--init` leaves its data directory behind
  (`main/initdb.c`, `cleanup()`).
- **A reload with no configuration file logs a complaint**,
  because `digres --init` leaves a data directory with none.
  This was not checked again after the move to threads.

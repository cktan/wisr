# digres: the plan for threads

This is a handover. It says where digres stands, what is to be
done next and in what order, and what is already known that
would otherwise be found the hard way. `README.md` describes
the tree, `WORKNOTE.md` the decisions taken so far (steps 1 to
9), and `../sm/PART4.md` §2 why the server must be threaded.

Written on 2026-10-09, with `main` and `wip-dg` both at
`edf7fad`. Step 1 is done since, in `43137f7` on `wip-dg`; §3
records how, and what it left for step 3. Step 2 is done too;
§4.1 records how.

## 1. Where digres stands

- `build/initdb DIR` makes a data directory, and
  `build/digres -D DIR` serves it. PostgreSQL's `psql` connects
  and runs `select 1 + 1`.
- It is still PostgreSQL's process model: a postmaster that
  forks one process for each connection, with shared memory,
  auxiliary processes and a crash restart.
- An error is a C++ exception (`WORKNOTE.md` step 7). That was
  the first step toward threads.
- Every writable global and static is `thread_local`, marked
  `/*SHARED*/` or `/*LOCAL*/` by what it holds (step 1, §3). The
  server still forks, so this changes nothing yet.
- There is no parallel query, index build or vacuum
  (`WORKNOTE.md` step 10), and no VACUUM or autovacuum at all
  (step 11).
- Almost no statement runs beyond integer constants. A lookup
  in a catalog table fails with an error; the lookups are being
  moved to rows compiled in (`WORKNOTE.md` step 6).
- There is no WAL, no recovery and no `pg_control`; the WAL
  code is gone (`WORKNOTE.md` step 12). PostgreSQL's
  transactions still run a statement, and write nothing.
- There is no asynchronous I/O and no I/O worker process
  (`WORKNOTE.md` step 13): a buffer is read with `smgrreadv()`.
- There is no checkpointer and no background writer
  (`WORKNOTE.md` step 14); `CHECKPOINT` succeeds and does
  nothing.

## 2. The goal, and the order

One process, one thread for each connection, and sm attached
once by that process. sm keeps its transaction table, lock
manager and SSI graph in process memory, so forked backends
cannot share a database: threads are what lets more than one
connection use a table.

The order was agreed with the owner, and each step lands on
its own:

1. The `thread_local` sweep.
2. N sessions in one process, under TSan, with no server.
3. Connections as threads.
4. Signals become flags; then the process-only subsystems are
   deleted.

After that comes the replacement of GUC, which is on hold
until the thread model is settled, and then the spike of
`../sm/PART4.md` §7: a table of integers through sm.

Steps 1 and 2 change no behaviour. Do not start step 3 until
both are done and committed.

## 3. Step 1: the `thread_local` sweep

Every mutable global and file-scope static in `src/` becomes
`thread_local`, except those meant to be shared by all
sessions.

### 3.1 The size of it

Measured with `nm --defined-only` over the 717 objects under
`src/`:

| Symbol type | Meaning | Count |
| --- | --- | --- |
| `B` | global, zero-initialised | 404 |
| `D` | global, initialised | 234 |
| `b` | static, zero-initialised | 808 |
| `d` | static, initialised | 266 |

That is 1,712 writable symbols. No `thread_local` or
`__thread` is in the tree today. The objects holding the most:

| Object | Writable symbols |
| --- | --- |
| `utils/misc/guc_tables.o` | 117 |
| `utils/init/globals.o` | 69 |
| `access/transam/xlogrecovery.o` | 62 |
| `access/transam/xlog.o` | 58 |
| `postmaster/postmaster.o` | 53 |
| `postmaster/autovacuum.o` | 32 |
| `optimizer/path/costsize.o` | 32 |
| `access/transam/xact.o` | 31 |
| `storage/buffer/bufmgr.o` | 30 |
| `utils/error/elog.o` | 26 |

### 3.2 How it was done

By a script, `src/tools/cxx_thread_local.py`, as the two
earlier sweeps were done.

- The variables come from the objects, not from a search of the
  source: a writable one lies in `.bss`, `.data`, `.data.rel` or
  `.data.rel.local`. That gives 1,480 of the 1,712 in §3.1; the
  other 232 are `const` tables in `.data.rel.ro`.
- `--apply SRC LIST` reads the line of each definition from
  `nm -l`, and writes the mark there and on every `extern`
  declaration of it. LIST names the variables to share, one a
  line with the reason. It was run once; the list was not
  committed, since nothing would keep it current.
- A shared variable was marked `PG_SHARED`, a section
  attribute the objects showed. §3.6 made it `thread_local` too.
- `--check SRC` runs before `build/digres` is linked. It fails
  if a writable variable is left in one of the four sections
  above, so a global added later is marked or the build fails.
- It also fails if any object has a static initialiser
  (`.init_array`). This is C++: a shared table whose initialiser
  holds the address of a `thread_local` compiles, and is filled
  once, at startup, with the first thread's address. The first
  build caught one: `default_lockmethod` in `lock.c` held
  `&Dummy_trace`, which is now `const`.
- flex leaves `yy_start_state_list` writable in
  `jsonpath_scan.c`; the Makefile makes it `const`.

The result, counted in the objects: 1,248 variables
`thread_local`, 230 `PG_SHARED` and 2 made `const`. The script
wrote 353 files, and `make test` passes as before.

### 3.3 What is shared

`grep -rn '/\*SHARED\*/' src` lists them. The kinds:

- **Pointers into shared memory**, set once at startup:
  `ProcGlobal`, `XLogCtl`, `ShmemBase`, the SLRU control
  structures and the like.
- **Values the postmaster sets and a session reads**, taken from
  what `EXEC_BACKEND` passes a child (`BackendParameters` in
  `launch_backend.c`): `DataDir`, `PostmasterPid`, `MaxBackends`,
  `PgStartTime`, `max_safe_fds`, the syslogger pipe; and what
  `utils/init/datadir.c` loads.
- **Hooks and what `shared_preload_libraries` registers** in the
  postmaster, and the list of loaded libraries in `dfmgr.c`.
- **Tables never written**, such as `child_process_kinds` and
  the `JSON_PROD_*` tables in `common/jsonapi.c`.
- **Postmaster-only state** in `postmaster.c` and `pmchild.c`,
  except its GUC variables. It is deleted in step 4.

### 3.4 The GUC decision

The owner chose to make the GUC variables thread-local now,
rather than leave them shared until the GUC replacement.

- The variables the GUC tables point at, 340 of them, are
  `thread_local` but one (below), and so are the five tables,
  `ConfigureNamesBool` and the rest.
  A `thread_local` table's initialiser runs in each thread on
  first use, and takes the addresses of that thread's own
  variables. No table had to be rebuilt by hand.
- `guc.c`'s own state (`guc_hashtab`, the lists) is
  `thread_local` too, so each session has its own settings and
  `SET` stays per session.
- One exception: `data_directory_mode` is a GUC variable the
  postmaster assigns directly, not through GUC, so it is
  marked `/*SHARED*/`.

What this leaves for step 3 is in §5.

### 3.5 Hazards that still hold

- **A forked child inherits; a new thread does not.** A new
  thread starts with each `thread_local` at the value written in
  the source, until it is given a copy of its parent's (§3.6).
  Steps 1 and 2 cannot show a mistake here, because the server
  still forks; step 3 will, one variable at a time.
- **The list of loaded libraries is shared but unlocked.** Two
  sessions running `LOAD` at once would race on it.
- **No C++ object with a destructor lives in a global** today.
  Keep it so: `thread_local` objects with destructors run them
  at thread exit, in an order that is hard to reason about.

### 3.6 Everything `thread_local`

The owner decided that every writable global is `thread_local`,
the shared ones too, and that a new thread is to start with a
copy of its parent's. That is what a forked child gets, so it is
correct by construction: what is truly shared lives in shared
memory, and a copied pointer still points at it.

- `PG_SHARED` is gone. Each variable it marked is
  `thread_local /*SHARED*/`, and every other one is
  `thread_local /*LOCAL*/`. The comment records what the
  variable holds, for the code that copies them.
- The 13 `JSON_PROD_*` tables in `common/jsonapi.c` could not be
  `thread_local`: a `constexpr` table takes their addresses.
  They are never written, so they are `const`.
- `--check` fails on any writable variable not `thread_local`.

## 4. Step 2: N sessions under TSan

Run several sessions as threads in one process, with no
network and no postmaster, and let ThreadSanitizer find what
the sweep missed.

- The tool mode is the place: `digres --parse --run` takes a
  statement through the parser, analyzer, planner and executor
  with no server (`src/backend/parser/parse_tool.c`). Add a way
  to run N of them at once, each in its own thread, each with
  its own memory contexts.
- `../sq` has a `make tsan` to copy from. TSan fails here with
  "unexpected memory mapping" unless it is run under
  `setarch $(uname -m) -R`.
- The tool mode crashes on a function call today, such as
  `select version()`: it does not set up the catalog caches,
  and the lookup dereferences a null one. Fix that first or
  keep the statements to constants and operators.

Success for this step: N threads each run the statements in
`test/*.sql` and print what one thread prints, and TSan
reports nothing.

### 4.1 How it was done

- **A thread is started as an `EXEC_BACKEND` child is**
  (`utils/init/session_thread.c`). Its `thread_local`s start
  at what the source wrote, not at its parent's values. It is
  given the SHARED values the parent set, only `progname` so
  far, and the settings not at their defaults, as GUC writes
  them for an exec'd child, through a memory stream
  (`write_nondefault_variables_to()`). It makes its own memory
  contexts and settings, and copies none of the parent's
  memory. The list of SHARED values grows in step 3, from
  `BackendParameters` in `launch_backend.c`.
- **`digres --parse ... --threads N`** runs the input in N
  sessions at once and prints what the first printed, and a
  line for any that printed otherwise. DestDebug prints to
  `debug_out`, a stream each thread sets. `test/run.sh` runs
  each file alone and in 8 threads.
- **`make tsan`** compiles every object again as
  `NAME.tsan.o`, links `build/digres-tsan`, and runs
  `test/run.sh` with it under `setarch -R`. sq and sm are not
  instrumented.
- **TSan found one race**: the `lc_messages` hook called
  `setlocale()`, which is the process's. It now keeps the
  value and does nothing: with no NLS, nothing is translated.
- **Left for later**: `check_locale()`, the check hook of
  `lc_monetary`, `lc_numeric`, `lc_time` and `lc_messages`,
  sets and restores the process's locale, so two sessions
  setting one at once would race. The tests do not, and TSan
  did not see it. A thread's memory contexts and settings are
  not freed when it ends.

## 5. Step 3: connections as threads

The listener starts a thread for a connection where it forks a
process today.

- The fork is in `postmaster.c` `BackendStartup()`, through
  `postmaster_child_launch()` in `launch_backend.c`, to
  `BackendMain()` and `PostgresMain()`.
- **A session must be able to end without ending the
  process.** `ereport(FATAL)` ends in `proc_exit()`, which
  calls `exit()`. It has to unwind the thread: an exception to
  the thread's top frame, which then runs that thread's exit
  callbacks. The `on_proc_exit` and `before_shmem_exit` lists
  become one a thread.
- **Each thread needs its own** memory contexts
  (`MemoryContextInit()`), `MyProc`, latch and `MyProcPort`.
- **Each thread needs the postmaster's settings** (§3.4). The
  thread starts with the built-in defaults, so the main thread's
  values have to be copied in. `EXEC_BACKEND`'s route,
  `write_nondefault_variables()` and
  `read_nondefault_variables()`, carries every setting not at
  its default. Neither carries a GUC variable assigned directly
  instead of through GUC; look for more like
  `data_directory_mode`.
- **The process attaches sm once**, at startup, and a session
  opens a database on it (`sm_opendb`).
- **Then delete what the fork made necessary**, all in
  `utils/init/datadir.c`: the child process that lists the
  databases, the list fixed at start, and the oid taken from a
  database's place in that list. A session asks sm directly.

Success for this step: `test/server.sh` passes, with a check
added that runs several sessions at once; the server has one
process; and the three items above are gone.

### 5.1 How far it got

Done. A connection is a thread of the postmaster's process,
and sm is attached once; the auxiliary processes
(checkpointer, background writer, startup) still fork, until
step 4. `WIP_THREADS.md` on `wip-dg` has the detail.

- **Signals to a backend are flags**, chosen as "option B"
  with the owner: `SignalProc()` sets a bit in its PGPROC and
  sets its latch, and it runs the handler it set with
  `pqsignal()` at its next `CHECK_FOR_INTERRUPTS()` or wait.
  Every `kill()` aimed at a backend goes through it. A session
  thread's `pqsignal()` only records the handler; it blocks
  every signal but `SIGALRM` and the faults.
- **A latch wakes its owning thread**: `SIGURG` by `tgkill()`
  to `owner_tid`.
- **A timer a thread**: `timeout.c` uses `timer_create()` with
  `SIGEV_THREAD_ID`.
- **A FATAL ends the thread**: `proc_exit()` throws to the
  thread's top, which queues the exit code for the postmaster
  and sets its latch; `process_pm_child_exit()` takes it as it
  takes a child from `waitpid()`.
- **The thread is given every SHARED variable** of the
  postmaster, by a table the build writes from the marks and
  the linker map (`src/tools/shared_tls.py`), and the settings
  not at their defaults; it then runs
  `AttachSharedMemoryStructs()`, as an EXEC_BACKEND child does,
  for the pointers into shared memory that LOCAL structures
  hold. `MyProcPid` is the thread id.
- **What a process's exit freed, a thread's end frees**: the
  client socket, the wait sets and signalfd, open files, the
  static snapshots' arrays, the memory contexts and the
  allocator's freelists. 300 connections leave the server's
  descriptors and heap where they were.
- **sm is attached once**, by the postmaster at startup
  (`utils/init/datadir.c`); its environment is SHARED. A
  session opens an sm session on its database
  (`sm_opendb()`) and closes it as it ends. A database's oid
  is the id sm gave it (`sm_dbid()`, sm1's never-reused
  `dbid`) above `FirstNormalObjectId`. The child process that
  listed the databases, the list fixed at start, and the oid
  from a place in it are gone.
- **A forked auxiliary process ends with `_exit()`** after its
  exit callbacks: `exit()` would run RocksDB's static
  destructors, which join thread-pool threads that are the
  postmaster's.
- **TSan runs the server** (`make tsan` runs
  `test/server.sh` too). Its reports were all of PostgreSQL's
  lock-free handoffs done with plain loads and barriers:
  atomics' read and write, latch flags, `lwWaiting`, the
  spinlock's test, the postmaster's child flags. They are
  relaxed atomics now (release and acquire for `lwWaiting`).

## 6. Step 4: signals, then deletion

- **Signals become flags.** Done in step 3 (§5.1): a signal to
  a backend is a flag in its PGPROC, a latch wakes its thread,
  and a timeout has a timer of its own.
- **Then delete the process-only subsystems**: the postmaster
  state machine and crash restart, shared memory setup (the
  structures become ordinary allocations), `EXEC_BACKEND`, and
  the auxiliary processes: startup is what is left of them.
  Autovacuum, the WAL writer, the archiver, the I/O workers, the
  checkpointer and the background writer are already gone
  (`WORKNOTE.md` steps 11 to 14).
- **What step 9 stubbed goes with them**: what is left of
  `xlog.c` (the in-memory control record, and the start of the
  transaction counters), the hot standby states of the
  postmaster, and `make_legacy_dirs()` in `postmaster.c`, whose
  list of directories shrinks as each subsystem is cut.

Success for this step: no `kill()` between sessions, no
signal handler that touches session state, and the files above
are gone with `make test` passing.

## 7. Decisions already taken

Do not reopen these without the owner:

- **Memory contexts and resource owners stay.** Contexts are
  arenas and are what makes an exception cheap to recover
  from. RAII is for new code and for guards around them.
- **No object with a destructor in `palloc`'d memory.**
- **`ereport(ERROR)` throws `pg_error_t`**; `PG_TRY` is a `try`.
- **sm's callbacks are `noexcept`**, and four C++ types cross
  `sm.h`: `std::string`, `std::vector`, `std::string_view` and
  `std::atomic`.
- **SPI stays.** SQL-language functions are cut.
- **One encoding, UTF8; default collation C;** ICU for a column
  or a query that asks.
- **Authentication is fixed**: peer on the local socket, SCRAM
  over TCP, no `pg_hba.conf`.
- **Every writable global is `thread_local`**, marked
  `/*SHARED*/` or `/*LOCAL*/`, and the build fails otherwise
  (§3.2, §3.6). A new thread is given its parent's SHARED
  variables. A SHARED variable that a thread must reset or redo
  after the copy was marked SHARED by mistake: mark it LOCAL
  (`src/include/c.h`).
- **The GUC variables and tables are per thread** (§3.4). GUC
  is not otherwise to be touched until the thread model is
  done.
- **No parallel workers.** Parallel query, index build and
  vacuum are removed, planner included; sorting and temporary
  files stay serial.
- **No VACUUM.** sm collects its own garbage. `VACUUM`
  succeeds and does nothing; `ANALYZE` stays.
- **No WAL.** sm keeps what is durable. There is no recovery,
  archiving or hot standby, and `PREPARE TRANSACTION` is
  refused.
- **No asynchronous I/O.** Disk I/O is to go through sm; the
  rest is the client socket, which waits on the latch. A buffer
  read is synchronous.
- **No checkpoints.** sm keeps what is durable, and RocksDB
  bounds its own recovery. `CHECKPOINT` succeeds and does
  nothing, as `VACUUM` does.

## 8. Working in this tree

- **Build and test**: `make -C dg -j4`, then
  `make -C dg -j4 test`. Never plain `make`. Save test output
  to a file and read the file.
- **`DEBUG=1` is set in the owner's shell**, so sm builds `-O1
  -g`. Changing it rebuilds sm.
- **`make format` in `dg/` is not clean**: it reformats 13
  files nobody touched. Format only the files you changed that
  were clean before, with `clang-format -i`. Run `make format`
  in `../sm` before a commit, as `../CLAUDE.md` asks.
- **Port 5432 is taken** on this machine by a PostgreSQL 16.
  `test/server.sh` uses 54329, or `DG_TEST_PORT`.
- **Debugging a backend**: `gdb -p` is refused here
  (`ptrace_scope`). Run the server under gdb with
  `set detach-on-fork off` and `set schedule-multiple on`, add
  `catch signal SIGSEGV` with `bt` as its commands, and after
  `run` repeat `inferior 1` and `continue`, since gdb stops
  each time a child exits.
- **A deep recursion looks like a segfault** in
  `LockAcquireExtended`. Print the outermost frames, `bt -15`,
  to see who started it.
- **Finding functions a cut left unused**: compile the old and
  the new file with `-Wunused-function` and without `-w`,
  using `-c -o /dev/null` (`-fsyntax-only` reports none), and
  compare the two lists.
- **Branches**: work on `wip-dg`, merge to `main` with
  `git merge --squash`, then recreate `wip-dg` from `main`.
  Commit and push only when the owner asks.
- **Rules for every change** are in `../CLAUDE.md`: one issue
  at a time, a doc comment on every function, a synopsis and
  numbered steps on every test, and `.md` prose wrapped at 65
  columns.

## 9. Left undone, and not part of this plan

- Function calls and table names fail: the search path looks
  up `"$user"` in `pg_authid`. This is the step 6 conversion,
  about 1,000 cache call sites.
- The default collation is not set up, so nothing that
  compares text runs.
- Thirteen operators went with the SQL-language functions,
  `interval + date` and `text || anynonarray` among them.
- Time zone abbreviations are not loaded.
- `initdb` does not clean up when interrupted.
- A reload with no configuration file logs a complaint.
- `hba.c` still holds the parser of `pg_hba.conf`, unreached.

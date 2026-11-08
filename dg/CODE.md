# Coding in digres

The rules and practices for changing `dg/`, written for coding
agents. [`README.md`](README.md) says what digres is and how it
builds,
[`DESIGN.md`](DESIGN.md) the design and decisions of record, and
[`WORKNOTE.md`](WORKNOTE.md) each step
taken and why. The repository's [`CLAUDE.md`](../CLAUDE.md) holds
the rules
for the whole repository; this file adds what is particular to
`dg/`. Where the two differ, [`CLAUDE.md`](../CLAUDE.md) wins.

## The overriding objective: a database is its directory

A database `X` is the directory `base/X` and nothing else. It
can be copied to another machine and imported, whole, into a
new digres instance there. Every change is held to this, and it
wins over convenience and over what PostgreSQL did.

- **Everything a database needs to be read is inside its
  directory**: its catalog, its rows, its sequences, its id
  allocators.
- **The package is what is transactionally consistent**: a
  database's data and its WAL, both in its RocksDB directory.
  State that must agree with a database's committed data goes
  there; what need not may be shared.
- **Temporary files are shared for that reason**: every
  database spills to `base/pgsql_tmp`. They are scratch space
  for a running statement, and no part of the package.
- **`global/` holds the list of databases**, and nothing a
  database reads to answer a query.
- **A database uses nothing of another's**, on disk or in a
  counter: no cluster-wide oid, sequence, transaction id or
  shared catalog. An id is the database's own, and two
  databases may hold the same one.
- **Nothing in the directory names where it is**: not the
  database's name or `dbid`, not its oid, not a path, not the
  instance. The importing instance gives it those.
- **What is compiled in is the same in every instance** of one
  version; a row may refer to it by oid. What the instance
  keeps outside a database's directory — the settings, and the
  roles, in the database `pg_global` — a database must not
  depend on to be read.
- **All role data is in `pg_global`, and only there**
  ([`../aa/ROLE.md`](../aa/ROLE.md#3-where-roles-live) §3): no
  owner, privilege or role setting
  in a database's directory, so a copy of it on another
  instance takes that instance's roles. aa keeps it
  ([`../aa/README.md`](../aa/README.md));
  `utils/init/pg_global.c` is digres's
  side, turning tuples and `Acl`s into aa's structs.
- **Before adding state, ask where it lives.** If a copied
  `base/X` would miss it, it is in the wrong place.

The import itself is not built yet
([`DESIGN.md`](DESIGN.md#4-decisions-of-record) §4).

## 1. Building and testing

- **Build with `make -j4`**, never plain `make`, from `dg/`.
  It builds `../sq` and `../sm` as needed.
- **`DEBUG=1 make -j4 test`** runs everything, on the debug
  build (sm `-O0 -g`, with `assert()`), as tests are run: sq's
  parser tests on `build/digres --parse`, the statements in
  `test/` alone and in eight sessions of a server, `initdb`,
  `test/server.sh`, which starts a server and connects with
  `psql`, the sqllogictest files, and PostgreSQL 18's
  regression files as ported (`make pgregress`). It passes
  with these lines:

  ```
  run: PASSED (124 files)
  run: PASSED (12 files, alone and in 8 sessions)
  initdb: PASSED
  server: PASSED
  slt: PASSED (49 files on digres, 10711 records, 11 skipped)
  all 231 ok
  ```

- **`make -j4 slt`** runs every sqllogictest file in
  `test/slt` on digres
  ([`test/slt/README.md`](test/slt/README.md)); their results
  are PostgreSQL 18's. `make test` runs all but
  `select5.test`, which takes three minutes in this build.

- **`make -j4 isolation`** runs PostgreSQL 18's isolation specs
  on digres with its `isolationtester`, from the PostgreSQL 18
  tree built in `$PG18_SRC` (`~/p/postgres18`), and checks each
  against `test/isolation_specs.txt`: which are to pass, and why
  the others fail. Run it for any change to locks or waits; it
  takes ten minutes.

- **`make -j4 regress`** runs PostgreSQL 18's regression
  suite on digres, from the same tree, each file alone in a
  database of its own, and writes which missing features stop
  the most files
  ([`test/regress/README.md`](test/regress/README.md)). It only
  measures, and is in no `make test`; it takes four minutes.

- **`make -j4 pgregress`** runs the same files as ported to
  digres
  ([`test/pgregress/README.md`](test/pgregress/README.md)): each
  with what digres
  does not run as PostgreSQL does commented out, against
  PostgreSQL 18's output for what is left. A difference fails
  it; `make test` runs it, in two minutes. A change that makes
  digres run a statement it could not is followed by trimming
  that file again.

- **`make -j4 tsan`** builds `build/digres-tsan` under the
  thread sanitizer and runs the threaded tests and
  `server.sh` on it; a report fails it. Run it for any change
  to threads, signals, latches, locks or shared state. RocksDB
  and sm, sm_sys.h's services with it, are instrumented with it;
  the first run builds RocksDB for it
  (`test/rocksdb.sh tsan`), which takes a while.
- **Run the full suite before every commit** that touches
  source. A commit of documents alone needs none.
- **Save test output to a file** and read the file, rather
  than running the suite again to grep it differently.
- **The link takes the objects the sources give**, and they
  all lie under `build/`, in a tree mirroring `src/`: a `.c`
  file you delete takes its object with it, and nothing stale
  is left to link.
- **A file that is not compiled is named in `SKIP`**, in the
  Makefile, with the reason beside it.
- **The build is the same on every machine.** No `uname -m`
  branches and no `-march` flags; the development machine is
  aarch64, and the build must also work on x86_64.
- **Debugging**: `gdb -p` is refused on the development
  machine (`ptrace_scope`); run the server under `gdb` instead.

## 2. C compiled as C++

Every `.c` file is compiled as strict C++20 (`g++ -x c++
-std=gnu++20`).

- **The standard library is available**: `std::mutex`,
  `std::unique_ptr`, `std::atomic` and the rest. Include
  `postgres.h` first, as always, then the standard headers.
- **Errors are exceptions.** `ereport(ERROR)` throws
  `pg_error_t`; `PG_TRY` and `PG_CATCH` are a `try` and a
  `catch` of it. `proc_exit()` in a session thread throws
  `proc_exit_t`, which the thread's top catches. So
  destructors run as an error unwinds: RAII is safe.
- **Never `catch (...)` without rethrowing.** It would swallow
  an error or a session's exit.
- **No RTTI.** No `dynamic_cast`, `typeid`, `std::type_info`,
  `std::type_index`, `std::any` or `std::dynamic_pointer_cast`.
  PostgreSQL says what a node is with its tag (`nodeTag()`,
  `IsA()`); do the same, or use an enum, a function pointer or
  a virtual call. Catching an exception by its type is not this
  rule's concern. The Makefile compiles with `-fno-rtti`, so the
  compiler refuses the rest.
- As in PostgreSQL, do not leave a `PG_TRY` block with
  `return`, `break`, `continue` or `goto`.

## 3. Globals

digres is one process; each session is a thread of it.

- **Every writable global is `thread_local`**, marked
  `thread_local /*LOCAL*/ int y;`: it starts at its initial
  value in each thread, and what one thread sets, others never
  see. `src/tools/cxx_thread_local.py --check` fails the build
  on anything else, and on any static initialiser.
- **A thread shares only through `pcb` and `shmem`.** Nothing
  is copied from a parent's thread-local variables; there is
  no `/*SHARED*/` mark ([`WORKNOTE.md`](WORKNOTE.md) step 229).
- **The control blocks replace them** (`utils/control_block.h`),
  four, by who owns a field and who reads it:
  - **`pcb_t`**: what the main thread owns and other threads
    read. They read it through `const pcb_t *pcb`; the main
    thread alone writes it, through `pcb_main()`, which
    asserts it is the main thread. A field it writes after
    the first thread starts is an atomic, or guarded by a
    mutex its comment names.
  - **`shmem_t`**: what the threads share and write. Each
    field is an atomic, a mutex whose comment names what it
    guards, or an object that guards itself.
  - **`tcb_t`**: what one thread has, alone, one per thread.
    What a thread inherits from the one that started it is
    copied in at its start, and is its own after: what a
    session changes, a setting among them, comes here, not to
    `pcb_t`. A tcb may point into
    `shmem_t`; nothing of it is reached from another thread.
  - **`postmaster_tcb_t`**: what the main thread owns and no
    other thread reads, the postmaster's own state, or a
    standalone backend's. `main()` makes it; it is reached
    through `thread_local postmaster_tcb_t *postmaster_tcb`,
    NULL in every other thread, and never freed.
  - A field is a value, or a `std::unique_ptr` to what the
    block owns. A non-owning raw pointer is fine, with a
    comment naming its owner (a memory context, shmem).
  - Memory the process keeps for its whole life is
    `malloc()`'d, and its pointer is a plain field of `pcb_t`
    or `shmem_t`: neither is ever freed. Do not call
    `ShmemAlloc()` for it.
  - A module's state is a struct the header only declares;
    the module defines it, the function that makes it and its
    deleter.
  - `tcb` is a pointer (`thread_local tcb_t *tcb`), made at a
    thread's start and freed at its end, and NULL while it is
    being freed. A signal handler checks for NULL.
  - **A setting that changes only with a restart
    (`PGC_POSTMASTER`) is read as `pcb->name`**, never as the
    GUC variable, which each thread has a copy of.
    `publish_postmaster_settings()` (`guc_tables.c`) copies it
    into `pcb_t` once the postmaster has read its settings; the
    variable is `static` there, so no other file can name it.
    A new such setting adds a `pcb_t` field and a line in that
    function. The same holds for a preset one (`PGC_INTERNAL`,
    such as `block_size`), which the build fixes. Each `pcb_t`
    field of a preset setting names, in a comment, the file
    the variable was defined in.
  - **A LOCAL global moves into `tcb_t`** when a change
    touches it. Put new state in a block from the start.
- **`src/tools/process_wide.txt`** lists the writable globals
  allowed outside `thread_local`. It holds `pcb` and `shmem`
  alone; keep it that way.

## 4. Threads, signals and processes

- **Nothing forks** but `popen()`, which execs at once
  (`COPY ... PROGRAM`). Do not add a process: add a thread
  through `session_thread_start()`.
- **Threads are `std::thread`, locks `std::mutex`**, not
  pthreads. `pthread_sigmask()` stays: it sets the calling
  thread's signal mask, which the standard library cannot.
- **Signals between threads are interrupt bits** (sm_sys.h),
  named in `storage/procsignal.h`: `INTERRUPT_CANCEL`, `_DIE`,
  `_QUIT`, `_RELOAD`, `_LOG_MEMORY`, `_ALARM`, and `_LATCH`,
  the thread's latch, which runs no handler.
  `session_interrupt()` raises them in a session's own thread,
  found by its record (`storage/session_registry.h`); a thread
  sets a handler for each of the others with
  `SetInterruptHandler()`, not
  `pqsignal()`, and it runs at its next
  `CHECK_FOR_INTERRUPTS()` or wait. Signal numbers are left
  only where the postmaster speaks to its children and in the
  SQL functions that send them (`InterruptsOfSignal()`).
- **No thread takes a signal; the postmaster reads them.**
  `PostmasterMain()` blocks SIGHUP, SIGINT, SIGQUIT and SIGTERM
  before any thread starts, so every thread inherits the mask,
  and `ServerLoop()` reads them from a signalfd: no handler
  runs, and no system call is interrupted. The other signals
  the process may be sent are ignored, and faults keep their
  default action. Start no thread before `PostmasterMain()`
  blocks them, and install no handler. A standalone backend
  blocks none: SIGINT or SIGTERM ends it.
- **A latch wakes one thread** through `sm_sys.h`: it keeps its
  owner's handle, and `SetLatch()` writes the owner's wake fd
  (`sm_interrupt()`, or `sm_interrupt_self()` in a signal
  handler).
- **A thread's timeouts are one `sm_sys.h` timer**
  (`timeout.c`), which raises `INTERRUPT_ALARM` in that thread
  alone; its handler fires what is due and arms the timer for
  the next.
- **Lock-free handoffs are atomics.** Where PostgreSQL read
  and wrote a flag shared between processes plainly, digres
  uses a relaxed atomic (or release and acquire where order
  matters), so the thread sanitizer stays quiet.
- **A thread that ends abnormally stops the server**: there is
  no crash restart. The postmaster reaps no child process; a
  session waits for its own `popen()` child.
- **There is no shared memory.** A structure the threads share
  is an ordinary allocation: `malloc()`'d, its pointer in
  shmem, made before the first thread starts. There is no
  registry to find one by name (`ShmemInitStruct()` is gone),
  and nothing is sized ahead of time; do not add a
  `*ShmemSize()` function.
- **What a process's exit freed, a thread's end must free:**
  `session_thread_end()` does it, and the tcb's destructors
  do it for what a block owns.

## 5. sm

- **The postmaster attaches sm once** (`sm_attach`, in
  `datadir_load()`); attaching recovers every database. Each
  session opens its database with `sm_opendb` and closes it
  with `sm_closedb`.
- **A database's oid is `FirstNormalObjectId` plus its sm
  `dbid`** (`sm_dbid`).
- **Only `initdb` calls `sm_init`.**

## 6. Logging

- **Every message goes to the log**, through `write_console()`
  in `elog.c` and `logfile_write()` in `logfile.c`: one
  `write()` under `shmem->stderr_mutex`, to stderr, or, with
  `logging_collector` on, to the log file. There is no log
  collector process, no pipe, no csvlog and no jsonlog.
- **`logfile.c` rotates in the writer's thread**, on size, on
  age and on `pg_rotate_logfile()`; age is tested as a message
  arrives. It points fd 2 at the log file, so a stray write to
  stderr lands there too.
- **Write to the log with `write_console()`, `ereport()` or
  `write_stderr()`**, never `fprintf(stderr)`: only those keep
  a message whole and count it toward `log_rotation_size`.
- **A thread sm owns has no memory context or GUC values**
  (`sm_log` in `datadir.c`): there, write with `write_console()`
  and a line you format yourself.

## 7. Deleting PostgreSQL code

Much of the work is removing what digres does not need.

- **Delete what belongs to a module that was deliberately
  removed** (WAL, the checkpointer, VACUUM, the syslogger and
  the rest in [`WORKNOTE.md`](WORKNOTE.md)): its code, its
  settings, its SQL
  functions, wait events, locks and process types.
- **Do not delete what is merely compiled out** by configure
  (`USE_OPENSSL`, `ENABLE_GSS`, `USE_BONJOUR`, OAuth): that is
  a build option, not a removal. Check whether code is absent
  because of a removal step or because of a `pg_config.h`
  `#undef` before proposing to delete it.
- **Remove what your change leaves unused**: a function with
  no caller left, a variable never set, an include, a
  comment that now describes nothing. Leave unused code you
  did not orphan alone, and mention it.
- **To find the functions a cut left unused**, compile the old
  and the new file with `-Wunused-function` and without `-w`,
  using `-c -o /dev/null` (`-fsyntax-only` reports none), and
  compare the two lists.
- **Generated files regenerate.** Edit the sources
  (`pg_proc.dat`, `wait_event_names.txt`) and
  let the build write `fmgrtab.c`, `cat_builtin.c`,
  `postgres.bki` and the wait-event tables.
- **Settings**: remove a setting's entry from `guc_tables.c`,
  its variable, its hooks in `guc_hooks.h`, and its line in
  `postgresql.conf.sample`.

## 8. Style

- **Match the code around you.** Most of `dg/` is PostgreSQL's
  code, in its naming (`CamelCase` functions, `snake_case`
  locals); keep to it when editing it.
- **New code follows [`CLAUDE.md`](../CLAUDE.md)**: `snake_case`
  names, types
  ending in `_t`, `m_` before a class's private members.
- **Every function gets a doc comment** of a line or two.
- **A call that does something significant gets a short
  comment saying why** — taking a lock, starting a thread,
  freeing a block — even when the call reads plainly.
- **No `strncpy()`**: `memcpy()` and write the terminator.
- **Use `std::optional` reluctantly.**
- **A constant goes on the left of `==` and `!=`**:
  `if (0 == x)`, `if (NULL != p)`, `if (SM_OK == rc)`, not
  `if (x == 0)`. A `=` typed for `==` then fails to compile.
  It is for the code you write and the lines you change;
  comparisons already there are not rewritten for it.
- **Format with `clang-format -i`** the files you change that
  were already formatted at `HEAD` (format the file at `HEAD`
  and compare); do not reformat a file that was not. Run
  `make format` in `../sm` before committing.

## 9. Tests

- **Every test opens with a one-line synopsis** of what it
  proves, then a numbered step list, one sentence a step,
  before the code.
- **Prove a test can fail.** After writing one, break the
  thing it checks (comment out the call, disable the path),
  confirm the test fails, and restore it.
- **The isolation scenarios in `../sm/sm1/mvcc` are the
  specification** of sm1's concurrency control. Changing or
  removing one needs the user's approval, with the reason
  given first.

## 10. Documents

- **Each change that removes or reshapes something gets a
  step in [`WORKNOTE.md`](WORKNOTE.md)**: what it was, why it
  goes, what
  went with it, and what was left on purpose.
- **A decision of record goes in [`DESIGN.md`](DESIGN.md)** §4.
- **Wrap prose at 65 columns** in every `.md` file; code
  blocks, tables and headings are not wrapped.

## 11. Git

- **Work on a `wip-*` branch** and commit there; merge into
  `main` only when asked. A small branch is squashed
  (`git merge --squash`); a larger one is merged plainly with
  `git merge`, keeping its commits.
- **Commit messages**: a short subject in the style
  `dg: the syslogger is gone`, then bullets.
- **Commit only when asked**, and push only when asked.
- **Never run `sprite-env checkpoints`.**

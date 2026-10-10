# Coding in digres

The rules and practices for changing `dg/`, written for coding
agents. `README.md` says what digres is and how it builds,
`PLAN.md` the decisions of record, and `WORKNOTE.md` each step
taken and why. The repository's `CLAUDE.md` holds the rules
for the whole repository; this file adds what is particular to
`dg/`. Where the two differ, `CLAUDE.md` wins.

## 1. Building and testing

- **Build with `make -j4`**, never plain `make`, from `dg/`.
  It builds `../sq` and `../sm` as needed.
- **`make -j4 test`** runs everything: the statements in
  `test/` alone and in eight threads, `initdb`, and
  `test/server.sh`, which starts a server and connects with
  `psql`. It passes with these lines:

  ```
  run: PASSED (118 files)
  run: PASSED (3 files, alone and in 8 threads)
  initdb: PASSED
  server: PASSED
  ```

- **`make -j4 tsan`** builds `build/digres-tsan` under the
  thread sanitizer and runs the threaded tests and
  `server.sh` on it; a report fails it. Run it for any change
  to threads, signals, latches, locks or shared state.
- **Run the full suite before every commit** that touches
  source. A commit of documents alone needs none.
- **Save test output to a file** and read the file, rather
  than running the suite again to grep it differently.
- **The link takes every `*.o` under `src/`.** A `.c` file you
  delete leaves its `.o`, `.d`, `.tsan.o` and `.tsan.d`
  behind, and the stale object is still linked. Delete them
  with the source.
- **A file that is not compiled is named in its directory's
  `SKIP`**, with the reason beside it.
- **The build is the same on every machine.** No `uname -m`
  branches and no `-march` flags; the development machine is
  aarch64, and the build must also work on x86_64.

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
- As in PostgreSQL, do not leave a `PG_TRY` block with
  `return`, `break`, `continue` or `goto`.

## 3. Globals

digres is one process; each session is a thread of it.

- **Every writable global is `thread_local`**, with a comment
  after the keyword saying what it is:
  `thread_local /*SHARED*/ int x;` is copied from the parent
  into each new thread; `thread_local /*LOCAL*/ int y;` starts
  at its initial value in each thread.
  `src/tools/cxx_thread_local.py --check` fails the build on
  anything else, and on any static initialiser.
- **SHARED does not mean shared.** Each thread has its own
  copy, taken when the thread starts; what one thread sets
  later, others never see. A SHARED variable that a thread
  must reset or redo after the copy was marked SHARED by
  mistake: mark it LOCAL.
- **The control blocks replace them** (`utils/control_block.h`).
  `pcb_t`, made once by `main()`, holds what there is one of
  for the process; `tcb_t`, one per thread, holds what each
  thread has its own of.
  - A field is a value, or a `std::unique_ptr` to what the
    block owns. A non-owning raw pointer is fine, with a
    comment naming its owner (a memory context, the pcb).
  - Memory the process keeps for its whole life is
    `malloc()`'d, and its pointer is a plain `pcb_t` field:
    the pcb is never freed. Do not call `ShmemAlloc()` for
    it.
  - A module's state is a struct the header only declares;
    the module defines it, the function that makes it and its
    deleter.
  - A `pcb_t` field is set before the first thread starts and
    only read after, or guards itself with a mutex or an
    atomic.
  - `tcb` is a pointer (`thread_local tcb_t *tcb`), made at a
    thread's start and freed at its end, and NULL while it is
    being freed. A signal handler checks for NULL.
  - **Move a global into a block only when your change
    touches it.** Put new state there from the start. Do not
    move untouched globals for the migration's sake.
- **`src/tools/process_wide.txt`** lists the writable globals
  allowed outside `thread_local`. It holds `pcb` alone; keep
  it that way.

## 4. Threads, signals and processes

- **Nothing forks** but `popen()`, which execs at once
  (`COPY ... PROGRAM`). Do not add a process: add a thread
  through `session_thread_start()`.
- **Threads are `std::thread`, locks `std::mutex`**, not
  pthreads. `pthread_sigmask()` stays: it sets the calling
  thread's signal mask, which the standard library cannot.
- **Signals between threads are interrupt bits** (sys.h), named
  in `storage/procsignal.h`: `INTERRUPT_CANCEL`, `_DIE`,
  `_QUIT`, `_RELOAD`, `_PROCSIG`. `InterruptProc()` raises them
  in the thread that owns a PGPROC; a thread sets a handler for
  each with `SetInterruptHandler()`, not `pqsignal()`, and it
  runs at its next `CHECK_FOR_INTERRUPTS()` or wait. Signal
  numbers are left only where the postmaster speaks to its
  children and in the SQL functions that send them
  (`InterruptsOfSignal()`). A session thread blocks every
  signal but SIGALRM, from its own timer, and the faults it
  causes itself.
- **A latch wakes one thread** through sys: it keeps its
  owner's handle, and `SetLatch()` writes the owner's wake fd
  (`sys_interrupt()`, or `sys_interrupt_self()` in a signal
  handler). A timeout has a timer of its own per thread.
- **Lock-free handoffs are atomics.** Where PostgreSQL read
  and wrote a flag shared between processes plainly, digres
  uses a relaxed atomic (or release and acquire where order
  matters), so the thread sanitizer stays quiet.
- **A thread that ends abnormally stops the server**: there is
  no crash restart. The postmaster reaps no child process; a
  session waits for its own `popen()` child.
- **There is no shared memory.** A structure the threads share
  is an ordinary allocation: `malloc()`'d, its pointer in the
  pcb. `ShmemInitStruct()` and `ShmemInitHash()` still make
  the older ones on the heap, zeroed, or find them by name in
  `pcb->shmem_index`. Nothing is sized ahead of time; do not
  add a `*ShmemSize()` function.
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

- **Every message goes to stderr**, through `write_console()`
  in `elog.c`, one `write()` under `pcb->stderr_mutex`. There
  is no log collector, no csvlog and no jsonlog; a logger
  that writes files is to come.

## 7. Deleting PostgreSQL code

Much of the work is removing what digres does not need.

- **Delete what belongs to a module that was deliberately
  removed** (WAL, the checkpointer, VACUUM, the syslogger and
  the rest in `WORKNOTE.md`): its code, its settings, its SQL
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
- **Generated files regenerate.** Edit the sources
  (`pg_proc.dat`, `wait_event_names.txt`, `lwlocklist.h`) and
  let the build write `fmgrtab.c`, `cat_builtin.c`,
  `postgres.bki` and the wait-event tables.
- **Settings**: remove a setting's entry from `guc_tables.c`,
  its variable, its hooks in `guc_hooks.h`, and its line in
  `postgresql.conf.sample`.

## 8. Style

- **Match the code around you.** Most of `dg/` is PostgreSQL's
  code, in its naming (`CamelCase` functions, `snake_case`
  locals); keep to it when editing it.
- **New code follows `CLAUDE.md`**: `snake_case` names, types
  ending in `_t`, `m_` before a class's private members.
- **Every function gets a doc comment** of a line or two.
- **A call that does something significant gets a short
  comment saying why** — taking a lock, starting a thread,
  freeing a block — even when the call reads plainly.
- **No `strncpy()`**: `memcpy()` and write the terminator.
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
  step in `WORKNOTE.md`**: what it was, why it goes, what
  went with it, and what was left on purpose.
- **A decision of record goes in `PLAN.md`.**
- **Wrap prose at 65 columns** in every `.md` file; code
  blocks, tables and headings are not wrapped.

## 11. Git

- **Work on a `wip-*` branch** and commit there; merge into
  `main` only when asked. The user keeps the branch's commits
  (fast-forward) rather than squashing.
- **Commit messages**: a short subject in the style
  `dg: the syslogger is gone`, then bullets.
- **Commit only when asked**, and push only when asked.
- **Never run `sprite-env checkpoints`.**

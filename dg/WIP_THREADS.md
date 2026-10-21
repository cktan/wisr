# Work in progress: step 3, connections as threads

Where step 3 of `PLAN.md` (§5) stood on `wip-dg` when it
ended. `PLAN.md` §5.1 is the summary; this is the detail, and
what was left.

**A record, kept as it was written.** Much of it has changed
since, each change a step of `WORKNOTE.md`: the `/*SHARED*/`
variables and the copy table of item 5 are gone (step 229),
and sm and RocksDB are built for TSan (`CODE.md` §1).

## Done

1. **A FATAL ends a session thread, not the process.**
   `proc_exit()` runs the thread's exit callbacks and throws
   `proc_exit_t` (`IsSessionThread`); the thread's top catches
   it. A session that returns ends through `proc_exit(0)`.
2. **A latch wakes its owning thread.** `Latch` has
   `owner_tid`; `SetLatch()` sends `SIGURG` with `tgkill()`.
3. **A signal to a backend is a flag** (option B).
   `SignalProc(proc, sig)` sets bit `sig` in
   `proc->pendingSignals` and sets `proc->procLatch`; the owner
   runs the handler `pqsignal()` recorded
   (`pqsignal_handler()`) from `DeliverPendingSignals()`: in
   `ProcessInterrupts()` and at the top of each pass of
   `WaitEventSetWait()`'s loop. `CHECK_FOR_INTERRUPTS()` tests
   `*MyPendingSignals`. `SignalSelf()` runs this thread's own
   handler; `SignalSelfLater()` leaves it for the next wait.
   `SendProcSignal()`, barriers, the cancel request,
   `pg_cancel_backend()`, `pg_terminate_backend()`, DROP
   DATABASE's terminate, the statement and lock timeouts and
   the postmaster's `signal_child()` all use it.
4. **A timer a thread.** `timeout.c` arms
   `timer_create(SIGEV_THREAD_ID)`, made on first use and
   deleted at the thread's exit.
5. **A thread is given every SHARED variable.** The build reads
   each `/*SHARED*/` variable's place in the thread-local block
   from one link (`src/tools/shared_tls.py`), compiles the
   table into a second, and checks a third reading.
   `session_thread_start()` copies those bytes, 197 variables,
   before anything else. A file's `__tls_guard` is not copied.
6. **A connection is a thread** (`postmaster/backend_thread.c`).
   - `BackendStartup()` calls `backend_thread_launch()`; the
     thread's id is the child's pid. The postmaster does not
     close the client socket; the thread does as it ends.
   - The thread sets up as a forked child does
     (`InitPostmasterChild()` stops before what belongs to a
     process: `setsid()`, `SIGQUIT`, the parent-death signal),
     then runs `BackendMain()`. `InitProcess()` runs
     `AttachSharedMemoryStructs()` for it, as for an
     EXEC_BACKEND child: some pointers into shared memory sit
     in LOCAL structures (`pgStatLocal`). `XLOGShmemInit()`
     does not free the copied `ControlFile`.
   - A backend's `HTAB *` onto a shared hash table is a handle
     it builds in its own memory, so it is LOCAL, not SHARED:
     `ShmemIndex`, `SharedBufHash`, the lock and predicate-lock
     tables, the custom wait events' two. The sweep had marked
     them SHARED; a copied `ShmemIndex` made a thread create a
     new, empty index. A pointer into shared memory is SHARED; a
     handle onto it is LOCAL.
   - A signal before the thread has a PGPROC goes to its launch
     record, with `SIGURG` to the thread;
     `AdoptPendingSignals()` moves it to the PGPROC.
     `SIGKILL` to a thread is dropped.
   - As it ends the thread queues itself with its exit code and
     sets the postmaster's latch; `process_pm_child_exit()`
     takes it from the queue.
   - In a session thread `MyProcPid` is the thread id;
     `CurrentProcPid()` replaces the `getpid()` checks. Signals
     are blocked but `SIGALRM` and the faults, whose handler is
     the process's wrapper running the thread's own.
   - A thread's end frees what a process exit did: the socket,
     the wait sets and signalfd, open files
     (`ShutdownFileAccess()`), the static snapshots' arrays, the
     memory contexts and the context freelists. 300 connections
     leave descriptors and heap flat.
   - `set_stack_base()` runs at each thread's start, and in
     `main()` before `--parse` is dispatched (it was after).
7. **TSan runs the server.** `make tsan` runs `test/server.sh`
   on `build/digres-tsan`; it is clean. PostgreSQL's lock-free
   handoffs became relaxed atomics: atomics' read and write,
   latch flags and owner, the spinlock's test, the postmaster's
   child flags, the `CHECK_FOR_INTERRUPTS()` test; `lwWaiting`
   is a release store and an acquire load.

`test/server.sh` checks 8 sessions at once, each a thread with
no backend process, an authentication timeout (`SignalSelf`),
and a fast shutdown with an idle session open (`SignalProc`).

8. **sm is attached once.** The postmaster attaches the home
   directory at startup; the `sm_env_t *` is SHARED. A session
   calls `sm_opendb()` for its database and `sm_closedb()` as
   it ends; a database's oid is `FirstNormalObjectId` plus the
   `dbid` sm gave it (`sm_dbid()`, new). `datadir.c` no longer
   forks or keeps a list. A forked auxiliary process ends with
   `_exit()`, so RocksDB's static destructors, which would join
   the postmaster's thread-pool threads, do not run in it.

## Left

- **Step 3 is done** (`PLAN.md` §5). What follows is step 4
  and these gaps.
- **sm and RocksDB are not built for TSan**: a race inside
  them would not show.
- **Crash and immediate shutdown.** `SIGQUIT` reaches a thread
  as a flag, and `quickdie()` calls `_exit(2)`: the whole
  server ends. A thread that ends abnormally stops the server
  too: there is no crash restart (`WORKNOTE.md` step 23).
- **Nothing forks but `popen()`** (`WORKNOTE.md` steps 17 and
  20): `COPY ... PROGRAM` and the like, which exec at once.
- **Untested**: cancel and terminate from SQL (a function call
  needs catalog tables), and a reload (`SIGHUP`) reaching a
  session.
- **A session thread has no `SIGFPE` handler**: the process's
  disposition, the postmaster's, applies.
- **RocksDB's files are counted by no one.** The sessions'
  are, under one ceiling for the process (`WORKNOTE.md` step
  135); RocksDB's `max_open_files` is still -1, every SST file
  open. `datadir_load()` could set it to a share of the
  ceiling, as MyRocks ties `rocksdb_max_open_files` to
  `open_files_limit`.
- `syslog_ident` (9 bytes) is not freed at a thread's end.

# Work in progress: step 3, connections as threads

Where step 3 of `PLAN.md` (§5) stood when it was set aside for
the removal of async I/O, on its own branch. Read `PLAN.md` §4
and §5 first.

## Done on this branch

1. **A FATAL ends a session, not the process.** In a thread
   started by `session_thread_start()`, `proc_exit()` runs the
   thread's exit callbacks and throws `proc_exit_t`
   (`storage/ipc/ipc.c`, `proc_exit_throws`); the thread's top
   catches it, and the exit code is the thread's result. A
   session that returns ends through `proc_exit(0)` too, so its
   callbacks run. A session thread registers no `atexit`
   handler.
   - `digres --parse` takes `-c NAME=VALUE`. `test/run.sh` runs
     `select_error.sql` with `exit_on_error=on` in 8 threads:
     8 FATALs on stderr, the process lives to print, status 1.
     That also tests the copy of settings to a thread.
2. **A latch wakes its owning thread.** `Latch` has `owner_tid`
   beside `owner_pid`; `SetLatch()` sends `SIGURG` to that
   thread with `tgkill()` (`WakeupOtherProc(pid, tid)`), and
   `WakeupMyProc()` to this thread. A thread's signalfd reads
   only signals sent to it and to the process, so the wake-up
   reaches the right thread, from this process or a forked one.
   No test covers it yet: the tool's sessions never wait on a
   latch.

## Open: how a session thread is reached (issue 2)

`BackendStartup()` starting a thread instead of a process is
blocked on signals:

- A backend installs handlers with `pqsignal()` (`die`,
  `StatementCancelHandler`, `procsignal_sigusr1_handler`, ...).
  Handlers are the process's, so a thread would replace the
  postmaster's.
- The postmaster stops a backend with `kill(pid, SIGTERM)`;
  cancel is `SIGINT`, ProcSignal is `SIGUSR1`, reload `SIGHUP`.
  For a thread that pid is the postmaster's own.
- The postmaster learns of a child's exit from `SIGCHLD` and
  `waitpid()`; a thread's end gives neither.

The options put to the owner, not yet chosen:

- **A, minimal.** Threads keep every signal blocked and
  `pqsignal()` does nothing in them; a thread's end is queued
  for the postmaster, which is woken by its latch;
  `signal_child()` skips threads. No cancel, terminate or
  reload; a fast shutdown waits for sessions to disconnect.
- **B, recommended.** "Signals become flags" (step 4) for
  sessions now: a session's `pqsignal()` records its handler in
  a per-thread table; `kill(pid, sig)` to a session sets a bit
  in its `PGPROC` and wakes it; it runs the handler when it
  wakes and in `CHECK_FOR_INTERRUPTS()`. `MyProcPid` becomes the
  thread id, so `pg_backend_pid()` and cancel keys stay
  distinct.

Either way the thread must be given the postmaster's SHARED
values (`ShmemBase`, `ProcGlobal`, `MaxBackends`, ...), from
the list `save_backend_variables()` in `launch_backend.c`
keeps; today it is given only `progname`.

## Waking a session from every wait

The rule settled: a request is kept as a flag in digres, and
the session is woken from wherever it waits, RocksDB excepted.

- **PostgreSQL's long waits** all go through `WaitEventSetWait`
  (`epoll_wait`): the client socket (non-blocking, so never
  asleep in `recv`/`send`), lock and condition-variable waits,
  timeouts. `SetLatch()` reaches them, now per thread.
  LWLocks (`sem_wait`), spinlock back-off and file I/O are short
  or not interruptible in PostgreSQL either.
- **sm's lock waits** sleep on `lock_cv`, a condition variable a
  signal does not wake. `sm_cancel(session)` sets the session's
  cancel and `notify_all()`s under `lock_mu`; both lock waits
  (`lock_wait`, `lock_wait_xact`) check it every pass and fail
  with `SM_ERR_CANCELLED`, and so does every later wait until
  the next statement. So a request is: set the bit, `SetLatch()`,
  and, for cancel, timeouts and terminate, `sm_cancel()`.
- **Cancel is all sm need offer.** The one request that must wake
  a wait without ending it is a ProcSignal barrier, and the only
  barrier left, `PROCSIGNAL_BARRIER_SMGRRELEASE`, goes with smgr.
  Not yet put to the owner as settled.
- **sm waits `sm_cancel()` does not end**: `sm_cat_index_wait`
  (`builder.cc:415`, takes the host's own cancel word, so digres
  can pass its flag), `builder_detach` (`builder.cc:324`, from
  `sm_rmdb`, `sm_detach(force)` and a failed `sm_opendb`; cannot
  be cut short), and mutexes held across another thread's
  RocksDB call (write latches, `alloc_mu`, `store_mu`, sm3's
  catalog mutex, sm1's `env->mu`), which are RocksDB waits.

## Next

Choose A or B, then issue 2; then sm attached once and the
fork workarounds in `utils/init/datadir.c` (`PLAN.md` §5).

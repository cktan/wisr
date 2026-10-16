# Coding in sm

The rules and practices for changing `sm/`, written for coding
agents. `README.md` lays out the levels, `DESIGN.md` holds the
decisions that cross them, and each level's `SMn_DESIGN.md` is
its design of record (sm0's is `sm0/README.md`). The
repository's `CLAUDE.md` holds the rules for the whole
repository; this file adds what is particular to `sm/`. Where
the two differ, `CLAUDE.md` wins.

## 1. Levels

- **A level calls only the levels below it**: sm1 calls sm0;
  sm2 calls sm1 and sm0; sm3 calls sm2, and sm1 directly where
  sm2 has nothing to add (`DESIGN.md` §5). Nothing calls
  upward, and the C wrapper (`include/sm.h`, `src/sm.cc`)
  belongs to no level.
- **sm0 depends on nothing in the repository** but the root's
  `include/sm_error.h`: only the C++ standard library and
  POSIX. It links no RocksDB. What every level needs and is
  about no storage goes there.
- **sm1's interface is the only channel into RocksDB.** No
  level above holds a RocksDB handle. A missing primitive is
  added to sm1's surface, not worked around.
- **A public header is opaque**: handles, not layouts, no
  private members and no RocksDB type.

## 2. The interface to sm's users

- **Only `std::string`, `std::string_view`, `std::vector` and
  `std::shared_ptr` cross to sm's users.** No other standard
  library type appears in what they call (`include/sm.h`,
  `sm_sys.h`): no `std::optional`, `std::span` or
  `std::function`. A `std::shared_ptr` is for an object whose
  life sm and its user share, such as a thread's control block
  or a statistics snapshot, and points to a `const` or opaque
  type.
- **The interface is C-like functions** returning 0 on
  success and -1 otherwise (§4). Their parameters may be C++
  references.
- **sm never throws an exception knowingly** (§5). One may
  still pass through it: from a callback the user supplied,
  or from the C++ library, as `std::vector::at()` and `new`
  may throw.

## 3. Building and testing

- **Build with `make -j4`** in the level you change
  (`make -C sm1 -j4`), never plain `make`. `make -j4` in `sm/`
  builds every level, then `build/libsm.a`, the one archive
  of every level's objects and the wrapper's that dg links.
- **`make -j4 test` in `sm/`** runs every level's tests in
  order, then the `rel` suite. It passes when every doctest
  line reads `0 failed`, sm1's line reads
  `scenarios: 109 scripts match` (or however many there are),
  and the last line is `All rel tests passed.`
- **Run the full suite before every commit** that touches
  source, and dg's (`make -C ../dg -j4 test`) when the change
  reaches what dg calls: `include/sm.h`, `sm_sys.h`, or the
  build. A commit of documents alone needs none.
- **`make tsan` and `make asan` in sm0** run its tests under
  the sanitizers; run them for a change to `sm_sys.h`'s
  threads, waits or timers.
- **Save test output to a file** and read the file, rather
  than running the suite again to grep it differently.
- **Run `make format` in `sm/` before committing.** It leaves
  out `sm1/include/sm1.h`, which is aligned by hand, and
  `sm0/include/sm_sys.h`.

## 4. Errors

- **A call returns 0 on success and -1 on failure**, and the
  code and message are read afterwards from the thread's error
  state: `sm1::errnum()` and `sm1::errmsg()` in C++,
  `sm_errnum()` and `sm_errmsg()` in C. Only "no row" is a
  positive return, 1 (`DESIGN.md` §5).
- **Record an error with sm0's setters**, so a failing path is
  one line: `return set_error(SM_ERR_..., "{} ...", ...);`
  (`sm0_error.h`; `sm1::seterr` takes a ready message).
  `set_errno_error` maps an `errno`, sm1's `set_status_error`
  a RocksDB status. They allocate nothing.
- **The codes are one enum for the whole stack**, in
  `include/sm_error.h`. It is append only: a new code goes at
  the end of its level's group, gets its name in sm0's
  `errname()` table, and is never renumbered or reused.
- **Out-parameters take one shape**: a required output is a
  reference, an optional one a pointer with `nullptr` for "not
  wanted", a list a `std::vector<T>&`, and bytes in engine
  memory a `std::string_view&`. A call that yields a handle
  hands it back through its last parameter, set only on
  success.

## 5. Exceptions and memory

- **sm never throws.** There is no `throw` in sm's library
  code: a failure is -1 and an error code. Tests may throw.
- **An exception may pass through sm**: one from the C++
  library (`std::system_error` when a thread cannot start),
  or from a callback whose contract allows one. sm1, sm2 and
  sm3 catch nothing; the C wrapper is the one catch-all
  (`DESIGN.md` §5).
- **Release with RAII, not catch and rethrow.** What a frame
  must undo on the way out is held by a guard: a
  `std::lock_guard`, a `std::unique_ptr`, or sm0's `defer` and
  `scope_guard_t` (dismissed on success).
- **Where an exception has no caller to reach** — a thread's
  top, a RocksDB callback, work after a commit point — it ends
  the process through `std::terminate`, unless the code there
  catches it, as the index builder's worker and its enqueue
  do. Beyond those two and the C wrapper, sm has no `catch`.
- **Running out of memory aborts the process.** The
  new-handler `sm1::attach` installs reports
  `*** FATAL ***` and aborts at the failing allocation; WAL
  replay recovers. Do not catch `std::bad_alloc`, and do not
  report a failed allocation as `SM_ERR_NOMEM`.

## 6. Names and headers

- **C-style names take one of two prefixes**: `sm_` for what
  sm's users may call (`include/sm.h`, `sm_sys.h`), `sm0_` for
  what only sm's own levels may use. No other prefix, `sys_`
  included.
- **C++ interfaces are a namespace per level**: `sm0::`,
  `sm1::`, `sm2::`, `sm3::`, each in its `smN.h`. sm1's
  internals are in `namespace wisr`. `sm1.h` brings sm0's error
  and log calls into `namespace sm1`, so the stack calls them
  `sm1::errnum()` and so on.
- **sm0's headers are named `sm0_*.h`**, but for `sm0.h`
  itself, the vendored `doctest.h`, and `sm_sys.h`, which is
  public.

## 7. Threads, waits and logging

- **Every wait can be cut short.** A wait on behalf of a
  caller is an `sm_wait()` with the thread's `sm_wait_mask()`,
  and fails with `SM_ERR_INTERRUPTED` when a bit in the mask
  is pending. sm takes no bit: the host does
  (`sm1/SM1_DESIGN.md`, `sm0/INTERRUPT.md`).
- **Threads are `std::thread`, locks `std::mutex`**, every
  mutex held by an RAII guard, so unwinding releases it.
- **sm writes no log of its own.** It reports through the
  caller's log hook (`sm1::set_log`, `log_msg`), and only what
  no caller would otherwise learn; an error a call returns is
  not also logged. The one exception is the out-of-memory
  report, which goes to stderr as well.
- **The log hook and the index evaluator are `noexcept`**:
  they are called with a library lock held, or from sm's own
  threads, where an exception has nowhere to go.

## 8. Style

- **Follow `CLAUDE.md`**: `snake_case` names, types ending in
  `_t`, `m_` before a class's private members, a doc comment
  of a line or two on every function, a short comment saying
  why on every significant call (a lock, a thread, an
  attach), and `memcpy()` with a written terminator instead
  of `strncpy()`.
- **Match the code around you**, and touch only what the
  change needs. Remove what your change leaves unused; leave
  unused code you did not orphan, and mention it.
- **No RTTI.** No `dynamic_cast`, `typeid`, `std::type_info`,
  `std::type_index`, `std::any` or `std::dynamic_pointer_cast`.
  What an object is, is known where it is made: say it with a
  kind field or an enum, a function pointer, or a virtual call.
  Catching an exception by its type is not this rule's concern.
  Every Makefile compiles with `-fno-rtti`, so the compiler
  refuses the rest.
- **Use `std::optional` reluctantly.**

## 9. Tests

- **Every test opens with a one-line synopsis** of what it
  proves, then a numbered step list, one sentence a step,
  before the code.
- **The isolation scenarios in `sm1/mvcc` are the
  specification** of sm1's concurrency control. A change that
  makes one fail is presumed wrong. Adding a scenario is fine;
  changing or removing one, or its expected output, needs the
  user's approval, with the reason given first. `make accept`
  is never run to make a failure go away.
- **A level's tests link that level and the ones below it**,
  never one above.

## 10. Documents

- **A decision that crosses levels goes in `DESIGN.md`**, one
  inside a level in that level's design document, and what is
  to be built next in `NEXT.md`.
- **Paths in `sm/`'s documents are relative to `sm/`**, and in
  a level's documents relative to that level.
- **Wrap prose at 65 columns** in every `.md` file; code
  blocks, tables and headings are not wrapped.

## 11. Git

- **Commit only when asked, and push only when asked.** Merge
  a small branch with `git merge --squash`, a larger one with a
  plain `git merge`, keeping its commits.
- **Commit messages**: a short subject naming the level, in
  the style `sm1: attach recovers every database` or
  `sm: never throw`, then bullets.
- **Never run `sprite-env checkpoints`.**

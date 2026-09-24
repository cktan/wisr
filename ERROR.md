# ERROR — how the stack reports failure

How a failure travels through sm1, sm2 and sm3's C++
interfaces and out through the top level's C API. It
settles the open question in `OBJECTIVE.md` — "one error type
shared by every level ... still to be chosen" — one decision
at a time, in the order the decisions depend on each other.
A settled answer moves into `OBJECTIVE.md` or `DESIGN.md`;
this document keeps the reasoning.

Fixed before any of it, by `OBJECTIVE.md`: the error taxonomy
is kept — a retryable error means the engine has already
aborted the transaction, and sm1's `UPDATED` means it has
not — and the C wrapper catches everything, because a
PostgreSQL `longjmp` across C++ frames, or a C++ exception
through PostgreSQL's C frames, is undefined behaviour.

---

## Facts

What the code does today, found rather than decided.

- sm1 reports through a thread-local code and message
  (`sm1/src/error.h`): an entry point writes
  `return set_error(...)`, which records both and returns -1.
  The message is formatted into a fixed buffer and allocates
  nothing, so reporting `NOMEM` cannot itself fail.
- sm1 already meets exceptions: the standard library throws,
  and `sm1/src/access.cc` and `sm1/src/catalog.cc` catch
  `std::bad_alloc` at the API edge to turn it into `NOMEM`.
- RocksDB never throws; every call returns a
  `rocksdb::Status`, which sm1 converts with
  `set_status_error`.
- sm1 runs a thread of its own, the GC thread
  (`sm1/src/gc.cc`), and RocksDB calls back into sm1 code —
  the compaction filter (`sm1/src/gc.cc`), and the comparator
  and prefix extractor (`sm1/src/version.cc`).
- Some of sm1's codes are ordinary results, not failures:
  `sm1.h` tells a caller to attempt `sm1_insert` and fall back
  on `DUPKEY`; `UPDATED` is the normal read committed path;
  a read that finds nothing already returns 1, not an error.
- sm1 builds with `-std=c++20`. The compiler here, GCC 13.3,
  supports `std::expected` under `-std=c++23`. Its glibc
  (2.39) unwinds without the global lock older systems took,
  so concurrent throws no longer contend on it.

---

## The decisions, and what hangs off each

```
how failures travel (Q1) — settled, DESIGN.md §5
├── which results are outcomes, and how an int returns
│     them (Q4) — settled, DESIGN.md §5
├── the shape of out-parameters in a C++ signature (Q5)
│     — settled, DESIGN.md §5
├── exceptions from the standard library (Q6)
│     — settled, DESIGN.md §5
├── catch-alls at the foreign boundaries: RocksDB
│     callbacks, the GC thread (Q7) — settled, DESIGN.md §5
├── the accessors' names, once each level has a namespace
│     (Q8)
└── what an error carries: code, message, retryable,
      SQLSTATE (Q11)
one error type for all levels, or one per level (Q2)
│     — settled, DESIGN.md §5: one errno namespace
├── how sm2 and sm3 record an error in sm1's state (Q9)
│     — settled, DESIGN.md §5
├── resetting the state, and naming a code (Q12)
│     — settled, DESIGN.md §5
└── how the code enum is numbered as the levels add codes
      (Q10)
the C wrapper's convention toward PostgreSQL (Q3)
      — settled, DESIGN.md §5
```

Round 1 is the three questions nothing else settles first;
the rest wait for their answers.

---

## Round 1

Q1, Q2 and Q3 are settled and recorded in `DESIGN.md` §5.
Q1, how a failure travels across a level's C++ interface: a
plain `int`, 0 or -1, with `sm1_errno()` and `sm1_errmsg()`.
Q2, one error type or one per level: one errno namespace for
the whole stack, every code defined in `sm1/include/sm1.h`.
Q3, what the C wrapper gives PostgreSQL: the same
convention, with the fork's glue calling `ereport` and
choosing the SQLSTATE; no SQLSTATE lives in the stack. The
numbering is kept so that a question's number never changes.

---

## Round 2

Unblocked by Q1 (`DESIGN.md` §5).

Q4 to Q7 are settled and recorded in `DESIGN.md` §5. Q4, which
results are outcomes: only "no row" is a positive return, and
every other outcome is -1 with its code. Q5, the shape of
out-parameters: references for required outputs, pointers for
optional ones, `std::vector<T>&` for lists, and
`std::string_view&` for bytes in engine memory. Q6, what happens
to exceptions: they cross sm1, sm2 and sm3, and only the top
level's C wrapper catches, aborting the session's transaction.
Q7, the catch-alls at the foreign boundaries: every RocksDB
callback and the GC thread's entry is `noexcept` with a catch-all
that logs; the compaction filter keeps the version on error.

---

## Round 3 — open

Unblocked by Q2 (`DESIGN.md` §5).

Q9 and Q12 are settled and recorded in `DESIGN.md` §5. Q9,
how sm2 and sm3 record an error: `sm1_seterr(code,
std::string_view msg)`, exported by sm1, returning -1. Q12,
resetting the state and naming a code: `sm1_reseterr()` and
`sm1_errname()`, which gives `"ERR_UPDATED"` for
`SM1_ERR_UPDATED`.

### Q8. What are the error accessors named?

`errno` is a macro, so a namespaced `sm1::errno()` cannot be
declared. With one errno namespace, these calls also answer
for sm2's and sm3's errors.

- **(a) Keep `sm1_errno()`, `sm1_errmsg()` and
  `sm1_retryable()`** at global scope in `sm1.h`, outside
  `namespace sm1`.
- **(b) Rename them inside the namespace**, such as
  `sm1::last_error()` and `sm1::last_message()`.

*Recommended: (a).* The names were kept when Q1 was
settled; the state really is sm1's; and a prefixed global
name is the one form the `errno` macro cannot break.

### Q10. How is the code enum numbered as the levels add codes?

*Recommended:* append only. A number is never reused or
renumbered, and a retired code stays with a note. Codes are
grouped by the level that produces them, under a comment
naming it, with no reserved numeric ranges. A number
matters once PostgreSQL sees it through the C wrapper; this
is the rule `sm2/TYPE.md` already applies to type codes.

### Q11. What does an error carry?

*Recommended:*

- A code and a message; nothing else is stored.
- Whether it is retryable is a function of the code, as
  `sm1_retryable()` already computes it.
- The message stays allocation-free, in the fixed
  thread-local buffer, so reporting out of memory cannot
  itself fail.
- No SQLSTATE: Q3 put the mapping from code to SQLSTATE in
  the fork's glue (`DESIGN.md` §5).

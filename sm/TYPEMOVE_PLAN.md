# TYPEMOVE_PLAN — types move into sm

Status: draft, nothing built. It settles the interface of an
operation before any code moves. The item is in
[`NEXT.md`](NEXT.md), item 11.

Every fact below was read from the code on 2026-11-09. Each
number marked "grep" is a count, not an audit.

---

## 1. What this is

A type's operations (add, round, `sqrt`, compare, hash) are in
digres today. sm2 keeps a type's storage only
([`sm2/TYPE.md`](sm2/TYPE.md)). The item moves the operations
into sm, for two reasons:

- sm evaluates expressions without digres (an index expression,
  a partial index, a check).
- sm plugs into another SQL system, not into digres alone.

Moved code cannot call `palloc()`. It cannot call `ereport()`
or read a session's setting either. These three are the
design problem. The arithmetic itself is not.

## 2. Where things stand

- **sm2's type table is already C++ with owned results.** A
  `type_t` has `in`, `out`, `key_encode` and more, and they
  write into a `std::string&` (`sm2/include/sm2.h`). This is
  fine for a storage function. It is not the call digres makes
  for `a + b` in a hot loop.
- **A datum is PostgreSQL's `Datum` layout** (`datum_t`).
  `decimal64` is by value: no memory for a result. `decimal128`
  is by reference, 16 bytes.
- **`arena_t` cannot be backed by `palloc()`** (`sm0/include/
  sm0_arena.h`). It cuts pieces from blocks it takes with
  `new`, it has no mutex, and it frees all at once. An earlier
  line of [`NEXT.md`](NEXT.md) said digres backs an arena with
  `palloc()`. That needs a change to `arena_t`, so §3 treats it
  as a cost, not as given.
- **digres's `decimal.c` is new code, not PostgreSQL's**: 3,575
  lines, 159 functions with `PG_FUNCTION_ARGS`, 15 `palloc`
  calls and 51 `ereport` or `elog` calls (grep). Most `palloc`
  calls are in wrappers: a value copied out, an output string,
  an aggregate's state, a set-returning function's state. The
  arithmetic core seems to need none. This is why decimal is
  the first type to move.
- **The errors are mostly a few kinds** (grep of `ERRCODE_` in
  `decimal.c`): out of range 20, invalid parameter 7, bad text
  5, bad binary 4, power 4, log 2, width bucket 2, division by
  zero 1, others 2.
- **sm's error codes have no kind for these.** `SM_ERR_INVALID`
  is the nearest (`include/sm_error.h`). The last is
  `SM_ERR_MOVED`.

## 3. Decision 1: where a result gets its memory

Three kinds of result, three answers:

| Result | Example | Memory |
| --- | --- | --- |
| By value | `decimal64 + decimal64`, `int4 * int4` | none: it is the return value |
| Fixed width by reference | `decimal128`, `uuid`, `interval` | the caller's storage: an out-parameter |
| Variable width | `text || text`, `replace()`, `repeat()` | to decide |

The first two need no design. Only the third does. Options:

| Option | For | Against |
| --- | --- | --- |
| **A. A buffer and a length.** The call returns the length it needs. | Allocates nothing. | The caller may call twice. The work may run twice, or the call must keep the result. |
| **B. An `arena_t`.** | Exists. Frees at once. | Cannot be backed by `palloc()` as built. A host makes the result and then copies it into its own memory: a second copy of each variable result. |
| **C. An allocator passed to the call**, as `sm_mem_t`: a function and a context. | One indirect call a variable result, and none for the others. digres passes a wrapper of `palloc()`. No copy. Ownership is plain: the caller supplied it. | A new small type in sm0. A caller must pass it on every variable call. |
| **D. A global allocator hook per session.** | The call has no extra argument. | Hides ownership, and a session's setting is state, which the move is to remove. |

**Recommendation: C.** Fixed results use out-parameters. Only a
variable result takes the allocator. sm's own callers pass a
wrapper of `malloc()` or an arena. D is rejected. A is kept for
`text` functions whose size is known before the work.

## 4. Decision 2: how an error is reported

Moved code cannot `ereport()`. digres must still say
PostgreSQL's SQLSTATE and text, so the 231 regression files
pass unchanged.

1. The operation returns a non-zero `int`.
2. The operation sets sm's error state ([`sm0/README.md`](
   sm0/README.md)) with a kind and a short message.
3. The host maps the kind to its own code and composes its own
   text from the operands it still holds.

Options for the kind:

| Option | For | Against |
| --- | --- | --- |
| **A. New `SM_ERR_` codes** (`SM_ERR_RANGE`, `SM_ERR_DIVZERO`). | One mechanism. | The list is for the storage stack; types add many kinds. |
| **B. One `SM_ERR_TYPE` and a second field, `sm_type_err_t`.** | The list stays short. A type adds a kind without a change to `sm_error.h`. | A second field to read and set. |

**Recommendation: B.** The kinds in §2 are the first list.

## 5. Decision 3: the call itself

Rules from [`NEXT.md`](NEXT.md): a plain C call, no handle, no
session lookup, and a statement's instructions do not move.

- The call is a C-style function in a header of its own, not
  in the 2,103 lines of `sm.h`. Proposed name pattern:
  `sm_dec128_add(const sm_dec128_t*, const sm_dec128_t*,
  sm_dec128_t* result)`.
- sm2's `type_t` table stays for storage. The operations are
  not entries in it.
- digres's wrapper keeps `PG_FUNCTION_ARGS`, the fmgr, the
  aggregate state, the set-returning functions, the support
  nodes, the sort support and the typmod rules. It unpacks the
  datums and calls sm.
- **Cost.** A wrapper adds one call from digres into `libsm.a`.
  Not measured. The measure is in §7, step 1.

## 6. Decision 4: what moves, by type

| Order | Type | Moves | Stays in digres |
| --- | --- | --- | --- |
| 1 | decimal | arithmetic, rounding, compare, hash, `sqrt`, `ln`, `exp`, `log`, power, the 512-bit magnitude | wrappers, aggregates, series, planner support, typmod, error text |
| 2 | integers, bool, uuid, bytea | the same shape: pure functions | wrappers, error text |
| 3 | date, time, interval, float text, text | hold: they read `DateStyle`, `TimeZone`, `extra_float_digits` and collation | until the hooks are settled |

Step 3 is a separate note. Its hooks are the hard part. Code
copied from PostgreSQL keeps its copyright notice
([`../CLAUDE.md`](../CLAUDE.md)).

## 7. Steps

1. **Baseline.** Record instructions a statement for three
   decimal queries with `perf stat`, as
   [`../ISSUE.md`](../ISSUE.md) §2.1 does, and run the
   numeric regression files. → verify: numbers saved, files
   pass.
2. **One function end to end.** Move `decimal64` add with
   decision 1's by-value result and decision 2's error. →
   verify: the regression files pass unchanged, and the
   instruction count is within the limit of the open question
   below.
3. **The rest of decimal's core**, in groups: arithmetic, then
   rounding, then functions of the exponent. → verify: the
   same, after each group.
4. **Tests at every layer** the change reaches: sm2 unit tests
   for each moved function (new), `rel` if an expression index
   uses one, and pgregress for digres. The mvcc scenarios do
   not apply: nothing here touches concurrency.

## 8. Open questions

1. How large a rise in a statement's instructions is the limit?
   This note proposes 1%.
2. Decision 1: option C, or A for the first types?
3. Decision 2: option A or B?
4. The header name and place for the operations.
5. May `arena_t` take an allocator, so that option B works too?
   If not, B is dropped.

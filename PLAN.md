# PLAN — `sm_strlist_t`

A counted, immutable list of names returned by the library
and released by the caller with one call.

## 1. Starting state (verified)

`API.md` as it stands has exactly one string list:

```
sm_lsdb(sm_env_t* env, char*** out, size_t* n)
sm_free_dbs(char** list, size_t n)
```

`sm_list_prepared` returns `sm_prepared_t*`, a struct with a
`gid` and a timestamp — not a string list, and out of scope
here. Nothing else in the API returns a vector of anything.

So today the construct has **one** user. The premise of this
plan is that it will have more; the prospective ones are
`sm_lsrel` (the Rel API has `sm_mkrel` and `sm_rmrel` but no
listing, and `mkdb/rmdb/lsdb` now sets the naming) and any
later call that enumerates names.

This matters because an abstraction over one call site is
normally premature. Two things make it not:

- The current shape has a real defect. `sm_free_dbs(list, n)`
  fails silently if `n` is not the `n` that came back, and its
  contract — does it free the strings, or only the array? — is
  invisible in the signature.
- The replacement is smaller than what it replaces: three
  functions with obvious contracts, against a `char***`, a
  paired `size_t*`, and a bespoke deallocator per list type.

## 2. The type

```c
typedef struct sm_strlist_t sm_strlist_t;

size_t      sm_strlist_count(const sm_strlist_t* sl);
const char* sm_strlist_at(const sm_strlist_t* sl, size_t i);
void        sm_strlist_free(sm_strlist_t* sl);
```

Producers return it through a single out-param:

```c
int sm_lsdb(sm_env_t* env, sm_strlist_t** out);
```

### Semantics

- On success `*out` is **never NULL**. An empty result is a
  valid handle with `count == 0`. This removes the "`*n` may
  be 0, in which case `*out` is NULL" special case the
  current `sm_lsdb` carries.
- `sm_strlist_at(sl, i)` returns a NUL-terminated string owned
  by the list, valid until `sm_strlist_free`. It returns NULL
  for `i >= count`, so a caller that loses track of the count
  gets NULL rather than a wild read.
- The list is **immutable**. There is no public append, and
  the returned `const char*` must not be modified or freed
  individually.
- `sm_strlist_free(NULL)` is a no-op.
- A list is **caller-owned and holds no engine state**: no
  lock, no snapshot, no session reference. Forgetting to free
  it leaks memory and nothing else. This is the opposite of
  `sm_scan_t`, where a missed `sm_scan_end` pins
  `gc_watermark` (`DESIGN.md` §8.3) — the distinction belongs
  in the docs so callers can tell the two apart.
- Immutable and self-contained, so it may be read from any
  thread; freed once, by one of them.

## 3. Representation

One allocation, opaque to the caller:

```
+-----------------------------+
| size_t count                |
| char*  ptr[count]  ---------+--> into the tail
| char   bytes[]: "a\0" "bb\0"|
+-----------------------------+
```

One `malloc`, one `free`, and the strings sit next to the
pointers that name them. The public type is opaque precisely
so this can change without an API break.

Construction is internal only — a small append-and-finish
builder in `src/strlist.cc`, not exported in `sm.h`. Callers
never build a list; they only read one.

## 4. Decisions and the alternatives rejected

| Decision | Alternative | Why |
| --- | --- | --- |
| Opaque handle | transparent `struct { char** v; size_t n; }` | Matches `sm_env_t`, `sm_session_t`, `sm_scan_t`; frees the layout; removes the "may I free a member?" question |
| Count carried inside | separate `size_t* n` | `n` and the array cannot disagree; one fewer out-param |
| Empty = handle, count 0 | empty = NULL | No special case at any call site |
| NUL-terminated elements | `(ptr, len)` pairs | Names cannot contain NUL; see §5 |
| Immutable, no builder exported | public append | Nothing asks for it |

## 5. The boundary: names only, never keys or values

Elements are NUL-terminated, which is lossless for every name
in the API — a `dbname` is "any byte but NUL" and a `relname`
is a NUL-terminated string. It is **not** lossless for keys
and values, which `API.md` defines as arbitrary byte strings
that may contain NUL.

So `sm_strlist_t` is for names. A future call that returns
keys needs a counted-buffer type, not this one. Stating that
now keeps someone from reaching for it later and silently
truncating a key at its first NUL byte.

## 6. Documentation changes

- `API.md`, Conventions: a short **String lists** subsection
  next to *Memory and lifetimes*, holding the ownership and
  lifetime rules above, so each producer can be one line.
- `API.md`: `sm_lsdb` re-signed; `sm_free_dbs` deleted.
- `API.md`, Divergences: entry 15 mentions `sm_lsdb` /
  `sm_free_dbs` and needs the new spelling; add entry 16 for
  the type itself.
- `DESIGN.md` §12.3: add `src/strlist.cc`.

No change to §1–§11 of `DESIGN.md`. This is a boundary
change; nothing below it moves.

## 7. Open questions

1. **`sm_strlist_free` or `sm_free_strlist`?** The first
   groups with its type, which is why it is written that way
   above; the second matches the existing `sm_free_prepared`.
   Picking the first leaves the file with two conventions
   until `sm_list_prepared` is reshaped.
2. **Should `sm_lsdb` sort?** It currently promises no order.
   Bytewise-sorted output costs one sort over a small list and
   makes tests deterministic. Recommend sorting.
3. **Add `sm_lsrel` in the same pass?** It is the second user
   that makes this abstraction earn its keep, and the Rel API
   is missing it anyway. It is also scope beyond the request,
   so it is a question, not a step below.
4. **Leave `sm_list_prepared` alone?** It returns structs, so
   it is not a string list. Recommend leaving it, accepting
   that `sm_free_prepared` stays as the one bespoke
   deallocator.

## 8. Execution

1. Add the type and its three accessors to `include/sm.h`,
   plus the internal builder in `src/strlist.cc` →
   verify: compiles; `make format` clean.
2. Unit-test the type in isolation → verify: empty list has
   count 0 and a non-NULL handle; `at(count)` is NULL; a list
   of names with embedded spaces and high bytes round-trips;
   free-after-read is clean under ASan.
3. Re-sign `sm_lsdb`, delete `sm_free_dbs` → verify: the
   database round-trip test lists what it created, in sorted
   order, and frees with one call.
4. Update `API.md` and `DESIGN.md` §12.3 per §6 above →
   verify: no reference to `sm_free_dbs` or `char***` remains
   anywhere in the two documents.
5. Full test suite → verify: green before committing.

Steps 1–3 have no implementation to attach to yet: there is
no `src/` in the repo. Until there is, step 4 is the whole of
the work, and steps 1–3 are the contract it commits to.

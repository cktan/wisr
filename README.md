# wisr

Three parts, one repository.

| Where | What it is |
| --- | --- |
| `sm/` | the storage stack: an MVCC storage manager on RocksDB, types and tuples over it, and schema and index maintenance over those, with one API, `sm/include/sm.h` |
| `dg/` | digres: a fork of PostgreSQL that runs on the stack. It compiles as C++, is one process with a thread a session, and serves PostgreSQL's own `psql`, its tables sm's; `dg/README.md` says what runs, and `sm/PART4.md` §10 how it is being made |
| `sq/` | the SQL parser: PostgreSQL 18's SQL in, a tree of nodes out, as a library with one API, `sq/include/sq.h`. It is built from nothing outside `sq/` |

Start with `sm/README.md`.

## Building

```
make -j4                build everything: sm, sq and digres
make -j4 test           test sm, then sq, then digres
make -j4 format         clang-format sm and sq
make -C sm -j4          build every level of the stack
make -C sm -j4 test     build and test every level
make -C dg -j4          build digres, dg/build/digres
make -C sm/sm0 -j4 test build level 0, with the system services, and test it
make -C sq -j4 test     build the SQL parser and test it
```

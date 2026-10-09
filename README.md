# wisr

Two parts, one repository.

| Where | What it is |
| --- | --- |
| `sm/` | the storage stack: an MVCC storage manager on RocksDB, types and tuples over it, and schema and index maintenance over those, with one API, `sm/include/sm.h` |
| `dg/` | digres: a fork of PostgreSQL that runs on the stack. It compiles as C++ and links, and cannot run a database yet; `sm/PART4.md` §10 says how it is to be made |
| `sq/` | the SQL parser: PostgreSQL 18's SQL in, a tree of nodes out, as a library with one API, `sq/include/sq.h`. It is built from nothing outside `sq/` |

Start with `sm/README.md`.

## Building

```
make -C sm -j4          build every level of the stack
make -C sm -j4 test     build and test every level
make -C dg -j4          build digres, dg/build/postgres
make -C sq -j4 test     build the SQL parser and test it
```

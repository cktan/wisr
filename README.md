# wisr

Two parts, one repository.

| Where | What it is |
| --- | --- |
| `sm/` | the storage stack: an MVCC storage manager on RocksDB, types and tuples over it, and schema and index maintenance over those, with one API, `sm/include/sm.h` |
| `dg/` | digres: a fork of PostgreSQL that runs on the stack. Not started; `sm/PART4.md` §10 says how it is to be made |

Start with `sm/README.md`.

## Building

```
make -C sm -j4          build every level of the stack
make -C sm -j4 test     build and test every level
```

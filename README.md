# wisr

Wisconsin storage on rocks: a PostgreSQL whose tables live in a
storage stack of its own, built on RocksDB. Four parts, one
repository.

| Where | What it is |
| --- | --- |
| `sm/` | the storage stack: an MVCC storage manager on RocksDB, types and tuples over it, and schema and index maintenance over those, with one API, `sm/include/sm.h` |
| `dg/` | digres: a fork of PostgreSQL 18.6 that runs on the stack. It compiles as C++, is one process with a thread a session, and serves PostgreSQL's own `psql`, its tables sm's; `dg/README.md` says what runs, and `sm/PART4.md` §10 how it is being made |
| `sq/` | the SQL parser: PostgreSQL 18's SQL in, a tree of nodes out, as a library with one API, `sq/include/sq.h`. It is built from nothing outside `sq/` |
| `aa/` | roles, authentication and authorization, a library on sm that digres links (`aa/AA_DESIGN.md`, `aa/USAGE.md`) |
| `bench/` | digres against PostgreSQL 18, query by query (`bench/README.md`) |

## How the parts fit

```
 psql, pgbench, any PostgreSQL client
        │  PostgreSQL's wire protocol
        ▼
 dg/  digres: one process, a thread a session
        parse ──────────────► sq/  the SQL parser
        plan, execute            (PostgreSQL 18's planner and
                                  executor, unchanged)
        roles, privileges ──► aa/  on sm
        tables, indexes ────► sm/include/sm.h, from
        │                     dg/src/backend/access/sm/
        ▼
 sm/  the storage stack, each level on the one below
        sm3  schema and index maintenance: catalog, index
             builds, scans, statistics
        sm2  types and tuples: key encoding, tuple format, row
             and index relations
        sm1  MVCC: stores, transactions, isolation, locks, 2PC,
             recovery, garbage collection
        RocksDB  bytes: ordered, durable, crash-atomic
     sm0  errors, helpers and the system services (sm_sys.h)
          that sm and dg both build on; depends on nothing
```

- **digres keeps PostgreSQL's planner and executor** and
  replaces what is under them: a table is an sm store (one
  RocksDB column family), a row an sm2 tuple, a scan an sm
  cursor. Its catalog is sm's; PostgreSQL's catalog tables are
  made from it on demand (`dg/SYSCATALOG.md`).
- **A database is its directory**, `base/X`, its rows and WAL
  in one RocksDB instance there (`dg/CODE.md`).
- **Each level owns its notions**: row versions, transactions
  and visibility are sm1's; columns, types and index entries
  sm2's; tables and index definitions sm3's. Each has a design
  document: `sm/sm1/SM1_DESIGN.md`, `sm/sm2/SM2_DESIGN.md`,
  `sm/sm3/SM3_DESIGN.md`, and `sm/sm0/README.md`.
- **`sm/rel/`** drives `sm.h` from scripts, the stack's tests
  without SQL.

Start with `sm/README.md`. Before changing code, read
`CLAUDE.md`, then `sm/CODE.md` for `sm/` and `aa/`, or
`dg/CODE.md` for `dg/`.

## Prerequisites

Developed on Ubuntu 24.04, aarch64; the build must also work
on x86_64. On Ubuntu:

```
sudo apt install build-essential cmake git flex bison python3 \
    clang-format librocksdb-dev libicu-dev \
    libsnappy-dev liblz4-dev libzstd-dev zlib1g-dev libbz2-dev \
    postgresql-client linux-tools-generic
```

- **RocksDB 8.9.1**: the system's (`librocksdb-dev` on 24.04)
  for the debug build; the release and TSan builds build their
  own from source (below).
- **`psql` and `pgbench`**, from `postgresql-client`: digres
  serves PostgreSQL's own clients. PostgreSQL 18's `psql`,
  built below, is the one to use by hand (§Connect).
- **PostgreSQL 18 from source**, for the tests that compare
  with it and for the benchmark:
  - `~/p/postgres18`, a clone of PostgreSQL on REL_18_STABLE,
    built and installed into its own `install/`
    (`./configure --prefix=$HOME/p/postgres18/install && make
    && make install`, and `make -C src/test/isolation`):
    pgregress, the isolation specs and the regression suite
    run its `psql` and `pg_regress`. Elsewhere, set
    `PG18_SRC`. Without it those tests say SKIPPED.
  - `bench/build/pg18`, PostgreSQL 18.6 itself (commit
    `724edf9`) built optimized with no `--enable-cassert` or
    `--enable-debug`, for the benchmark
    (`bench/DESIGN.md` §2).

## Building

Every build is `make -j4`, never plain `make`. digres builds
sq, sm and aa first.

| Build | Command | What it makes |
| --- | --- | --- |
| debug | `DEBUG=1 make -C dg -j4` | `dg/build/digres`: digres `-O0 -g` with `Assert()`, sm `-O0 -g` with `assert()`, the system's RocksDB. What the tests run |
| release | `make -C dg release -j4` | `dg/build/digres-release`: digres `-O2 -DNDEBUG`, sm in `sm/build/release` at `-O3 -DNDEBUG`, sq's `libsq-release.a`, and RocksDB 8.9.1 built `-O3 -DNDEBUG` and linked statically. What the benchmark runs |
| TSan | `make -C dg tsan -j4` | `dg/build/digres-tsan` under the thread sanitizer, sm and RocksDB instrumented with it, then the threaded tests and `server.sh` on it |
| one level | `DEBUG=1 make -C sm/sm1 -j4` | a level of the stack alone; without `DEBUG=1`, `-O3 -DNDEBUG` |

- **The release and TSan builds build RocksDB themselves** the
  first time: `dg/test/rocksdb.sh` clones v8.9.1 into
  `dg/build/rocksdb-src` and builds it with cmake, which takes
  a while. Ubuntu's `librocksdb` keeps its `assert()`s, and
  the sanitizer cannot see into it.
- **`digres --build` says how a binary was built**: the flags
  of digres, sq and sm, and which RocksDB it links. A release
  build shows `-DNDEBUG` on each, and RocksDB `static`.
- **Flags are not tracked**: after changing build flags,
  delete the `*.rel.o` objects under `dg/src` and
  `sq/build/rel` before building the release again.
- **`make -j4 format`** in `sm/` (and `sq/`, `aa/`) before a
  commit. dg is formatted by the lines a change touches
  (`dg/CODE.md` §8).

## Running digres

### Make a data directory

```
dg/build/digres --init /path/to/data
```

`digres --init` is digres's `initdb` (`--init --help` lists
its options). It makes the database `postgres`, and
`pg_global`, which holds the roles and which no client
connects to, with one superuser:

- **`-U NAME`**: the superuser, by default your OS user name.
- **`-W`** (prompt) or **`--pwfile=FILE`**: its password.
  Without one, only local socket connections work.
- The encoding is UTF8 and the collation C. Authentication is
  not configurable: a local socket connection is admitted
  when the OS user is the role (peer), a TCP connection by the
  role's password (SCRAM-SHA-256). There is no `pg_hba.conf`.

### Start the server

```
dg/build/digres -D /path/to/data -p 5432 -k /tmp
```

- It runs in the foreground and logs to stderr; Ctrl-C, or
  `kill -INT $(head -1 /path/to/data/postmaster.pid)`, shuts
  it down.
- **`-p`**: the TCP port, 5432 by default. **`-k`**: the
  socket's directory, `/tmp` by default; keep its path short,
  as a socket's may not pass 107 bytes. It listens on
  `localhost`.
- **Settings**: `-c NAME=VALUE`, or lines in
  `/path/to/data/postgresql.conf`, which `--init` does not
  write. `shared_buffers` (128MB by default) is the size of
  RocksDB's block cache: the benchmark uses 2GB.
- The release build, `dg/build/digres-release`, runs the same
  way.

### Connect

```
psql -h /tmp -p 5432 -d postgres                 # the socket, as your OS user
psql -h localhost -p 5432 -U NAME -d postgres    # TCP, with the password
```

- **Use PostgreSQL 18's `psql`** (`~/p/postgres18/install/bin`,
  above). Ubuntu 24.04's `psql` 16 connects and runs SQL, and
  `\d`, `\dt` and `\di` work, but `\l` fails, as it does
  against PostgreSQL 18 itself: it reads
  `pg_database.daticulocale`, which PostgreSQL 17 renamed
  `datlocale`. Any PostgreSQL driver connects.
- `CREATE DATABASE app` makes another database; each is a
  directory, `base/app`, of its own.
- What digres does not run yet fails with an error, not a
  wrong answer (`dg/UNSUPPORTED.md`).

## Testing

Tests run on the debug build. The full suites, in this order,
each saved to a log of its own:

```
make -C sm -j4                        # first: aa's tests link sm
DEBUG=1 make -C sm -j4 test > sm.log 2>&1 &
make -C sq -j4 test > sq.log 2>&1 &
make -C aa -j4 test > aa.log 2>&1 &
wait
DEBUG=1 make -C dg -j4 test > dg.log 2>&1
```

sm's, sq's and aa's unit tests are independent and run at
once; dg's last. Every commit that touches source passes all
four. dg's `test` passes with these lines:

```
run: PASSED (124 files)
run: PASSED (11 files, alone and in 8 sessions)
initdb: PASSED
server: PASSED
slt: PASSED (49 files on digres, 10711 records, 11 skipped)
all 231 ok
```

What each is:

- **sm**: each level's unit tests (`sm/smN/test`), the
  isolation scenarios of sm1's concurrency control
  (`sm/sm1/mvcc`, its specification: change one only with
  the maintainer's approval), and `rel` (`sm/rel/test`), which
  drives `sm.h` from scripts.
- **sq**: the parser's tests, unit tests and the SQL files of
  `sq/test`; `make -C sq corpus-test` checks it over
  PostgreSQL's regression SQL.
- **aa**: its unit tests.
- **dg**: the `.sql` files of `dg/test`, alone and in eight
  sessions; `initdb`; `server.sh`, a server driven by `psql`;
  the sqllogictest files of `dg/test/slt`, whose results are
  PostgreSQL 18's (`dg/test/slt/README.md`); and pgregress,
  PostgreSQL 18's regression files with what digres does not
  run commented out, against PostgreSQL's output
  (`dg/test/pgregress/README.md`).

Run on their own, outside `make test`:

| Command | What it runs |
| --- | --- |
| `make -C dg -j4 slt` | every sqllogictest file, `select5.test` too |
| `dg/test/slt.sh FILE...` | some of them; `--pg18` runs them on PostgreSQL 18 instead |
| `make -C dg -j4 pgregress` | the ported regression files alone |
| `make -C dg -j4 isolation` | PostgreSQL 18's isolation specs, against `dg/test/isolation_specs.txt`; for any change to locks or waits; ten minutes |
| `make -C dg -j4 regress` | PostgreSQL 18's whole regression suite, unported: only measures what is missing |
| `make -C dg -j4 tsan` | the TSan build and its tests; for any change to threads, latches, locks or shared state |

A test is written with a one-line synopsis and numbered steps,
and is shown to fail by breaking what it checks before it is
committed (`CLAUDE.md` §5).

## Benchmarks

`bench/` measures digres against PostgreSQL 18, on optimized
builds only (`bench/CODE.md`): a debug build's number measures
the build.

```
make -C dg release -j4         # dg/build/digres-release
bench/run.sh                   # both systems, and a report
bench/run.sh dg                # one system, for a look; no report
```

- `run.sh` loads each system in turn into
  `bench/build/run`, checks each query's plan, times Q1 to Q6
  (sequential and index scans) and runs pgbench for Q7, Q8,
  Q10 and Q11 (reads by key, by a secondary index, and an
  update), then writes `bench/results/<date>.md` and a line in
  `bench/results/LOG.md`. `RUNS`, `WARM` and `Q7_SECONDS`
  change how long.
- It refuses a debug build of either side: a server whose
  `debug_assertions` is on, or a digres whose `--build` is not
  a release's.
- The queries are in `bench/sql/` (`queries.txt`, `q7.sql`
  and the others), the method in `bench/DESIGN.md`.
- Judge digres against PostgreSQL, never against an older
  digres.
- **On a machine whose clock varies** (a VM, a laptop),
  latency swings run to run with the same work. Compare
  instructions a statement instead: pin the server
  (`taskset -c 0-3`) and pgbench (`taskset -c 4`), and count
  the server's with `perf stat -e instructions -p PID` over a
  fixed `pgbench -t N`.
- `bench/smbench/`, `bench/getseek/` and `bench/relq6/`
  measure layers below SQL (`bench/README.md`).

## Where things are

| Document | What it holds |
| --- | --- |
| `sm/README.md` | the stack's levels, and each one's design document |
| `sm/NEXT.md` | what is to be built next, in order |
| `dg/README.md`, `dg/PLAN.md` | what digres is, and its decisions of record |
| `dg/WORKNOTE.md` | every step taken in digres, and why |
| `sm/BTREE.md` and the other notes in `sm/` | designs of a feature each, built or proposed |

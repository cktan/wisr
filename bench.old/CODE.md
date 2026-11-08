# Working in bench

Rules for changing and running the benchmark.
[`README.md`](README.md)
says what it is for, [`DESIGN.md`](DESIGN.md) how it measures.

## Builds: optimized, never debug

AI agents, and anyone else running the benchmark, must make
sure that every program measured is compiled optimized and
without debug checks: PostgreSQL 18, digres, and everything
digres links, the RocksDB library above all. A number from a
debug build measures the build, not the system, and is not to
be reported.

- **PostgreSQL**: `bench/build/pg18`, built from 18.6
  (`724edf9`) with `./configure` and no `--enable-cassert` or
  `--enable-debug`: `-O2`, `USE_ASSERT_CHECKING` undefined.
  `SHOW debug_assertions` on the server says `off`.
- **digres**: `dg/build/digres-release`, from `make -C dg
  release` ([`DESIGN.md`](DESIGN.md#2-optimized-builds) §2),
  never `dg/build/digres`, which is
  the tests' `-O0 -g` build with `Assert()` on. The release
  build compiles every object `-O2 -DNDEBUG` without
  `USE_ASSERT_CHECKING`, and links:
  - **RocksDB** built by `dg/test/rocksdb.sh release`, 8.9.1
    at `-O3 -DNDEBUG`, statically: not the system's
    `librocksdb.so`, which Ubuntu builds with its `assert()`s
    on. `ldd dg/build/digres-release` names no `librocksdb`.
  - **sm** built into `sm/build/release`, `-O3 -DNDEBUG`.
  - **sq** as `sq/build/libsq-release.a`, `-O2 -DNDEBUG`.
  - **aa** built into `aa/build/release`, `-O3 -DNDEBUG`,
    which `digres --build` does not show.
- **Flags are not tracked**: digres's make does not rebuild
  an object when its flags change, so after a change to them
  delete the `*.rel.o` objects under `dg/src` and
  `sq/build/rel` itself before `make -C dg release`.
- **`digres --build` says how a digres was built**: the
  flags of its objects, of sq and of sm, and the RocksDB it
  links. `digres-release --build` must show `-DNDEBUG` on
  each, and RocksDB `static`.
- **`run.sh` checks**: it stops if a server says
  `debug_assertions` is on, or if `digres --build` is not a
  release build's; and copies `--build` into its report.
- **Each report says so**: the builds and their flags are in
  its header ([`DESIGN.md`](DESIGN.md#7-a-report) §7), and a run
  on any other build is
  not a result.

## Reports: logged, with date and commit

- **Every report is logged**: [`results/LOG.md`](results/LOG.md)
  has one row
  per report, oldest first: the date and time of the run,
  the digres commit it measured, the machine, and a link to
  the report. `run.sh` appends the row when it writes the
  report; a report written any other way gets its row by
  hand.
- **A report names its commit and date**: its file name is
  the date (`YYYYMMDD-HHMM.md`), and its header the commit
  ([`DESIGN.md`](DESIGN.md#7-a-report) §7). Commit the report and
  its log row
  together.

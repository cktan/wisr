# SOAK: a busy digres server for 4 hours

This plan runs a school web app against digres for 4 hours.
It shows whether digres leaks memory or gets slower with time.
The scripts are `soak.sh`, `soak/schema.sql`, the transaction
scripts in `soak/`, and `soak/report.py`.

## 1. Objective and pass criteria

The run passes when all of these hold. Warm-up (15 minutes) is
not scored.

- RSS of the postmaster and of each backend is flat. The slope
  over the scored part is under 1 MB an hour.
- File descriptor count and thread count are flat.
- The data directory size reaches a steady state, as the
  insert rate and the delete rate are equal.
- Tps, p50 latency and p99 latency in the last 10% of the run are
  within 5% of the first 10% of the scored part.
- The end-state checks in section 7 pass.

PostgreSQL 18 runs the same workload. A flat curve there shows
that a drift in digres is real, not caused by the harness.

## 2. Workload

The school is fictitious. All names come from a fixed seed.

| table | rows | notes |
| --- | --: | --- |
| prof | 1000 | id, name, department |
| student | 20000 | id, name, year |
| class | about 2500 | prof_id, term, slot, capacity |
| enrollment | about 100000 | student_id, class_id, grade |

- Primary key of `enrollment` is (student_id, class_id).
- Secondary indexes, both `USING btree` (digres makes lsm ones
  by default): `enrollment(class_id, student_id)` and
  `class(prof_id)`.
- A drop-class deletes the enrollment row.

Transaction mix, by count:

| share | transaction | kind |
| --: | --- | --- |
| 80% | student schedule (join of enrollment and class) | read |
| | prof schedule, class roster, a student's grades | read |
| 10% | set the grade of a student in a class | update |
| 5% | drop a class: delete one enrollment | delete |
| 5% | add a class: check capacity, insert enrollment | insert |

- The update share (15%) is the grade update (10%) plus the
  drop (5%).
- Delete and insert are equal, so the table size stays steady.
- The roster read and the grade update pick a class with a Zipf
  skew: some classes are hot. Students and profs are uniform.
- The add picks a class uniformly. With hot classes the adds
  would fail on full classes, the deletes would not, and the
  table would shrink without end.
- The add runs `SERIALIZABLE`, so two students cannot take the
  last seat. The other transactions run at the default level.
- The drop deletes with `class_id IN (subquery)`. The form
  `class_id = (subquery)` hits a digres bug: an empty subquery
  gives a NULL key, and digres scans the whole index.
- A serialization failure is retried (`--max-tries=10`) and
  counted, not an error.

## 3. Roles

The clients do not connect as the superuser.

- `student_app`: SELECT on `class`, `enrollment`, `student`;
  INSERT and DELETE on `enrollment`.
- `prof_app`: SELECT on all tables; UPDATE on `enrollment`.
- Each script runs as the role that the real web app would use.
- The reconnecting clients log in on every transaction. This
  exercises login and the privilege checks on each connection.

## 4. Clients

Use `pgbench` with custom scripts and weights (`@80`, `@10`,
`@5`, `@5`), not `psql` in a loop. It picks random ids, retries
failures, and reports latency by interval.

| group | clients | mode |
| --- | --: | --- |
| reconnecting | 5 | `-C`: connect, run one transaction, disconnect |
| persistent | 4 | one connection for the whole run |

- The reconnecting group finds leaks at connection teardown.
- The persistent group finds leaks in a long session: plan
  caches, snapshots, open iterators.
- Each group has one role: three student clients and two prof
  clients reconnect; three student and one prof client stay.
- Both groups run at the same time, for 4 hours after the
  warm-up (`DURATION=15300` in `soak.sh`).
- The query count is a floor, not the limit: the run must also
  pass 100k queries, and it will, by a wide margin.

## 5. Steps

1. Build the optimized server: `make -C dg release -j4`. Check
   that `digres-release --build` shows `-NDEBUG`.
2. Make a data directory under `bench/build/soak`, with `TMPDIR`
   set to a directory there. Keep the socket in `/tmp`.
3. Start digres with `shared_buffers = 2GB`, in the same way as
   `run.sh`.
4. Create the schema, the roles and the grants.
5. Load the rows from the fixed seed, then analyze the tables.
6. Run the sampler (section 6) and take a first sample.
7. Run the warm-up for 15 minutes with both client groups.
8. Run the scored part for 4 hours with both client groups and
   `--progress=60 -r`.
9. Stop the clients. Take a last sample.
10. Run the end-state checks (section 7).
11. Stop the server with SIGINT, start it again, and check that
    the data is intact.
12. Do steps 2 to 11 for PostgreSQL 18, with the settings of
    `run.sh`, for the comparison. It runs 2 hours after the
    warm-up, not 4: it is the reference curve.
13. Write the report (section 8).

## 6. Sampler

A script writes one CSV row each minute to `bench/build/soak`.

- For the postmaster and each backend, from `/proc`: RSS, PSS,
  thread count and open file descriptor count.
- Size of the data directory.
- The number of files in the data directory. RocksDB's own
  statistics are not visible from outside the server.
- The client side: the `pgbench` progress line (tps, latency,
  failures, retries).

## 7. End-state checks

- No class has more enrollments than its capacity.
- No enrollment row names a student or a class that is absent.
- The row count of `enrollment` is the same by a table scan, by
  the primary key and by the secondary index.
- Restart: the server starts again with no error in its log,
  and the counts above are the same.

## 8. Report

Put the results in `bench/results-gcp/`, with the same header as
the other runs (machine, commit, `digres --build`).

- Plots against time: RSS, tps, p99 latency, data directory size.
- Each pass criterion of section 1, with pass or fail.
- The PostgreSQL curves beside the digres ones.

## 9. Leak hunt

The release run says how much memory is lost. It does not say
where. The repository has a thread-sanitizer build only
(`dg/build/digres-tsan`), and no ASan or LSan build.

When RSS drifts, choose one of these:

1. Add a `make` target for an ASan build with LSan. Run the same
   workload for 1 hour. Stop the server with SIGINT: LSan prints
   the leaks at exit.
2. Run the server under `valgrind --leak-check=full` for a
   shorter run.

Both are far slower than the release build: do not use them for
the latency criteria.

## 10. Risks

- `pgbench -C` measures connection cost more than query cost.
  Judge latency from the persistent group.
- Four cores run nine clients and the server. Pin the server to
  one CPU and the clients to the others, as `privcheck.sh` does,
  so the clients do not take the server's CPU.
- RSS grows at first as caches fill. This is why the first 15
  minutes are not scored.

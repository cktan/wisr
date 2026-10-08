# Work notes: decisions taken without asking

Written while working through `DGWORK.md` unattended. Each entry
is a guess made in place of a question; any of them can be
reversed. Newest step last.

## Step 1: the client

- **Deleted, not carved out**: `src/interfaces/libpq`,
  `src/bin/psql`, `src/fe_utils` and `src/include/fe_utils`.
  Stock PostgreSQL 18 `psql` is the client.
- **`zic` is deleted, and the time zone data with it.** `zic.c`
  is a client program, and the only reason left to compile
  anything twice. `pg_config.h` now sets `SYSTEMTZDIR` to
  `/usr/share/zoneinfo`, so the server reads the system's time
  zone database. Guess: a Linux-only server can rely on the
  `tzdata` package.
- **`dg.mk` lost `BOTH` and `FRONTEND`**: every file is compiled
  once, for the server.
- **Left in place**: the `#ifdef FRONTEND` branches inside
  `src/common` and `src/port`, and the headers only they use,
  such as `postgres_fe.h` and `common/logging.h`. They are dead
  code now, and removing them is a separate pass over 40 files.
- **Fixed on the way**: `make clean` removed `*.d`, which took
  `src/backend/utils/probes.d`, a source file, with it. It now
  removes only the objects' own `.d` files.

## Step 2: getting the server to link

`make -j4` now links `build/postgres`, with no undefined symbol.
It prints its version and nothing more useful: there is no
bootstrap, so no catalog and no database to start. That is the
state `../sm/PART4.md` §10 expects before the lookup layer.

How it was done: the headers of each subsystem that was left out
of the copy were deleted, and then every use the compiler
reported was removed. Nothing was stubbed, with the one exception
of the hash access method below.

**Built-ins**

- `src/tools/cut_builtins.py` is new. It removes `pg_proc` rows by
  C function name and then every row `genbki.pl` reports as
  dangling. About 1,700 rows went.
- **The type hash functions are back**, as
  `src/backend/utils/adt/hashfunc.c`. They were in
  `access/hash`, which was left out as an index method, but hash
  joins and hash aggregates need them.
- **There is a hash access method in name only**, `hashhandler` in
  the same file. A hash operator class has to belong to an access
  method, and the planner asks that method what a strategy number
  means. It answers that and has no callbacks. Guess: this is
  better than losing every hash operator class. Open: `CREATE
  INDEX ... USING hash` would call a null callback; nothing
  refuses it yet.
- **gist, gin, spgist and brin are gone from the catalog data**,
  with their operator classes, and so are their support functions
  in `utils/adt`, such as `rangetypes_gist.c`.

**Text search**

- **Removed, types included.** The copy had left out `tsearch`
  and `snowball` but kept `tsvector` and `tsquery` in `utils/adt`,
  which could not link without the parser. All of it is deleted,
  with `commands/tsearchcmds.c` and `utils/cache/ts_cache.c`.
- Text search DDL parses, then reports that it is not supported.
- **Left in place**: the `pg_ts_*` catalogs, the `regconfig` and
  `regdictionary` types, and the code in `catalog/` that knows
  them. It goes when the catalog does.

**Replication**

- Everything under `src/include/replication` is deleted, and
  every caller: WAL senders and receivers, slots, origins, logical
  decoding, the apply workers, synchronous replication.
- **A replication connection is refused** at the startup packet,
  with a FATAL saying it is not supported.
- **Standby mode has no streaming.** `xlogrecovery.c` kept its
  state machine; the "stream" state now fails at once, so a
  standby polls the archive and `pg_wal` only. Guess: the least
  rewriting of code that the storage stack replaces anyway.
- **`synchronous_commit` stays** as a setting, without the hook
  that belonged to synchronous replication.
- **Publications are left in place and inert.** They link, and
  nothing reads them. `CheckCmdReplicaIdentity` is gone, so an
  UPDATE no longer checks a published table's replica identity.
- Subscriptions parse, then report that they are not supported.
  `catalog/pg_subscription.c` is still there.
- **The backend types remain**: `B_WAL_SENDER`, `B_WAL_RECEIVER`,
  `B_WAL_SUMMARIZER` and `B_SLOTSYNC_WORKER` are still in the
  enum, with no main function. Removing them touches the
  postmaster's state machine and the I/O statistics tables.

**Other subsystems**

- **JIT**: removed, with `es_jit*` in `EState`, `jitFlags` in
  `PlannedStmt`, and EXPLAIN's JIT section.
- **Base backup**: `pg_backup_start` and `pg_backup_stop` are
  gone, and the tablespace map. Recovery still reads a
  `backup_label` if it finds one.
- **WAL summarizer**: removed.
- **Bootstrap**: `--boot` and `--check` print that there is no
  bootstrap mode. `IsBootstrapProcessingMode()` is still tested in
  some 40 places and is always false.
- **Statistics**: the replication slot and subscription kinds are
  gone, and the kinds after them are renumbered so the table has
  no holes.

**Settings removed**: every `jit*`, `max_wal_senders`,
`max_replication_slots`, `wal_sender_timeout`,
`wal_receiver_*`, `hot_standby_feedback`, `primary_conninfo`,
`primary_slot_name`, `recovery_min_apply_delay`,
`synchronous_standby_names`, `synchronized_standby_slots`,
`sync_replication_slots`, `idle_replication_slot_timeout`,
`max_logical_replication_workers` and its two per-subscription
limits, `max_active_replication_origins`,
`logical_decoding_work_mem`, `debug_logical_replication_streaming`,
`log_replication_commands`, `summarize_wal`,
`wal_summary_keep_time`, `gin_fuzzy_search_limit`,
`gin_pending_list_limit`, `default_text_search_config`.

**Known leftovers**, all dead code that still links:

- `XLogSetReplicationSlotMinimumLSN` and `max_slot_wal_keep_size`
  in `xlog.c`, and the replication slot xmin in `procarray.c`.
- `max_wal_senders` in the control file, written as 0.
- The `XACT_XINFO_HAS_ORIGIN` part of a commit record is still
  parsed, never written.
- The catalogs `pg_replication_origin`, `pg_subscription`,
  `pg_subscription_rel` and `pg_publication*`.

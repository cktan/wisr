# WALPREALLOC — RocksDB's log in files already their size

Status: built (`../../dg/WORKNOTE.md` step 317).

## 1. What

A commit syncs its status record into RocksDB's write-ahead
log (`SM1_DESIGN.md` §8.2), one `fdatasync` a commit, shared
by the commits that arrive together. RocksDB appends to its
log, so every sync also commits the file's new size to the
file system's journal. On this machine (WSL2, ext4):

| 100 bytes, then `fdatasync` | per sync |
| --- | --- |
| appended, the file growing | 2,776 µs |
| written over bytes the file has | 545 µs |
| RocksDB's log, in digres, a one-row update a commit | 2,682 µs |

An update by key from one client took 3.2 ms in digres, its
CPU 0.5 ms, and 1.4 ms in PostgreSQL, whose WAL segments are
zero-filled before use, so its syncs never grow a file
(`../../dg/WORKNOTE.md` step 317). Group commit was no
different: at 16 clients digres shared a sync among 2.6
commits, PostgreSQL among 2.9. The sync's price is the gap.

RocksDB's own answer, `recycle_log_file_num`, reuses old log
files full size; RocksDB turns it off under
`kPointInTimeRecovery`, the recovery sm1 runs, its default
(`db/db_impl/db_impl_open.cc`).

## 2. Design

- **A file system wrapper for sm1's RocksDB instances**
  (`src/walfs.cc`): a `rocksdb::FileSystemWrapper` over the
  default file system, given to each database's instance
  through its `Env` (`db_open()`). It changes one call:
  `NewWritableFile` of a log file, `NNNNNN.log` in the
  database's directory. Every other file, and every other
  call, passes through.
- **A new log is zero-filled to its size first**: RocksDB
  creates it (`O_TRUNC`); the wrapper opens it again, writes
  `wal_prealloc_bytes` of zeros, 64 MB, syncs it once, and
  closes its descriptor; RocksDB then writes from offset 0
  over the zeros, and its syncs leave the size alone. A log
  that outgrows the size grows as now. At a clean close
  RocksDB truncates the file to what it wrote, as it does
  the space it preallocates.
- **Recovery reads a zeroed tail as the end** (`db/
  log_reader.cc`): a header of zeros is a record of type 0
  and length 0, "produced by ... preallocated file regions",
  skipped without a corruption to the end of the file.
- **A torn last record** -- the crash came in the middle of
  writing it, so it was never synced and its commit never
  acknowledged -- reads as a bad checksum where today it
  reads as a short file; under `kPointInTimeRecovery` both
  end the replay there, with every record before it.
- **The zeroing is off the write path**: RocksDB makes a new
  log when it switches memtables, in the write that does. From
  a directory's second log, a thread zeroes the next one ahead,
  `sm_walspare`, one a home, and the new log is that file
  renamed (`ReuseWritableFile`), so RocksDB writes from offset 0
  over its zeros. A log with no spare ready waits for it; with
  none being made, as a directory's first, it is zeroed in
  place. A crash can leave a spare until its database next
  opens; RocksDB ignores it.
- **The temporary instance** writes no log (`../TEMPTABLE.md`
  §2): it is not wrapped.
- **Only on WSL**: a home off it keeps 0, with nothing timed.
  Elsewhere a tree's load paid 64 MB a log for a commit sync no
  one had measured to need it. WSL is known by its kernel's
  release, `/proc/sys/kernel/osrelease` (`wal_on_wsl()`).
- **On WSL, sm decides, once, for the home directory**: what a
  sync of a growing file costs is the file system's, and another
  may charge nothing for it. The home's first attach -- digres's
  `initdb` -- times 8 syncs after a 100-byte append and 8
  after a 100-byte overwrite, in a scratch file in `base/`
  where the databases are (`wal_probe()`, `src/walfs.cc`), and
  zero-fills new logs if the median append sync costs half
  again the overwrite's or more (`wal_prealloc_pays()`); here,
  2,776 µs to 545 µs. It keeps the answer, 64 MB or 0, in the
  root db (`Mwal_prealloc`), and every attach after reads it
  and probes nothing. There is no option and no setting: a
  probe that fails zero-fills nothing and keeps nothing, so
  the next attach probes again. A home moved to another file
  system keeps its first answer.

## 3. Cases

| Case | What happens |
| --- | --- |
| a commit's sync | over zeros: no size change, the cheaper sync |
| a log past 64 MB | grows as now from there |
| clean close | RocksDB truncates the log to its records |
| crash, last record whole | replayed |
| crash, last record torn | replay ends before it, as now |
| crash while the zeros are written | the log is empty or zeros: nothing in it to replay |
| 2PC, a prepared transaction across a restart | its PREPARED record is in the log like any other |
| bulk load into files (`../BULKLOAD.md`) | ingested files, no log: unchanged |
| temporary stores | no log: unchanged |
| a database's directory copied | the logs are bigger by their zeros until RocksDB deletes them, after a flush |
| a file system with no cost to growing a file | the first attach keeps 0: RocksDB's file system, unwrapped, as before |

## 4. Tests

- sm1 unit tests (`test/test_crash.cc`): a new home's first
  attach keeps a decision and a second keeps it unchanged; with
  4 MB kept, a new database's log is 4 MB, with 0 kept its
  records alone; the probe times both syncs and leaves no
  file, and the rule's edges; commits written and the process
  killed before a clean close (the crash tests' way),
  reopened: every acknowledged commit is there, and nothing
  more; a record torn in the zeros, reopened: the records
  before it are there, and the database opens.
- The crash tests and the rest of sm, sq and dg's suites: on
  WSL every log of a home is made so, off it none is.
- `mvcc`: no isolation scenario reaches a log file.
- Measured: one-row updates at 1 and 16 clients, Q8 and Q11,
  before and after; the commit that makes a log, its time.

## 5. Left out

- **The MANIFEST**: synced on every store made or dropped,
  which DDL alone does.

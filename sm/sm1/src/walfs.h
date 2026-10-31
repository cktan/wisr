/*
 * walfs.h -- the Env a database's RocksDB instance writes through
 * when its new write-ahead logs are zero-filled first, so a commit's
 * sync never grows a file (WALPREALLOC.md).
 */
#ifndef WISR_WALFS_H
#define WISR_WALFS_H

#include <stdint.h>

#include <memory>
#include <string>

#include <rocksdb/env.h>

namespace wisr {

/* The size a new log is zero-filled to, where zero-filling pays. */
constexpr uint64_t wal_prealloc_bytes = 64ULL << 20;

/* The next log, made ahead, beside the log that called for it. */
constexpr const char *wal_spare_name = "sm_walspare";

/*
 * An Env over RocksDB's default file system whose new write-ahead
 * logs, NNNNNN.log, are zero-filled to prealloc_bytes and synced
 * before RocksDB writes them: a thread makes the next one ahead, one
 * for the Env, and a log takes it; a log with none is zeroed in place.
 * Every other file and call passes through.
 */
std::unique_ptr<rocksdb::Env> walfs_env(uint64_t prealloc_bytes);

/*
 * Time a sync after a small append and after a small overwrite, in a
 * scratch file in dir, the medians in microseconds into *append_us and
 * *overwrite_us. Returns 0, or -1 if the file could not be written.
 */
int wal_probe(const std::string &dir, double *append_us, double *overwrite_us);

/* Whether release, a kernel's, is WSL's, as "...microsoft-standard". */
bool wal_release_is_wsl(const std::string &release);

/* Whether this kernel is WSL's: only there are new logs zero-filled. */
bool wal_on_wsl();

/* Whether a sync that grows a file costs enough more to zero-fill. */
bool wal_prealloc_pays(double append_us, double overwrite_us);

} // namespace wisr

#endif

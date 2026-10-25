/*
 * chain.h -- a chained key's layout (../VCHAIN.md §3), in its store's
 * one column family: the newest version under a current key, the
 * older ones under old keys, each pointing to the one before it.
 *
 *   current key   = key_class_current | userkey
 *   current value = flags (1B) | xid (8B, unstamped) or commit_ts
 *                   (8B, stamped) | wts (8B) | seq (4B) | prev (8B) |
 *                   payload
 *   frozen value  = flags (1B) | payload
 *   old key       = key_class_old | be64(id)
 *   old value     = the current value's header | displaced (8B) |
 *                   payload
 *
 * prev is the old id of the version before, 0 for none. displaced is
 * the xid of the writer whose version replaced this one, its
 * commit_ts once stamped (chain_displaced_stamped), or, once the
 * entry is unlinked (chain_unlinked), the ts it was unlinked at.
 *
 * A frozen current value (chain_frozen) is one GC found committed at
 * or below the watermark: every snapshot, and every one to come, sees
 * it, and no one walks past it, so it keeps its flags alone. It reads
 * as stamped at commit_ts 0, wts 0, seq 0, prev 0; moved to OLD, it is
 * written so, with the whole header (../VCHAIN.md §3).
 */
#ifndef WISR_CHAIN_H
#define WISR_CHAIN_H

#include "version.h"

#include <stdint.h>

#include <string>

#include <rocksdb/slice.h>

namespace wisr {

/*
 * An old version no chain reaches any more: a repair or a rollback
 * put the version back, or passed it. displaced holds the ts it was
 * unlinked at; GC drops it once no snapshot is older.
 */
constexpr uint8_t chain_unlinked = 0x10;

/*
 * An old version whose displaced holds its displacer's commit_ts, not
 * its xid: GC stamped it, as the displacer's status record may be
 * swept (SM1_DESIGN.md §3.4).
 */
constexpr uint8_t chain_displaced_stamped = 0x20;

/*
 * A current version every snapshot sees, its header cut to its flags
 * (see above). Always with version_stamped; never in an old value.
 */
constexpr uint8_t chain_frozen = 0x40;

/* A chained version, current or old, decoded. Slices borrow the input. */
struct chain_version_t {
  uint8_t flags = 0;
  uint64_t xid = 0;       /* the writer; valid only if unstamped */
  uint64_t commit_ts = 0; /* valid only if flags & version_stamped */
  uint64_t wts = 0;
  uint32_t seq = 0;
  uint64_t prev = 0;      /* the old id of the version before, or 0 */
  uint64_t displaced = 0; /* an old one's: who replaced it, or when unlinked */
  rocksdb::Slice payload;

  /* The same version as the visibility test takes it. */
  version_t as_version() const;
};

/* Encode v as a current value, or with old as an old value. */
std::string chain_value(const chain_version_t &v, bool old);

/* Decode a current value, or with old an old value; false if malformed. */
bool chain_decode(rocksdb::Slice value, chain_version_t *v, bool old);

/* The current key of userkey: key_class_current | userkey. */
std::string curr_key(rocksdb::Slice userkey);

/* The old key of id: key_class_old | be64(id). */
std::string old_key(uint64_t id);

} // namespace wisr

#endif /* WISR_CHAIN_H */

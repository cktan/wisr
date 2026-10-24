/*
 * version.h -- the row-version codec and the store column
 * families' comparator and prefix extractor (SM1_DESIGN.md §3.1).
 *
 *   key   = class (1B, key_class_versioned) | userkey |
 *           ~wts (8B BE) | ~seq (4B BE)
 *   value = flags (1B) | xid (8B, unstamped) or commit_ts (8B,
 *           stamped) | payload
 *
 * Every key of a store opens with a class byte (../VCHAIN.md §2):
 * versioned keys are ordered by userkey, then newest version
 * first; current and old keys bytewise. No caller of sm1 sees it.
 */
#ifndef WISR_VERSION_H
#define WISR_VERSION_H

#include <stdint.h>

#include <memory>
#include <string>

#include <rocksdb/comparator.h>
#include <rocksdb/slice.h>
#include <rocksdb/slice_transform.h>

namespace wisr {

/* Bytes after the userkey in every version key. */
constexpr size_t version_tail_size = 12;

/* The class byte that opens every key of a store (../VCHAIN.md §2). */
constexpr char key_class_versioned = 0x00; /* userkey | ~wts | ~seq */
constexpr char key_class_current = 0x01;   /* a chained key's newest */
constexpr char key_class_old = 0x02;       /* be64(id): an older one */

/* Value flags. */
constexpr uint8_t version_tombstone = 0x01; /* this version is a delete */
constexpr uint8_t version_stamped =
    0x02; /* commit_ts, not xid, follows flags */
constexpr uint8_t version_moved =
    0x04; /* a tombstone of a row that moved to another key */

/* A version key or value, decoded. Slices borrow the input. */
struct version_t {
  rocksdb::Slice userkey;
  uint64_t wts = 0; /* write timestamp, from the key */
  uint32_t seq = 0;
  uint8_t flags = 0;
  uint64_t xid = 0;       /* the writer; valid only if unstamped */
  uint64_t commit_ts = 0; /* valid only if flags & version_stamped */
  rocksdb::Slice payload;
};

/* Build class | userkey | ~wts | ~seq. */
std::string version_key(rocksdb::Slice userkey, uint64_t wts, uint32_t seq);

/* Build class | userkey, then a tail of twelve copies of fill. */
std::string version_bound(rocksdb::Slice userkey, char fill);

/* The userkey part of a version key: past the class, before the tail. */
rocksdb::Slice version_userkey(rocksdb::Slice key);

/* Decode a version key into v's userkey, wts, and seq. */
void version_decode_key(rocksdb::Slice key, version_t *v);

/* Build an unstamped value: flags, xid, then payload. */
std::string version_value(uint8_t flags, uint64_t xid, rocksdb::Slice payload);

/* Decode a version value into v; false if it is malformed. */
bool version_decode_value(rocksdb::Slice value, version_t *v);

/* The wisr.store.v2 comparator, a process-wide singleton. */
const rocksdb::Comparator *version_comparator();

/*
 * The prefix extractor: a version key's class and userkey; any other
 * key whole.
 */
std::shared_ptr<const rocksdb::SliceTransform> version_prefix_extractor();

} // namespace wisr

#endif /* WISR_VERSION_H */

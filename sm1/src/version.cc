/*
 * version.cc -- the row-version codec, the wisr.version.v1
 * comparator, and the userkey prefix extractor (DESIGN.md §3.1).
 */

#include "version.h"

#include "codec.h"

namespace wisr {

namespace {

/*
 * Compare userkeys with memcmp, shorter first on a tie, then
 * the tails with memcmp. The default bytewise comparator would
 * compare one key's tail against a longer key's extra bytes
 * (DESIGN.md §3.1).
 */
class version_comparator_t : public rocksdb::Comparator {
public:
  /* The persisted name; changing the order means a new name. */
  const char *Name() const override { return "wisr.version.v1"; }

  /* Order by userkey, then by tail (newest version first). */
  int Compare(const rocksdb::Slice &a, const rocksdb::Slice &b) const override {
    // Split off each key's userkey.
    rocksdb::Slice ua = version_userkey(a);
    rocksdb::Slice ub = version_userkey(b);
    // Compare userkeys by memcmp, the shorter first on a tie.
    int c = ua.compare(ub);
    // Different userkeys: that decides the order.
    if (c != 0) {
      return c;
    }
    // Same userkey: compare the tails, newest version first.
    rocksdb::Slice ta(a.data() + ua.size(), a.size() - ua.size());
    rocksdb::Slice tb(b.data() + ub.size(), b.size() - ub.size());
    return ta.compare(tb);
  }

  /* No-op: every key must keep its full tail. */
  void FindShortestSeparator(std::string *,
                             const rocksdb::Slice &) const override {}

  /* No-op, as FindShortestSeparator. */
  void FindShortSuccessor(std::string *) const override {}
};

/*
 * Strip the 12-byte tail so a bloom filter keys on the userkey.
 * Variable-length, so FullLengthEnabled stays false (§10.2).
 */
class version_prefix_t : public rocksdb::SliceTransform {
public:
  /* The persisted name. */
  const char *Name() const override { return "wisr.userkey.v1"; }

  /* The userkey of a version key. */
  rocksdb::Slice Transform(const rocksdb::Slice &key) const override {
    return version_userkey(key);
  }

  /* Every version key carries a full tail. */
  bool InDomain(const rocksdb::Slice &key) const override {
    return key.size() >= version_tail_size;
  }
};

} // namespace

/* Build userkey | ~xid | ~seq. */
std::string version_key(rocksdb::Slice userkey, uint64_t xid, uint32_t seq) {
  std::string k;
  // Reserve room for the userkey and the 12-byte tail.
  k.reserve(userkey.size() + version_tail_size);
  // Copy the userkey.
  k.append(userkey.data(), userkey.size());
  // Complement xid, so newer versions sort first.
  put_be64(k, ~xid);
  // Complement seq, so later writes of one xid sort first.
  put_be32(k, ~seq);
  return k;
}

/* Build userkey followed by twelve fill bytes: a scan bound. */
std::string version_bound(rocksdb::Slice userkey, char fill) {
  std::string k;
  // Reserve room for the userkey and the 12-byte tail.
  k.reserve(userkey.size() + version_tail_size);
  // Copy the userkey.
  k.append(userkey.data(), userkey.size());
  // Fill the tail: all 00 sorts first, all FF last.
  k.append(version_tail_size, fill);
  return k;
}

/* The userkey part: everything before the tail. */
rocksdb::Slice version_userkey(rocksdb::Slice key) {
  // Everything before the tail is the userkey.
  size_t n = key.size() >= version_tail_size ? key.size() - version_tail_size
                                             : key.size();
  return rocksdb::Slice(key.data(), n);
}

/* Decode userkey, xid, and seq from a version key. */
void version_decode_key(rocksdb::Slice key, version_t *v) {
  // Split off the userkey.
  v->userkey = version_userkey(key);
  // Locate the tail after it.
  const char *tail = key.data() + v->userkey.size();
  // Undo the complement of xid.
  v->xid = ~get_be64(tail);
  // Undo the complement of seq.
  v->seq = ~get_be32(tail + 8);
}

/* Build flags | payload. */
std::string version_value(uint8_t flags, rocksdb::Slice payload) {
  std::string val;
  // Reserve room for the flags byte and the payload.
  val.reserve(1 + payload.size());
  // Write the flags byte.
  val.push_back(static_cast<char>(flags));
  // Append the payload.
  val.append(payload.data(), payload.size());
  return val;
}

/* Decode flags, the optional commit_ts stamp, and the payload. */
bool version_decode_value(rocksdb::Slice value, version_t *v) {
  // Reject a value too short to hold its flags.
  if (value.empty()) {
    return false;
  }
  // Read the flags byte.
  v->flags = static_cast<uint8_t>(value[0]);
  value.remove_prefix(1);
  // No stamp until the flags say so.
  v->commit_ts = 0;
  // A stamped version carries its commit_ts next.
  if (v->flags & version_stamped) {
    // Reject a stamp cut short.
    if (value.size() < 8) {
      return false;
    }
    // Read the stamp.
    v->commit_ts = get_be64(value.data());
    value.remove_prefix(8);
  }
  // The rest is the payload.
  v->payload = value;
  return true;
}

/* The comparator singleton; RocksDB holds a raw pointer to it. */
const rocksdb::Comparator *version_comparator() {
  static version_comparator_t cmp;
  return &cmp;
}

/* A prefix extractor; the options that take one share it. */
std::shared_ptr<const rocksdb::SliceTransform> version_prefix_extractor() {
  return std::make_shared<version_prefix_t>();
}

} // namespace wisr

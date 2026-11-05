/*
 * toast.cc -- a datum out of line (../TOAST.md §2). A datum is
 * cut into chunks of toast_chunk bytes, each an sm1 row of the
 * table's own store, keyed by the TOAST relid, the datum's id and
 * the chunk's number: a datum's chunks are next to one another and
 * in order, so reading one back is one scan. A datum is never
 * changed once written, only erased.
 */

#include "toast.h"
#include "native.h"
#include "sm0_defer.h"

#include <algorithm>
#include <string>

namespace {

/* The sm1 key of chunk seq of datum id: relid, id, seq, big-endian. */
std::string chunk_key(uint64_t id, uint32_t seq) {
  std::string key;
  sm2::put_be(sm2::toast_relid, key);
  sm2::put_be(id, key);
  sm2::put_be(seq, key);
  return key;
}

/* How many chunks a datum of len bytes is cut into. */
uint32_t chunk_count(uint32_t len) {
  return static_cast<uint32_t>((len + sm2::toast_chunk - 1) / sm2::toast_chunk);
}

} // namespace

namespace sm2 {

/* The pointer p as a varlena; see sm2.h. */
std::string make_toast_ptr(const toast_ptr_t &p) {
  std::string v, word;
  v += varlena_toast;
  v += static_cast<char>(toast_tag);
  store_le(p.len, word);
  v += word;
  store_le(p.storeid, word);
  v += word;
  store_le(p.id, word);
  v += word;
  return v;
}

/* The pointer the varlena v is; see sm2.h. */
bool parse_toast_ptr(std::string_view v, toast_ptr_t &out) {
  if (v.size() != toast_ptr_len || v[0] != varlena_toast ||
      static_cast<uint8_t>(v[1]) != toast_tag) {
    return false;
  }
  out.len = load_le<uint32_t>(v.substr(2, 4));
  out.storeid = load_le<uint64_t>(v.substr(6, 8));
  out.id = load_le<uint64_t>(v.substr(14, 8));
  return true;
}

/* Write a datum's chunks; see toast.h. */
int toast_write(sm1::session_t *session, const toast_ptr_t &p,
                std::string_view data) {
  for (uint32_t seq = 0; !data.empty(); seq++) {
    std::string_view chunk = data.substr(0, toast_chunk);
    // One chunk, a row of the store in the caller's transaction.
    if (sm1::insert(session, p.storeid, chunk_key(p.id, seq), chunk) != 0) {
      return -1;
    }
    data.remove_prefix(chunk.size());
  }
  return 0;
}

/* Erase a datum's chunks; see toast.h. */
int toast_erase(sm1::session_t *session, const toast_ptr_t &p) {
  for (uint32_t seq = 0; seq < chunk_count(p.len); seq++) {
    // One chunk, erased in the caller's transaction.
    if (sm1::erase(session, p.storeid, chunk_key(p.id, seq)) != 0) {
      return -1;
    }
  }
  return 0;
}

/* Read a datum from its chunks; see toast.h. */
int toast_read(sm1::session_t *session, const toast_ptr_t &p,
               std::string &out) {
  std::string lo = chunk_key(p.id, 0), hi = chunk_key(p.id, UINT32_MAX);
  // One scan over the datum's chunks, in order.
  sm1::scan_t *scan;
  if (sm1::scan(session, p.storeid, lo.data(), lo.size(), 1, hi.data(),
                hi.size(), 1, scan) != 0) {
    return -1;
  }
  // Release sm1's cursor and its snapshot, however the read ends.
  defer { sm1::scanend(scan); };
  std::string data;
  data.reserve(p.len);
  std::string_view key, chunk;
  int rc;
  while ((rc = sm1::scannxt(scan, key, chunk)) == 0) {
    data += chunk;
  }
  if (rc < 0) {
    return -1;
  }
  if (data.empty()) {
    return sm1::seterr(sm1::SM_ERR_NOTFOUND, "toast: datum " +
                                                 std::to_string(p.id) +
                                                 " is not there");
  }
  if (data.size() != p.len) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT,
                       "toast: datum " + std::to_string(p.id) + " is " +
                           std::to_string(data.size()) + " bytes, not " +
                           std::to_string(p.len));
  }
  out = std::move(data);
  return 0;
}

/* Read a settled row's datum from its chunks; see toast.h. */
int toast_read_settled(sm1::session_t *session, const toast_ptr_t &p,
                       std::string &out) {
  sm1::rawpin_t *pin = sm1::rawpin_new();
  defer { sm1::rawpin_free(pin); };
  std::string data;
  data.reserve(p.len);
  for (uint32_t seq = 0; seq < chunk_count(p.len); seq++) {
    std::string_view chunk;
    int rc = sm1::read_settled_versioned(session, p.storeid,
                                         chunk_key(p.id, seq), pin, chunk);
    if (rc < 0) {
      return -1;
    }
    // No lock keeps a writer out: it may have erased the datum since
    // the row was read. That is no corruption.
    if (0 != rc) {
      return sm1::seterr(sm1::SM_ERR_NOTFOUND,
                         "toast: a chunk of datum " + std::to_string(p.id) +
                             " is erased, or being erased");
    }
    data += chunk;
  }
  if (data.size() != p.len) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT,
                       "toast: datum " + std::to_string(p.id) + " is " +
                           std::to_string(data.size()) + " bytes, not " +
                           std::to_string(p.len));
  }
  out = std::move(data);
  return 0;
}

} // namespace sm2

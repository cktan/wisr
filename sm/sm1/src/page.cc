/*
 * page.cc -- pages (../BTREE.md §2, ../../BUFFERPOOL.md,
 * ../../MAJORDECISION.md D2): keys of class 0x04 in a store, whose
 * values the level above lays out as B+tree pages. They are not
 * versioned: a page is written outside every transaction, in one
 * RocksDB write batch, and read as it is.
 *
 * A read goes through the environment's buffer pool (bufpool.h),
 * whose frames are never dirty: a page's writer pins it exclusive,
 * writes the batch to RocksDB, then copies the page in. A miss holds
 * its frame exclusive through its read, so no write passes it.
 */

#include "bufpool.h"
#include "error.h"
#include "session.h"
#include "version.h"

#include "sm0_codec.h"

#include <cstring>
#include <string>
#include <vector>

#include <rocksdb/write_batch.h>

static_assert(sm1::page_size == wisr::bufpool_frame_bytes);
static_assert(sm1::page_key_max == wisr::bufpool_key_max);

using namespace wisr;

namespace {

/* A page's RocksDB key: its class, then the caller's key. */
std::string page_key(std::string_view key) {
  std::string k;
  k.reserve(1 + key.size());
  k.push_back(key_class_page);
  k.append(key);
  return k;
}

/*
 * Check a call on pages of storeid: an active transaction, a key the
 * size of one, a live store other than the catalog. 0 with its
 * handle in st, or -1 with the error set.
 */
int enter_pages(sm1::session_t *s, uint64_t storeid, const store_t *&st) {
  if (xct_check_active(s) != 0) {
    return -1;
  }
  if (storeid == 0) {
    return set_error(sm1::SM_ERR_INVALID, "store 0 holds no pages");
  }
  int r = catalog_lookup(s, xct_view(s), storeid);
  if (r == 1) {
    return set_error(sm1::SM_ERR_NOTFOUND, "store {} does not exist", storeid);
  }
  if (r < 0) {
    return -1;
  }
  st = &session_store(s, storeid);
  if (!st->cf) {
    return set_error(sm1::SM_ERR_CORRUPT, "column family store.{} is missing",
                     storeid);
  }
  return 0;
}

/* Check a page's key: at most sm1::max_keylen less its class byte. */
int check_page_key(std::string_view key) {
  if (key.size() + 1 > sm1::max_keylen) {
    return set_error(sm1::SM_ERR_INVALID, "a page's key is too long");
  }
  return 0;
}

/* What a miss reads: a page of a store. */
struct page_read_t {
  const store_t *st;
  std::string_view key;
};

/*
 * Read a page into a frame, for a miss (bufpool_load_t). The frame is
 * its copy, so RocksDB's block cache keeps none (BUFFERPOOL.md §1.3).
 */
int load_page(void *arg, char *buf, size_t &len) {
  const auto *r = static_cast<const page_read_t *>(arg);
  rocksdb::ReadOptions ro;
  ro.fill_cache = false;
  rocksdb::PinnableSlice value;
  rocksdb::Status rs = r->st->rdb->Get(ro, r->st->cf, page_key(r->key), &value);
  if (rs.IsNotFound()) {
    return 1;
  }
  if (!rs.ok()) {
    return set_status_error(rs, "read a page");
  }
  if (value.size() > sm1::page_size) {
    return set_error(sm1::SM_ERR_CORRUPT, "a page of {} bytes", value.size());
  }
  memcpy(buf, value.data(), value.size());
  len = value.size();
  return 0;
}

} // namespace

/* Pin a page through the buffer pool; see sm1.h. */
int sm1::pin_page(sm1::session_t *session, uint64_t storeid,
                  std::string_view key, sm1::page_mode_t mode, unsigned flags,
                  sm1::page_t *&out, std::string_view &bytes) {
  bytes = std::string_view();
  const store_t *st = nullptr;
  if (enter_pages(session, storeid, st) != 0) {
    return -1;
  }
  bool ex = sm1::page_mode_t::ex == mode;
  unsigned pool_flags =
      (0 != (flags & sm1::page_inner) ? bufpool_inner : 0) |
      (0 != (flags & sm1::page_no_read) ? bufpool_no_read : 0);
  page_read_t read{st, key};
  bufpool_t::frame_t *f = nullptr;
  int rc = session->db->bufpool->pin({session->db->dbid, storeid, key}, ex,
                                     pool_flags, load_page, &read, f, bytes);
  if (0 == rc) {
    out = reinterpret_cast<sm1::page_t *>(f);
  }
  return rc;
}

/* Unpin a page pin_page pinned; see sm1.h. */
void sm1::unpin_page(sm1::session_t *session, sm1::page_t *page) {
  if (nullptr != page) {
    session->db->bufpool->unpin(reinterpret_cast<bufpool_t::frame_t *>(page));
  }
}

/* Whether a page is in the buffer pool; see sm1.h. */
bool sm1::page_cached(sm1::session_t *session, uint64_t storeid,
                      std::string_view key) {
  return session->db->bufpool->cached({session->db->dbid, storeid, key});
}

/* The pages pinned exclusive; see sm1.h. */
size_t sm1::pages_exclusive(sm1::session_t *session) {
  return session->db->bufpool->exclusive();
}

/* Write pages, outside every transaction; see sm1.h. */
int sm1::page_write(sm1::session_t *session, uint64_t storeid,
                    const std::vector<sm1::page_put_t> &puts) {
  const store_t *st = nullptr;
  if (enter_pages(session, storeid, st) != 0) {
    return -1;
  }
  rocksdb::WriteBatch batch;
  for (const sm1::page_put_t &p : puts) {
    if (p.bytes.size() > sm1::page_size) {
      return set_error(sm1::SM_ERR_INVALID, "a page of {} bytes",
                       p.bytes.size());
    }
    if (p.erase) {
      batch.Delete(st->cf, page_key(p.key));
    } else {
      batch.Put(st->cf, page_key(p.key), p.bytes);
    }
  }
  // The temporary instance is never logged; any other write is in
  // RocksDB's WAL, unsynced: a later commit's sync covers it, the WAL
  // being one sequence.
  rocksdb::WriteOptions wo;
  wo.disableWAL = st->rdb == session->db->tmp;
  rocksdb::Status rs = st->rdb->Write(wo, &batch);
  if (!rs.ok()) {
    return set_status_error(rs, "write pages");
  }
  // RocksDB has them: nothing past here can fail, so a frame is never
  // half changed, and never ahead of RocksDB.
  bufpool_t *pool = session->db->bufpool;
  for (const sm1::page_put_t &p : puts) {
    auto *f = reinterpret_cast<bufpool_t::frame_t *>(p.page);
    if (p.erase) {
      pool->discard(f);
    } else {
      pool->install(f, p.bytes);
    }
  }
  return 0;
}

/* The bytes of the pages under a prefix; see sm1.h. */
int sm1::page_approxsize(sm1::session_t *session, uint64_t storeid,
                         std::string_view prefix, uint64_t &disk,
                         uint64_t &mem) {
  disk = mem = 0;
  const store_t *st = nullptr;
  if (enter_pages(session, storeid, st) != 0 || check_page_key(prefix) != 0) {
    return -1;
  }
  std::string lo = page_key(prefix);
  std::string hi = lo;
  while (!hi.empty() && static_cast<unsigned char>(hi.back()) == 0xFF) {
    hi.pop_back();
  }
  hi.back() = static_cast<char>(static_cast<unsigned char>(hi.back()) + 1);
  rocksdb::Range range(lo, hi);
  for (bool files : {true, false}) {
    rocksdb::SizeApproximationOptions so;
    so.include_files = files;
    so.include_memtables = !files;
    uint64_t n = 0;
    rocksdb::Status rs =
        st->rdb->GetApproximateSizes(so, st->cf, &range, 1, &n);
    if (!rs.ok()) {
      return set_status_error(rs, "approximate pages' size");
    }
    (files ? disk : mem) = n;
  }
  return 0;
}

/* Remove every page under a prefix; see sm1.h. */
int sm1::page_drop(sm1::session_t *session, uint64_t storeid,
                   std::string_view prefix) {
  // Outside a transaction, as sm1::on_commit runs, the store as the
  // database has it now: none, if it went with the transaction.
  store_t outside;
  const store_t *st = nullptr;
  if (session && !session->active) {
    if (storeid == 0 || check_page_key(prefix) != 0) {
      return set_error(sm1::SM_ERR_INVALID, "store 0 holds no pages");
    }
    outside = db_store(session->db, storeid);
    if (!outside.cf) {
      return 0;
    }
    st = &outside;
  } else if (enter_pages(session, storeid, st) != 0 ||
             check_page_key(prefix) != 0) {
    return -1;
  }
  std::string lo = page_key(prefix);
  // The least key past every key that starts with lo; class 0x04's
  // byte is below 0xFF, so there is one.
  std::string hi = lo;
  while (!hi.empty() && static_cast<unsigned char>(hi.back()) == 0xFF) {
    hi.pop_back();
  }
  hi.back() = static_cast<char>(static_cast<unsigned char>(hi.back()) + 1);
  rocksdb::WriteBatch batch;
  batch.DeleteRange(st->cf, lo, hi);
  rocksdb::WriteOptions wo;
  wo.disableWAL = st->rdb == session->db->tmp;
  rocksdb::Status rs = st->rdb->Write(wo, &batch);
  if (!rs.ok()) {
    return set_status_error(rs, "drop pages");
  }
  // Empty their frames, a read that misses now finding no page: a page
  // key may be made again, under the same prefix, by a later tree.
  session->db->bufpool->purge(session->db->dbid, storeid, prefix);
  return 0;
}

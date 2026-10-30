/*
 * page.cc -- pages (../BTREE.md §2, ../../MAJORDECISION.md D2): keys
 * of class 0x04 in a store, whose values the level above lays out as
 * B+tree pages. They are not versioned: a page is written outside
 * every transaction, in one RocksDB write batch, and read as it is.
 * A read goes through the environment's block cache, where a page's
 * bytes are an entry of their own, shared by every session.
 *
 * The cache is never dirty: a write puts the batch to RocksDB, then
 * replaces each page's entry. What keeps a stale page out is a fill
 * latch, a stripe by page: a read that misses holds it across its
 * Get and its insert, and a write across its batch and its inserts.
 * So a read that missed before a write cannot put back the page the
 * write replaced (../../MAJORDECISION.md D3). A hit takes no latch.
 */

#include "error.h"
#include "session.h"
#include "version.h"

#include "sm0_codec.h"
#include "sm0_defer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <rocksdb/advanced_cache.h>
#include <rocksdb/write_batch.h>

using namespace wisr;

namespace {

/* The fill latches' stripes, for the whole process. */
constexpr size_t fill_stripes = 256;
std::array<std::mutex, fill_stripes> fill_latch;

/* Free a cached page's bytes when the cache drops its entry. */
void page_delete(rocksdb::Cache::ObjectPtr obj, rocksdb::MemoryAllocator *) {
  delete static_cast<std::string *>(obj);
}

/* How a cached page is freed and accounted. */
const rocksdb::Cache::CacheItemHelper
    page_helper(rocksdb::CacheEntryRole::kMisc, page_delete);

/*
 * The cache key of key in storeid of db: a tag no RocksDB block key
 * starts with at this length, the database, the store, the key. Made
 * in place for a key the length of a tree's, on the heap past it:
 * every read of a page makes one.
 */
class cache_key_t {
public:
  cache_key_t(const db_t *db, uint64_t storeid, std::string_view key) {
    size_t n = 1 + 8 + 8 + key.size();
    char *p = m_buf;
    if (n > sizeof(m_buf)) {
      m_big.resize(n);
      p = m_big.data();
    }
    p[0] = 'P';
    for (int i = 0; i < 8; i++) {
      p[1 + i] = static_cast<char>(db->dbid >> (56 - 8 * i));
      p[9 + i] = static_cast<char>(storeid >> (56 - 8 * i));
    }
    memcpy(p + 17, key.data(), key.size());
    m_view = std::string_view(p, n);
  }
  cache_key_t(const cache_key_t &) = delete;
  cache_key_t &operator=(const cache_key_t &) = delete;

  std::string_view view() const { return m_view; }
  operator rocksdb::Slice() const { return {m_view.data(), m_view.size()}; }

private:
  char m_buf[64];
  std::string m_big;
  std::string_view m_view;
};

/* The stripe of the fill latch a page's cache key is under. */
size_t stripe_of(std::string_view ck) {
  return std::hash<std::string_view>{}(ck) % fill_stripes;
}

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

} // namespace

/* The dbid of session's database; see sm1.h. */
uint64_t sm1::session_dbid(const sm1::session_t *session) {
  return session->db->dbid;
}

/* Read a page through the block cache; see sm1.h. */
int sm1::pin_page(sm1::session_t *session, uint64_t storeid,
                  std::string_view key, bool inner, sm1::page_t *&out,
                  std::string_view &bytes) {
  bytes = std::string_view();
  const store_t *st = nullptr;
  if (enter_pages(session, storeid, st) != 0 || check_page_key(key) != 0) {
    return -1;
  }
  rocksdb::Cache *cache = session->db->block_cache.get();
  cache_key_t ck(session->db, storeid, key);
  rocksdb::Cache::Handle *h = cache->Lookup(ck);
  if (!h) {
    // A miss: read the page and insert it under its stripe's fill
    // latch, so no write lands between the two (file comment).
    std::lock_guard<std::mutex> fill(fill_latch[stripe_of(ck.view())]);
    h = cache->Lookup(ck);
    if (!h) {
      auto *value = new std::string;
      rocksdb::Status rs =
          st->rdb->Get(rocksdb::ReadOptions(), st->cf, page_key(key), value);
      if (rs.IsNotFound()) {
        delete value;
        return 1;
      }
      if (!rs.ok()) {
        delete value;
        return set_status_error(rs, "read a page");
      }
      size_t charge =
          sizeof(std::string) + value->capacity() + ck.view().size();
      rs = cache->Insert(ck, value, &page_helper, charge, &h,
                         inner ? rocksdb::Cache::Priority::HIGH
                               : rocksdb::Cache::Priority::LOW);
      if (!rs.ok()) {
        // A full cache strict about its capacity: Insert freed value.
        return set_status_error(rs, "cache a page");
      }
    }
  }
  // The handle is the page, so a read allocates nothing on a hit.
  out = reinterpret_cast<sm1::page_t *>(h);
  const auto *value = static_cast<const std::string *>(cache->Value(h));
  bytes = *value;
  return 0;
}

/* Unpin a page pin_page pinned; see sm1.h. */
void sm1::unpin_page(sm1::session_t *session, sm1::page_t *page) {
  if (page) {
    session->db->block_cache->Release(
        reinterpret_cast<rocksdb::Cache::Handle *>(page));
  }
}

/* Whether a page is in the block cache; see sm1.h. */
bool sm1::page_cached(sm1::session_t *session, uint64_t storeid,
                      std::string_view key) {
  rocksdb::Cache *cache = session->db->block_cache.get();
  cache_key_t ck(session->db, storeid, key);
  rocksdb::Cache::Handle *h = cache->Lookup(ck);
  if (h) {
    cache->Release(h);
  }
  return nullptr != h;
}

/* Write pages, outside every transaction; see sm1.h. */
int sm1::page_write(sm1::session_t *session, uint64_t storeid,
                    const std::vector<sm1::page_put_t> &puts) {
  const store_t *st = nullptr;
  if (enter_pages(session, storeid, st) != 0) {
    return -1;
  }
  rocksdb::WriteBatch batch;
  std::vector<std::string> cks;
  std::vector<size_t> stripes;
  cks.reserve(puts.size());
  for (const sm1::page_put_t &p : puts) {
    if (check_page_key(p.key) != 0) {
      return -1;
    }
    if (p.erase) {
      batch.Delete(st->cf, page_key(p.key));
    } else {
      batch.Put(st->cf, page_key(p.key), p.bytes);
    }
    cks.emplace_back(cache_key_t(session->db, storeid, p.key).view());
    stripes.push_back(stripe_of(cks.back()));
  }
  // Hold every page's fill latch, in stripe order so two writes
  // cannot deadlock, across the write and the cache's update.
  std::sort(stripes.begin(), stripes.end());
  stripes.erase(std::unique(stripes.begin(), stripes.end()), stripes.end());
  for (size_t i : stripes) {
    fill_latch[i].lock();
  }
  sm0::scope_guard_t unlatch([&] {
    for (size_t i : stripes) {
      fill_latch[i].unlock();
    }
  });
  // The temporary instance is never logged; any other write is in
  // RocksDB's WAL, unsynced: a later commit's sync covers it, the WAL
  // being one sequence.
  rocksdb::WriteOptions wo;
  wo.disableWAL = st->rdb == session->db->tmp;
  rocksdb::Status rs = st->rdb->Write(wo, &batch);
  if (!rs.ok()) {
    return set_status_error(rs, "write pages");
  }
  // Replace each page's entry, or drop it; one that does not fit the
  // cache is read again at its next miss.
  rocksdb::Cache *cache = session->db->block_cache.get();
  for (size_t i = 0; i < puts.size(); i++) {
    if (puts[i].erase) {
      cache->Erase(cks[i]);
      continue;
    }
    auto *value = new std::string(puts[i].bytes);
    size_t charge = sizeof(std::string) + value->capacity() + cks[i].size();
    rocksdb::Status cs =
        cache->Insert(cks[i], value, &page_helper, charge, nullptr,
                      puts[i].inner ? rocksdb::Cache::Priority::HIGH
                                    : rocksdb::Cache::Priority::LOW);
    if (!cs.ok()) {
      cache->Erase(cks[i]);
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
  // An iterator over the pages as they are, opened before they go,
  // to find each one's cache entry after.
  rocksdb::ReadOptions ro;
  rocksdb::Slice upper(hi);
  ro.iterate_upper_bound = &upper;
  std::unique_ptr<rocksdb::Iterator> it(st->rdb->NewIterator(ro, st->cf));
  rocksdb::WriteBatch batch;
  batch.DeleteRange(st->cf, lo, hi);
  rocksdb::WriteOptions wo;
  wo.disableWAL = st->rdb == session->db->tmp;
  rocksdb::Status rs = st->rdb->Write(wo, &batch);
  if (!rs.ok()) {
    return set_status_error(rs, "drop pages");
  }
  // Drop each page's cache entry, under its fill latch: a read that
  // misses now finds no page, and a page key may be made again, under
  // the same prefix, by a later tree.
  rocksdb::Cache *cache = session->db->block_cache.get();
  for (it->Seek(lo); it->Valid(); it->Next()) {
    rocksdb::Slice k = it->key();
    std::string_view key(k.data() + 1, k.size() - 1);
    cache_key_t ck(session->db, storeid, key);
    std::lock_guard<std::mutex> fill(fill_latch[stripe_of(ck.view())]);
    cache->Erase(ck);
  }
  if (!it->status().ok()) {
    return set_status_error(it->status(), "drop pages");
  }
  return 0;
}

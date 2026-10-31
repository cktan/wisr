/*
 * bufpool.h -- the buffer pool of a tree's pages (../../BUFFERPOOL.md):
 * frames of 4 KB, each holding one page, found by a page's tag.
 *
 * A pin is a latch, shared or exclusive, held only as long as a latch
 * is. A pool's pages are always clean: a change is made on a copy,
 * written to RocksDB, and copied in under an exclusive pin, by
 * install(), which cannot fail. So a failed write leaves a frame equal
 * to RocksDB's.
 *
 * It knows tags, bytes and pins; nothing of sessions, stores or
 * RocksDB, which stay in page.cc: a miss reads through the caller's
 * loader. So it is tested alone (../test/test_bufpool.cc).
 */
#ifndef WISR_BUFPOOL_H
#define WISR_BUFPOOL_H

#include <stddef.h>
#include <stdint.h>

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string_view>

namespace wisr {

/* The bytes a frame holds, and so the most a page may be. */
constexpr size_t bufpool_frame_bytes = 4096;

/* The fewest frames a pool is made with: pins must find frames. */
constexpr size_t bufpool_min_frames = 64;

/* The longest page key a frame's tag holds. */
constexpr size_t bufpool_key_max = 16;

/* A page, across the process: its database, store and key. */
struct bufpool_page_t {
  uint64_t dbid;
  uint64_t storeid;
  std::string_view key;
};

/* pin's flags. */
constexpr unsigned bufpool_inner = 1;   /* the clock's age: an inner page */
constexpr unsigned bufpool_no_read = 2; /* a new page: exclusive alone */

/*
 * Read a page into buf, of bufpool_frame_bytes, for a miss, setting len:
 * 0; 1 if there is no such page; -1 with the error set.
 */
typedef int (*bufpool_load_t)(void *arg, char *buf, size_t &len);

class bufpool_t {
public:
  struct frame_t;

  /* A pool of frames frames, at least 1. */
  explicit bufpool_t(size_t frames);
  ~bufpool_t();
  bufpool_t(const bufpool_t &) = delete;
  bufpool_t &operator=(const bufpool_t &) = delete;

  /*
   * Pin page, shared or, if ex, exclusive: 0 with the frame in out and
   * its bytes in bytes, valid until unpin; 1 if load finds no page,
   * nothing pinned; -1 with the error set: SM_ERR_INVALID for a key
   * over bufpool_key_max, SM_ERR_INTERRUPTED if a bit in the thread's
   * wait mask ends a wait, or load's. A miss takes a frame, which may
   * evict a page, and holds it exclusive through load, so its page's
   * other pins wait there. With bufpool_no_read, ex alone, a miss loads
   * nothing and bytes are empty: unless install is called before
   * unpin, the frame goes back unused.
   *
   * A thread may pin several pages; writers keep to one order
   * (../../BTREE.md §16). A thread holding a page shared may pin it
   * shared again; no other second pin of a page is allowed.
   */
  int pin(const bufpool_page_t &page, bool ex, unsigned flags,
          bufpool_load_t load, void *arg, frame_t *&out,
          std::string_view &bytes);

  /* Release a pin; nullptr is a no-op. */
  void unpin(frame_t *frame);

  /*
   * Copy bytes, at most bufpool_frame_bytes, into a frame pinned
   * exclusive, as the page now is. It cannot fail, so a change is
   * installed once RocksDB has it. A view of the frame's old bytes is
   * over; the frame's own bytes may not be given.
   */
  void install(frame_t *frame, std::string_view bytes);

  /* Take a page's frame, pinned exclusive, out: its page erased. */
  void discard(frame_t *frame);

  /*
   * Take every frame of storeid in dbid whose key begins with prefix
   * out, waiting out each one's pins. A frame's page must already be
   * gone from what load reads. The caller holds no pin of them.
   */
  void purge(uint64_t dbid, uint64_t storeid, std::string_view prefix);

  /* Whether page has a frame now: for tests. */
  bool cached(const bufpool_page_t &page) const;

  /* The frames pinned exclusive now: for tests. */
  size_t exclusive() const;

  size_t frames() const { return m_nframes; }

private:
  struct tag_t;
  struct waitq_t;
  static constexpr size_t waitqs = 64;
  static constexpr size_t bucket_locks = 256;

  frame_t *find(const tag_t &tag) const;
  int acquire(frame_t *f, bool ex, const tag_t &tag);
  void release(frame_t *f, uint32_t held);
  void wake(frame_t *f);
  int victim(frame_t *&out);
  void unlink(frame_t *f);
  int take(const tag_t &tag, frame_t *&out);
  size_t bucket_of(const tag_t &tag) const;
  waitq_t &waitq_of(const frame_t *f) const;

  size_t m_nframes;
  std::unique_ptr<frame_t[]> m_frames;
  std::unique_ptr<char[]> m_mem;
  size_t m_mask;
  std::unique_ptr<std::atomic<int32_t>[]> m_buckets;
  std::unique_ptr<std::mutex[]> m_locks;
  std::unique_ptr<waitq_t[]> m_waitqs;
  std::atomic<uint64_t> m_hand{0};
};

} // namespace wisr

#endif

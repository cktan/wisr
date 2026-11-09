/*
 * tree.h -- a B+tree of an index's own pages (../BTREEINDEX.md), a
 * Lehman and Yao B-link tree: each page has a high key and a right
 * link, so a reader pins one page at a time, and a split that moves a
 * key leaves it reachable. Its pages are sm1's (sm1::pin_page,
 * sm1::page_write), under the index's relid:
 *
 *   page key = relid (2B, big-endian) | pageid (8B, big-endian)
 *
 * Page 0 is the root, for the tree's life; its level + 1 is the tree's
 * height. Other page ids come from one sequence for the database
 * (sm2::tree_page_seq). A tree's keys are distinct byte strings,
 * compared bytewise; a key's value is the caller's. Every change is
 * a top-level write of sm1's, made when it is made, whatever the
 * caller's transaction then does. No entry is removed but by
 * tree_prune, tree_reap_leaf, and tree_remove_if, which only a test
 * calls; no page is ever freed.
 */
#ifndef SM2_TREE_TREE_H
#define SM2_TREE_TREE_H

#include "page.h"

#include "sm1.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sm2::tree {

static_assert(page_limit <= sm1::page_size);

/*
 * Told of a leaf not all visible, with no latch held, by whoever finds
 * one, reader or writer (../BTREEINDEX.md §9). It does not fail.
 */
using leaf_note_fn = void (*)(void *ctx, uint64_t storeid, uint16_t relid,
                              uint64_t pageid);

/*
 * A tree, as a session opens it. It holds no page: a pin lasts as long
 * as a latch, inside one call.
 */
struct tree_t {
  sm1::session_t *session = nullptr;
  uint64_t storeid = 0;
  uint16_t relid = 0;
  size_t limit = page_limit;
  /* Told of a leaf that needs reaping; none by default. */
  leaf_note_fn note = nullptr;
  void *note_ctx = nullptr;
  /* The sequence of page ids, looked up at the tree's first split. */
  sm1::seqref_t seq;
  bool has_seq = false;
};

/* Open the tree of relid in storeid through session; reads nothing. */
void tree_open(tree_t &tree, sm1::session_t *session, uint64_t storeid,
               uint16_t relid, size_t limit = page_limit);

/*
 * Make an empty tree: drop every page under its relid, left by a tree
 * before it, then write an empty leaf at page 0, the root.
 */
int tree_create(tree_t &tree);

/* Remove every page of relid's tree in storeid. */
int tree_drop(sm1::session_t *session, uint64_t storeid, uint16_t relid);

/*
 * Insert key with value: 0; 1 if the tree has the key already, its
 * entry marked in doubt and nothing else changed; -1 with
 * SM_ERR_KEYTOOLONG if its entry, or the key's as an inner entry, with
 * a page id, is over entry_max of the tree's limit, or another error.
 * Either way the key's leaf is no longer all visible (page_all_visible):
 * a key found there is a write's too, its row just changed.
 */
int tree_insert(tree_t &tree, std::string_view key, std::string_view value);

/*
 * Insert key, which begins with prefix, with value, unless an entry
 * beginning with prefix conflicts (../BTREEINDEX.md §7). Returns 0
 * inserted; 1 if key was there already, its entry marked in doubt; 2,
 * every latch let go, for the caller to wait as check found and call
 * again; -1 on an error or a duplicate, its error check's to set.
 * 1. Latch the leaf that covers prefix. Every insert of a key
 *    beginning with prefix takes this latch first, and holds it until
 *    it has inserted; so two inserts of one prefix take turns.
 * 2. Judge each other entry beginning with prefix by check(entry key,
 *    entry value): 0 no conflict, 1 a duplicate, 2 a writer to wait
 *    for, -1 an error. Return at the first conflict.
 * 3. Insert the key. Its leaf is left not all visible, as by
 *    tree_insert.
 */
int tree_insert_unique(
    tree_t &tree, std::string_view prefix, std::string_view key,
    std::string_view value,
    const std::function<int(std::string_view, std::string_view)> &check);

/*
 * Mark key's entry in doubt, and clear the all-visible bit of the leaf
 * that covers key, under its latch, writing the leaf only if either
 * changes: 0, or -1. An entry not there, a pending index's, leaves
 * only the bit cleared. A delete's at its old key, and an update's that
 * changes a key, after the row's write (../BTREEINDEX.md §9): the latch
 * orders it against vacuum, which judges a leaf latched before it
 * clears an entry's mark or sets the bit.
 */
int tree_mark_in_doubt(tree_t &tree, std::string_view key);

/*
 * Remove key, if the tree has it and keep(key, value) says no, under
 * its leaf's latch, which an insert of the key takes too: 0 removed,
 * 1 not, -1 on error, keep's own included. keep is called with no
 * other latch held.
 */
template <typename F>
int tree_remove_if(tree_t &tree, std::string_view key, F keep);

/* A cursor over a tree's keys in order, forward only. */
struct cursor_t {
  tree_t *tree = nullptr;
  /* The upper bound, if any, and whether it is in the range. */
  bool has_hi = false;
  bool hi_inclusive = false;
  std::string hi;
  /* Where it is: past from, or at it if from_inclusive. */
  std::string from;
  bool from_inclusive = true;
  bool started = false;
  bool done = false;
  /*
   * The leaf the batch was read from, and the batch: its entries'
   * bytes copied end to end, the next one's place in them, and how
   * many have been handed out, at, of end; the last one's key.
   */
  uint64_t leaf = 0;
  /* Whether that leaf was all visible as the batch was copied. */
  bool visible = false;
  std::string bytes;
  size_t pos = 0;
  size_t at = 0;
  size_t end = 0;
  std::string_view last;
};

/*
 * Open cursor over tree's keys from lo to hi, either nullptr for no
 * bound, each in the range or not as its inclusive flag says.
 */
void cursor_open(tree_t &tree, cursor_t &cursor, const std::string *lo,
                 bool lo_inclusive, const std::string *hi, bool hi_inclusive);

/*
 * The next key and its value: 0, valid until the cursor copies its
 * next leaf, as a call when cursor_held() is 0 does, or is aimed
 * again; 1 at the end; -1 on error. A cursor holds no page between
 * calls: a leaf's keys in range are copied, and the next leaf is
 * reached again from the one they were copied from, so a split
 * meanwhile hides none.
 */
int cursor_next(cursor_t &cursor, std::string_view &key,
                std::string_view &value);

/*
 * The entries of the cursor's copy not yet handed out: while more
 * than 0, cursor_next reads the next from the copy, and every key and
 * value it gave from the copy stays valid.
 */
size_t cursor_held(const cursor_t &cursor);

/*
 * Aim cursor at the keys from lo to hi, as cursor_open does, over the
 * tree it was opened on; its buffer kept, as an IN list's probes each
 * aim one cursor.
 */
void cursor_rebound(cursor_t &cursor, const std::string *lo, bool lo_inclusive,
                    const std::string *hi, bool hi_inclusive);

/* Move cursor forward to lo, in the range or not as inclusive says. */
void cursor_seek(cursor_t &cursor, const std::string &lo, bool inclusive);

/*
 * Walk every leaf, left to right, and judge its entries in doubt: dead
 * (key, value) is 1 to remove an entry, 0 to keep it in doubt, 2 to
 * keep it as settled and live, -1 to stop with its error. For each
 * leaf (../BTREEINDEX.md §9):
 * 1. Skip a leaf set all visible (page_all_visible) already.
 * 2. Judge the leaf's entries in doubt with no latch. No other entry
 *    is asked of.
 * 3. If any is dead or settled, or none is in doubt, latch the leaf.
 * 4. Judge those entries again. So dead reads what a writer wrote
 *    meanwhile: it may be called twice for an entry, and only that
 *    second answer counts.
 * 5. Remove the dead, and clear the mark of the settled.
 * 6. Set the leaf all visible if no entry is left in doubt.
 * 7. Write the leaf once, if anything changed.
 *
 * A leaf that splits while it is judged keeps its right half's entries
 * until the next prune. removed counts the entries removed. Pages are
 * not merged: an empty leaf stays linked.
 */
int tree_prune(
    tree_t &tree,
    const std::function<int(std::string_view, std::string_view)> &dead,
    size_t &removed);

/*
 * tree_prune of one page, by its id, from step 1 for a leaf: 0, doubt
 * set if an entry is left in doubt; 1 if the page is no leaf; or -1.
 * The btree reaper's (../BTREEINDEX.md §9), whose queued page id may be stale:
 * pages are never freed, so it names a page still. removed counts the
 * entries removed.
 */
int tree_reap_leaf(
    tree_t &tree, uint64_t pageid,
    const std::function<int(std::string_view, std::string_view)> &dead,
    size_t &removed, bool &doubt);

/*
 * A tree built whole, from the bottom up, from keys given in order
 * (../BTREELOAD_PLAN.md):
 * 1. build_add fills each leaf to leaf_fill of the tree's limit, then
 *    hands it out, linked, with the next leaf's first key as its high
 *    key.
 * 2. build_end builds each level above from the first key and id of
 *    each page below, inner pages filled to inner_fill, until a level
 *    fits one page.
 * 3. build_root writes that level as page 0, and tells the tree's
 *    owner of each leaf.
 * Every page but page 0 goes to put, in page id order, ids drawn as
 * pages start: no frame of the pool holds one. The caller writes
 * them to the store before build_root, as page 0 names them. Every
 * entry is as its caller gives it, in doubt or not; no leaf is all
 * visible.
 */
constexpr size_t leaf_fill = 90;  /* percent */
constexpr size_t inner_fill = 70; /* percent */

/* A page a build hands out: its sm1 key and bytes; 0, or -1. */
using build_put_fn =
    std::function<int(std::string_view key, std::string_view bytes)>;

/* One level of a build: the page being filled, and those handed out. */
struct build_level_t {
  uint8_t level = 0;
  size_t target = 0;  /* the bytes a page fills to */
  page_t page;        /* the page being filled */
  size_t payload = 0; /* its entries' bytes, slots included */
  uint64_t id = 0;    /* its id, drawn once a page before it is out */
  bool has_id = false;
  uint64_t prev = 0; /* the level's page before it, 0 none */
  /* Each page handed out: its low key, empty for the first, and id. */
  std::vector<std::pair<std::string, uint64_t>> lows;
};

/* A build of tree's pages, by build_open. */
struct build_t {
  tree_t *tree = nullptr;
  build_put_fn put;
  build_level_t leaves;
  std::vector<uint64_t> leaf_ids; /* each leaf handed out */
  page_t root;                    /* page 0, by build_end */
};

/* Begin building tree, whose page 0 is an empty leaf, its pages to put. */
void build_open(tree_t &tree, build_t &build, build_put_fn put);

/*
 * Add key, past every key added before, with value: 0; -1 with
 * SM_ERR_KEYTOOLONG as tree_insert, SM_ERR_INVALID if out of order,
 * or put's error.
 */
int build_add(build_t &build, std::string_view key, std::string_view value);

/* Hand out the last leaf and every level above but page 0: 0, or -1. */
int build_end(build_t &build);

/*
 * Write page 0, the top level, once the pages put are in the store,
 * and tell the tree's owner of each leaf: 0, or -1.
 */
int build_root(build_t &build);

/* The tree's height, from its root: for tests. */
int tree_height(tree_t &tree, size_t &height);

/*
 * Check the whole tree: every level's pages in order, linked, each key
 * under its page's high key and at or past its parent's entry, every
 * leaf at one depth, no entry in doubt on an all-visible leaf. 0, or
 * -1 with SM_ERR_CORRUPT: for tests.
 */
int tree_check(tree_t &tree);

namespace detail {

/* A page pinned and viewed; released at scope's end. */
struct pinned_t {
  sm1::session_t *session = nullptr;
  sm1::page_t *pin = nullptr;
  page_view_t view;

  pinned_t() = default;
  pinned_t(const pinned_t &) = delete;
  pinned_t &operator=(const pinned_t &) = delete;
  ~pinned_t() { sm1::unpin_page(session, pin); }

  /* Release the page now. */
  void release() {
    sm1::unpin_page(session, pin);
    pin = nullptr;
  }
};

/*
 * A leaf latched for a change: its id, its exclusive pin, its bytes
 * checked whole, and its page decoded, when a split or a removal asks
 * for it (decode()).
 */
struct latched_t {
  uint64_t pageid = 0;
  pinned_t pin;
  page_t page;

  /* The page decoded from its bytes into page. */
  void decode() { pin.view.load(page); }
};

/* Find and latch the leaf that holds key, any split it shows finished. */
int latch_leaf(tree_t &tree, std::string_view key, latched_t &out);

/* Write a latched page back, its version moved. */
int write_back(tree_t &tree, latched_t &l);

/*
 * While above 0, a split's post fails before it begins, counting
 * down, as a crash between a split's two writes would leave the
 * tree: for tests.
 */
extern int fail_posts;

} // namespace detail

/* Remove key unless keep says to keep it; see above. */
template <typename F>
int tree_remove_if(tree_t &tree, std::string_view key, F keep) {
  detail::latched_t l;
  if (0 != detail::latch_leaf(tree, key, l)) {
    return -1;
  }
  l.decode();
  auto &entries = l.page.entries;
  size_t lo = 0, hi = entries.size();
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (std::string_view(entries[mid].first) < key) {
      lo = mid + 1;
    } else {
      hi = mid;
    }
  }
  if (entries.size() == lo || entries[lo].first != key) {
    return 1;
  }
  int k = keep(key, std::string_view(entries[lo].second));
  if (k < 0) {
    return -1;
  }
  if (0 != k) {
    return 1;
  }
  entries.erase(entries.begin() + lo);
  return 0 == detail::write_back(tree, l) ? 0 : -1;
}

} // namespace sm2::tree

#endif

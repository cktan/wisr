/*
 * tree.cc -- a B-link tree over sm1's pages (../BTREE.md §4, §5,
 * ../../../MAJORDECISION.md D4).
 *
 * A reader descends from the root it last read, latching nothing,
 * and moves right past a page whose high key its key is at or past.
 * A writer latches the leaf, moving right with the latch coupled, and
 * writes it whole. A split is two writes: the new right page, the
 * left page linked to it and flagged incomplete, and the meta page
 * that gave the right page its id; then the parent's new entry and
 * the left page unflagged. The parent is found by a descent from the
 * meta page's root to the level above. A crash between the two
 * leaves the flag, and the next writer to latch the page finishes the
 * post before its own change. Latches are taken child before parent,
 * left before right, and the meta page's last.
 */

#include "tree.h"

#include "sm0_codec.h"

#include <algorithm>
#include <cassert>
#include <cstring>

namespace sm2::tree {

namespace {

/* The meta page's id. */
constexpr uint64_t meta_id = 0;

/* What the meta page says. */
struct meta_t {
  uint64_t root = 0;
  uint64_t next = 0;
  uint64_t height = 0;
};

/* The meta page's bytes: a tag, then root, next and height. */
constexpr char meta_tag[4] = {'B', 'T', 'M', '1'};
constexpr size_t meta_size = 4 + 8 * 3;

/* Append v to out, little-endian. */
void put_le64(std::string &out, uint64_t v) {
  for (int i = 0; i < 8; i++) {
    out.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
  }
}

/* The little-endian 64-bit integer at p. */
uint64_t get_le64(const char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) {
    v |= static_cast<uint64_t>(static_cast<uint8_t>(p[i])) << (8 * i);
  }
  return v;
}

/* An inner entry's value: its child's page id. */
std::string child_value(uint64_t pageid) {
  std::string v;
  put_le64(v, pageid);
  return v;
}

/* The page id an inner entry's value names; 0 if it is not one. */
uint64_t child_of(std::string_view value) {
  return 8 == value.size() ? get_le64(value.data()) : 0;
}

/* The meta page's bytes. */
std::string meta_bytes(const meta_t &m) {
  std::string out(meta_tag, 4);
  put_le64(out, m.root);
  put_le64(out, m.next);
  put_le64(out, m.height);
  return out;
}

/* Fail as a tree that is not one. */
int corrupt(const std::string &what) {
  return sm1::seterr(sm1::SM_ERR_CORRUPT, "tree: " + what);
}

/* The sm1 key of a page of tree. */
std::string key_of(const tree_t &tree, uint64_t pageid) {
  std::string k;
  k.reserve(10);
  k.push_back(static_cast<char>(tree.relid >> 8));
  k.push_back(static_cast<char>(tree.relid & 0xFF));
  sm0::put_be64(k, pageid);
  return k;
}

/* The name a page of tree is latched by. */
page_name_t name_of(const tree_t &tree, uint64_t pageid) {
  return page_name_t{tree.dbid, tree.storeid, tree.relid, pageid};
}

/* A page pinned in the cache and viewed; released at scope's end. */
struct pinned_t {
  sm1::page_t *pin = nullptr;
  page_view_t view;

  pinned_t() = default;
  pinned_t(const pinned_t &) = delete;
  pinned_t &operator=(const pinned_t &) = delete;
  ~pinned_t() { sm1::page_release(pin); }

  /* Release the page now. */
  void release() {
    sm1::page_release(pin);
    pin = nullptr;
  }
};

/* Read and view page pageid of tree; a missing page is corrupt. */
int read_page(tree_t &tree, uint64_t pageid, bool inner, pinned_t &out) {
  out.release();
  std::string_view bytes;
  int rc = sm1::page_read(tree.session, tree.storeid, key_of(tree, pageid),
                          inner, out.pin, bytes);
  if (rc < 0) {
    return -1;
  }
  if (1 == rc) {
    out.pin = nullptr;
    return corrupt("page " + std::to_string(pageid) + " of index " +
                   std::to_string(tree.relid) + " is missing");
  }
  return out.view.open(bytes);
}

/* Read the meta page. */
int read_meta(tree_t &tree, meta_t &m) {
  sm1::page_t *pin = nullptr;
  std::string_view bytes;
  int rc = sm1::page_read(tree.session, tree.storeid, key_of(tree, meta_id),
                          true, pin, bytes);
  if (rc < 0) {
    return -1;
  }
  if (1 == rc) {
    return corrupt("the meta page of index " + std::to_string(tree.relid) +
                   " is missing");
  }
  bool ok = meta_size == bytes.size() && 0 == memcmp(bytes.data(), meta_tag, 4);
  if (ok) {
    m.root = get_le64(bytes.data() + 4);
    m.next = get_le64(bytes.data() + 12);
    m.height = get_le64(bytes.data() + 20);
  }
  sm1::page_release(pin);
  if (!ok || 0 == m.root || 0 == m.height) {
    return corrupt("the meta page of index " + std::to_string(tree.relid) +
                   " is not one");
  }
  return 0;
}

/* A page to write: its id, its bytes, and whether it is inner. */
struct out_t {
  uint64_t pageid;
  std::string bytes;
  bool inner;
};

/* Write pages at once, outside every transaction. */
int write_pages(tree_t &tree, const std::vector<out_t> &pages) {
  std::vector<std::string> keys;
  keys.reserve(pages.size());
  std::vector<sm1::page_put_t> puts;
  puts.reserve(pages.size());
  for (const out_t &p : pages) {
    keys.push_back(key_of(tree, p.pageid));
    puts.push_back(sm1::page_put_t{keys.back(), p.bytes, false, p.inner});
  }
  return sm1::page_write(tree.session, tree.storeid, puts);
}

/* A page decoded, laid out to write, its version moved. */
out_t out_of(uint64_t pageid, page_t &page) {
  page.version++;
  return out_t{pageid, page_bytes(page), 0 != page.level};
}

/*
 * Descend to the page at level that covers key: from the root last
 * read, or from the meta page's own with from_meta; moving right past
 * each page whose high key key is at or past. Latches nothing.
 */
int descend(tree_t &tree, std::string_view key, uint8_t level, bool from_meta,
            uint64_t &out) {
  uint64_t pageid = tree.root;
  if (from_meta || 0 == pageid) {
    meta_t m;
    if (0 != read_meta(tree, m)) {
      return -1;
    }
    tree.root = pageid = m.root;
  }
  pinned_t p;
  for (;;) {
    if (0 != read_page(tree, pageid, true, p)) {
      return -1;
    }
    if (p.view.past_high(key)) {
      pageid = p.view.right();
      continue;
    }
    if (p.view.level() == level) {
      out = pageid;
      return 0;
    }
    if (p.view.level() < level || 0 == p.view.count()) {
      // A root remembered from before a split of the level asked for
      // is below it: only the meta page's root is sure to be above.
      if (!from_meta) {
        return descend(tree, key, level, true, out);
      }
      return corrupt("no page at level " + std::to_string(level));
    }
    uint64_t child = child_of(p.view.value(p.view.child(key)));
    if (0 == child) {
      return corrupt("an inner entry names no page");
    }
    pageid = child;
  }
}

/*
 * Latch page pageid and read it decoded into page, moving right,
 * the latch coupled, while key is at or past its high key.
 */
int latch_covering(tree_t &tree, std::string_view key, uint64_t &pageid,
                   page_latch_t &latch, page_t &page) {
  latch = page_latch_t(page_latches(), name_of(tree, pageid));
  pinned_t p;
  for (;;) {
    if (0 != read_page(tree, pageid, false, p)) {
      return -1;
    }
    if (!p.view.past_high(key)) {
      p.view.load(page);
      return 0;
    }
    pageid = p.view.right();
    // Latch the right page before the left's goes: left to right.
    page_latch_t next(page_latches(), name_of(tree, pageid));
    latch = std::move(next);
  }
}

int post(tree_t &tree, uint64_t childid, page_t &child, page_latch_t &latch,
         const std::string &sep, uint64_t rightid);

/*
 * Give a split's right page an id from the meta page, under its latch,
 * and write the split's first change: the right page, the left linked
 * to it and flagged incomplete, and the meta page.
 */
int write_split(tree_t &tree, uint64_t leftid, page_t &left, page_t &right,
                uint64_t &rightid, std::vector<out_t> more = {}) {
  // The meta page's latch, last of all, orders the ids it gives.
  page_latch_t ml(page_latches(), name_of(tree, meta_id));
  meta_t m;
  if (0 != read_meta(tree, m)) {
    return -1;
  }
  rightid = m.next++;
  left.right = rightid;
  left.flags |= page_incomplete;
  more.push_back(out_of(rightid, right));
  more.push_back(out_of(leftid, left));
  more.push_back(out_t{meta_id, meta_bytes(m), true});
  return write_pages(tree, more);
}

/*
 * Finish the split of a page found flagged incomplete, latched by
 * latch: post its right page to its parent. The latch is released.
 */
int finish_split(tree_t &tree, uint64_t pageid, page_t &page,
                 page_latch_t &latch) {
  std::string sep = page.high;
  return post(tree, pageid, page, latch, sep, page.right);
}

/*
 * Post a split's right page, rightid, whose least key is sep, to the
 * parent of its left page childid, which latch holds, decoded in child
 * and flagged incomplete; or make a new root over the two. The child
 * is written unflagged with the parent's change, and the latch
 * released.
 */
int post(tree_t &tree, uint64_t childid, page_t &child, page_latch_t &latch,
         const std::string &sep, uint64_t rightid) {
  if (detail::fail_posts > 0) {
    detail::fail_posts--;
    return sm1::seterr(sm1::SM_ERR_IO, "tree: a post failed, for a test");
  }
  uint8_t level = child.level + 1;
  child.flags &= static_cast<uint8_t>(~page_incomplete);
  {
    // The meta page's latch, last of all, so no root split races this.
    page_latch_t ml(page_latches(), name_of(tree, meta_id));
    meta_t m;
    if (0 != read_meta(tree, m)) {
      return -1;
    }
    if (m.root == childid) {
      // The root split: a new root over the two.
      page_t root;
      root.level = level;
      page_insert(root, "", child_value(childid), tree.limit);
      page_insert(root, sep, child_value(rightid), tree.limit);
      uint64_t rootid = m.next++;
      m.root = rootid;
      m.height = level + 1;
      std::vector<out_t> outs;
      outs.push_back(out_of(rootid, root));
      outs.push_back(out_of(childid, child));
      outs.push_back(out_t{meta_id, meta_bytes(m), true});
      if (0 != write_pages(tree, outs)) {
        return -1;
      }
      tree.root = rootid;
      latch.release();
      return 0;
    }
  }
  for (;;) {
    uint64_t parentid = 0;
    if (0 != descend(tree, sep, level, true, parentid)) {
      return -1;
    }
    page_latch_t pl;
    page_t parent;
    if (0 != latch_covering(tree, sep, parentid, pl, parent)) {
      return -1;
    }
    if (0 != (parent.flags & page_incomplete)) {
      // A split of the parent a crash left half done: finish it, then
      // find the parent again.
      if (0 != finish_split(tree, parentid, parent, pl)) {
        return -1;
      }
      continue;
    }
    int rc = page_insert(parent, sep, child_value(rightid), tree.limit);
    if (rc < 0) {
      return -1;
    }
    if (1 == rc) {
      // Posted already, by a post a crash cut short before it wrote
      // the child: unflag the child alone.
      std::vector<out_t> outs{out_of(childid, child)};
      return write_pages(tree, outs);
    }
    if (0 == rc) {
      std::vector<out_t> outs{out_of(parentid, parent), out_of(childid, child)};
      if (0 != write_pages(tree, outs)) {
        return -1;
      }
      latch.release();
      return 0;
    }
    // The parent is full: split it with the entry, unflag the child in
    // the same write, then post the parent's split a level up.
    page_t pright;
    page_split(parent, pright, sep, child_value(rightid), tree.limit);
    uint64_t prightid = 0;
    std::vector<out_t> outs{out_of(childid, child)};
    if (0 != write_split(tree, parentid, parent, pright, prightid, outs)) {
      return -1;
    }
    latch.release();
    std::string psep = pright.entries.front().first;
    return post(tree, parentid, parent, pl, psep, prightid);
  }
}

} // namespace

/* Open a tree; see tree.h. */
void tree_open(tree_t &tree, sm1::session_t *session, uint64_t storeid,
               uint16_t relid, size_t limit) {
  tree = tree_t{};
  tree.session = session;
  tree.dbid = sm1::session_dbid(session);
  tree.storeid = storeid;
  tree.relid = relid;
  tree.limit = limit;
}

/* Make an empty tree; see tree.h. */
int tree_create(tree_t &tree) {
  if (0 != tree_drop(tree.session, tree.storeid, tree.relid)) {
    return -1;
  }
  meta_t m;
  m.root = 1;
  m.next = 2;
  m.height = 1;
  page_t leaf;
  std::vector<out_t> outs{out_of(1, leaf), out_t{meta_id, meta_bytes(m), true}};
  if (0 != write_pages(tree, outs)) {
    return -1;
  }
  tree.root = 1;
  return 0;
}

/* Remove a tree's pages; see tree.h. */
int tree_drop(sm1::session_t *session, uint64_t storeid, uint16_t relid) {
  std::string prefix;
  prefix.push_back(static_cast<char>(relid >> 8));
  prefix.push_back(static_cast<char>(relid & 0xFF));
  return sm1::page_drop(session, storeid, prefix);
}

namespace detail {

int fail_posts = 0;

/* Find and latch the leaf that holds key; see tree.h. */
int latch_leaf(tree_t &tree, std::string_view key, latched_t &out) {
  for (;;) {
    uint64_t pageid = 0;
    if (0 != descend(tree, key, 0, false, pageid)) {
      return -1;
    }
    if (0 != latch_covering(tree, key, pageid, out.latch, out.page)) {
      return -1;
    }
    if (0 != (out.page.flags & page_incomplete)) {
      // A split a crash left half done: finish it, and look again.
      if (0 != finish_split(tree, pageid, out.page, out.latch)) {
        return -1;
      }
      continue;
    }
    out.pageid = pageid;
    return 0;
  }
}

/* Write a latched page back; see tree.h. */
int write_back(tree_t &tree, latched_t &l) {
  std::vector<out_t> outs{out_of(l.pageid, l.page)};
  return write_pages(tree, outs);
}

} // namespace detail

/* Insert a key; see tree.h. */
int tree_insert(tree_t &tree, std::string_view key, std::string_view value) {
  // Its key must fit an inner entry too, whose value is a page id.
  if (entry_size(key.size(), std::max<size_t>(value.size(), 8)) >
      entry_max(tree.limit)) {
    return sm1::seterr(sm1::SM_ERR_KEYTOOLONG,
                       "tree: an entry over a third of its page");
  }
  detail::latched_t l;
  if (0 != detail::latch_leaf(tree, key, l)) {
    return -1;
  }
  int rc = page_insert(l.page, key, value, tree.limit);
  if (rc < 0) {
    return -1;
  }
  if (1 == rc) {
    return 1;
  }
  if (0 == rc) {
    return detail::write_back(tree, l);
  }
  page_t right;
  page_split(l.page, right, key, value, tree.limit);
  uint64_t rightid = 0;
  if (0 != write_split(tree, l.pageid, l.page, right, rightid)) {
    return -1;
  }
  std::string sep = right.entries.front().first;
  return post(tree, l.pageid, l.page, l.latch, sep, rightid);
}

/* Open a cursor; see tree.h. */
void cursor_open(tree_t &tree, cursor_t &cursor, const std::string *lo,
                 bool lo_inclusive, const std::string *hi, bool hi_inclusive) {
  cursor = cursor_t{};
  cursor.tree = &tree;
  if (nullptr != lo) {
    cursor.from = *lo;
    cursor.from_inclusive = lo_inclusive;
  }
  if (nullptr != hi) {
    cursor.has_hi = true;
    cursor.hi = *hi;
    cursor.hi_inclusive = hi_inclusive;
  }
}

/* Move a cursor forward; see tree.h. */
void cursor_seek(cursor_t &cursor, const std::string &lo, bool inclusive) {
  cursor.from = lo;
  cursor.from_inclusive = inclusive;
  cursor.started = false;
  cursor.batch.clear();
  cursor.at = 0;
}

namespace {

/* Whether key is past the cursor's upper bound. */
bool past_hi(const cursor_t &c, std::string_view key) {
  if (!c.has_hi) {
    return false;
  }
  return c.hi_inclusive ? key > c.hi : key >= c.hi;
}

/*
 * Read the next batch: the keys past from, or at it, in range, of the
 * first leaf from the cursor's on that has any, moving right.
 */
int fill(cursor_t &c) {
  tree_t &tree = *c.tree;
  c.batch.clear();
  c.at = 0;
  uint64_t pageid = c.leaf;
  if (!c.started) {
    if (0 != descend(tree, c.from, 0, false, pageid)) {
      return -1;
    }
    c.started = true;
  }
  pinned_t p;
  for (;;) {
    if (0 != read_page(tree, pageid, false, p)) {
      return -1;
    }
    const page_view_t &v = p.view;
    size_t i = v.lower_bound(c.from);
    if (i < v.count() && !c.from_inclusive && v.key(i) == c.from) {
      i++;
    }
    for (; i < v.count(); i++) {
      if (past_hi(c, v.key(i))) {
        c.done = true;
        break;
      }
      c.batch.emplace_back(std::string(v.key(i)), std::string(v.value(i)));
    }
    c.leaf = pageid;
    if (!c.batch.empty() || c.done) {
      return 0;
    }
    // Nothing here: every key to the right is at or past the high key.
    if (0 == v.right() || (v.has_high() && past_hi(c, v.high()))) {
      c.done = true;
      return 0;
    }
    pageid = v.right();
  }
}

} // namespace

/* The next key of a cursor; see tree.h. */
int cursor_next(cursor_t &cursor, std::string_view &key,
                std::string_view &value) {
  if (cursor.at == cursor.batch.size()) {
    if (cursor.done) {
      return 1;
    }
    // The next batch starts past the last key handed out.
    if (!cursor.batch.empty()) {
      cursor.from = cursor.batch.back().first;
      cursor.from_inclusive = false;
    }
    if (0 != fill(cursor)) {
      return -1;
    }
    if (cursor.batch.empty()) {
      return 1;
    }
  }
  const auto &e = cursor.batch[cursor.at++];
  key = e.first;
  value = e.second;
  return 0;
}

/* The tree's height; see tree.h. */
int tree_height(tree_t &tree, size_t &height) {
  meta_t m;
  if (0 != read_meta(tree, m)) {
    return -1;
  }
  height = m.height;
  return 0;
}

/* Check a whole tree; see tree.h. */
int tree_check(tree_t &tree) {
  meta_t m;
  if (0 != read_meta(tree, m)) {
    return -1;
  }
  // Walk each level from its leftmost page, which the level above's
  // first entry names, checking order, links and high keys.
  uint64_t first = m.root;
  for (size_t level = m.height; level-- > 0;) {
    uint64_t pageid = first;
    uint64_t next_first = 0;
    std::string low; /* the least key the page may hold */
    pinned_t p;
    while (0 != pageid) {
      if (0 != read_page(tree, pageid, level > 0, p)) {
        return -1;
      }
      const page_view_t &v = p.view;
      if (v.level() != level) {
        return corrupt("page " + std::to_string(pageid) + " at level " +
                       std::to_string(v.level()) + ", not " +
                       std::to_string(level));
      }
      if (0 != (v.flags() & page_incomplete)) {
        return corrupt("page " + std::to_string(pageid) + " left incomplete");
      }
      for (size_t i = 0; i < v.count(); i++) {
        if (i > 0 && !(v.key(i - 1) < v.key(i))) {
          return corrupt("keys out of order on page " + std::to_string(pageid));
        }
        if (v.key(i) < low || (v.has_high() && v.key(i) >= v.high())) {
          return corrupt("a key out of its page's range on page " +
                         std::to_string(pageid));
        }
      }
      if (level > 0 && 0 == next_first) {
        if (0 == v.count()) {
          return corrupt("an empty inner page");
        }
        next_first = child_of(v.value(0));
      }
      if (v.has_high() == (0 == v.right())) {
        return corrupt("page " + std::to_string(pageid) +
                       " has a high key but no right link, or the reverse");
      }
      low = std::string(v.high());
      pageid = v.right();
    }
    first = next_first;
  }
  return 0;
}

} // namespace sm2::tree

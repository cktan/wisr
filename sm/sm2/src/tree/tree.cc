/*
 * tree.cc -- a B-link tree over sm1's pages (../BTREE.md §4, §5,
 * ../../../MAJORDECISION.md D4).
 *
 * A pin is a latch (../../BUFFERPOOL.md). A reader descends from page
 * 0, the root, pinning one page at a time, shared, and moves right
 * past a page whose high key its key is at or past: B-link needs no
 * coupling, so a reader holds no page while it waits for another. A
 * writer pins the leaf exclusive, moving right with the pin coupled,
 * and writes it whole.
 *
 * A split is two writes:
 * 1. Write the new right page, and the left page linked to it and
 *    flagged incomplete.
 * 2. Find the parent by a descent to the level above. Write the
 *    parent's new entry, and the left page unflagged.
 * A crash between the two leaves the flag, and the next writer to
 * latch the page finishes the post before its own change.
 *
 * The root's split is one write: its halves to new pages, and page 0
 * a level up. So page 0 alone changes level, and whoever reads it
 * again by its id checks it is at the level wanted. Exclusive pins
 * are taken child before parent, left before right, and a shared pin
 * held under one only goes up or right.
 */

#include "tree.h"

#include "sm0_codec.h"
#include "sm2.h"

#include <algorithm>
#include <cassert>

namespace sm2::tree {

namespace {

/* The root's page id, for the tree's life. */
constexpr uint64_t root_id = 0;

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

/* The little-endian 16-bit integer at p. */
size_t get_le16(const char *p) {
  return static_cast<uint8_t>(p[0]) |
         static_cast<size_t>(static_cast<uint8_t>(p[1])) << 8;
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

using detail::pinned_t;

/*
 * Pin and view page pageid into out, empty, by a store's handle if
 * given; a missing page is corrupt.
 */
int pin_into(tree_t &tree, uint64_t pageid, bool inner, sm1::page_mode_t mode,
             pinned_t &out, const sm1::page_store_t *store = nullptr) {
  out.session = tree.session;
  std::string_view bytes;
  unsigned flags = inner ? sm1::page_inner : 0;
  int rc =
      nullptr != store
          ? sm1::pin_page(tree.session, tree.storeid, store,
                          key_of(tree, pageid), mode, flags, out.pin, bytes)
          : sm1::pin_page(tree.session, tree.storeid, key_of(tree, pageid),
                          mode, flags, out.pin, bytes);
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

/*
 * Pin and view page pageid, shared. out's page goes first: a reader
 * holding a page while it waits for a child could wait on a writer
 * that is waiting for the page it holds.
 */
int read_page(tree_t &tree, uint64_t pageid, bool inner, pinned_t &out,
              const sm1::page_store_t *store = nullptr) {
  out.release();
  return pin_into(tree, pageid, inner, sm1::page_mode_t::sh, out, store);
}

/*
 * Pin and view page pageid to change it. out's page goes only once this
 * one is held, so a move right is coupled.
 */
int pin_ex(tree_t &tree, uint64_t pageid, bool inner, pinned_t &out) {
  pinned_t next;
  // Wait for the page's other pins before reading what it may change.
  if (0 != pin_into(tree, pageid, inner, sm1::page_mode_t::ex, next)) {
    return -1;
  }
  out.release();
  out.session = next.session;
  out.pin = next.pin;
  out.view = next.view;
  next.pin = nullptr;
  return 0;
}

/* Pin new page pageid to write it: nothing is read for it. */
int pin_new(tree_t &tree, uint64_t pageid, bool inner, pinned_t &out) {
  out.release();
  out.session = tree.session;
  std::string_view bytes;
  return sm1::pin_page(
      tree.session, tree.storeid, key_of(tree, pageid), sm1::page_mode_t::ex,
      sm1::page_no_read | (inner ? sm1::page_inner : 0), out.pin, bytes);
}

/* A page to write: its id, its bytes, and its exclusive pin. */
struct out_t {
  uint64_t pageid;
  std::string bytes;
  sm1::page_t *pin;
};

/* Write pages at once, outside every transaction. */
int write_pages(tree_t &tree, const std::vector<out_t> &pages) {
  std::vector<std::string> keys;
  keys.reserve(pages.size());
  std::vector<sm1::page_put_t> puts;
  puts.reserve(pages.size());
  for (const out_t &p : pages) {
    keys.push_back(key_of(tree, p.pageid));
    puts.push_back(sm1::page_put_t{p.pin, keys.back(), p.bytes});
  }
  return sm1::page_write(tree.session, tree.storeid, puts);
}

/*
 * A new page's id, from the database's sequence, looked up once by
 * its name, a read of sm1's catalog, at the tree's first split.
 */
int next_page(tree_t &tree, uint64_t &id) {
  if (!tree.has_seq) {
    if (0 != sm1::seqref(tree.session, tree_page_seq, tree.seq)) {
      return -1;
    }
    tree.has_seq = true;
  }
  return sm1::nextseq(tree.session, tree.seq, id);
}

/* A page decoded, laid out to write under pin, its version moved. */
out_t out_of(uint64_t pageid, page_t &page, sm1::page_t *pin) {
  page.version++;
  return out_t{pageid, page_bytes(page), pin};
}

/*
 * Descend to the page at level that covers key, from page 0, one page
 * pinned at a time. leaf, if given, takes the page found, still
 * pinned, so it is not read again.
 * 1. If page 0 is above level, read it from the session's own copy of
 *    it, with no pin, while the copy is the page (../../BTREE.md §4.2).
 * 2. Else pin the page shared. Keep a copy of page 0 if it is above
 *    level.
 * 3. If key is at or past the page's high key, move right to the next
 *    page, and go to step 2.
 * 4. If the page is at level, return it.
 * 5. Else move down to the child that covers key, and go to step 2.
 */
int descend(tree_t &tree, std::string_view key, uint8_t level, uint64_t &out,
            pinned_t *leaf = nullptr) {
  // The store's checks once a descent, not once a page: a probe's
  // pins are its cost (../../BUFFERPOOL.md §4.6).
  const sm1::page_store_t *store = nullptr;
  if (0 != sm1::page_enter(tree.session, tree.storeid, store)) {
    return -1;
  }
  uint64_t pageid = root_id;
  pinned_t p;
  // The page to read next; nullptr until it is pinned. A copy of page
  // 0 stale by a split since is as good as the page read just before
  // the split: the pages it names are still there, and their right
  // links lead on.
  const std::string root_key = key_of(tree, root_id);
  page_view_t copy;
  const page_view_t *v = nullptr;
  std::string_view bytes;
  if (sm1::page_copy(tree.session, tree.storeid, root_key, bytes)) {
    if (0 != copy.open(bytes)) {
      return -1;
    }
    if (copy.level() > level) {
      v = &copy;
    }
  }
  // Whether the page to read is inner: the root is taken to be, as
  // every descent reads it; below it, the page above says.
  bool inner = true;
  for (;;) {
    if (nullptr == v) {
      if (0 != read_page(tree, pageid, inner, p, store)) {
        return -1;
      }
      v = &p.view;
      // A copy only of a page 0 a descent passes through, so it moves
      // only with an inner page's split.
      if (root_id == pageid && v->level() > level) {
        sm1::keep_page_copy(tree.session, tree.storeid, root_key, p.pin,
                            v->bytes());
      }
    }
    if (v->past_high(key)) {
      pageid = v->next();
      inner = 0 != v->level();
      v = nullptr;
      continue;
    }
    if (v->level() == level) {
      out = pageid;
      if (nullptr != leaf) {
        leaf->release();
        leaf->session = p.session;
        leaf->pin = p.pin;
        leaf->view = p.view;
        p.pin = nullptr;
      }
      return 0;
    }
    if (v->level() < level || 0 == v->count()) {
      return corrupt("no page at level " + std::to_string(level));
    }
    uint64_t child = child_of(v->value(v->child(key)));
    if (0 == child) {
      return corrupt("an inner entry names no page");
    }
    inner = v->level() > 1;
    pageid = child;
    v = nullptr;
  }
}

/*
 * Pin page pageid, of level, exclusive into p, moving right, the pin
 * coupled, while key is at or past its high key: 0; 1 if it is page 0,
 * at another level by now, for the caller to descend again; or -1.
 */
int latch_covering(tree_t &tree, std::string_view key, uint64_t &pageid,
                   pinned_t &p, uint8_t level) {
  for (;;) {
    if (0 != pin_ex(tree, pageid, 0 != level, p)) {
      return -1;
    }
    if (level != p.view.level()) {
      return root_id == pageid ? 1 : corrupt("a page left its level");
    }
    if (!p.view.past_high(key)) {
      // A writer checks every entry of the page it is to change.
      return p.view.check();
    }
    // The right page is pinned before the left's goes: left to right.
    pageid = p.view.next();
  }
}

/* latch_covering, the page decoded into page. */
int latch_covering(tree_t &tree, std::string_view key, uint64_t &pageid,
                   pinned_t &p, page_t &page, uint8_t level) {
  int rc = latch_covering(tree, key, pageid, p, level);
  if (0 != rc) {
    return rc;
  }
  p.view.load(page);
  return 0;
}

int post(tree_t &tree, uint64_t childid, page_t &child, pinned_t &pin,
         const std::string &sep, uint64_t rightid);

/*
 * Give a split's right page an id, and write the split's first change,
 * so prev and next agree however a crash cuts the split. leftpin holds
 * the left page.
 * 1. Take the right page's id from the sequence, and pin the page new.
 * 2. If the left page has an old right sibling, pin it exclusive, and
 *    link it back to the right page.
 * 3. Link the left page and the right page to each other, and flag the
 *    left page incomplete.
 * 4. Write the right page, the left page, the sibling and more's pages
 *    in one write.
 */
int write_split(tree_t &tree, uint64_t leftid, page_t &left,
                sm1::page_t *leftpin, page_t &right, uint64_t &rightid,
                std::vector<out_t> more = {}) {
  if (0 != next_page(tree, rightid)) {
    return -1;
  }
  bool inner = 0 != left.level;
  pinned_t rightpin;
  if (0 != pin_new(tree, rightid, inner, rightpin)) {
    return -1;
  }
  pinned_t sibling;
  if (0 != right.next) {
    // Left to right: the sibling's writers wait while its link moves.
    if (0 != pin_ex(tree, right.next, inner, sibling) ||
        0 != sibling.view.check()) {
      return -1;
    }
    page_t page;
    sibling.view.load(page);
    page.prev = rightid;
    more.push_back(out_of(right.next, page, sibling.pin));
  }
  left.next = rightid;
  left.flags |= page_incomplete;
  right.prev = leftid;
  more.push_back(out_of(rightid, right, rightpin.pin));
  more.push_back(out_of(leftid, left, leftpin));
  return write_pages(tree, more);
}

/*
 * Split the root, page 0, whose halves are left and right, in one
 * write. It is never half done, so it is never flagged. rootpin holds
 * page 0.
 * 1. Take two new page ids, and pin the two pages new.
 * 2. Make page 0 a level up, the halves' parent, with an entry for each.
 * 3. Link the halves to each other.
 * 4. Write the halves to the new pages, page 0 and more's pages in one
 *    write.
 */
int split_root(tree_t &tree, sm1::page_t *rootpin, page_t &left, page_t &right,
               std::vector<out_t> more = {}) {
  uint64_t leftid = 0, rightid = 0;
  if (0 != next_page(tree, leftid) || 0 != next_page(tree, rightid)) {
    return -1;
  }
  pinned_t leftpin, rightpin;
  if (0 != pin_new(tree, leftid, 0 != left.level, leftpin) ||
      0 != pin_new(tree, rightid, 0 != left.level, rightpin)) {
    return -1;
  }
  page_t root;
  root.level = left.level + 1;
  root.version = left.version;
  page_insert(root, "", child_value(leftid), tree.limit);
  page_insert(root, right.entries.front().first, child_value(rightid),
              tree.limit);
  left.next = rightid;
  right.prev = leftid;
  more.push_back(out_of(leftid, left, leftpin.pin));
  more.push_back(out_of(rightid, right, rightpin.pin));
  more.push_back(out_of(root_id, root, rootpin));
  return write_pages(tree, more);
}

/*
 * Finish the split of a page found flagged incomplete, pinned by pin:
 * post its right page to its parent. The pin is released.
 */
int finish_split(tree_t &tree, uint64_t pageid, page_t &page, pinned_t &pin) {
  std::string sep = page.high;
  return post(tree, pageid, page, pin, sep, page.next);
}

/*
 * Post a split's right page, rightid, whose least key is sep, to the
 * parent of its left page childid, which pin holds, decoded in child
 * and flagged incomplete; or make a new root over the two. The child
 * is written unflagged with the parent's change, and the pin released.
 * 1. Clear the child's incomplete flag, in child.
 * 2. Descend to the level above, and latch the page there that covers
 *    sep, the parent.
 * 3. If page 0 split past this level meanwhile, go to step 2.
 * 4. If the parent is flagged incomplete, finish its split, and go to
 *    step 2.
 * 5. Insert the entry, sep and rightid, into the parent. If it is there
 *    already, a post that a crash cut short made it: write the child
 *    alone.
 * 6. If the parent has room, write the parent and the child in one
 *    write.
 * 7. If the parent is full, split it with the entry, and write the
 *    child in the same write. If the parent is page 0, split_root
 *    makes it a level up; else post the parent's split a level up.
 */
int post(tree_t &tree, uint64_t childid, page_t &child, pinned_t &pin,
         const std::string &sep, uint64_t rightid) {
  if (detail::fail_posts > 0) {
    detail::fail_posts--;
    return sm1::seterr(sm1::SM_ERR_IO, "tree: a post failed, for a test");
  }
  uint8_t level = child.level + 1;
  child.flags &= static_cast<uint8_t>(~page_incomplete);
  for (;;) {
    uint64_t parentid = 0;
    if (0 != descend(tree, sep, level, parentid)) {
      return -1;
    }
    pinned_t pl;
    page_t parent;
    int found = latch_covering(tree, sep, parentid, pl, parent, level);
    if (found < 0) {
      return -1;
    }
    if (1 == found) {
      // The root split past this level: the parent is a page below it.
      continue;
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
      std::vector<out_t> outs{out_of(childid, child, pin.pin)};
      if (0 != write_pages(tree, outs)) {
        return -1;
      }
      pin.release();
      return 0;
    }
    if (0 == rc) {
      std::vector<out_t> outs{out_of(parentid, parent, pl.pin),
                              out_of(childid, child, pin.pin)};
      if (0 != write_pages(tree, outs)) {
        return -1;
      }
      pin.release();
      return 0;
    }
    // The parent is full: split it with the entry, unflag the child in
    // the same write, then post the parent's split a level up, unless
    // it is the root, which has no level up to post to.
    page_t pright;
    page_split(parent, pright, sep, child_value(rightid), tree.limit);
    std::vector<out_t> outs{out_of(childid, child, pin.pin)};
    if (root_id == parentid) {
      if (0 != split_root(tree, pl.pin, parent, pright, std::move(outs))) {
        return -1;
      }
      pin.release();
      return 0;
    }
    uint64_t prightid = 0;
    if (0 !=
        write_split(tree, parentid, parent, pl.pin, pright, prightid, outs)) {
      return -1;
    }
    pin.release();
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
  tree.storeid = storeid;
  tree.relid = relid;
  tree.limit = limit;
}

/* Make an empty tree; see tree.h. */
int tree_create(tree_t &tree) {
  if (0 != tree_drop(tree.session, tree.storeid, tree.relid)) {
    return -1;
  }
  pinned_t root;
  if (0 != pin_new(tree, root_id, true, root)) {
    return -1;
  }
  page_t leaf;
  std::vector<out_t> outs{out_of(root_id, leaf, root.pin)};
  return write_pages(tree, outs);
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
    if (0 != descend(tree, key, 0, pageid)) {
      return -1;
    }
    int found = latch_covering(tree, key, pageid, out.pin, 0);
    if (found < 0) {
      return -1;
    }
    if (1 == found) {
      // Page 0 was a leaf when the descent read it, and has split.
      out.pin.release();
      continue;
    }
    if (0 != (out.pin.view.flags() & page_incomplete)) {
      // A split a crash left half done: finish it, and look again.
      out.decode();
      if (0 != finish_split(tree, pageid, out.page, out.pin)) {
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
  std::vector<out_t> outs{out_of(l.pageid, l.page, l.pin.pin)};
  return write_pages(tree, outs);
}

} // namespace detail

namespace {

/* Refuse an entry whose key would not fit an inner entry, a page id its value.
 */
int check_entry(const tree_t &tree, std::string_view key,
                std::string_view value) {
  if (entry_size(key.size(), std::max<size_t>(value.size(), 8)) >
      entry_max(tree.limit)) {
    return sm1::seterr(sm1::SM_ERR_KEYTOOLONG,
                       "tree: an entry over a third of its page");
  }
  return 0;
}

/*
 * Insert key with value into the leaf l latches, which covers it,
 * splitting it if full: 0, 1 if the key is there already, or -1.
 */
int insert_into(tree_t &tree, detail::latched_t &l, std::string_view key,
                std::string_view value) {
  // Into the page's bytes, decoding no entry, while it has room.
  std::vector<out_t> outs(1);
  int rc = page_insert_bytes(l.pin.view, key, value, tree.limit, outs[0].bytes);
  if (rc < 0) {
    return -1;
  }
  if (1 == rc) {
    // Found there, as an update A to B and back finds A's: the row has
    // just changed, so its leaf is not all visible (../BTREE.md §4.1).
    if (0 == (l.pin.view.flags() & page_all_visible)) {
      return 1;
    }
    l.decode();
    l.page.flags &= static_cast<uint8_t>(~page_all_visible);
    return 0 == detail::write_back(tree, l) ? 1 : -1;
  }
  if (0 == rc) {
    outs[0].pageid = l.pageid;
    outs[0].pin = l.pin.pin;
    return write_pages(tree, outs);
  }
  // Full: decoded, and split with the entry.
  l.decode();
  page_t right;
  page_split(l.page, right, key, value, tree.limit);
  if (root_id == l.pageid) {
    return split_root(tree, l.pin.pin, l.page, right);
  }
  uint64_t rightid = 0;
  if (0 != write_split(tree, l.pageid, l.page, l.pin.pin, right, rightid)) {
    return -1;
  }
  std::string sep = right.entries.front().first;
  return post(tree, l.pageid, l.page, l.pin, sep, rightid);
}

/* Whether key begins with prefix. */
bool begins(std::string_view key, std::string_view prefix) {
  return key.size() >= prefix.size() &&
         0 == key.compare(0, prefix.size(), prefix);
}

} // namespace

/* Insert a key; see tree.h. */
int tree_insert(tree_t &tree, std::string_view key, std::string_view value) {
  if (0 != check_entry(tree, key, value)) {
    return -1;
  }
  detail::latched_t l;
  if (0 != detail::latch_leaf(tree, key, l)) {
    return -1;
  }
  return insert_into(tree, l, key, value);
}

/* Clear the all-visible bit of key's leaf; see tree.h. */
int tree_unset_visible(tree_t &tree, std::string_view key) {
  detail::latched_t l;
  // Latched even to read the bit: a vacuum judging the leaf holds the
  // latch from its first row read to its write, so this writer, whose
  // row is written already, either sees the bit vacuum set or had its
  // row seen by vacuum, unsettled (../BTREE.md §4.1).
  if (0 != detail::latch_leaf(tree, key, l)) {
    return -1;
  }
  if (0 == (l.pin.view.flags() & page_all_visible)) {
    return 0;
  }
  l.decode();
  l.page.flags &= static_cast<uint8_t>(~page_all_visible);
  return detail::write_back(tree, l);
}

/* Insert a key unless an entry of its prefix conflicts; see tree.h. */
int tree_insert_unique(
    tree_t &tree, std::string_view prefix, std::string_view key,
    std::string_view value,
    const std::function<int(std::string_view, std::string_view)> &check) {
  if (0 != check_entry(tree, key, value)) {
    return -1;
  }
  for (;;) {
    // The leaf that covers prefix, latched until the key is in: every
    // insert of the prefix waits here.
    detail::latched_t first;
    if (0 != detail::latch_leaf(tree, prefix, first)) {
      return -1;
    }
    // Judge every entry of the prefix, on this leaf and to its right,
    // the right ones read without a latch: an insert of the prefix
    // waits for this one's latch, and a split only moves them right.
    auto judge = [&](std::string_view k, std::string_view v) {
      // The key's own entry, there already, is no conflict.
      if (k == key) {
        return 0;
      }
      return check(k, v);
    };
    const page_view_t &fv = first.pin.view;
    bool more = true;
    for (size_t i = fv.lower_bound(prefix); i < fv.count(); i++) {
      if (!begins(fv.key(i), prefix)) {
        more = false;
        break;
      }
      int r = judge(fv.key(i), fv.value(i));
      if (0 != r) {
        return r == 2 ? 2 : -1;
      }
    }
    uint64_t next = fv.next();
    pinned_t p;
    while (more && 0 != next) {
      if (0 != read_page(tree, next, false, p)) {
        return -1;
      }
      const page_view_t &v = p.view;
      for (size_t j = 0; j < v.count(); j++) {
        if (!begins(v.key(j), prefix)) {
          more = false;
          break;
        }
        int r = judge(v.key(j), v.value(j));
        if (0 != r) {
          return r == 2 ? 2 : -1;
        }
      }
      next = v.next();
    }
    p.release();
    // The key goes on the first leaf, or on one to its right, latched
    // left to right, the first still held; a key there already is
    // found there by insert_into, which clears its leaf's all-visible
    // bit.
    if (!fv.has_high() || key < fv.high()) {
      return insert_into(tree, first, key, value);
    }
    detail::latched_t cur;
    cur.pageid = fv.next();
    bool restart = false;
    for (;;) {
      if (0 != pin_ex(tree, cur.pageid, false, cur.pin) ||
          0 != cur.pin.view.check()) {
        return -1;
      }
      const page_view_t &cv = cur.pin.view;
      if (0 != (cv.flags() & page_incomplete)) {
        // A split a crash left half done: finish it, and begin again.
        cur.decode();
        if (0 != finish_split(tree, cur.pageid, cur.page, cur.pin)) {
          return -1;
        }
        restart = true;
        break;
      }
      if (cv.has_high() && key >= cv.high()) {
        cur.pageid = cv.next();
        continue;
      }
      break;
    }
    if (restart) {
      continue;
    }
    return insert_into(tree, cur, key, value);
  }
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

/* Aim a cursor at new bounds; see tree.h. */
void cursor_rebound(cursor_t &cursor, const std::string *lo, bool lo_inclusive,
                    const std::string *hi, bool hi_inclusive) {
  std::string bytes = std::move(cursor.bytes);
  cursor_open(*cursor.tree, cursor, lo, lo_inclusive, hi, hi_inclusive);
  bytes.clear();
  cursor.bytes = std::move(bytes);
}

/* Move a cursor forward; see tree.h. */
void cursor_seek(cursor_t &cursor, const std::string &lo, bool inclusive) {
  cursor.from = lo;
  cursor.from_inclusive = inclusive;
  cursor.started = false;
  cursor.at = cursor.end = cursor.pos = 0;
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
 * first leaf from the cursor's on that has any, moving right; those
 * entries copied end to end, once, and read from the copy.
 */
int fill(cursor_t &c) {
  tree_t &tree = *c.tree;
  c.at = c.end = c.pos = 0;
  uint64_t pageid = c.leaf;
  pinned_t p;
  // The first leaf, as the descent read it.
  if (!c.started) {
    if (0 != descend(tree, c.from, 0, pageid, &p)) {
      return -1;
    }
    c.started = true;
  }
  for (;;) {
    if (nullptr == p.pin && 0 != read_page(tree, pageid, false, p)) {
      return -1;
    }
    // Page 0, a leaf when the cursor read it, has split: its keys are
    // on pages below it now.
    if (0 != p.view.level()) {
      p.release();
      if (0 != descend(tree, c.from, 0, pageid, &p)) {
        return -1;
      }
    }
    const page_view_t &v = p.view;
    size_t i = v.lower_bound(c.from);
    if (i < v.count() && !c.from_inclusive && v.key(i) == c.from) {
      i++;
    }
    size_t j = i;
    while (j < v.count() && !past_hi(c, v.key(j))) {
      j++;
    }
    c.done = j < v.count();
    c.leaf = pageid;
    c.visible = 0 != (v.flags() & page_all_visible);
    if (j > i) {
      // The leaf is let go before the caller reads rows (../BTREE.md
      // §4): the entries in range are copied end to end, in key order,
      // and read from the copy.
      c.bytes.clear();
      for (size_t k = i; k < j; k++) {
        std::string_view e = v.entry(k);
        if (e.empty()) {
          return sm1::seterr(sm1::SM_ERR_CORRUPT,
                             "tree: a leaf's entries past its end");
        }
        c.bytes.append(e);
      }
      c.at = 0;
      c.end = j - i;
      c.pos = 0;
      return 0;
    }
    if (c.done) {
      return 0;
    }
    // Nothing here: every key to the right is at or past the high key.
    if (0 == v.next() || (v.has_high() && past_hi(c, v.high()))) {
      c.done = true;
      return 0;
    }
    pageid = v.next();
    p.release();
  }
}

} // namespace

/* The next key of a cursor; see tree.h. */
int cursor_next(cursor_t &cursor, std::string_view &key,
                std::string_view &value) {
  if (cursor.at == cursor.end) {
    if (cursor.done) {
      return 1;
    }
    // The next batch starts past the last key handed out.
    if (cursor.end > 0) {
      cursor.from.assign(cursor.last);
      cursor.from_inclusive = false;
    }
    if (0 != fill(cursor)) {
      return -1;
    }
    if (cursor.at == cursor.end) {
      return 1;
    }
  }
  // The next entry of the copy: a key's length, the key, a value's
  // length, the value.
  const std::string &b = cursor.bytes;
  size_t at = cursor.pos;
  if (at + 2 > b.size()) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "tree: a leaf's entry cut short");
  }
  size_t klen = get_le16(b.data() + at);
  if (at + 2 + klen + 2 > b.size()) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "tree: a leaf's entry cut short");
  }
  size_t vlen = get_le16(b.data() + at + 2 + klen);
  if (at + 2 + klen + 2 + vlen > b.size()) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "tree: a leaf's entry cut short");
  }
  key = std::string_view(b.data() + at + 2, klen);
  value = std::string_view(b.data() + at + 2 + klen + 2, vlen);
  cursor.last = key;
  cursor.pos = at + 2 + klen + 2 + vlen;
  cursor.at++;
  return 0;
}

namespace {

/*
 * Remove from leaf pageid each entry of gone, sorted, that dead says
 * again is dead, the leaf latched: an insert that found its entry there
 * changed nothing and moved no version, but it wrote its row first, so
 * only asking again sees it. next takes the leaf's next link. 0; 1 if
 * pageid is page 0, a leaf no longer; or -1.
 */
int prune_leaf(
    tree_t &tree, uint64_t pageid, const std::vector<std::string> &gone,
    const std::function<int(std::string_view, std::string_view)> &dead,
    size_t &removed, uint64_t &next) {
  detail::latched_t l;
  l.pageid = pageid;
  // The leaf's writers wait while what is to go is judged again.
  if (0 != pin_ex(tree, pageid, false, l.pin) || 0 != l.pin.view.check()) {
    return -1;
  }
  if (0 != l.pin.view.level()) {
    return 1;
  }
  l.decode();
  next = l.page.next;
  auto &entries = l.page.entries;
  size_t kept = 0;
  for (size_t i = 0; i < entries.size(); i++) {
    int d = 0;
    if (std::binary_search(gone.begin(), gone.end(), entries[i].first)) {
      d = dead(entries[i].first, entries[i].second);
    }
    if (d < 0) {
      return -1;
    }
    // Kept unless dead: 0 or 2, settled or not.
    if (1 != d) {
      if (kept != i) {
        entries[kept] = std::move(entries[i]);
      }
      kept++;
    }
  }
  size_t went = entries.size() - kept;
  entries.resize(kept);
  if (0 != went) {
    if (0 != detail::write_back(tree, l)) {
      return -1;
    }
    removed += went;
  }
  return 0;
}

/*
 * Judge every entry of leaf pageid again: 0; 1 if pageid is page 0, a
 * leaf no longer; or -1. next takes the leaf's next link.
 * 1. Latch the leaf. The latch is held from the first row read to the
 *    write.
 * 2. Remove the entries dead says are dead.
 * 3. Set the leaf all visible if every one left is settled (2).
 * 4. Write the leaf if an entry went, or if step 3 set the bit.
 * A writer whose row changed takes this latch before it clears the
 * bit, so it either waits for the write and clears what it set, or
 * wrote its row before the judging, which then finds it unsettled
 * (../BTREE.md §4.1).
 */
int settle_leaf(
    tree_t &tree, uint64_t pageid,
    const std::function<int(std::string_view, std::string_view)> &dead,
    size_t &removed, uint64_t &next) {
  detail::latched_t l;
  l.pageid = pageid;
  // The leaf's readers and writers wait while it is judged whole.
  if (0 != pin_ex(tree, pageid, false, l.pin) || 0 != l.pin.view.check()) {
    return -1;
  }
  if (0 != l.pin.view.level()) {
    return 1;
  }
  l.decode();
  next = l.page.next;
  auto &entries = l.page.entries;
  size_t kept = 0;
  bool settled = true;
  for (size_t i = 0; i < entries.size(); i++) {
    int d = dead(entries[i].first, entries[i].second);
    if (d < 0) {
      return -1;
    }
    if (1 == d) {
      continue;
    }
    settled = settled && 2 == d;
    if (kept != i) {
      entries[kept] = std::move(entries[i]);
    }
    kept++;
  }
  size_t went = entries.size() - kept;
  entries.resize(kept);
  bool set = settled && 0 == (l.page.flags & page_all_visible);
  if (set) {
    l.page.flags |= page_all_visible;
  }
  if (0 != went || set) {
    if (0 != detail::write_back(tree, l)) {
      return -1;
    }
    removed += went;
  }
  return 0;
}

} // namespace

/* Remove the entries dead says to, leaf by leaf; see tree.h. */
int tree_prune(
    tree_t &tree,
    const std::function<int(std::string_view, std::string_view)> &dead,
    size_t &removed) {
  removed = 0;
  uint64_t pageid = 0;
  if (0 != descend(tree, "", 0, pageid)) {
    return -1;
  }
  for (;;) {
    page_t copy;
    bool leaf = false;
    {
      pinned_t p;
      if (0 != read_page(tree, pageid, false, p) || 0 != p.view.check()) {
        return -1;
      }
      leaf = 0 == p.view.level();
      if (leaf) {
        p.view.load(copy);
      }
    }
    // Rows are read with no latch held, so a clean leaf's readers and
    // writers never wait on a vacuum.
    int rc = leaf ? 0 : 1;
    uint64_t next = copy.next;
    // A leaf all visible holds no dead entry: every write since the
    // bit was set would have cleared it.
    if (leaf && 0 == (copy.flags & page_all_visible)) {
      std::vector<std::string> gone;
      bool settled = true;
      for (const auto &[key, value] : copy.entries) {
        int d = dead(key, value);
        if (d < 0) {
          return -1;
        }
        if (1 == d) {
          gone.push_back(key);
        } else {
          settled = settled && 2 == d;
        }
      }
      // Only a leaf that may be set is judged whole under its latch:
      // the others' readers and writers do not wait on its rows.
      if (settled) {
        rc = settle_leaf(tree, pageid, dead, removed, next);
      } else if (!gone.empty()) {
        rc = prune_leaf(tree, pageid, gone, dead, removed, next);
      }
    }
    if (rc < 0) {
      return -1;
    }
    if (1 == rc) {
      // Page 0, a leaf when the descent read it, has split.
      if (0 != descend(tree, "", 0, pageid)) {
        return -1;
      }
      continue;
    }
    pageid = next;
    if (0 == pageid) {
      return 0;
    }
  }
}

/* The tree's height; see tree.h. */
int tree_height(tree_t &tree, size_t &height) {
  pinned_t p;
  if (0 != read_page(tree, root_id, true, p)) {
    return -1;
  }
  height = static_cast<size_t>(p.view.level()) + 1;
  return 0;
}

/* Check a whole tree; see tree.h. */
int tree_check(tree_t &tree) {
  size_t height = 0;
  if (0 != tree_height(tree, height)) {
    return -1;
  }
  // Walk each level from its leftmost page, which the level above's
  // first entry names, checking order, links and high keys.
  uint64_t first = root_id;
  for (size_t level = height; level-- > 0;) {
    uint64_t pageid = first;
    uint64_t next_first = 0;
    uint64_t before = 0; /* the page to its left, 0 none */
    std::string low;     /* the least key the page may hold */
    pinned_t p;
    for (;;) {
      if (0 != read_page(tree, pageid, level > 0, p)) {
        return -1;
      }
      const page_view_t &v = p.view;
      if (0 != v.check()) {
        return -1;
      }
      if (v.level() != level) {
        return corrupt("page " + std::to_string(pageid) + " at level " +
                       std::to_string(v.level()) + ", not " +
                       std::to_string(level));
      }
      if (0 != (v.flags() & page_incomplete)) {
        return corrupt("page " + std::to_string(pageid) + " left incomplete");
      }
      if (before != v.prev()) {
        return corrupt("page " + std::to_string(pageid) + " links back to " +
                       std::to_string(v.prev()) + ", not " +
                       std::to_string(before));
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
      if (v.has_high() == (0 == v.next())) {
        return corrupt("page " + std::to_string(pageid) +
                       " has a high key but no next link, or the reverse");
      }
      low = std::string(v.high());
      before = pageid;
      pageid = v.next();
      if (0 == pageid) {
        break;
      }
    }
    first = next_first;
  }
  return 0;
}

} // namespace sm2::tree

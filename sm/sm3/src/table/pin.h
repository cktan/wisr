/*
 * pin.h -- the session's pool of pins (../../../ROW.md §4): what a
 * pin and a cursor hold, and the calls table.cc takes, holds and
 * releases them with.
 */
#ifndef SM3_TABLE_PIN_H
#define SM3_TABLE_PIN_H

#include "../catalog/internal.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

/*
 * A pin: one row's bytes kept in memory, and how far they are
 * deformed. It is on one of its session's lists: free, held by a
 * read or a write, or a cursor's.
 */
struct sm3::pin_t {
  sm1::rawpin_t *raw = nullptr;           /* a read's row; made with the pin */
  std::string_view row;                   /* the row's bytes; empty when free */
  std::shared_ptr<const table_def_t> def; /* the row's layout */
  sm2::tup_reader_t reader;     /* how far it is deformed; begun lazily */
  uint64_t gen = 1;             /* increased on each release */
  session_t *session = nullptr; /* whose pool it is in */
  scan_t *scan = nullptr;       /* the cursor whose row it holds, or none */
  pin_t *prev = nullptr;        /* on its list */
  pin_t *next = nullptr;
};

/*
 * A cursor over a table's rows: its rel scan, and the pin of its row;
 * or, over a tree (../BTREE.md §4), the tree's entries, each row read
 * into the pin and kept if it still has its entry's key.
 */
struct sm3::scan_t {
  sm2::rel_scan_t *rel = nullptr;
  pin_t *pin = nullptr;
  sm2::index_scan_t *entries = nullptr;
  table_t *table = nullptr;
  size_t index = 0; /* the tree's place in the table's indexes */
  /*
   * A tree's entries read ahead, their rows read in one call
   * (../../BATCHREAD.md): each entry's key, its row's key and _id,
   * and the row in a raw pin of the batch's; batch the next read's
   * size, doubling to tree_batch_max, as digres's read-ahead does.
   */
  std::vector<std::string> keys, rows;
  std::vector<uint64_t> ids;
  std::vector<sm1::rawpin_t *> raws;
  std::vector<std::string_view> tuples;
  std::vector<int> rcs;
  /*
   * Each entry's cut flag, and whether its row goes unread: an
   * index-only scan's (entries_only) entry, on a leaf all visible,
   * whose key gives its columns back (../BTREE.md §4.1).
   */
  std::vector<uint8_t> cuts, unread;
  /*
   * The iterator its batches' rows are read by, made at the first
   * batch of two or more and kept to the cursor's end, not made again
   * for each (../../BATCHREAD.md §2.1).
   */
  sm1::batch_iter_t *iter = nullptr;
  size_t at = 0, n = 0, batch = 1;
  size_t last = 0; /* the entry handed out last, of the batch */
  std::string key; /* the recheck's entry key, its buffer reused */
  bool entries_done = false;
  bool entries_only = false;
  /* tbl_scan_cols's by-reference datums. */
  std::vector<std::optional<std::string>> datums;
  /* raws[0] is the cursor's pin's own, the batch's others its. */
  ~scan_t() {
    for (size_t i = 1; i < raws.size(); i++) {
      sm1::rawpin_free(raws[i]);
    }
    sm1::batch_iter_free(iter);
  }
};

namespace sm3 {

/*
 * A pin from s's pool, to read a row into: a free one, or a new one
 * with its raw pin. On no list until pin_hold, pin_put or
 * pin_scan_hold; nullptr with SM_ERR_NOMEM if none can be made.
 */
pin_t *pin_take(session_t *s);

/*
 * The pin ref holds a read's row in, taken back to read another
 * row into: off its list, refs to it stale, its row let go, but
 * its raw pin's iterator kept for sm1 to seek again (sm1's
 * SM1_DESIGN.md §10.1). nullptr if ref names no held pin; ref is
 * left naming none.
 */
pin_t *pin_reread(pin_ref_t &ref);

/* Hold row, read under def, in a taken pin, and point ref at it. */
void pin_hold(pin_t *pin, const std::shared_ptr<const table_def_t> &def,
              std::string_view row, pin_ref_t &ref);

/* Give a taken pin that holds no row the caller keeps back to the pool. */
void pin_put(pin_t *pin);

/* Make a taken pin scan's, for rows read under def. */
void pin_scan_hold(pin_t *pin, scan_t *scan,
                   const std::shared_ptr<const table_def_t> &def);

/* Point a cursor's pin at row, its next row, or none: refs go stale. */
void pin_scan_move(pin_t *pin, std::string_view row);

/* Release a held or a cursor's pin to the pool: refs go stale. */
void pin_release(pin_t *pin);

/* Make the pins of the cursors a rollback closed stale (sm1::rollback). */
void pins_rolled_back(session_t *s);

/* Free every pin of s's pool, at session_close. */
void pins_close(session_t *s);

} // namespace sm3

#endif /* SM3_TABLE_PIN_H */

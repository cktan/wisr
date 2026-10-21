/*
 * pin.h -- the session's pool of pins (../../../ROW.md §4): what a
 * pin and a cursor hold, and the calls table.cc takes, holds and
 * releases them with.
 */
#ifndef SM3_TABLE_PIN_H
#define SM3_TABLE_PIN_H

#include "../catalog/internal.h"

#include <memory>
#include <string_view>

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

/* A cursor over a table's rows: its rel scan, and the pin of its row. */
struct sm3::scan_t {
  sm2::rel_scan_t *rel = nullptr;
  pin_t *pin = nullptr;
};

namespace sm3 {

/*
 * A pin from s's pool, to read a row into: a free one, or a new one
 * with its raw pin. On no list until pin_hold, pin_put or
 * pin_scan_hold; nullptr with SM_ERR_NOMEM if none can be made.
 */
pin_t *pin_take(session_t *s);

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

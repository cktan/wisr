/*
 * rel.h -- a rel's state and a cursor over its rows (rel.cc), which
 * the index over a rel's primary key (index.cc) reads and scans its
 * rows by.
 */
#ifndef SM2_REL_H
#define SM2_REL_H

#include "sm2.h"

#include <memory>
#include <string>
#include <vector>

struct sm2::rel_t {
  sm1::session_t *session;
  uint64_t storeid;
  uint16_t relid;
  /* rel_open's layout of the schema it borrows; none from rel_open_desc */
  std::unique_ptr<tup_desc_t> own_desc;
  const tup_desc_t *desc; /* own_desc, or the one rel_open_desc borrows */
  std::span<const sm2::key_column_t> key; /* borrowed; none: by _id */
  std::span<const int32_t> key_atts;      /* each key column's attribute */
  std::string id_seq;
  std::string toast_seq; /* "" if the rel moves no datum out of line */
  bool toasts;           /* a toast_seq, and a variable-width attribute */
  std::vector<sm2::toast_ptr_t> held; /* datums rel_release is to erase */
  /*
   * The pin a write keeps the old row in when the caller gives
   * none, while its datums out of line are found: made on first
   * use, emptied after each, freed at rel_close.
   */
  sm1::rawpin_t *scratch = nullptr;
  /*
   * id_seq, looked up at the rel's first draw (next_id), which the
   * table open keeps from being dropped.
   */
  sm1::seqref_t id_ref;
  bool has_id_ref = false;
  /*
   * The store is chained (../../../VCHAIN.md): its rows are sm1's
   * chained keys, read and written with the chain_ calls. TOAST
   * stays on the versioned ones.
   */
  bool chained = false;
};

/*
 * A cursor over a rel's rows: by _id, over the rows of a rel keyed
 * by _id; or by key, over a keyed rel's rows (idx_scan_rows).
 */
struct sm2::rel_scan_t {
  sm1::scan_t *scan;
  sm1::session_t *session;
  uint64_t storeid;
  uint16_t relid;
  bool by_key;          /* walks a keyed rel's rows, in key order */
  std::string_view key; /* the last row's key, sm1's */
};

#endif

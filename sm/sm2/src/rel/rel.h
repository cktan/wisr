/*
 * rel.h -- a rel's state (rel.cc), which the index over a rel's
 * primary key (index.cc) reads its rows by.
 */
#ifndef SM2_REL_H
#define SM2_REL_H

#include "sm2.h"

#include <string>
#include <vector>

struct sm2::rel_t {
  sm1::session_t *session;
  uint64_t storeid;
  uint16_t relid;
  tup_desc_t desc;                        /* over the borrowed schema */
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
};

#endif

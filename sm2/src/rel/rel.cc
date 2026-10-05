/*
 * rel.cc -- the heap rel (SM2_DESIGN.md §5): a table's rows in
 * one sm1 store, keyed by prefix and rowno, holding their tuples.
 * It is a codec in front of sm1, not a state machine: every
 * durable effect is one sm1 call in the caller's transaction,
 * sm1's errors pass through as they are, and the tuples it hands
 * out are sm1's views.
 */

#include "native.h"
#include "sm2.h"

#include <string>

struct sm2::rel_t {
  sm1::session_t *session;
  uint64_t storeid;
  uint16_t prefix;
  tup_desc_t desc; /* over the borrowed schema */
  std::string rowno_seq;
};

struct sm2::rel_scan_t {
  sm1::scan_t *scan;
  uint16_t prefix;
};

namespace {

/* The sm1 key of the row at rowno: prefix then rowno, big-endian. */
std::string row_key(uint16_t prefix, uint64_t rowno) {
  std::string key;
  sm2::put_be(prefix, key);
  sm2::put_be(rowno, key);
  return key;
}

/* Write tuple, already checked, under a new rowno from rel's sequence. */
int write_new(sm2::rel_t *rel, std::string_view tuple, uint64_t &rowno) {
  // Draw the rowno from the table's sequence.
  uint64_t r;
  if (sm1::nextseq(rel->session, rel->rowno_seq, r) != 0) {
    return -1;
  }
  // Write the row.
  if (sm1::insert(rel->session, rel->storeid, row_key(rel->prefix, r), tuple) !=
      0) {
    return -1;
  }
  rowno = r;
  return 0;
}

/* Replace the row at rowno with tuple, already checked. */
int write_update(sm2::rel_t *rel, uint64_t rowno, std::string_view tuple,
                 std::string_view *old) {
  return sm1::update(rel->session, rel->storeid, row_key(rel->prefix, rowno),
                     tuple, old);
}

} // namespace

namespace sm2 {

/* Open a heap rel; see sm2.h. */
int rel_open(sm1::session_t *session, uint64_t storeid, uint16_t prefix,
             std::span<const attribute_t> schema, std::string_view rowno_seq,
             rel_t *&out) {
  return catch_nomem([&] {
    if (session == nullptr) {
      return sm1::seterr(sm1::SM_ERR_INVALID, "rel: no session");
    }
    // Every attribute must be one the codec can lay out.
    for (size_t i = 0; i < schema.size(); i++) {
      if (schema[i].type == nullptr ||
          datum_width(schema[i].type, schema[i].typmod) == 0) {
        return sm1::seterr(sm1::SM_ERR_INVALID,
                           "rel: attribute " + std::to_string(i) +
                               " has no type, or a typmod its type refuses");
      }
    }
    // Lay the schema out once, for every row the rel encodes.
    out = new rel_t{session, storeid, prefix, tup_desc_t(schema),
                    std::string(rowno_seq)};
    return 0;
  });
}

/* Close a rel; nullptr is a no-op. */
void rel_close(rel_t *rel) { delete rel; }

/* The schema a rel lays its rows out by. */
std::span<const attribute_t> rel_schema(const rel_t *rel) {
  return rel->desc.schema;
}

/* Insert a row under a new rowno; see sm2.h. */
int rel_insert(rel_t *rel, std::string_view tuple, uint64_t &rowno) {
  return catch_nomem([&] {
    // Check first, so a tuple that cannot be stored draws no rowno.
    if (tup_check(rel->desc, tuple) != 0) {
      return -1;
    }
    return write_new(rel, tuple, rowno);
  });
}

/* Insert a row by datum_t and isnull; see sm2.h. */
int rel_insert(rel_t *rel, std::span<const datum_t> datums,
               std::span<const bool> isnull, uint64_t &rowno) {
  return catch_nomem([&] {
    std::string tuple;
    if (tup_encode(rel->desc, datums, isnull, tuple) != 0) {
      return -1;
    }
    return write_new(rel, tuple, rowno);
  });
}

/* Read the row at rowno: 0, 1 if absent, or -1. */
int rel_read(rel_t *rel, uint64_t rowno, std::string_view &tuple) {
  return catch_nomem([&] {
    return sm1::read(rel->session, rel->storeid, row_key(rel->prefix, rowno),
                     tuple);
  });
}

/* Lock the row at rowno in mode, without writing it. */
int rel_lock(rel_t *rel, uint64_t rowno, sm1::lockmode_t mode) {
  return catch_nomem([&] {
    // Held to the end of the transaction, as a write's lock is.
    return sm1::lock(rel->session, rel->storeid, row_key(rel->prefix, rowno),
                     mode);
  });
}

/* Read the newest committed version of the row at rowno. */
int rel_read_latest(rel_t *rel, uint64_t rowno, std::string_view &tuple) {
  return catch_nomem([&] {
    return sm1::read_latest(rel->session, rel->storeid,
                            row_key(rel->prefix, rowno), tuple);
  });
}

/* Replace the row at rowno with an already-encoded tuple; see sm2.h. */
int rel_update(rel_t *rel, uint64_t rowno, std::string_view tuple,
               std::string_view *old) {
  return catch_nomem([&] {
    if (tup_check(rel->desc, tuple) != 0) {
      return -1;
    }
    return write_update(rel, rowno, tuple, old);
  });
}

/* Replace the row at rowno with datum_t and isnull; see sm2.h. */
int rel_update(rel_t *rel, uint64_t rowno, std::span<const datum_t> datums,
               std::span<const bool> isnull, std::string_view *old) {
  return catch_nomem([&] {
    std::string tuple;
    if (tup_encode(rel->desc, datums, isnull, tuple) != 0) {
      return -1;
    }
    return write_update(rel, rowno, tuple, old);
  });
}

/* Delete the row at rowno, handing back its tuple. */
int rel_delete(rel_t *rel, uint64_t rowno, std::string_view *old) {
  return catch_nomem([&] {
    return sm1::erase(rel->session, rel->storeid, row_key(rel->prefix, rowno),
                      old);
  });
}

/* Open a cursor over rows first..last, both included. */
int rel_scan(rel_t *rel, uint64_t first, uint64_t last, rel_scan_t *&out) {
  return catch_nomem([&] {
    std::string lo = row_key(rel->prefix, first);
    std::string hi = row_key(rel->prefix, last);
    // sm1 copies the bounds into the cursor.
    sm1::scan_t *c;
    if (sm1::scan(rel->session, rel->storeid, lo.data(), lo.size(), 1,
                  hi.data(), hi.size(), 1, c) != 0) {
      return -1;
    }
    try {
      out = new rel_scan_t{c, rel->prefix};
    } catch (...) {
      sm1::scanend(c);
      throw;
    }
    return 0;
  });
}

/* The next row of a cursor: 0, 1 at the end, or -1. */
int rel_scannxt(rel_scan_t *scan, uint64_t &rowno, std::string_view &tuple) {
  return catch_nomem([&] {
    std::string_view key;
    int rc = sm1::scannxt(scan->scan, key, tuple);
    if (rc != 0) {
      return rc;
    }
    // The range holds only this prefix's 10-byte keys.
    if (key.size() != 10) {
      return sm1::seterr(sm1::SM_ERR_CORRUPT, "rel: a row key of " +
                                                  std::to_string(key.size()) +
                                                  " bytes, not 10");
    }
    uint64_t r;
    key.remove_prefix(2);
    get_be(key, r);
    rowno = r;
    return 0;
  });
}

/* Move a cursor forward to rowno. */
int rel_scan_seek(rel_scan_t *scan, uint64_t rowno) {
  return catch_nomem([&] {
    return sm1::scan_seek(scan->scan, row_key(scan->prefix, rowno), 1);
  });
}

/* Close a cursor; nullptr is a no-op. */
void rel_scanend(rel_scan_t *scan) {
  if (scan == nullptr) {
    return;
  }
  // Release sm1's cursor and its snapshot.
  sm1::scanend(scan->scan);
  delete scan;
}

} // namespace sm2

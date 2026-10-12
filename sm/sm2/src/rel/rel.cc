/*
 * rel.cc -- the heap rel (SM2_DESIGN.md §5): a table's rows in
 * one sm1 store, keyed by relid and rowno, holding their tuples.
 * It is a codec in front of sm1, not a state machine: a durable
 * effect is an sm1 call in the caller's transaction, sm1's errors
 * pass through as they are, and the tuples it hands out are sm1's
 * views. A row with a datum out of line (TOAST, §4.2) is more than
 * one sm1 call: the row, then the datum's chunks (toast.cc).
 */

#include "native.h"
#include "sm2.h"
#include "toast.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

struct sm2::rel_t {
  sm1::session_t *session;
  uint64_t storeid;
  uint16_t relid;
  tup_desc_t desc; /* over the borrowed schema */
  std::string rowno_seq;
  std::string toast_seq; /* "" if the rel moves no datum out of line */
  bool toasts;           /* a toast_seq, and a variable-width attribute */
  std::string old;       /* the old tuple a write handed back, when toasts */
  std::vector<sm2::toast_ptr_t> held; /* datums rel_release is to erase */
};

struct sm2::rel_scan_t {
  sm1::scan_t *scan;
  uint16_t relid;
};

namespace {

/* The sm1 key of the row at rowno: relid then rowno, big-endian. */
std::string row_key(uint16_t relid, uint64_t rowno) {
  std::string key;
  sm2::put_be(relid, key);
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
  if (sm1::insert(rel->session, rel->storeid, row_key(rel->relid, r), tuple) !=
      0) {
    return -1;
  }
  rowno = r;
  return 0;
}

/* Replace the row at rowno with tuple, already checked. */
int write_update(sm2::rel_t *rel, uint64_t rowno, std::string_view tuple,
                 std::string_view *old) {
  return sm1::update(rel->session, rel->storeid, row_key(rel->relid, rowno),
                     tuple, old);
}

/* A datum to be written out of line: its pointer, and its bytes. */
struct moved_t {
  sm2::toast_ptr_t ptr;
  std::string_view data;
};

/*
 * A row made ready to write (SM2_DESIGN.md §4.2): its tuple, the
 * datums it moves out of line, and the pointers it was given and
 * keeps, by attribute. own holds what the tuple's datums point at
 * that the caller does not: a pointer made here, a value fetched.
 */
struct ready_t {
  std::string tuple;
  std::vector<moved_t> moved;
  std::vector<std::pair<size_t, std::string>> kept;
  std::vector<std::string> own;
};

/*
 * Make datums and isnull ready to write to rel. A pointer among
 * them is kept, for an update, or has its value fetched to be
 * stored again, for an insert. Then the longest datums are moved
 * out of line, longest first, while the tuple is longer than
 * toast_threshold, each given an id from the rel's sequence; none
 * shorter than toast_min is. Nothing is written.
 */
int make_ready(sm2::rel_t *rel, std::span<const sm2::datum_t> datums,
               std::span<const bool> isnull, bool update, ready_t &out) {
  const auto &schema = rel->desc.schema;
  std::vector<sm2::datum_t> d(datums.begin(), datums.end());
  if (!rel->toasts || d.size() != schema.size() ||
      isnull.size() != schema.size()) {
    // Nothing to move, or a row tup_encode refuses for its count.
    return sm2::tup_encode(rel->desc, d, isnull, out.tuple);
  }
  // own's strings must not move once a datum points at one: at most
  // a fetched value and a pointer for each attribute.
  out.own.reserve(2 * d.size());
  // The variable-width datums, by attribute, each with its bytes.
  std::vector<std::pair<size_t, std::string_view>> var;
  for (size_t i = 0; i < d.size(); i++) {
    if (isnull[i] || rel->desc.width[i] > 0 || schema[i].dropped) {
      continue;
    }
    std::string_view v;
    if (sm2::datum_bytes(schema[i].type, schema[i].typmod, d[i], v) != 0) {
      return -1;
    }
    sm2::toast_ptr_t p;
    if (!sm2::parse_toast_ptr(v, p)) {
      var.emplace_back(i, v);
      continue;
    }
    if (update) {
      // Checked against the row's own once the row is written.
      out.kept.emplace_back(i, std::string(v));
      continue;
    }
    // Two rows never share a datum: this one gets a copy of its own.
    if (p.storeid != rel->storeid) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "rel: a TOAST pointer of another store");
    }
    std::string value;
    if (sm2::toast_read(rel->session, p, value) != 0) {
      return -1;
    }
    out.own.push_back(sm2::make_varlena(value));
    d[i] = sm2::datum_from_ptr(out.own.back().data());
    var.emplace_back(i, out.own.back());
  }
  size_t len;
  if (sm2::tup_size(rel->desc, d, isnull, len) != 0) {
    return -1;
  }
  // Longest first, until the tuple is short enough.
  std::sort(var.begin(), var.end(), [](const auto &a, const auto &b) {
    return a.second.size() > b.second.size();
  });
  for (const auto &[i, v] : var) {
    std::string_view data = sm2::varlena_payload(v);
    if (len <= sm2::toast_threshold || data.size() < sm2::toast_min) {
      break;
    }
    // The datum's id, from the rel's own sequence.
    uint64_t id;
    if (sm1::nextseq(rel->session, rel->toast_seq, id) != 0) {
      return -1;
    }
    sm2::toast_ptr_t p{static_cast<uint32_t>(data.size()), rel->storeid, id};
    out.moved.push_back({p, data});
    out.own.push_back(sm2::make_toast_ptr(p));
    d[i] = sm2::datum_from_ptr(out.own.back().data());
    // What the tuple held of it, in its shortest form, less a pointer.
    len -= data.size() + (data.size() <= 126 ? 1 : 4) - sm2::toast_ptr_len;
  }
  return sm2::tup_encode(rel->desc, d, isnull, out.tuple);
}

/* Write the datums a row moved out of line, after the row. */
int write_moved(sm2::rel_t *rel, const ready_t &row) {
  for (const moved_t &m : row.moved) {
    if (sm2::toast_write(rel->session, m.ptr, m.data) != 0) {
      return -1;
    }
  }
  return 0;
}

/*
 * The TOAST pointers the tuple holds, each with its attribute, in
 * out. A tuple too short for an attribute holds none for it.
 */
int pointers_of(const sm2::rel_t *rel, std::string_view tuple,
                std::vector<std::pair<size_t, std::string>> &out) {
  for (size_t i = 0; i < rel->desc.schema.size(); i++) {
    if (rel->desc.width[i] > 0) {
      continue;
    }
    sm2::datum_t val;
    bool isnull;
    if (sm2::tup_decode(rel->desc, tuple, i, val, isnull) != 0) {
      return -1;
    }
    const char *v = static_cast<const char *>(sm2::datum_to_ptr(val));
    if (!isnull && v[0] == sm2::varlena_toast) {
      out.emplace_back(i, std::string(v, sm2::toast_ptr_len));
    }
  }
  return 0;
}

/* Erase the datums rel holds for rel_release, and hold none. */
int release(sm2::rel_t *rel) {
  std::vector<sm2::toast_ptr_t> held;
  held.swap(rel->held);
  for (const sm2::toast_ptr_t &p : held) {
    if (sm2::toast_erase(rel->session, p) != 0) {
      return -1;
    }
  }
  return 0;
}

/*
 * After the row that held the tuple old was replaced, or deleted:
 * check each pointer the new row kept is the one old held for
 * that attribute, and erase the datums old held that are not kept
 * -- now, or with hold when the caller calls rel_release.
 */
int settle_old(sm2::rel_t *rel, std::string_view old,
               const std::vector<std::pair<size_t, std::string>> &kept,
               bool hold) {
  std::vector<std::pair<size_t, std::string>> had;
  if (pointers_of(rel, old, had) != 0) {
    return -1;
  }
  for (const auto &k : kept) {
    if (std::find(had.begin(), had.end(), k) == had.end()) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "rel: attribute " + std::to_string(k.first) +
                             " is a TOAST pointer the row does not hold");
    }
  }
  for (const auto &h : had) {
    if (std::find(kept.begin(), kept.end(), h) != kept.end()) {
      continue;
    }
    sm2::toast_ptr_t p;
    sm2::parse_toast_ptr(h.second, p);
    rel->held.push_back(p);
  }
  return hold ? 0 : release(rel);
}

/* Insert the row datums and isnull make, and what it moves out. */
int insert_row(sm2::rel_t *rel, std::span<const sm2::datum_t> datums,
               std::span<const bool> isnull, uint64_t &rowno) {
  ready_t row;
  if (make_ready(rel, datums, isnull, false, row) != 0) {
    return -1;
  }
  uint64_t r;
  if (write_new(rel, row.tuple, r) != 0 || write_moved(rel, row) != 0) {
    return -1;
  }
  rowno = r;
  return 0;
}

/*
 * Replace the row at rowno with the one datums and isnull make:
 * the row first, so that a write sm1 refuses has written nothing,
 * then what the old row held and the new does not is erased, and
 * what the new row moves out is written.
 */
int update_row(sm2::rel_t *rel, uint64_t rowno,
               std::span<const sm2::datum_t> datums,
               std::span<const bool> isnull, std::string_view *old, bool hold) {
  ready_t row;
  if (make_ready(rel, datums, isnull, true, row) != 0) {
    return -1;
  }
  if (!rel->toasts) {
    return write_update(rel, rowno, row.tuple, old);
  }
  std::string_view was;
  if (write_update(rel, rowno, row.tuple, &was) != 0) {
    return -1;
  }
  // sm1's view ends at the next call on the session: keep a copy.
  rel->old = was;
  if (settle_old(rel, rel->old, row.kept, hold) != 0 ||
      write_moved(rel, row) != 0) {
    return -1;
  }
  if (old != nullptr) {
    *old = rel->old;
  }
  return 0;
}

/* The datums and null flags of a tuple, already checked, under rel. */
int deform(const sm2::rel_t *rel, std::string_view tuple,
           std::vector<sm2::datum_t> &datums, std::unique_ptr<bool[]> &isnull) {
  size_t n = rel->desc.schema.size();
  datums.assign(n, 0);
  isnull.reset(new bool[n]());
  return sm2::tup_deform(rel->desc, tuple, datums,
                         std::span<bool>(isnull.get(), n));
}

} // namespace

namespace sm2 {

/* Open a heap rel; see sm2.h. */
int rel_open(sm1::session_t *session, uint64_t storeid, uint16_t relid,
             std::span<const attribute_t> schema, std::string_view rowno_seq,
             std::string_view toast_seq, rel_t *&out) {
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
  out = new rel_t{session,
                  storeid,
                  relid,
                  tup_desc_t(schema),
                  std::string(rowno_seq),
                  std::string(toast_seq),
                  false,
                  {}};
  // Only a variable-width datum is ever moved out of line.
  for (int32_t w : out->desc.width) {
    out->toasts = out->toasts || (w < 0 && !toast_seq.empty());
  }
  return 0;
}

/* Close a rel; nullptr is a no-op. */
void rel_close(rel_t *rel) { delete rel; }

/* The schema a rel lays its rows out by. */
std::span<const attribute_t> rel_schema(const rel_t *rel) {
  return rel->desc.schema;
}

/* Insert a row under a new rowno; see sm2.h. */
int rel_insert(rel_t *rel, std::string_view tuple, uint64_t &rowno) {
  // Check first, so a tuple that cannot be stored draws no rowno.
  if (tup_check(rel->desc, tuple) != 0) {
    return -1;
  }
  if (!rel->toasts) {
    return write_new(rel, tuple, rowno);
  }
  // Its datums may be moved out of line, as any row's.
  std::vector<datum_t> datums;
  std::unique_ptr<bool[]> isnull;
  if (deform(rel, tuple, datums, isnull) != 0) {
    return -1;
  }
  return insert_row(rel, datums,
                    std::span<const bool>(isnull.get(), datums.size()), rowno);
}

/* Insert a row by datum_t and isnull; see sm2.h. */
int rel_insert(rel_t *rel, std::span<const datum_t> datums,
               std::span<const bool> isnull, uint64_t &rowno) {
  return insert_row(rel, datums, isnull, rowno);
}

/* Read the row at rowno: 0, 1 if absent, or -1. */
int rel_read(rel_t *rel, uint64_t rowno, std::string_view &tuple) {
  return sm1::read(rel->session, rel->storeid, row_key(rel->relid, rowno),
                   tuple);
}

/* Lock the row at rowno in mode, without writing it. */
int rel_lock(rel_t *rel, uint64_t rowno, sm1::lockmode_t mode) {
  // Held to the end of the transaction, as a write's lock is.
  return sm1::lock(rel->session, sm1::LOCKTYPE_ROW, rel->storeid,
                   row_key(rel->relid, rowno), mode);
}

/* Read the newest committed version of the row at rowno. */
int rel_read_latest(rel_t *rel, uint64_t rowno, std::string_view &tuple) {
  return sm1::read_latest(rel->session, rel->storeid,
                          row_key(rel->relid, rowno), tuple);
}

/* Replace the row at rowno with an already-encoded tuple; see sm2.h. */
int rel_update(rel_t *rel, uint64_t rowno, std::string_view tuple,
               std::string_view *old, bool hold) {
  if (tup_check(rel->desc, tuple) != 0) {
    return -1;
  }
  if (!rel->toasts) {
    return write_update(rel, rowno, tuple, old);
  }
  // Its datums may be moved out of line, as any row's.
  std::vector<datum_t> datums;
  std::unique_ptr<bool[]> isnull;
  if (deform(rel, tuple, datums, isnull) != 0) {
    return -1;
  }
  return update_row(rel, rowno, datums,
                    std::span<const bool>(isnull.get(), datums.size()), old,
                    hold);
}

/* Replace the row at rowno with datum_t and isnull; see sm2.h. */
int rel_update(rel_t *rel, uint64_t rowno, std::span<const datum_t> datums,
               std::span<const bool> isnull, std::string_view *old, bool hold) {
  return update_row(rel, rowno, datums, isnull, old, hold);
}

/* Delete the row at rowno, handing back its tuple. */
int rel_delete(rel_t *rel, uint64_t rowno, std::string_view *old, bool hold) {
  if (!rel->toasts) {
    return sm1::erase(rel->session, rel->storeid, row_key(rel->relid, rowno),
                      old);
  }
  std::string_view was;
  if (sm1::erase(rel->session, rel->storeid, row_key(rel->relid, rowno),
                 &was) != 0) {
    return -1;
  }
  // sm1's view ends at the next call on the session: keep a copy.
  rel->old = was;
  // The datums the row held out of line go with it.
  if (settle_old(rel, rel->old, {}, hold) != 0) {
    return -1;
  }
  if (old != nullptr) {
    *old = rel->old;
  }
  return 0;
}

/* Erase the datums the rel's writes held; see sm2.h. */
int rel_release(rel_t *rel) { return release(rel); }

/* Read the datum a TOAST pointer names; see sm2.h. */
int toast_fetch(rel_t *rel, std::string_view ptr, std::string &out) {
  toast_ptr_t p;
  if (!parse_toast_ptr(ptr, p) || p.storeid != rel->storeid) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "toast: not a TOAST pointer of this rel's store");
  }
  return toast_read(rel->session, p, out);
}

/* Open a cursor over rows first..last, both included. */
int rel_scan(rel_t *rel, uint64_t first, uint64_t last, rel_scan_t *&out) {
  std::string lo = row_key(rel->relid, first);
  std::string hi = row_key(rel->relid, last);
  // sm1 copies the bounds into the cursor.
  sm1::scan_t *c;
  if (sm1::scan(rel->session, rel->storeid, lo.data(), lo.size(), 1, hi.data(),
                hi.size(), 1, c) != 0) {
    return -1;
  }
  out = new rel_scan_t{c, rel->relid};
  return 0;
}

/* The next row of a cursor: 0, 1 at the end, or -1. */
int rel_scannxt(rel_scan_t *scan, uint64_t &rowno, std::string_view &tuple) {
  std::string_view key;
  int rc = sm1::scannxt(scan->scan, key, tuple);
  if (rc != 0) {
    return rc;
  }
  // The range holds only this relid's 10-byte keys.
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
}

/* Move a cursor forward to rowno. */
int rel_scan_seek(rel_scan_t *scan, uint64_t rowno) {
  return sm1::scan_seek(scan->scan, row_key(scan->relid, rowno), 1);
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

/* The bytes under relid, from sm1's estimate of its key range. */
int relid_size(sm1::session_t *session, uint64_t storeid, uint16_t relid,
               uint64_t &disk, uint64_t &mem) {
  // Every key of the rel starts with its relid, so the range is
  // [relid, relid + 1); the last relid's is open above.
  std::string lo, hi;
  put_be(relid, lo);
  if (relid == UINT16_MAX) {
    return sm1::approxsize(session, storeid, lo.data(), lo.size(), nullptr, 0,
                           disk, mem);
  }
  put_be(static_cast<uint16_t>(relid + 1), hi);
  return sm1::approxsize(session, storeid, lo.data(), lo.size(), hi.data(),
                         hi.size(), disk, mem);
}

} // namespace sm2

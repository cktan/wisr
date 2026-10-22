/*
 * rel.cc -- the rel (../CLUSTER.md §2): a table's rows in one sm1
 * store, keyed by relid and their primary key or _id, holding their
 * _id and tuple, and in a keyed rel an index from _id to key. It is
 * a codec in front of sm1, not a state machine: a durable effect is
 * an sm1 call in the caller's transaction, sm1's errors pass
 * through as they are, and the tuples it hands out are sm1's
 * views. A row with a datum out of line (TOAST, SM2_DESIGN.md
 * §4.2) is more than one sm1 call: the row, then the datum's chunks
 * (toast.cc); so is a row of a keyed rel: the row, then its _id
 * entry.
 */

#include "rel.h"
#include "native.h"
#include "sm2.h"
#include "toast.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

struct sm2::rel_scan_t {
  sm1::scan_t *scan; /* over the rows, or a keyed rel's _id index */
  sm1::session_t *session;
  uint64_t storeid;
  uint16_t relid;
  bool keyed;
  sm1::rawpin_t *pin = nullptr; /* a keyed rel's row, read by its key */
  std::string key;              /* the last row's key */
};

namespace {

/* The sm1 key of the row at key: relid then key. */
std::string row_key(uint16_t relid, std::string_view key) {
  std::string k;
  sm2::put_be(relid, k);
  k += key;
  return k;
}

/* The sm1 key of _id id's entry in the _id index. */
std::string id_entry(uint64_t id) {
  std::string k;
  sm2::put_be(sm2::id_relid, k);
  sm2::put_be(id, k);
  return k;
}

/* A row's value: its _id, big-endian, then its tuple. */
std::string row_value(uint64_t id, std::string_view tuple) {
  std::string v;
  v.reserve(sm2::id_len + tuple.size());
  sm2::put_be(id, v);
  v += tuple;
  return v;
}

/*
 * Split a row's value into its _id, if wanted, and its tuple,
 * which views the value.
 */
int split_value(std::string_view value, uint64_t *id, std::string_view &tuple) {
  uint64_t i;
  if (!sm2::get_be(value, i)) {
    return sm1::seterr(sm1::SM_ERR_CORRUPT, "rel: a row without its _id");
  }
  if (nullptr != id) {
    *id = i;
  }
  tuple = value;
  return 0;
}

/*
 * A read's return, rc, with the tuple it gave split from the row's
 * _id: as rc, or -1 if the value is no row's.
 */
int split_read(int rc, std::string_view &tuple, uint64_t *id) {
  if (0 != rc) {
    tuple = {};
    return rc;
  }
  return split_value(tuple, id, tuple);
}

/*
 * Write to an index entry of k by write, again after reading its
 * newest version where sm1 answers SM_ERR_UPDATED: the caller
 * decided on the row, and an entry has no filter to re-check.
 */
template <typename F>
int write_entry(sm1::session_t *session, uint64_t storeid, const std::string &k,
                F write) {
  int rc = write();
  if (rc != 0 && sm1::errnum() == sm1::SM_ERR_UPDATED) {
    std::string_view latest;
    if (sm1::read_latest(session, storeid, k, latest) < 0) {
      return -1;
    }
    rc = write();
  }
  return rc;
}

/* Point _id id's entry at key, as a move leaves the row. */
int set_id_entry(sm2::rel_t *rel, uint64_t id, std::string_view key) {
  std::string k = id_entry(id);
  return write_entry(rel->session, rel->storeid, k, [&] {
    return sm1::update(rel->session, rel->storeid, k, key);
  });
}

/* Erase _id id's entry, as its row is deleted. */
int erase_id_entry(sm2::rel_t *rel, uint64_t id) {
  std::string k = id_entry(id);
  return write_entry(rel->session, rel->storeid, k,
                     [&] { return sm1::erase(rel->session, rel->storeid, k); });
}

/*
 * Write tuple, already checked, under key with _id id: the row,
 * then in a keyed rel its _id entry.
 */
int write_new(sm2::rel_t *rel, std::string_view key, uint64_t id,
              std::string_view tuple) {
  if (sm1::insert(rel->session, rel->storeid, row_key(rel->relid, key),
                  row_value(id, tuple)) != 0) {
    return -1;
  }
  return rel->key.empty()
             ? 0
             : sm1::insert(rel->session, rel->storeid, id_entry(id), key);
}

/*
 * Replace the row at key with _id id and tuple, already checked;
 * with pin, the tuple replaced pinned into it, and old viewing it.
 */
int write_update(sm2::rel_t *rel, std::string_view key, uint64_t id,
                 std::string_view tuple, sm1::rawpin_t *pin,
                 std::string_view *old) {
  std::string k = row_key(rel->relid, key), v = row_value(id, tuple);
  if (!pin) {
    return sm1::update(rel->session, rel->storeid, k, v);
  }
  return split_read(sm1::update(rel->session, rel->storeid, k, v, pin, *old),
                    *old, nullptr);
}

/*
 * Delete the row at key, with pin or without, and in a keyed rel
 * its _id entry, the _id read from the row erased; with pin, its
 * tuple pinned into it, and old viewing it.
 */
int write_delete(sm2::rel_t *rel, std::string_view key, sm1::rawpin_t *pin,
                 std::string_view *old) {
  std::string k = row_key(rel->relid, key);
  if (!pin) {
    return sm1::erase(rel->session, rel->storeid, k);
  }
  uint64_t id = 0;
  if (split_read(sm1::erase(rel->session, rel->storeid, k, pin, *old), *old,
                 &id) != 0) {
    return -1;
  }
  return rel->key.empty() ? 0 : erase_id_entry(rel, id);
}

/*
 * The pin a write keeps the old row in: pin, or else the rel's
 * scratch pin, made now if it has none; nullptr with
 * SM_ERR_NOMEM if it cannot be.
 */
sm1::rawpin_t *old_pin(sm2::rel_t *rel, sm1::rawpin_t *pin) {
  if (pin) {
    return pin;
  }
  if (!rel->scratch) {
    rel->scratch = sm1::rawpin_new();
  }
  return rel->scratch;
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

/*
 * The key of the row with _id id and values d and isnull, into key:
 * id_key(id), or the primary key's columns encoded, a column out
 * of line fetched first.
 */
int key_of(sm2::rel_t *rel, uint64_t id, std::span<const sm2::datum_t> d,
           std::span<const bool> isnull, std::string &key) {
  key.clear();
  if (rel->key.empty()) {
    sm2::put_be(id, key);
    return 0;
  }
  const auto &schema = rel->desc.schema;
  if (d.size() != schema.size() || isnull.size() != schema.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "rel: a datum and null flag for every attribute");
  }
  size_t n = rel->key.size();
  std::vector<sm2::datum_t> cols(n);
  std::unique_ptr<bool[]> nulls(new bool[n]());
  // A fetched value must not move once a datum points at it.
  std::vector<std::string> own;
  own.reserve(n);
  for (size_t j = 0; j < n; j++) {
    int32_t att = rel->key_atts[j];
    if (isnull[att]) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "rel: attribute " + std::to_string(att) +
                             " of the primary key is null");
    }
    cols[j] = d[att];
    if (rel->desc.width[att] > 0) {
      continue;
    }
    std::string_view v;
    if (sm2::datum_bytes(schema[att].type, schema[att].typmod, d[att], v) !=
        0) {
      return -1;
    }
    sm2::toast_ptr_t p;
    if (!sm2::parse_toast_ptr(v, p)) {
      continue;
    }
    std::string value;
    if (sm2::toast_read(rel->session, p, value) != 0) {
      return -1;
    }
    own.push_back(sm2::make_varlena(value));
    cols[j] = sm2::datum_from_ptr(own.back().data());
  }
  bool cut;
  return sm2::encode_key(rel->key, cols, std::span<const bool>(nulls.get(), n),
                         sm2::max_pk_len, sm2::overflow_t::refuse, key, cut);
}

/*
 * Insert the row datums and isnull make, and what it moves out,
 * under a new _id, or under reserved if that is not 0; its key
 * into key.
 */
int insert_row(sm2::rel_t *rel, std::span<const sm2::datum_t> datums,
               std::span<const bool> isnull, uint64_t &id, std::string &key,
               uint64_t reserved = 0) {
  ready_t row;
  if (make_ready(rel, datums, isnull, false, row) != 0) {
    return -1;
  }
  // A keyed row's key, before an _id is drawn for a row it refuses.
  std::string k;
  if (!rel->key.empty() && key_of(rel, 0, datums, isnull, k) != 0) {
    return -1;
  }
  // Draw the _id from the table's sequence, unless it was.
  uint64_t r = reserved;
  if (r == 0 && sm1::nextseq(rel->session, rel->id_seq, r) != 0) {
    return -1;
  }
  if (rel->key.empty()) {
    k = sm2::id_key(r);
  }
  if (write_new(rel, k, r, row.tuple) != 0 || write_moved(rel, row) != 0) {
    return -1;
  }
  id = r;
  key = std::move(k);
  return 0;
}

/*
 * Move the row at key, _id id, to new_key, with tuple: erase it,
 * its old row pinned into p and viewed by was, then insert it
 * under new_key, and point its _id entry there (../CLUSTER.md §4).
 */
int move_row(sm2::rel_t *rel, std::string_view key, uint64_t id,
             std::string_view new_key, std::string_view tuple, sm1::rawpin_t *p,
             std::string_view &was) {
  if (split_read(sm1::erase(rel->session, rel->storeid,
                            row_key(rel->relid, key), p, was),
                 was, nullptr) != 0) {
    return -1;
  }
  if (sm1::insert(rel->session, rel->storeid, row_key(rel->relid, new_key),
                  row_value(id, tuple)) != 0) {
    return -1;
  }
  return set_id_entry(rel, id, new_key);
}

/*
 * Replace the row at key, _id id, with the one datums and isnull
 * make: the row first, so that a write sm1 refuses has written
 * nothing, then what the old row held and the new does not is
 * erased, and what the new row moves out is written. A row whose
 * primary key changed moves. With pin, the old row is pinned into
 * it, and old views it; moved_to, if wanted, is the row's key after.
 */
int update_row(sm2::rel_t *rel, std::string_view key, uint64_t id,
               std::span<const sm2::datum_t> datums,
               std::span<const bool> isnull, sm1::rawpin_t *pin,
               std::string_view *old, bool hold, std::string *moved_to) {
  // No old row until the write succeeds.
  if (nullptr != old) {
    *old = {};
  }
  ready_t row;
  if (make_ready(rel, datums, isnull, true, row) != 0) {
    return -1;
  }
  std::string new_key(key);
  if (!rel->key.empty() && key_of(rel, id, datums, isnull, new_key) != 0) {
    return -1;
  }
  bool moves = new_key != key;
  if (!rel->toasts && !moves) {
    if (write_update(rel, key, id, row.tuple, pin, old) != 0) {
      return -1;
    }
    if (nullptr != moved_to) {
      *moved_to = std::move(new_key);
    }
    return 0;
  }
  // The old row's pointers are read after more calls on the
  // session, so it is pinned: into pin, or the rel's own.
  sm1::rawpin_t *p = old_pin(rel, pin);
  if (!p) {
    return -1;
  }
  std::string_view was;
  int rc = 0 != (moves ? move_row(rel, key, id, new_key, row.tuple, p, was)
                       : write_update(rel, key, id, row.tuple, p, &was)) ||
                   (rel->toasts && 0 != settle_old(rel, was, row.kept, hold)) ||
                   0 != write_moved(rel, row)
               ? -1
               : 0;
  if (p != pin) {
    sm1::unpin(p);
  }
  if (0 != rc) {
    return -1;
  }
  if (nullptr != old) {
    *old = was;
  }
  if (nullptr != moved_to) {
    *moved_to = std::move(new_key);
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

/* The key of the row with _id id in a rel keyed by _id. */
std::string id_key(uint64_t id) {
  std::string k;
  put_be(id, k);
  return k;
}

/* Open a rel; see sm2.h. */
int rel_open(sm1::session_t *session, uint64_t storeid, uint16_t relid,
             std::span<const attribute_t> schema, const rel_key_t &key,
             std::string_view id_seq, std::string_view toast_seq, rel_t *&out) {
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
  // Every key column is a live attribute of its own type.
  if (key.cols.size() != key.attnums.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "rel: an attribute for every key column, no more");
  }
  for (size_t j = 0; j < key.cols.size(); j++) {
    int32_t att = key.attnums[j];
    if (att < 0 || static_cast<size_t>(att) >= schema.size() ||
        schema[att].dropped || key.cols[j].type != schema[att].type) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "rel: key column " + std::to_string(j) +
                             " is no live attribute of its type");
    }
  }
  // Lay the schema out once, for every row the rel encodes.
  out = new rel_t{session,
                  storeid,
                  relid,
                  tup_desc_t(schema),
                  key.cols,
                  key.attnums,
                  std::string(id_seq),
                  std::string(toast_seq),
                  false,
                  {}};
  // Only a variable-width datum is ever moved out of line.
  for (int32_t w : out->desc.width) {
    out->toasts = out->toasts || (w < 0 && !toast_seq.empty());
  }
  return 0;
}

/* Close a rel, and free its scratch pin; nullptr is a no-op. */
void rel_close(rel_t *rel) {
  if (rel) {
    sm1::rawpin_free(rel->scratch);
  }
  delete rel;
}

/* The schema a rel lays its rows out by. */
std::span<const attribute_t> rel_schema(const rel_t *rel) {
  return rel->desc.schema;
}

/* Whether a rel is keyed by a primary key; see sm2.h. */
bool rel_keyed(const rel_t *rel) { return !rel->key.empty(); }

/* The key of a row of these values; see sm2.h. */
int rel_row_key(rel_t *rel, uint64_t id, std::span<const datum_t> datums,
                std::span<const bool> isnull, std::string &key) {
  return key_of(rel, id, datums, isnull, key);
}

/* Insert a row under a new _id; see sm2.h. */
int rel_insert(rel_t *rel, std::string_view tuple, uint64_t &id,
               std::string *key) {
  // Check first, so a tuple that cannot be stored draws no _id.
  if (tup_check(rel->desc, tuple) != 0) {
    return -1;
  }
  std::string k;
  if (!rel->toasts && rel->key.empty()) {
    uint64_t r;
    if (sm1::nextseq(rel->session, rel->id_seq, r) != 0 ||
        write_new(rel, id_key(r), r, tuple) != 0) {
      return -1;
    }
    id = r;
    if (nullptr != key) {
      *key = id_key(r);
    }
    return 0;
  }
  // Its datums may be moved out of line, and make its key.
  std::vector<datum_t> datums;
  std::unique_ptr<bool[]> isnull;
  if (deform(rel, tuple, datums, isnull) != 0 ||
      insert_row(rel, datums,
                 std::span<const bool>(isnull.get(), datums.size()), id,
                 k) != 0) {
    return -1;
  }
  if (nullptr != key) {
    *key = std::move(k);
  }
  return 0;
}

/* Insert a row by datum_t and isnull; see sm2.h. */
int rel_insert(rel_t *rel, std::span<const datum_t> datums,
               std::span<const bool> isnull, uint64_t &id, std::string *key) {
  std::string k;
  if (insert_row(rel, datums, isnull, id, k) != 0) {
    return -1;
  }
  if (nullptr != key) {
    *key = std::move(k);
  }
  return 0;
}

/* Draw an _id for a row to be inserted later; see sm2.h. */
int rel_reserve(rel_t *rel, uint64_t &id) {
  // From the table's sequence, as rel_insert draws one.
  return sm1::nextseq(rel->session, rel->id_seq, id);
}

/* Insert a row under an _id it is given; see sm2.h. */
int rel_insert_at(rel_t *rel, uint64_t id, std::span<const datum_t> datums,
                  std::span<const bool> isnull, std::string *key) {
  if (id == 0) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "rel: no _id reserved");
  }
  uint64_t r;
  std::string k;
  if (insert_row(rel, datums, isnull, r, k, id) != 0) {
    return -1;
  }
  if (nullptr != key) {
    *key = std::move(k);
  }
  return 0;
}

/* The key of the row with _id id, as the snapshot shows; see sm2.h. */
int rel_find(rel_t *rel, uint64_t id, std::string &key) {
  if (rel->key.empty()) {
    key = id_key(id);
    return 0;
  }
  std::string_view v;
  int rc = sm1::read(rel->session, rel->storeid, id_entry(id), v);
  if (rc == 0) {
    key.assign(v);
  }
  return rc;
}

/* The key of the row with _id id now, under a lock; see sm2.h. */
int rel_find_latest(rel_t *rel, uint64_t id, std::string &key) {
  if (rel->key.empty()) {
    key = id_key(id);
    return 0;
  }
  std::string_view v;
  int rc = sm1::read_latest(rel->session, rel->storeid, id_entry(id), v);
  if (rc == 0) {
    key.assign(v);
  }
  return rc;
}

/* Lock _id id's entry in the _id index; see sm2.h. */
int rel_lock_id(rel_t *rel, uint64_t id, sm1::lockmode_t mode, bool nowait) {
  if (rel->key.empty()) {
    return 0;
  }
  // Held to the end of the transaction: the row stays at its key.
  return nowait ? sm1::trylock(rel->session, sm1::LOCKTYPE_ROW, rel->storeid,
                               id_entry(id), mode)
                : sm1::lock(rel->session, sm1::LOCKTYPE_ROW, rel->storeid,
                            id_entry(id), mode);
}

/* Read the row at key: 0, 1 if absent, or -1. */
int rel_read(rel_t *rel, std::string_view key, std::string_view &tuple,
             uint64_t *id) {
  return split_read(
      sm1::read(rel->session, rel->storeid, row_key(rel->relid, key), tuple),
      tuple, id);
}

/* Read the row at key into pin. */
int rel_read(rel_t *rel, std::string_view key, sm1::rawpin_t *pin,
             std::string_view &tuple, uint64_t *id) {
  return split_read(sm1::read(rel->session, rel->storeid,
                              row_key(rel->relid, key), pin, tuple),
                    tuple, id);
}

/* Lock the row at key in mode, without writing it. */
int rel_lock(rel_t *rel, std::string_view key, sm1::lockmode_t mode) {
  // Held to the end of the transaction, as a write's lock is.
  return sm1::lock(rel->session, sm1::LOCKTYPE_ROW, rel->storeid,
                   row_key(rel->relid, key), mode);
}

/* Lock the row at key in mode if it is free now; see sm2.h. */
int rel_trylock(rel_t *rel, std::string_view key, sm1::lockmode_t mode) {
  // The same lock, asked for without waiting.
  return sm1::trylock(rel->session, sm1::LOCKTYPE_ROW, rel->storeid,
                      row_key(rel->relid, key), mode);
}

/* Read the newest committed version of the row at key. */
int rel_read_latest(rel_t *rel, std::string_view key, std::string_view &tuple,
                    uint64_t *id) {
  return split_read(sm1::read_latest(rel->session, rel->storeid,
                                     row_key(rel->relid, key), tuple),
                    tuple, id);
}

/* Read the newest committed version of the row at key into a pin. */
int rel_read_latest(rel_t *rel, std::string_view key, sm1::rawpin_t *pin,
                    std::string_view &tuple, uint64_t *id) {
  return split_read(sm1::read_latest(rel->session, rel->storeid,
                                     row_key(rel->relid, key), pin, tuple),
                    tuple, id);
}

/*
 * rel_update of an already-encoded tuple, with pin and old both
 * given or both null.
 */
static int update_tuple(rel_t *rel, std::string_view key, uint64_t id,
                        std::string_view tuple, sm1::rawpin_t *pin,
                        std::string_view *old, bool hold,
                        std::string *moved_to) {
  // No old row until the write succeeds.
  if (nullptr != old) {
    *old = {};
  }
  if (tup_check(rel->desc, tuple) != 0) {
    return -1;
  }
  if (!rel->toasts && rel->key.empty()) {
    if (write_update(rel, key, id, tuple, pin, old) != 0) {
      return -1;
    }
    if (nullptr != moved_to) {
      moved_to->assign(key);
    }
    return 0;
  }
  // Its datums may be moved out of line, and make its key.
  std::vector<datum_t> datums;
  std::unique_ptr<bool[]> isnull;
  if (deform(rel, tuple, datums, isnull) != 0) {
    return -1;
  }
  return update_row(rel, key, id, datums,
                    std::span<const bool>(isnull.get(), datums.size()), pin,
                    old, hold, moved_to);
}

/* Replace the row at key with an already-encoded tuple; see sm2.h. */
int rel_update(rel_t *rel, std::string_view key, uint64_t id,
               std::string_view tuple, std::string *moved_to) {
  return update_tuple(rel, key, id, tuple, nullptr, nullptr, false, moved_to);
}

/* As above, pinning the tuple replaced into pin; see sm2.h. */
int rel_update(rel_t *rel, std::string_view key, uint64_t id,
               std::string_view tuple, sm1::rawpin_t *pin,
               std::string_view &old, bool hold, std::string *moved_to) {
  return update_tuple(rel, key, id, tuple, pin, &old, hold, moved_to);
}

/* Replace the row at key with datum_t and isnull; see sm2.h. */
int rel_update(rel_t *rel, std::string_view key, uint64_t id,
               std::span<const datum_t> datums, std::span<const bool> isnull,
               std::string *moved_to) {
  return update_row(rel, key, id, datums, isnull, nullptr, nullptr, false,
                    moved_to);
}

/* As above, pinning the tuple replaced into pin; see sm2.h. */
int rel_update(rel_t *rel, std::string_view key, uint64_t id,
               std::span<const datum_t> datums, std::span<const bool> isnull,
               sm1::rawpin_t *pin, std::string_view &old, bool hold,
               std::string *moved_to) {
  return update_row(rel, key, id, datums, isnull, pin, &old, hold, moved_to);
}

/*
 * rel_delete, with pin and old both given or both null: the datums
 * the row held out of line go with it, read from the row pinned,
 * and in a keyed rel its _id entry, by the _id read there.
 */
static int delete_row(rel_t *rel, std::string_view key, sm1::rawpin_t *pin,
                      std::string_view *old, bool hold) {
  // No old row until the write succeeds.
  if (nullptr != old) {
    *old = {};
  }
  if (!rel->toasts && rel->key.empty()) {
    return write_delete(rel, key, pin, old);
  }
  sm1::rawpin_t *p = old_pin(rel, pin);
  if (!p) {
    return -1;
  }
  std::string_view was;
  int rc = 0 != write_delete(rel, key, p, &was) ||
                   (rel->toasts && 0 != settle_old(rel, was, {}, hold))
               ? -1
               : 0;
  if (p != pin) {
    sm1::unpin(p);
  }
  if (0 == rc && nullptr != old) {
    *old = was;
  }
  return rc;
}

/* Delete the row at key. */
int rel_delete(rel_t *rel, std::string_view key) {
  return delete_row(rel, key, nullptr, nullptr, false);
}

/* Delete the row at key, pinning its tuple into pin. */
int rel_delete(rel_t *rel, std::string_view key, sm1::rawpin_t *pin,
               std::string_view &old, bool hold) {
  return delete_row(rel, key, pin, &old, hold);
}

/* Put a row into a load under a new _id; see sm2.h. */
int rel_load(rel_t *rel, sm1::loader_t *loader, size_t stream, size_t id_stream,
             std::span<const datum_t> datums, std::span<const bool> isnull,
             uint64_t &id, std::string *key) {
  size_t len;
  if (tup_size(rel->desc, datums, isnull, len) != 0) {
    return -1;
  }
  // A row longer than a tuple may be goes the other way, where it
  // is refused or stored out of line.
  if (len > max_tup_len) {
    return 1;
  }
  // So does one rel_insert would move a datum out of: a load has
  // no stream for the chunks. And one given a pointer, whose
  // value rel_insert copies.
  for (size_t i = 0; rel->toasts && i < datums.size(); i++) {
    if (isnull[i] || rel->desc.width[i] > 0) {
      continue;
    }
    std::string_view v;
    if (datum_bytes(rel->desc.schema[i].type, rel->desc.schema[i].typmod,
                    datums[i], v) != 0) {
      return -1;
    }
    if (varlena_is_toast(v) ||
        (len > toast_threshold && varlena_payload(v).size() >= toast_min)) {
      return 1;
    }
  }
  std::string tuple, k;
  if (tup_encode(rel->desc, datums, isnull, tuple) != 0 ||
      (!rel->key.empty() && key_of(rel, 0, datums, isnull, k) != 0)) {
    return -1;
  }
  // Draw the _id from the table's sequence: a load's ascend.
  uint64_t r;
  if (sm1::nextseq(rel->session, rel->id_seq, r) != 0) {
    return -1;
  }
  if (rel->key.empty()) {
    k = id_key(r);
  }
  if (sm1::load_put(loader, stream, row_key(rel->relid, k),
                    row_value(r, tuple)) != 0 ||
      (!rel->key.empty() &&
       sm1::load_put(loader, id_stream, id_entry(r), k) != 0)) {
    return -1;
  }
  id = r;
  if (nullptr != key) {
    *key = std::move(k);
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

/* Read the datum a TOAST pointer names, from its store; see sm2.h. */
int toast_fetch(sm1::session_t *session, std::string_view ptr,
                std::string &out) {
  toast_ptr_t p;
  if (!parse_toast_ptr(ptr, p)) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "toast: not a TOAST pointer");
  }
  return toast_read(session, p, out);
}

/* Open a cursor over the rows with _id first..last, both included. */
int rel_scan(rel_t *rel, uint64_t first, uint64_t last, rel_scan_t *&out) {
  bool keyed = !rel->key.empty();
  // A keyed rel's rows are in _id order in its _id index.
  std::string lo = keyed ? id_entry(first) : row_key(rel->relid, id_key(first));
  std::string hi = keyed ? id_entry(last) : row_key(rel->relid, id_key(last));
  // sm1 copies the bounds into the cursor.
  sm1::scan_t *c;
  if (sm1::scan(rel->session, rel->storeid, lo.data(), lo.size(), 1, hi.data(),
                hi.size(), 1, c) != 0) {
    return -1;
  }
  out = new rel_scan_t{
      c,          rel->session, rel->storeid,
      rel->relid, keyed,        keyed ? sm1::rawpin_new() : nullptr,
      {}};
  return 0;
}

/* The next row of a cursor: 0, 1 at the end, or -1. */
int rel_scannxt(rel_scan_t *scan, uint64_t &id, std::string_view &tuple) {
  for (;;) {
    std::string_view key, value;
    int rc = sm1::scannxt(scan->scan, key, value);
    if (rc != 0) {
      return rc;
    }
    // The range holds only this relid's or the _id index's 10-byte
    // keys.
    if (key.size() != relid_len + id_len) {
      return sm1::seterr(sm1::SM_ERR_CORRUPT,
                         "rel: a key of " + std::to_string(key.size()) +
                             " bytes, not 10, under an _id");
    }
    uint64_t r;
    key.remove_prefix(relid_len);
    get_be(key, r);
    if (!scan->keyed) {
      scan->key = id_key(r);
      id = r;
      return split_value(value, nullptr, tuple);
    }
    // An entry of the _id index: its value is the row's key, and
    // the row is read by it, into the cursor's pin.
    scan->key.assign(value);
    std::string_view v;
    rc = sm1::read(scan->session, scan->storeid,
                   row_key(scan->relid, scan->key), scan->pin, v);
    if (rc < 0) {
      return -1;
    }
    // Gone at the transaction's view, as this transaction moved
    // or deleted it since the cursor's.
    if (rc == 1) {
      continue;
    }
    id = r;
    return split_value(v, nullptr, tuple);
  }
}

/* The key of the row a cursor last gave; see sm2.h. */
std::string_view rel_scan_key(const rel_scan_t *scan) { return scan->key; }

/* Move a cursor forward to _id id. */
int rel_scan_seek(rel_scan_t *scan, uint64_t id) {
  return sm1::scan_seek(
      scan->scan, scan->keyed ? id_entry(id) : row_key(scan->relid, id_key(id)),
      1);
}

/* Close a cursor; nullptr is a no-op. */
void rel_scanend(rel_scan_t *scan) {
  if (scan == nullptr) {
    return;
  }
  // Release sm1's cursor and its snapshot, and the row it read.
  sm1::scanend(scan->scan);
  sm1::rawpin_free(scan->pin);
  delete scan;
}

/* Whether a rollback closed the cursor; see sm2.h. */
bool rel_scan_rolled_back(const rel_scan_t *scan) {
  return sm1::scan_rolled_back(scan->scan);
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

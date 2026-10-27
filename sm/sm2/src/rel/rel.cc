/*
 * rel.cc -- the rel (../CLUSTER.md §2, ../ROWKEY.md §2.1): a table's
 * rows in one sm1 store, keyed by relid and their primary key, or
 * in a rel without one by their _id, each holding its tuple alone.
 * It is a codec in front of sm1, not a state machine: a durable
 * effect is an sm1 call in the caller's transaction, sm1's errors
 * pass through as they are, and the tuples it hands out are sm1's
 * views. A row with a datum out of line (TOAST, SM2_DESIGN.md
 * §4.2) is more than one sm1 call: the row, then the datum's chunks
 * (toast.cc).
 */

#include "rel.h"
#include "native.h"
#include "sm2.h"
#include "toast.h"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

/* The sm1 key of the row at key: relid then key. */
std::string row_key(uint16_t relid, std::string_view key) {
  std::string k;
  sm2::put_be(relid, k);
  k += key;
  return k;
}

/*
 * The _id of the row at key, a key past the relid: a keyless rel's
 * row has it as its key; a keyed rel's has none, 0.
 */
uint64_t id_at(const sm2::rel_t *rel, std::string_view key) {
  uint64_t id = 0;
  if (rel->key.empty()) {
    sm2::get_be(key, id);
  }
  return id;
}

/*
 * A read's return, rc, of the row at key, with its _id into id if
 * wanted: as rc, the tuple emptied on anything but 0.
 */
int read_out(const sm2::rel_t *rel, std::string_view key, int rc,
             std::string_view &tuple, uint64_t *id) {
  if (0 != rc) {
    tuple = {};
    return rc;
  }
  if (nullptr != id) {
    *id = id_at(rel, key);
  }
  return 0;
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

/*
 * A new _id for a row of rel, from its sequence, looked up once by
 * its name, a read of sm1's catalog, at the rel's first draw.
 */
int next_id(sm2::rel_t *rel, uint64_t &id) {
  if (!rel->has_id_ref) {
    if (sm1::seqref(rel->session, rel->id_seq, rel->id_ref) != 0) {
      return -1;
    }
    rel->has_id_ref = true;
  }
  return sm1::nextseq(rel->session, rel->id_ref, id);
}

/*
 * Write tuple, already checked, under key. A keyless rel's key is
 * an _id drawn from its sequence, so it was never written before,
 * and goes in with no check that it is absent (sm1::insert_new); a
 * keyed row's is checked, as its key is its primary key's.
 */
int write_new(sm2::rel_t *rel, std::string_view key, std::string_view tuple) {
  std::string k = row_key(rel->relid, key);
  if (rel->chained) {
    return rel->key.empty()
               ? sm1::chain_insert_new(rel->session, rel->storeid, k, tuple)
               : sm1::chain_insert(rel->session, rel->storeid, k, tuple);
  }
  return rel->key.empty()
             ? sm1::insert_new(rel->session, rel->storeid, k, tuple)
             : sm1::insert(rel->session, rel->storeid, k, tuple);
}

/*
 * Replace the row at key with tuple, already checked; with pin, the
 * tuple replaced pinned into it, and old viewing it.
 */
int write_update(sm2::rel_t *rel, std::string_view key, std::string_view tuple,
                 sm1::rawpin_t *pin, std::string_view *old) {
  std::string k = row_key(rel->relid, key);
  if (rel->chained) {
    return pin ? sm1::chain_update(rel->session, rel->storeid, k, tuple, pin,
                                   *old)
               : sm1::chain_update(rel->session, rel->storeid, k, tuple);
  }
  return pin ? sm1::update(rel->session, rel->storeid, k, tuple, pin, *old)
             : sm1::update(rel->session, rel->storeid, k, tuple);
}

/*
 * Delete the row at key, with pin or without; with pin, its tuple
 * pinned into it, and old viewing it.
 */
int write_delete(sm2::rel_t *rel, std::string_view key, sm1::rawpin_t *pin,
                 std::string_view *old) {
  std::string k = row_key(rel->relid, key);
  if (rel->chained) {
    return pin ? sm1::chain_erase(rel->session, rel->storeid, k, pin, *old)
               : sm1::chain_erase(rel->session, rel->storeid, k);
  }
  return pin ? sm1::erase(rel->session, rel->storeid, k, pin, *old)
             : sm1::erase(rel->session, rel->storeid, k);
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
  const auto &schema = rel->desc->schema;
  std::vector<sm2::datum_t> d(datums.begin(), datums.end());
  if (!rel->toasts || d.size() != schema.size() ||
      isnull.size() != schema.size()) {
    // Nothing to move, or a row tup_encode refuses for its count.
    return sm2::tup_encode(*rel->desc, d, isnull, out.tuple);
  }
  // own's strings must not move once a datum points at one: at most
  // a fetched value and a pointer for each attribute.
  out.own.reserve(2 * d.size());
  // The variable-width datums, by attribute, each with its bytes.
  std::vector<std::pair<size_t, std::string_view>> var;
  for (size_t i = 0; i < d.size(); i++) {
    if (isnull[i] || rel->desc->width[i] > 0 || schema[i].dropped) {
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
  if (sm2::tup_size(*rel->desc, d, isnull, len) != 0) {
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
  return sm2::tup_encode(*rel->desc, d, isnull, out.tuple);
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
  for (size_t i = 0; i < rel->desc->schema.size(); i++) {
    if (rel->desc->width[i] > 0) {
      continue;
    }
    sm2::datum_t val;
    bool isnull;
    if (sm2::tup_decode(*rel->desc, tuple, i, val, isnull) != 0) {
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
 * The primary key of rel encoded into key from cols, the values of
 * its columns in the key's order, a column out of line fetched
 * first. A null among them fails with SM_ERR_INVALID.
 */
int encode_cols(sm2::rel_t *rel, std::span<const sm2::datum_t> cols,
                std::span<const bool> isnull, std::string &key) {
  const auto &schema = rel->desc->schema;
  size_t n = rel->key.size();
  // The first column out of line, n for none: the usual key is
  // encoded from cols as they are, with nothing allocated.
  size_t first_toast = n;
  for (size_t j = 0; j < n; j++) {
    int32_t att = rel->key_atts[j];
    if (isnull[j]) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "rel: attribute " + std::to_string(att) +
                             " of the primary key is null");
    }
    if (n != first_toast || rel->desc->width[att] > 0) {
      continue;
    }
    std::string_view v;
    if (0 !=
        sm2::datum_bytes(schema[att].type, schema[att].typmod, cols[j], v)) {
      return -1;
    }
    sm2::toast_ptr_t p;
    if (sm2::parse_toast_ptr(v, p)) {
      first_toast = j;
    }
  }
  key.clear();
  bool cut;
  // No null got this far: isnull is all false.
  if (n == first_toast) {
    return sm2::encode_key(rel->key, cols, isnull, sm2::max_pk_len,
                           sm2::overflow_t::refuse, key, cut);
  }
  std::vector<sm2::datum_t> c(cols.begin(), cols.end());
  // A fetched value must not move once a datum points at it.
  std::vector<std::string> own;
  own.reserve(n);
  for (size_t j = first_toast; j < n; j++) {
    int32_t att = rel->key_atts[j];
    if (rel->desc->width[att] > 0) {
      continue;
    }
    std::string_view v;
    if (0 != sm2::datum_bytes(schema[att].type, schema[att].typmod, c[j], v)) {
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
    c[j] = sm2::datum_from_ptr(own.back().data());
  }
  return sm2::encode_key(rel->key, c, isnull, sm2::max_pk_len,
                         sm2::overflow_t::refuse, key, cut);
}

/*
 * The key of the row with _id id and values d and isnull, into key:
 * id_key(id), or the primary key's columns encoded.
 */
int key_of(sm2::rel_t *rel, uint64_t id, std::span<const sm2::datum_t> d,
           std::span<const bool> isnull, std::string &key) {
  key.clear();
  if (rel->key.empty()) {
    sm2::put_be(id, key);
    return 0;
  }
  const auto &schema = rel->desc->schema;
  if (d.size() != schema.size() || isnull.size() != schema.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "rel: a datum and null flag for every attribute");
  }
  size_t n = rel->key.size();
  std::vector<sm2::datum_t> cols(n);
  std::unique_ptr<bool[]> nulls(new bool[n]());
  for (size_t j = 0; j < n; j++) {
    cols[j] = d[rel->key_atts[j]];
    nulls[j] = isnull[rel->key_atts[j]];
  }
  return encode_cols(rel, cols, std::span<const bool>(nulls.get(), n), key);
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
  // A keyed row is named by its primary key, and has no _id.
  std::string k;
  uint64_t r = 0;
  if (!rel->key.empty()) {
    if (key_of(rel, 0, datums, isnull, k) != 0) {
      return -1;
    }
  } else {
    // Draw the _id from the table's sequence, unless it was.
    r = reserved;
    if (r == 0 && next_id(rel, r) != 0) {
      return -1;
    }
    k = sm2::id_key(r);
  }
  if (write_new(rel, k, row.tuple) != 0 || write_moved(rel, row) != 0) {
    return -1;
  }
  id = r;
  key = std::move(k);
  return 0;
}

/*
 * Move the row at key to new_key, with tuple: erase it with a
 * move's tombstone, which a writer at key then meets as
 * SM_ERR_MOVED (../ROWKEY.md §2.3), its old row pinned into p and
 * viewed by was; then insert it under new_key (../CLUSTER.md §4).
 */
int move_row(sm2::rel_t *rel, std::string_view key, std::string_view new_key,
             std::string_view tuple, sm1::rawpin_t *p, std::string_view &was) {
  std::string from = row_key(rel->relid, key);
  std::string to = row_key(rel->relid, new_key);
  if (rel->chained) {
    return 0 != sm1::chain_erase_moved(rel->session, rel->storeid, from, p, was)
               ? -1
               : sm1::chain_insert(rel->session, rel->storeid, to, tuple);
  }
  if (sm1::erase_moved(rel->session, rel->storeid, from, p, was) != 0) {
    return -1;
  }
  return sm1::insert(rel->session, rel->storeid, to, tuple);
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
    if (write_update(rel, key, row.tuple, pin, old) != 0) {
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
  int rc = 0 != (moves ? move_row(rel, key, new_key, row.tuple, p, was)
                       : write_update(rel, key, row.tuple, p, &was)) ||
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
  size_t n = rel->desc->schema.size();
  datums.assign(n, 0);
  isnull.reset(new bool[n]());
  return sm2::tup_deform(*rel->desc, tuple, datums,
                         std::span<bool>(isnull.get(), n));
}

/*
 * Fail with SM_ERR_CORRUPT: a key of n bytes under an _id. Out of
 * line, so the scan's step builds no message.
 */
[[gnu::cold, gnu::noinline]] int bad_id_key(size_t n) {
  return sm1::seterr(sm1::SM_ERR_CORRUPT, "rel: a key of " + std::to_string(n) +
                                              " bytes, not 10, under an _id");
}

} // namespace

namespace sm2 {

/* The key of the row with _id id in a rel keyed by _id. */
std::string id_key(uint64_t id) {
  std::string k;
  put_be(id, k);
  return k;
}

/*
 * Open a rel laid out by desc, which own holds or the caller keeps
 * alive: rel_open's and rel_open_desc's body.
 */
static int open_rel(sm1::session_t *session, uint64_t storeid, uint16_t relid,
                    const tup_desc_t &desc, std::unique_ptr<tup_desc_t> own,
                    const rel_key_t &key, std::string_view id_seq,
                    std::string_view toast_seq, rel_t *&out) {
  if (session == nullptr) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "rel: no session");
  }
  std::span<const attribute_t> schema = desc.schema;
  // Every attribute must be one the codec can lay out.
  if (SIZE_MAX != desc.bad) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "rel: attribute " + std::to_string(desc.bad) +
                           " has no type, or a typmod its type refuses");
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
  auto rel = std::make_unique<rel_t>();
  rel->session = session;
  rel->storeid = storeid;
  rel->relid = relid;
  rel->own_desc = std::move(own);
  rel->desc = &desc;
  rel->key = key.cols;
  rel->key_atts = key.attnums;
  rel->id_seq = std::string(id_seq);
  rel->toast_seq = std::string(toast_seq);
  // Only a variable-width datum is ever moved out of line.
  rel->toasts = desc.varwidth && !toast_seq.empty();
  // Its rows are chained keys if its store was made so.
  if (sm1::store_chained(session, storeid, rel->chained) != 0) {
    return -1;
  }
  out = rel.release();
  return 0;
}

/* Open a rel, laying its schema out; see sm2.h. */
int rel_open(sm1::session_t *session, uint64_t storeid, uint16_t relid,
             std::span<const attribute_t> schema, const rel_key_t &key,
             std::string_view id_seq, std::string_view toast_seq, rel_t *&out) {
  // Lay the schema out once, for every row the rel encodes.
  auto own = std::make_unique<tup_desc_t>(schema);
  const tup_desc_t &desc = *own;
  return open_rel(session, storeid, relid, desc, std::move(own), key, id_seq,
                  toast_seq, out);
}

/* Open a rel laid out by a desc it borrows; see sm2.h. */
int rel_open_desc(sm1::session_t *session, uint64_t storeid, uint16_t relid,
                  const tup_desc_t &desc, const rel_key_t &key,
                  std::string_view id_seq, std::string_view toast_seq,
                  rel_t *&out) {
  return open_rel(session, storeid, relid, desc, nullptr, key, id_seq,
                  toast_seq, out);
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
  return rel->desc->schema;
}

/* Whether a rel is keyed by a primary key; see sm2.h. */
bool rel_keyed(const rel_t *rel) { return !rel->key.empty(); }

/* Whether a rel's rows are a chained store's; see sm2.h. */
bool rel_chained(const rel_t *rel) { return rel->chained; }

/* The key of a row of these values; see sm2.h. */
int rel_row_key(rel_t *rel, uint64_t id, std::span<const datum_t> datums,
                std::span<const bool> isnull, std::string &key) {
  return key_of(rel, id, datums, isnull, key);
}

/* The key of a row named by its key's values; see sm2.h. */
int rel_key(rel_t *rel, std::span<const datum_t> datums,
            std::span<const bool> isnull, std::string &key) {
  size_t n = rel->key.empty() ? 1 : rel->key.size();
  if (datums.size() != n || isnull.size() != n) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "rel: a key of " + std::to_string(n) + " columns");
  }
  if (!rel->key.empty()) {
    return encode_cols(rel, datums, isnull, key);
  }
  if (isnull[0]) {
    return sm1::seterr(sm1::SM_ERR_INVALID, "rel: a null _id");
  }
  key = id_key(static_cast<uint64_t>(datum_to_int64(datums[0])));
  return 0;
}

/* Insert a row under a new _id; see sm2.h. */
int rel_insert(rel_t *rel, std::string_view tuple, uint64_t &id,
               std::string *key) {
  // Check first, so a tuple that cannot be stored draws no _id.
  if (tup_check(*rel->desc, tuple) != 0) {
    return -1;
  }
  std::string k;
  if (!rel->toasts && rel->key.empty()) {
    uint64_t r;
    if (next_id(rel, r) != 0 || write_new(rel, id_key(r), tuple) != 0) {
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
  if (!rel->key.empty()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "rel: a keyed rel's rows have no _id to reserve");
  }
  // From the table's sequence, as rel_insert draws one.
  return next_id(rel, id);
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

/* Read the row at key: 0, 1 if absent, or -1. */
int rel_read(rel_t *rel, std::string_view key, std::string_view &tuple,
             uint64_t *id) {
  std::string k = row_key(rel->relid, key);
  return read_out(rel, key,
                  rel->chained
                      ? sm1::chain_read(rel->session, rel->storeid, k, tuple)
                      : sm1::read(rel->session, rel->storeid, k, tuple),
                  tuple, id);
}

/* Read the row at key into pin. */
int rel_read(rel_t *rel, std::string_view key, sm1::rawpin_t *pin,
             std::string_view &tuple, uint64_t *id) {
  std::string k = row_key(rel->relid, key);
  return read_out(
      rel, key,
      rel->chained ? sm1::chain_read(rel->session, rel->storeid, k, pin, tuple)
                   : sm1::read(rel->session, rel->storeid, k, pin, tuple),
      tuple, id);
}

/* Lock the row at key in mode, without writing it. */
int rel_lock(rel_t *rel, std::string_view key, sm1::lockmode_t mode) {
  // Held to the end of the transaction, as a write's lock is.
  std::string k = row_key(rel->relid, key);
  return rel->chained ? sm1::chain_lock(rel->session, rel->storeid, k, mode)
                      : sm1::lock(rel->session, sm1::LOCKTYPE_ROW, rel->storeid,
                                  k, mode);
}

/* Lock the row at key in mode if it is free now; see sm2.h. */
int rel_trylock(rel_t *rel, std::string_view key, sm1::lockmode_t mode) {
  // The same lock, asked for without waiting.
  std::string k = row_key(rel->relid, key);
  return rel->chained ? sm1::chain_trylock(rel->session, rel->storeid, k, mode)
                      : sm1::trylock(rel->session, sm1::LOCKTYPE_ROW,
                                     rel->storeid, k, mode);
}

/* Read the newest committed version of the row at key. */
int rel_read_latest(rel_t *rel, std::string_view key, std::string_view &tuple,
                    uint64_t *id) {
  std::string k = row_key(rel->relid, key);
  return read_out(
      rel, key,
      rel->chained
          ? sm1::chain_read_latest(rel->session, rel->storeid, k, tuple)
          : sm1::read_latest(rel->session, rel->storeid, k, tuple),
      tuple, id);
}

/* Read the newest committed version of the row at key into a pin. */
int rel_read_latest(rel_t *rel, std::string_view key, sm1::rawpin_t *pin,
                    std::string_view &tuple, uint64_t *id) {
  std::string k = row_key(rel->relid, key);
  return read_out(
      rel, key,
      rel->chained
          ? sm1::chain_read_latest(rel->session, rel->storeid, k, pin, tuple)
          : sm1::read_latest(rel->session, rel->storeid, k, pin, tuple),
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
  if (tup_check(*rel->desc, tuple) != 0) {
    return -1;
  }
  if (!rel->toasts && rel->key.empty()) {
    if (write_update(rel, key, tuple, pin, old) != 0) {
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
 * the row held out of line go with it, read from the row pinned.
 */
static int delete_row(rel_t *rel, std::string_view key, sm1::rawpin_t *pin,
                      std::string_view *old, bool hold) {
  // No old row until the write succeeds.
  if (nullptr != old) {
    *old = {};
  }
  if (!rel->toasts) {
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

/* Put a row into a load, a keyless one under a new _id; see sm2.h. */
int rel_load(rel_t *rel, sm1::loader_t *loader, size_t stream,
             std::span<const datum_t> datums, std::span<const bool> isnull,
             uint64_t &id, std::string *key) {
  size_t len;
  if (tup_size(*rel->desc, datums, isnull, len) != 0) {
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
    if (isnull[i] || rel->desc->width[i] > 0) {
      continue;
    }
    std::string_view v;
    if (datum_bytes(rel->desc->schema[i].type, rel->desc->schema[i].typmod,
                    datums[i], v) != 0) {
      return -1;
    }
    if (varlena_is_toast(v) ||
        (len > toast_threshold && varlena_payload(v).size() >= toast_min)) {
      return 1;
    }
  }
  std::string tuple, k;
  if (tup_encode(*rel->desc, datums, isnull, tuple) != 0 ||
      (!rel->key.empty() && key_of(rel, 0, datums, isnull, k) != 0)) {
    return -1;
  }
  // A keyless row's _id from the table's sequence: a load's ascend.
  uint64_t r = 0;
  if (rel->key.empty()) {
    if (next_id(rel, r) != 0) {
      return -1;
    }
    k = id_key(r);
  }
  if (sm1::load_put(loader, stream, row_key(rel->relid, k), tuple) != 0) {
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

/*
 * Open a cursor over the rows with _id first..last, both included,
 * of a rel without a primary key; a keyed rel's rows are scanned in
 * key order (idx_scan_rows): SM_ERR_INVALID.
 */
int rel_scan(rel_t *rel, uint64_t first, uint64_t last, rel_scan_t *&out) {
  if (!rel->key.empty()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "rel: a keyed rel's rows have no _id to scan by");
  }
  std::string lo = row_key(rel->relid, id_key(first));
  std::string hi = row_key(rel->relid, id_key(last));
  // sm1 copies the bounds into the cursor.
  sm1::scan_t *c;
  if ((rel->chained
           ? sm1::chain_scan(rel->session, rel->storeid, lo.data(), lo.size(),
                             1, hi.data(), hi.size(), 1, c)
           : sm1::scan(rel->session, rel->storeid, lo.data(), lo.size(), 1,
                       hi.data(), hi.size(), 1, c)) != 0) {
    return -1;
  }
  out = new rel_scan_t{c, rel->session, rel->storeid, rel->relid, false, {}};
  return 0;
}

/*
 * The next row of a cursor: 0, 1 at the end, or -1. id is a keyless
 * row's _id, read from its key, or 0 for a keyed row.
 */
int rel_scannxt(rel_scan_t *scan, uint64_t &id, std::string_view &tuple) {
  // sm1 hands out the key into the cursor and the row into tuple,
  // both empty at the end and on failure.
  int rc = sm1::scannxt(scan->scan, scan->key, tuple);
  if (rc != 0) {
    return rc;
  }
  // The row's key is sm1's, past the relid, which every key in the
  // range begins with.
  scan->key.remove_prefix(relid_len);
  id = 0;
  if (!scan->by_key) {
    // The range holds only this relid's 10-byte keys.
    if (scan->key.size() != id_len) {
      return bad_id_key(relid_len + scan->key.size());
    }
    std::string_view k = scan->key;
    get_be(k, id);
  }
  return 0;
}

/* The key of the row a cursor last gave; see sm2.h. */
std::string_view rel_scan_key(const rel_scan_t *scan) { return scan->key; }

/* Move a cursor forward to _id id. */
int rel_scan_seek(rel_scan_t *scan, uint64_t id) {
  if (scan->by_key) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "rel: a cursor in key order seeks no _id");
  }
  return sm1::scan_seek(scan->scan, row_key(scan->relid, id_key(id)), 1);
}

/* Move a cursor in key order forward to a key; see sm2.h. */
int rel_scan_seek_key(rel_scan_t *scan, std::string_view key) {
  return sm1::scan_seek(scan->scan, row_key(scan->relid, key), 1);
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

/* Whether a rollback closed the cursor; see sm2.h. */
bool rel_scan_rolled_back(const rel_scan_t *scan) {
  return sm1::scan_rolled_back(scan->scan);
}

} // namespace sm2

namespace {

/*
 * The fewest bytes sm1's estimate of a range is trusted to split:
 * a sorted file resolves a range to its data blocks, 4 KB, and a
 * memtable's sampled estimate no better.
 */
constexpr uint64_t leaf_bytes = 4096;

/* The most halvings of a key range, past which a range is a leaf. */
constexpr int max_depth = 256;

/*
 * A key strictly between a and b, keys under one relid past it,
 * or with end above every such key: their mean, read as base-256
 * fractions, to one byte past the longer, trailing zeros dropped,
 * so that byte order and the fractions' order agree.
 */
std::string midpoint(std::string_view a, std::string_view b, bool end) {
  size_t n = std::max(a.size(), end ? 0 : b.size()) + 1;
  // Digit 0 holds what carries past the point.
  std::vector<unsigned> s(n + 1, 0);
  s[0] = end ? 1 : 0;
  for (size_t i = 0; i < n; i++) {
    unsigned x = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
    unsigned y = !end && i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
    s[i + 1] = x + y;
  }
  for (size_t i = n; i > 0; i--) {
    s[i - 1] += s[i] / 256;
    s[i] %= 256;
  }
  unsigned rem = 0;
  for (size_t i = 0; i <= n; i++) {
    unsigned v = rem * 256 + s[i];
    s[i] = v / 2;
    rem = v % 2;
  }
  std::string m;
  for (size_t i = 1; i <= n; i++) {
    m.push_back(static_cast<char>(s[i]));
  }
  while (!m.empty() && m.back() == '\0') {
    m.pop_back();
  }
  return m;
}

/* A rel's key range to bisect, and its estimate's reader. */
struct bisect_t {
  sm2::rel_t *rel;
  std::string prefix; /* the relid, every row key's first bytes */
  std::string limit;  /* the next relid, or empty for none */

  /* The bytes of the rel's keys from a up to b, or to its end. */
  int size(std::string_view a, std::string_view b, bool end, uint64_t &out) {
    std::string lo = prefix + std::string(a);
    std::string hi = end ? limit : prefix + std::string(b);
    uint64_t disk, mem;
    const char *h = hi.empty() ? nullptr : hi.data();
    if (0 != (rel->chained
                  ? sm1::chain_approxsize(rel->session, rel->storeid, lo.data(),
                                          lo.size(), h, hi.size(), disk, mem)
                  : sm1::approxsize(rel->session, rel->storeid, lo.data(),
                                    lo.size(), h, hi.size(), disk, mem))) {
      return -1;
    }
    out = disk + mem;
    return 0;
  }

  /*
   * Put into keys[at..] the keys at fractions fs of the range a to
   * b (or its end), each in [0, 1) of it.
   */
  int find(std::string_view a, std::string_view b, bool end,
           std::span<const double> fs, size_t at,
           std::vector<std::string> &keys, int depth) {
    if (fs.empty()) {
      return 0;
    }
    std::string m = midpoint(a, b, end);
    uint64_t left = 0, right = 0;
    if (depth < max_depth &&
        (0 != size(a, m, false, left) || 0 != size(m, b, end, right))) {
      return -1;
    }
    // Too few bytes to tell apart: every fraction is at its start.
    if (depth >= max_depth || left + right <= leaf_bytes) {
      for (size_t i = 0; i < fs.size(); i++) {
        keys[at + i].assign(a);
      }
      return 0;
    }
    double p = static_cast<double>(left) / static_cast<double>(left + right);
    size_t k = std::lower_bound(fs.begin(), fs.end(), p) - fs.begin();
    std::vector<double> lo(k), hi(fs.size() - k);
    for (size_t i = 0; i < k; i++) {
      lo[i] = fs[i] / p;
    }
    for (size_t i = k; i < fs.size(); i++) {
      hi[i - k] = (fs[i] - p) / (1 - p);
    }
    return 0 != find(a, m, false, lo, at, keys, depth + 1) ||
                   0 != find(m, b, end, hi, at + k, keys, depth + 1)
               ? -1
               : 0;
  }
};

} // namespace

namespace sm2 {

/* The keys at fractions of the rel's bytes; see sm2.h. */
int rel_keys_at(rel_t *rel, std::span<const double> fs,
                std::vector<std::string> &keys) {
  for (size_t i = 0; i < fs.size(); i++) {
    if (fs[i] < 0 || fs[i] >= 1 || (i > 0 && fs[i] < fs[i - 1])) {
      return sm1::seterr(sm1::SM_ERR_INVALID,
                         "rel: fractions in [0, 1), ascending");
    }
  }
  bisect_t b{rel, {}, {}};
  put_be(rel->relid, b.prefix);
  if (rel->relid != UINT16_MAX) {
    put_be(static_cast<uint16_t>(rel->relid + 1), b.limit);
  }
  keys.assign(fs.size(), std::string());
  if (0 != b.find({}, {}, true, fs, 0, keys, 0)) {
    return -1;
  }
  // A start estimated empty sends 0 past a block it cannot see:
  // the key at 0 is the rel's first, whatever the estimate.
  for (size_t i = 0; i < fs.size() && 0 == fs[i]; i++) {
    keys[i].clear();
  }
  return 0;
}

/* The bytes of the rel's keys in a range; see sm2.h. */
int rel_range_size(rel_t *rel, std::string_view lo, const std::string *hi,
                   uint64_t &out) {
  bisect_t b{rel, {}, {}};
  put_be(rel->relid, b.prefix);
  if (rel->relid != UINT16_MAX) {
    put_be(static_cast<uint16_t>(rel->relid + 1), b.limit);
  }
  return b.size(lo, hi ? *hi : std::string_view(), !hi, out);
}

/* The bytes under relid, from sm1's estimate of its key range. */
int relid_size(sm1::session_t *session, uint64_t storeid, uint16_t relid,
               uint64_t &disk, uint64_t &mem, bool old) {
  // Every key of the rel starts with its relid, so the range is
  // [relid, relid + 1); the last relid's is open above.
  std::string lo, hi;
  put_be(relid, lo);
  if (relid != UINT16_MAX) {
    put_be(static_cast<uint16_t>(relid + 1), hi);
  }
  const char *h = hi.empty() ? nullptr : hi.data();
  if (sm1::approxsize(session, storeid, lo.data(), lo.size(), h, hi.size(),
                      disk, mem) != 0) {
    return -1;
  }
  // A chained store's rows are its chained keys: a rel lives in the
  // one or the other, so the two files' estimates add. The memtables'
  // bytes are sm1's count of the relid's writes, chained ones and
  // their old versions included, so the chained estimate's are not.
  bool chained;
  if (sm1::store_chained(session, storeid, chained) != 0) {
    return -1;
  }
  if (chained) {
    uint64_t cdisk, cmem;
    if (sm1::chain_approxsize(session, storeid, lo.data(), lo.size(), h,
                              hi.size(), cdisk, cmem, old) != 0) {
      return -1;
    }
    disk += cdisk;
  }
  return 0;
}

} // namespace sm2

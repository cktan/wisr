/*
 * blob.cc -- the catalog's values (CATALOG.md §3). A value is a
 * sequence of parts, each an sm2 tuple under a schema this file
 * fixes, framed by its length as a varint. A part evolves as any
 * tuple does: an attribute appended to its schema reads as its
 * missing value in a part written before it.
 */

#include "catalog.h"

#include <algorithm>
#include <cstring>
#include <string>

namespace {

using sm2::attribute_t;

/* A live attribute of type, null when missing. */
attribute_t att(const sm2::type_t &type) {
  return {&type, -1, false, std::nullopt};
}

/* Fail with SM_ERR_CORRUPT: a catalog value is not what it should be. */
int corrupt(const char *why) {
  return sm1::seterr(sm1::SM_ERR_CORRUPT, std::string("catalog: ") + why);
}

/* Append v as an unsigned LEB128 varint, as sm2 frames natts. */
void put_varint(uint64_t v, std::string &out) {
  do {
    uint8_t byte = v & 0x7F;
    v >>= 7;
    out += static_cast<char>(byte | (v ? 0x80 : 0));
  } while (v);
}

/* Read a varint of at most 5 bytes at pos; false if malformed. */
bool get_varint(std::string_view in, size_t &pos, uint64_t &v) {
  v = 0;
  for (int i = 0; i < 5 && pos < in.size(); i++) {
    uint8_t byte = static_cast<uint8_t>(in[pos++]);
    v |= static_cast<uint64_t>(byte & 0x7F) << (7 * i);
    if (!(byte & 0x80)) {
      return true;
    }
  }
  return false;
}

/*
 * A part being written: its values and null flags, and the
 * varlenas its text and bytea values point at, which it owns.
 */
class writer_t {
public:
  /* A part of n attributes, every one null until set. */
  explicit writer_t(size_t n) : m_values(n, 0), m_isnull(new bool[n]), m_n(n) {
    // own must not move once a value points into it.
    m_own.reserve(n);
    for (size_t i = 0; i < n; i++) {
      m_isnull[i] = true;
    }
  }

  /* Set attribute i to an integer, a bool, or bytes. */
  void i2(size_t i, int16_t v) { set(i, sm2::datum_from_int16(v)); }
  void i4(size_t i, int32_t v) { set(i, sm2::datum_from_int32(v)); }
  void i8(size_t i, uint64_t v) {
    set(i, sm2::datum_from_int64(static_cast<int64_t>(v)));
  }
  void b(size_t i, bool v) { set(i, sm2::datum_from_bool(v)); }
  void bytes(size_t i, std::string_view v) {
    m_own.push_back(sm2::make_varlena(v));
    set(i, sm2::datum_from_ptr(m_own.back().data()));
  }
  void opt_i8(size_t i, const std::optional<uint64_t> &v) {
    if (v) {
      i8(i, *v);
    }
  }
  void f4(size_t i, float v) { set(i, sm2::datum_from_float(v)); }
  void f8(size_t i, double v) { set(i, sm2::datum_from_double(v)); }
  void opt_f4(size_t i, const std::optional<float> &v) {
    if (v) {
      f4(i, *v);
    }
  }

  /*
   * Set attribute i to the value of type whose bytes, as
   * sm2::datum_bytes gives them, are v: a by-value one copied into
   * the datum, a by-reference one pointing at a copy of v.
   */
  void value(size_t i, const sm2::type_t *type, std::string_view v) {
    if (!type->byval) {
      m_own.emplace_back(v);
      v = m_own.back();
    }
    set(i, sm3::datum_of_bytes(type, v));
  }

  /* Encode the part under desc and append it, framed, to blob. */
  int append(const sm2::tup_desc_t &desc, std::string &blob) const {
    std::string tuple;
    if (sm2::tup_encode(desc, m_values,
                        std::span<const bool>(m_isnull.get(), m_n),
                        tuple) != 0) {
      return -1;
    }
    put_varint(tuple.size(), blob);
    blob += tuple;
    return 0;
  }

private:
  /* Set attribute i, not null. */
  void set(size_t i, sm2::datum_t v) {
    m_values[i] = v;
    m_isnull[i] = false;
  }

  std::vector<sm2::datum_t> m_values;
  std::unique_ptr<bool[]> m_isnull;
  size_t m_n;
  std::vector<std::string> m_own;
};

/*
 * A part read back, with typed access to its attributes. Each
 * required attribute must be there; a null one is corruption.
 */
class reader_t {
public:
  /* Read the part at pos in blob under part's schema. */
  int read(std::string_view blob, size_t &pos, sm3::part_t part) {
    m_part = part;
    return sm3::read_part(blob, pos, sm3::part_desc(part), m_row);
  }

  /* Read the part at pos in blob under desc, a layout of the caller's. */
  int read(std::string_view blob, size_t &pos, const sm2::tup_desc_t &desc) {
    return sm3::read_part(blob, pos, desc, m_row);
  }

  /* Attribute i as an integer, a bool, or bytes; ok false if null. */
  int16_t i2(size_t i) { return sm2::datum_to_int16(get(i)); }
  int32_t i4(size_t i) { return sm2::datum_to_int32(get(i)); }
  uint64_t i8(size_t i) {
    return static_cast<uint64_t>(sm2::datum_to_int64(get(i)));
  }
  bool b(size_t i) { return sm2::datum_to_bool(get(i)); }
  std::string bytes(size_t i) {
    const attribute_t &a = sm3::part_schema(m_part)[i];
    std::string_view v;
    if (m_row.isnull[i] ||
        sm2::datum_bytes(a.type, a.typmod, m_row.values[i], v) != 0) {
      ok = false;
      return {};
    }
    return std::string(sm2::varlena_payload(v));
  }
  std::optional<std::string> opt_bytes(size_t i) {
    if (m_row.isnull[i]) {
      return std::nullopt;
    }
    return bytes(i);
  }
  std::optional<uint64_t> opt_i8(size_t i) {
    if (m_row.isnull[i]) {
      return std::nullopt;
    }
    return i8(i);
  }
  std::optional<bool> opt_b(size_t i) {
    if (m_row.isnull[i]) {
      return std::nullopt;
    }
    return b(i);
  }
  std::optional<int16_t> opt_i2(size_t i) {
    if (m_row.isnull[i]) {
      return std::nullopt;
    }
    return i2(i);
  }
  std::optional<int32_t> opt_i4(size_t i) {
    if (m_row.isnull[i]) {
      return std::nullopt;
    }
    return i4(i);
  }
  float f4(size_t i) { return sm2::datum_to_float(get(i)); }
  double f8(size_t i) { return sm2::datum_to_double(get(i)); }
  std::optional<float> opt_f4(size_t i) {
    if (m_row.isnull[i]) {
      return std::nullopt;
    }
    return f4(i);
  }

  /* Attribute i, of type a, as its bytes; ok false if null. */
  std::string value(size_t i, const attribute_t &a) {
    std::string_view v;
    if (m_row.isnull[i] ||
        sm2::datum_bytes(a.type, a.typmod, m_row.values[i], v) != 0) {
      ok = false;
      return {};
    }
    return std::string(v);
  }

  /* False once a required attribute was found null. */
  bool ok = true;

private:
  /* Attribute i's value; a null one clears ok. */
  sm2::datum_t get(size_t i) {
    if (m_row.isnull[i]) {
      ok = false;
      return 0;
    }
    return m_row.values[i];
  }

  sm3::part_t m_part = sm3::part_t::meta;
  sm3::row_t m_row;
};

/* The schema of each part, in part_t order (CATALOG.md §3). */
const std::vector<attribute_t> &schema_of(sm3::part_t part) {
  using namespace sm2;
  static const std::vector<attribute_t> schemas[] = {
      /* meta */ {att(type_int4)},
      /* name */ {att(type_int2), att(type_int8), att(type_int8)},
      /* table */
      {att(type_int8), att(type_text), att(type_int8), att(type_text),
       att(type_int2)},
      /* column */
      {att(type_text), att(type_int2), att(type_int4), att(type_bool),
       att(type_bool), att(type_bytea), att(type_int8), att(type_bytea),
       att(type_int2), att(type_int8)},
      /* index */
      {att(type_int8), att(type_text), att(type_int2), att(type_bool),
       att(type_int4), att(type_int4), att(type_int2), att(type_int2),
       att(type_text), att(type_bytea), att(type_int4), att(type_bool)},
      /* index_key */
      {att(type_int4), att(type_bool), att(type_bool), att(type_int8),
       att(type_int2), att(type_int4)},
      /* include */ {att(type_int4)},
      /* nspace */ {att(type_text)},
      /* collation */
      {att(type_text), att(type_text), att(type_bool), att(type_text)},
      /* index_state */ {att(type_int2), att(type_int2), att(type_text)},
      /* sequence */
      {att(type_int8), att(type_text), att(type_int8), att(type_int8),
       att(type_int8), att(type_int4)},
      /* stats_table */
      {att(type_int4), att(type_int4), att(type_int4), att(type_int8),
       att(type_int8), att(type_float8), att(type_int8), att(type_int8),
       att(type_int4), att(type_int4), att(type_int4)},
      /* stats_index */
      {att(type_int8), att(type_int8), att(type_int8), att(type_float8),
       att(type_float4)},
      /* statistic */
      {att(type_int8), att(type_int4), att(type_float4), att(type_int4),
       att(type_float4), att(type_float4), att(type_int4), att(type_int4)},
      /* constraint */
      {att(type_int8), att(type_text), att(type_int2), att(type_bool),
       att(type_bytea)},
      /* fkey */
      {att(type_int8), att(type_int8), att(type_int4), att(type_int2),
       att(type_int2), att(type_int2), att(type_bool), att(type_bool)},
      /* fkey_pair */ {att(type_int4), att(type_int4)},
      /* view */
      {att(type_int8), att(type_text), att(type_bytea), att(type_bytea),
       att(type_int4)},
      /* view_read */ {att(type_int8), att(type_int4)},
  };
  return schemas[static_cast<size_t>(part)];
}

/* Fail unless every part of blob was read: pos is its end. */
int at_end(std::string_view blob, size_t pos) {
  return pos == blob.size() ? 0 : corrupt("bytes past a value's last part");
}

} // namespace

namespace sm3 {

/* A part's schema; see catalog.h. */
const std::vector<attribute_t> &part_schema(part_t part) {
  return schema_of(part);
}

/* A part's layout, built once from its schema. */
const sm2::tup_desc_t &part_desc(part_t part) {
  static const sm2::tup_desc_t descs[] = {
      schema_of(part_t::meta),        schema_of(part_t::name),
      schema_of(part_t::table),       schema_of(part_t::column),
      schema_of(part_t::index),       schema_of(part_t::index_key),
      schema_of(part_t::include),     schema_of(part_t::nspace),
      schema_of(part_t::collation),   schema_of(part_t::index_state),
      schema_of(part_t::sequence),    schema_of(part_t::stats_table),
      schema_of(part_t::stats_index), schema_of(part_t::statistic),
      schema_of(part_t::constraint),  schema_of(part_t::fkey),
      schema_of(part_t::fkey_pair),   schema_of(part_t::view),
      schema_of(part_t::view_read),
  };
  return descs[static_cast<size_t>(part)];
}

/* Read one framed part; see catalog.h. */
int read_part(std::string_view blob, size_t &pos, const sm2::tup_desc_t &desc,
              row_t &row) {
  uint64_t len;
  if (!get_varint(blob, pos, len) || len > blob.size() - pos) {
    return corrupt("a part cut short");
  }
  std::string_view tuple = blob.substr(pos, len);
  size_t n = desc.schema.size();
  row.values.assign(n, 0);
  row.isnull.reset(new bool[n]);
  if (sm2::tup_deform(desc, tuple, row.values,
                      std::span<bool>(row.isnull.get(), n)) != 0) {
    return corrupt("a part that is not a tuple of its schema");
  }
  pos += len;
  return 0;
}

/* META's value: the format version. */
int encode_meta(int32_t version, std::string &value) {
  writer_t w(1);
  w.i4(0, version);
  value.clear();
  return w.append(part_desc(part_t::meta), value);
}

/* Read META's value. */
int decode_meta(std::string_view value, int32_t &version) {
  reader_t r;
  size_t pos = 0;
  if (r.read(value, pos, part_t::meta) != 0) {
    return -1;
  }
  version = r.i4(0);
  return r.ok ? at_end(value, pos) : corrupt("META without its version");
}

/* A NAME key's value: what it names. */
int encode_name(const name_rec_t &rec, std::string &value) {
  writer_t w(3);
  w.i2(0, static_cast<int16_t>(rec.kind));
  w.i8(1, rec.oid);
  w.opt_i8(2, rec.table);
  value.clear();
  return w.append(part_desc(part_t::name), value);
}

/* Read a NAME key's value. */
int decode_name(std::string_view value, name_rec_t &rec) {
  reader_t r;
  size_t pos = 0;
  if (r.read(value, pos, part_t::name) != 0) {
    return -1;
  }
  rec.kind = static_cast<kind_t>(r.i2(0));
  rec.oid = r.i8(1);
  rec.table = r.opt_i8(2);
  return r.ok ? at_end(value, pos) : corrupt("a NAME value cut short");
}

/* A table's value: its own fields. */
int encode_table(const table_rec_t &rec, std::string &blob) {
  blob.clear();
  writer_t t(5);
  t.i8(0, rec.nspoid);
  t.bytes(1, rec.name);
  t.i8(2, rec.storeid);
  t.bytes(3, rec.rowno_seq);
  t.i2(4, static_cast<int16_t>(rec.next_relid));
  return t.append(part_desc(part_t::table), blob);
}

/* Read a table's value; its columns and indexes are left empty. */
int decode_table(std::string_view blob, table_rec_t &rec) {
  size_t pos = 0;
  reader_t t;
  if (t.read(blob, pos, part_t::table) != 0) {
    return -1;
  }
  rec.nspoid = t.i8(0);
  rec.name = t.bytes(1);
  rec.storeid = t.i8(2);
  rec.rowno_seq = t.bytes(3);
  rec.next_relid = static_cast<uint16_t>(t.i2(4));
  if (!t.ok) {
    return corrupt("a table part cut short");
  }
  rec.columns.clear();
  rec.indexes.clear();
  return at_end(blob, pos);
}

/* A column's value. */
int encode_column(const column_rec_t &c, std::string &blob) {
  blob.clear();
  writer_t w(10);
  w.bytes(0, c.name);
  w.i2(1, c.type_code);
  w.i4(2, c.typmod);
  w.b(3, c.not_null);
  w.b(4, c.dropped);
  if (c.missing) {
    w.bytes(5, *c.missing);
  }
  w.opt_i8(6, c.collation);
  if (c.default_expr) {
    w.bytes(7, *c.default_expr);
  }
  // An identity column's kind and sequence, or both null.
  if (c.identity != 0) {
    w.i2(8, c.identity);
    w.i8(9, c.identity_seq);
  }
  return w.append(part_desc(part_t::column), blob);
}

/* Read a column's value. */
int decode_column(std::string_view blob, column_rec_t &c) {
  size_t pos = 0;
  reader_t r;
  if (r.read(blob, pos, part_t::column) != 0) {
    return -1;
  }
  c.name = r.bytes(0);
  c.type_code = static_cast<uint8_t>(r.i2(1));
  c.typmod = r.i4(2);
  c.not_null = r.b(3);
  c.dropped = r.b(4);
  c.missing = r.opt_bytes(5);
  c.collation = r.opt_i8(6);
  c.default_expr = r.opt_bytes(7);
  c.identity = static_cast<char>(r.opt_i2(8).value_or(0));
  c.identity_seq = r.opt_i8(9).value_or(0);
  if (!r.ok) {
    return corrupt("a column part cut short");
  }
  return at_end(blob, pos);
}

/* An index's value: the index, then its key, INCLUDE and read columns. */
int encode_index(const index_rec_t &x, std::string &blob) {
  blob.clear();
  writer_t w(12);
  w.i8(0, x.oid);
  w.bytes(1, x.name);
  w.i2(2, static_cast<int16_t>(x.relid));
  w.b(3, x.unique);
  w.i4(4, static_cast<int32_t>(x.keys.size()));
  w.i4(5, static_cast<int32_t>(x.includes.size()));
  // Attributes 6 to 8 held the index's state, which has an
  // entry of its own now; they are left null.
  // Only a partial or an expression index has a blob, and
  // columns it reads, which follow its INCLUDE parts.
  if (!x.eval.empty()) {
    w.bytes(9, x.eval);
    w.i4(10, static_cast<int32_t>(x.reads.size()));
  }
  // A primary key's mark; null, as an index written before it was
  // kept has it, for any other.
  if (x.primary) {
    w.b(11, true);
  }
  if (w.append(part_desc(part_t::index), blob) != 0) {
    return -1;
  }
  for (const index_key_rec_t &k : x.keys) {
    writer_t kw(6);
    kw.i4(0, k.attnum);
    kw.b(1, k.desc);
    kw.b(2, k.nulls_first);
    kw.opt_i8(3, k.collation);
    // An expression has no column to take its type from.
    if (k.attnum < 0) {
      kw.i2(4, static_cast<int16_t>(k.type_code));
      kw.i4(5, k.typmod);
    }
    if (kw.append(part_desc(part_t::index_key), blob) != 0) {
      return -1;
    }
  }
  for (int32_t attnum : x.includes) {
    writer_t iw(1);
    iw.i4(0, attnum);
    if (iw.append(part_desc(part_t::include), blob) != 0) {
      return -1;
    }
  }
  // A column read is laid out as an INCLUDE column is.
  for (int32_t attnum : x.reads) {
    writer_t rw(1);
    rw.i4(0, attnum);
    if (rw.append(part_desc(part_t::include), blob) != 0) {
      return -1;
    }
  }
  return 0;
}

/* Read an index's value. */
int decode_index(std::string_view blob, index_rec_t &x) {
  size_t pos = 0;
  x = index_rec_t{};
  reader_t r;
  if (r.read(blob, pos, part_t::index) != 0) {
    return -1;
  }
  x.oid = r.i8(0);
  x.name = r.bytes(1);
  x.relid = static_cast<uint16_t>(r.i2(2));
  x.unique = r.b(3);
  int32_t nkey = r.i4(4), ninclude = r.i4(5);
  if (!r.ok || nkey < 0 || ninclude < 0) {
    return corrupt("an index part cut short");
  }
  x.eval = r.opt_bytes(9).value_or("");
  x.primary = r.opt_b(11).value_or(false);
  int32_t nread = r.opt_i4(10).value_or(0);
  if (nread < 0) {
    return corrupt("an index part cut short");
  }
  for (int32_t k = 0; k < nkey; k++) {
    reader_t kr;
    if (kr.read(blob, pos, part_t::index_key) != 0) {
      return -1;
    }
    x.keys.push_back({kr.i4(0), kr.b(1), kr.b(2), kr.opt_i8(3),
                      static_cast<uint8_t>(kr.opt_i2(4).value_or(0)),
                      kr.opt_i4(5).value_or(-1)});
    if (!kr.ok) {
      return corrupt("an index key part cut short");
    }
  }
  for (int32_t k = 0; k < ninclude; k++) {
    reader_t ir;
    if (ir.read(blob, pos, part_t::include) != 0) {
      return -1;
    }
    x.includes.push_back(ir.i4(0));
    if (!ir.ok) {
      return corrupt("an include part cut short");
    }
  }
  for (int32_t k = 0; k < nread; k++) {
    reader_t rr;
    if (rr.read(blob, pos, part_t::include) != 0) {
      return -1;
    }
    x.reads.push_back(rr.i4(0));
    if (!rr.ok) {
      return corrupt("a column an index reads cut short");
    }
  }
  return at_end(blob, pos);
}

/* An index's state: the state, and why its build failed. */
int encode_index_state(const index_status_t &rec, std::string &blob) {
  writer_t w(3);
  w.i2(0, static_cast<int16_t>(rec.state));
  // Only a failed index has a reason to keep.
  if (rec.state == index_state_t::failed) {
    w.i2(1, static_cast<int16_t>(rec.error));
    w.bytes(2, rec.reason);
  }
  blob.clear();
  return w.append(part_desc(part_t::index_state), blob);
}

/* Read an index's state. */
int decode_index_state(std::string_view blob, index_status_t &rec) {
  reader_t r;
  size_t pos = 0;
  if (r.read(blob, pos, part_t::index_state) != 0) {
    return -1;
  }
  int16_t state = r.i2(0);
  if (!r.ok || state < 0 ||
      state > static_cast<int16_t>(index_state_t::failed)) {
    return corrupt("an index in no known state");
  }
  rec.state = static_cast<index_state_t>(state);
  rec.error = static_cast<sm1::errnum_t>(r.opt_i2(1).value_or(sm1::SM_OK));
  rec.reason = r.opt_bytes(2).value_or("");
  return at_end(blob, pos);
}

/* A namespace's blob. */
int encode_nspace(const nspace_rec_t &rec, std::string &blob) {
  writer_t w(1);
  w.bytes(0, rec.name);
  blob.clear();
  return w.append(part_desc(part_t::nspace), blob);
}

/* Read a namespace's blob. */
int decode_nspace(std::string_view blob, nspace_rec_t &rec) {
  reader_t r;
  size_t pos = 0;
  if (r.read(blob, pos, part_t::nspace) != 0) {
    return -1;
  }
  rec.name = r.bytes(0);
  return r.ok ? at_end(blob, pos) : corrupt("a namespace part cut short");
}

/* A collation's blob. */
int encode_collation(const collation_rec_t &rec, std::string &blob) {
  writer_t w(4);
  w.bytes(0, rec.name);
  w.bytes(1, rec.locale);
  w.b(2, rec.deterministic);
  w.bytes(3, rec.icu_version);
  blob.clear();
  return w.append(part_desc(part_t::collation), blob);
}

/* Read a collation's blob. */
int decode_collation(std::string_view blob, collation_rec_t &rec) {
  reader_t r;
  size_t pos = 0;
  if (r.read(blob, pos, part_t::collation) != 0) {
    return -1;
  }
  rec.name = r.bytes(0);
  rec.locale = r.bytes(1);
  rec.deterministic = r.b(2);
  rec.icu_version = r.bytes(3);
  return r.ok ? at_end(blob, pos) : corrupt("a collation part cut short");
}

/* A sequence's blob. */
int encode_sequence(const sequence_rec_t &rec, std::string &blob) {
  writer_t w(6);
  w.i8(0, rec.nspoid);
  w.bytes(1, rec.name);
  w.i8(2, rec.start);
  w.i8(3, rec.increment);
  // Its owner, a table's column, or both null.
  if (rec.owner) {
    w.i8(4, *rec.owner);
    w.i4(5, rec.owner_attnum);
  }
  blob.clear();
  return w.append(part_desc(part_t::sequence), blob);
}

/* Read a sequence's blob. */
int decode_sequence(std::string_view blob, sequence_rec_t &rec) {
  reader_t r;
  size_t pos = 0;
  if (r.read(blob, pos, part_t::sequence) != 0) {
    return -1;
  }
  rec.nspoid = r.i8(0);
  rec.name = r.bytes(1);
  rec.start = r.i8(2);
  rec.increment = r.i8(3);
  rec.owner = r.opt_i8(4);
  rec.owner_attnum = r.opt_i4(5).value_or(-1);
  return r.ok ? at_end(blob, pos) : corrupt("a sequence part cut short");
}

/*
 * A foreign key's value (FOREIGNKEY.md §3): the constraint, its type
 * 'f'; the foreign key; a pair a column of each end.
 */
int encode_constraint(const fkey_rec_t &rec, std::string &blob) {
  if (rec.attnums.size() != rec.ref_attnums.size()) {
    return sm1::seterr(sm1::SM_ERR_INVALID,
                       "a foreign key's two ends have as many columns");
  }
  blob.clear();
  writer_t c(5);
  c.i8(0, rec.oid);
  c.bytes(1, rec.name);
  c.i2(2, 'f');
  c.b(3, rec.validated);
  if (!rec.host.empty()) {
    c.bytes(4, rec.host);
  }
  if (c.append(part_desc(part_t::constraint), blob) != 0) {
    return -1;
  }
  writer_t f(8);
  f.i8(0, rec.ref_table);
  f.i8(1, rec.ref_index);
  f.i4(2, static_cast<int32_t>(rec.attnums.size()));
  f.i2(3, rec.on_update);
  f.i2(4, rec.on_delete);
  f.i2(5, rec.match);
  f.b(6, rec.deferrable);
  f.b(7, rec.deferred);
  if (f.append(part_desc(part_t::fkey), blob) != 0) {
    return -1;
  }
  for (size_t i = 0; i < rec.attnums.size(); i++) {
    writer_t p(2);
    p.i4(0, rec.attnums[i]);
    p.i4(1, rec.ref_attnums[i]);
    if (p.append(part_desc(part_t::fkey_pair), blob) != 0) {
      return -1;
    }
  }
  return 0;
}

/* Read a foreign key's value. */
int decode_constraint(std::string_view blob, fkey_rec_t &rec) {
  size_t pos = 0;
  rec = fkey_rec_t{};
  reader_t c;
  if (c.read(blob, pos, part_t::constraint) != 0) {
    return -1;
  }
  rec.oid = c.i8(0);
  rec.name = c.bytes(1);
  int16_t type = c.i2(2);
  rec.validated = c.b(3);
  rec.host = c.opt_bytes(4).value_or("");
  if (!c.ok || type != 'f') {
    return corrupt("a constraint part cut short, or not a foreign key");
  }
  reader_t f;
  if (f.read(blob, pos, part_t::fkey) != 0) {
    return -1;
  }
  rec.ref_table = f.i8(0);
  rec.ref_index = f.i8(1);
  int32_t npairs = f.i4(2);
  rec.on_update = static_cast<char>(f.i2(3));
  rec.on_delete = static_cast<char>(f.i2(4));
  rec.match = static_cast<char>(f.i2(5));
  rec.deferrable = f.b(6);
  rec.deferred = f.b(7);
  if (!f.ok || npairs < 1) {
    return corrupt("a foreign key part cut short");
  }
  for (int32_t i = 0; i < npairs; i++) {
    reader_t p;
    if (p.read(blob, pos, part_t::fkey_pair) != 0) {
      return -1;
    }
    rec.attnums.push_back(p.i4(0));
    rec.ref_attnums.push_back(p.i4(1));
    if (!p.ok) {
      return corrupt("a foreign key's pair cut short");
    }
  }
  return at_end(blob, pos);
}

/* A view's value: the view, then a part for each relation it reads. */
int encode_view(const view_def_t &rec, std::string &blob) {
  blob.clear();
  writer_t v(5);
  v.i8(0, rec.nspoid);
  v.bytes(1, rec.name);
  v.bytes(2, rec.query);
  if (!rec.options.empty()) {
    v.bytes(3, rec.options);
  }
  v.i4(4, static_cast<int32_t>(rec.reads.size()));
  if (v.append(part_desc(part_t::view), blob) != 0) {
    return -1;
  }
  for (const view_read_t &r : rec.reads) {
    writer_t p(2);
    p.i8(0, r.relation);
    p.i4(1, r.attnum);
    if (p.append(part_desc(part_t::view_read), blob) != 0) {
      return -1;
    }
  }
  return 0;
}

/* Read a view's value. */
int decode_view(std::string_view blob, view_def_t &rec) {
  size_t pos = 0;
  rec = view_def_t{};
  reader_t v;
  if (v.read(blob, pos, part_t::view) != 0) {
    return -1;
  }
  rec.nspoid = v.i8(0);
  rec.name = v.bytes(1);
  rec.query = v.bytes(2);
  rec.options = v.opt_bytes(3).value_or("");
  int32_t nread = v.i4(4);
  if (!v.ok || nread < 0) {
    return corrupt("a view part cut short");
  }
  for (int32_t i = 0; i < nread; i++) {
    reader_t p;
    if (p.read(blob, pos, part_t::view_read) != 0) {
      return -1;
    }
    rec.reads.push_back({p.i8(0), p.i4(1)});
    if (!p.ok) {
      return corrupt("a view's read cut short");
    }
  }
  return at_end(blob, pos);
}

} // namespace sm3

namespace {

/*
 * The attribute the values of statistic s have under def, into out:
 * its column's, or its index expression's. False if def has no such
 * one: a column out of range or dropped, an index gone, or a place
 * that is a column's and not an expression's.
 */
bool stat_attribute(const sm3::stat_t &s, const sm3::table_def_t &def,
                    attribute_t &out) {
  if (!s.index) {
    if (s.position < 0 ||
        static_cast<size_t>(s.position) >= def.columns.size()) {
      return false;
    }
    const sm3::column_def_t &c = def.columns[s.position];
    out = {c.type, c.typmod, false, std::nullopt};
    return !c.dropped;
  }
  for (const sm3::index_def_t &x : def.indexes) {
    if (x.oid != *s.index) {
      continue;
    }
    if (s.position < 0 ||
        static_cast<size_t>(s.position) >= x.key_attnums.size() ||
        x.key_attnums[s.position] != -1) {
      return false;
    }
    const sm2::key_column_t &k = x.key[s.position];
    out = {k.type, k.typmod, false, std::nullopt};
    return true;
  }
  return false;
}

/* Whether def has the index oid. */
bool has_index(const sm3::table_def_t &def, uint64_t oid) {
  return std::any_of(def.indexes.begin(), def.indexes.end(),
                     [&](const sm3::index_def_t &x) { return x.oid == oid; });
}

/* Fail with SM_ERR_INVALID: statistics to store are not well formed. */
int bad_stats(const char *why) {
  return sm1::seterr(sm1::SM_ERR_INVALID, std::string("statistics: ") + why);
}

} // namespace

namespace sm3 {

/* A datum from its bytes; see catalog.h. */
sm2::datum_t datum_of_bytes(const sm2::type_t *type, std::string_view bytes) {
  if (!type->byval) {
    return sm2::datum_from_ptr(bytes.data());
  }
  // A narrow by-value datum is sign-extended, as sm2 builds one.
  int64_t n = 0;
  std::memcpy(&n, bytes.data(), std::min(bytes.size(), sizeof(n)));
  switch (bytes.size()) {
  case 1:
    return sm2::datum_from_bool(bytes[0] != 0);
  case 2:
    return sm2::datum_from_int16(static_cast<int16_t>(n));
  case 4:
    return sm2::datum_from_int32(static_cast<int32_t>(n));
  default:
    return sm2::datum_from_int64(n);
  }
}

/* A table's own statistics entry: its counts, sizes and indexes. */
int encode_stats_table(const stats_t &st, std::string &blob) {
  blob.clear();
  if (st.format != stats_format) {
    return bad_stats("an unknown format");
  }
  writer_t t(11);
  t.i4(0, st.format);
  t.i4(1, st.target);
  t.i4(2, st.run_length);
  t.i8(3, st.rows_sampled);
  t.i8(4, st.rowno_limit);
  t.f8(5, st.live_rows);
  t.i8(6, st.heap_disk);
  t.i8(7, st.heap_mem);
  t.i4(8, st.avg_row_bytes);
  t.i4(9, static_cast<int32_t>(st.indexes.size()));
  t.i4(10, static_cast<int32_t>(st.stats.size()));
  if (t.append(part_desc(part_t::stats_table), blob) != 0) {
    return -1;
  }
  for (const stats_index_t &x : st.indexes) {
    writer_t w(5);
    w.i8(0, x.oid);
    w.i8(1, x.disk);
    w.i8(2, x.mem);
    w.f8(3, x.entries);
    w.f4(4, x.belongs);
    if (w.append(part_desc(part_t::stats_index), blob) != 0) {
      return -1;
    }
  }
  return 0;
}

/* Read a table's own statistics entry, leaving out indexes gone. */
int decode_stats_table(std::string_view blob, const table_def_t &def,
                       stats_t &st) {
  size_t pos = 0;
  reader_t t;
  if (t.read(blob, pos, part_t::stats_table) != 0) {
    return -1;
  }
  st.format = t.i4(0);
  if (t.ok && st.format != stats_format) {
    return corrupt("statistics of an unknown format");
  }
  st.target = t.i4(1);
  st.run_length = t.i4(2);
  st.rows_sampled = t.i8(3);
  st.rowno_limit = t.i8(4);
  st.live_rows = t.f8(5);
  st.heap_disk = t.i8(6);
  st.heap_mem = t.i8(7);
  st.avg_row_bytes = t.i4(8);
  int32_t nindex = t.i4(9);
  if (!t.ok || nindex < 0) {
    return corrupt("a statistics part cut short");
  }
  st.indexes.clear();
  st.stats.clear();
  for (int32_t i = 0; i < nindex; i++) {
    reader_t r;
    if (r.read(blob, pos, part_t::stats_index) != 0) {
      return -1;
    }
    stats_index_t x{r.i8(0), r.i8(1), r.i8(2), r.f8(3), r.f4(4)};
    if (!r.ok) {
      return corrupt("an index statistics part cut short");
    }
    if (has_index(def, x.oid)) {
      st.indexes.push_back(x);
    }
  }
  return at_end(blob, pos);
}

/* One statistic's entry: the statistic, its common values, its bounds. */
int encode_stat(const stat_t &s, const table_def_t &def, std::string &blob) {
  blob.clear();
  attribute_t a;
  if (!stat_attribute(s, def, a)) {
    return bad_stats("a statistic of no column or index expression");
  }
  if (s.mcv.size() != s.mcv_freq.size()) {
    return bad_stats("common values and frequencies of unequal counts");
  }
  writer_t w(8);
  w.opt_i8(0, s.index);
  w.i4(1, s.position);
  w.f4(2, s.null_frac);
  w.i4(3, s.width);
  w.f4(4, s.distinct);
  w.opt_f4(5, s.correlation);
  w.i4(6, static_cast<int32_t>(s.mcv.size()));
  w.i4(7, static_cast<int32_t>(s.bounds.size()));
  if (w.append(part_desc(part_t::statistic), blob) != 0) {
    return -1;
  }
  // A value's part is laid out by its own type, and a fixed-width
  // one must be that width.
  std::vector<attribute_t> mcv_schema = {a, att(sm2::type_float4)};
  std::vector<attribute_t> bound_schema = {a};
  sm2::tup_desc_t mcv_desc(mcv_schema), bound_desc(bound_schema);
  auto fits = [&](const std::string &v) {
    return a.type->len < 0 || v.size() == static_cast<size_t>(a.type->len);
  };
  for (size_t i = 0; i < s.mcv.size(); i++) {
    if (!fits(s.mcv[i])) {
      return bad_stats("a common value not of its type's width");
    }
    writer_t m(2);
    m.value(0, a.type, s.mcv[i]);
    m.f4(1, s.mcv_freq[i]);
    if (m.append(mcv_desc, blob) != 0) {
      return -1;
    }
  }
  for (const std::string &b : s.bounds) {
    if (!fits(b)) {
      return bad_stats("a histogram bound not of its type's width");
    }
    writer_t m(1);
    m.value(0, a.type, b);
    if (m.append(bound_desc, blob) != 0) {
      return -1;
    }
  }
  return 0;
}

/* Read one statistic's entry, unless what it describes is gone. */
int decode_stat(std::string_view blob, const table_def_t &def, stat_t &s,
                bool &kept) {
  size_t pos = 0;
  reader_t r;
  kept = false;
  if (r.read(blob, pos, part_t::statistic) != 0) {
    return -1;
  }
  s = stat_t{};
  s.index = r.opt_i8(0);
  s.position = r.i4(1);
  s.null_frac = r.f4(2);
  s.width = r.i4(3);
  s.distinct = r.f4(4);
  s.correlation = r.opt_f4(5);
  int32_t nmcv = r.i4(6), nbound = r.i4(7);
  if (!r.ok || nmcv < 0 || nbound < 0) {
    return corrupt("a statistic part cut short");
  }
  attribute_t a;
  // Its column dropped or its index gone: its values are not read.
  if (!stat_attribute(s, def, a)) {
    return 0;
  }
  std::vector<attribute_t> mcv_schema = {a, att(sm2::type_float4)};
  std::vector<attribute_t> bound_schema = {a};
  sm2::tup_desc_t mcv_desc(mcv_schema), bound_desc(bound_schema);
  for (int32_t j = 0; j < nmcv; j++) {
    reader_t m;
    if (m.read(blob, pos, mcv_desc) != 0) {
      return -1;
    }
    s.mcv.push_back(m.value(0, a));
    s.mcv_freq.push_back(m.f4(1));
    if (!m.ok) {
      return corrupt("a common value part cut short");
    }
  }
  for (int32_t j = 0; j < nbound; j++) {
    reader_t m;
    if (m.read(blob, pos, bound_desc) != 0) {
      return -1;
    }
    s.bounds.push_back(m.value(0, a));
    if (!m.ok) {
      return corrupt("a histogram bound part cut short");
    }
  }
  kept = true;
  return at_end(blob, pos);
}

} // namespace sm3

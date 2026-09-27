/*
 * blob.cc -- the catalog's values (CATALOG.md §3). A value is a
 * sequence of parts, each an sm2 tuple under a schema this file
 * fixes, framed by its length as a varint. A part evolves as any
 * tuple does: an attribute appended to its schema reads as its
 * missing value in a part written before it.
 */

#include "catalog.h"

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
       att(type_int2), att(type_int4), att(type_int4)},
      /* column */
      {att(type_text), att(type_int2), att(type_int4), att(type_bool),
       att(type_bool), att(type_bytea), att(type_int8)},
      /* index */
      {att(type_int8), att(type_text), att(type_int2), att(type_bool),
       att(type_int4), att(type_int4)},
      /* index_key */
      {att(type_int4), att(type_bool), att(type_bool), att(type_int8)},
      /* include */ {att(type_int4)},
      /* nspace */ {att(type_text)},
      /* collation */
      {att(type_text), att(type_text), att(type_bool), att(type_text)},
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
      schema_of(part_t::meta),      schema_of(part_t::name),
      schema_of(part_t::table),     schema_of(part_t::column),
      schema_of(part_t::index),     schema_of(part_t::index_key),
      schema_of(part_t::include),   schema_of(part_t::nspace),
      schema_of(part_t::collation),
  };
  return descs[static_cast<size_t>(part)];
}

/* Read one framed part; see catalog.h. */
int read_part(std::string_view blob, size_t &pos, const sm2::tup_desc_t &desc,
              row_t &row) {
  return catch_nomem([&] {
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
  });
}

/* META's value: the format version. */
int encode_meta(int32_t version, std::string &value) {
  return catch_nomem([&] {
    writer_t w(1);
    w.i4(0, version);
    value.clear();
    return w.append(part_desc(part_t::meta), value);
  });
}

/* Read META's value. */
int decode_meta(std::string_view value, int32_t &version) {
  return catch_nomem([&] {
    reader_t r;
    size_t pos = 0;
    if (r.read(value, pos, part_t::meta) != 0) {
      return -1;
    }
    version = r.i4(0);
    return r.ok ? at_end(value, pos) : corrupt("META without its version");
  });
}

/* A NAME key's value: what it names. */
int encode_name(const name_rec_t &rec, std::string &value) {
  return catch_nomem([&] {
    writer_t w(3);
    w.i2(0, static_cast<int16_t>(rec.kind));
    w.i8(1, rec.oid);
    w.opt_i8(2, rec.table);
    value.clear();
    return w.append(part_desc(part_t::name), value);
  });
}

/* Read a NAME key's value. */
int decode_name(std::string_view value, name_rec_t &rec) {
  return catch_nomem([&] {
    reader_t r;
    size_t pos = 0;
    if (r.read(value, pos, part_t::name) != 0) {
      return -1;
    }
    rec.kind = static_cast<kind_t>(r.i2(0));
    rec.oid = r.i8(1);
    rec.table = r.opt_i8(2);
    return r.ok ? at_end(value, pos) : corrupt("a NAME value cut short");
  });
}

/* A table's blob: the table, its columns, its indexes. */
int encode_table(const table_rec_t &rec, std::string &blob) {
  return catch_nomem([&] {
    blob.clear();
    writer_t t(7);
    t.i8(0, rec.nspoid);
    t.bytes(1, rec.name);
    t.i8(2, rec.storeid);
    t.bytes(3, rec.rowno_seq);
    t.i2(4, static_cast<int16_t>(rec.next_prefix));
    t.i4(5, static_cast<int32_t>(rec.columns.size()));
    t.i4(6, static_cast<int32_t>(rec.indexes.size()));
    if (t.append(part_desc(part_t::table), blob) != 0) {
      return -1;
    }
    for (const column_rec_t &c : rec.columns) {
      writer_t w(7);
      w.bytes(0, c.name);
      w.i2(1, c.type_code);
      w.i4(2, c.typmod);
      w.b(3, c.not_null);
      w.b(4, c.dropped);
      if (c.missing) {
        w.bytes(5, *c.missing);
      }
      w.opt_i8(6, c.collation);
      if (w.append(part_desc(part_t::column), blob) != 0) {
        return -1;
      }
    }
    for (const index_rec_t &x : rec.indexes) {
      writer_t w(6);
      w.i8(0, x.oid);
      w.bytes(1, x.name);
      w.i2(2, static_cast<int16_t>(x.prefix));
      w.b(3, x.unique);
      w.i4(4, static_cast<int32_t>(x.keys.size()));
      w.i4(5, static_cast<int32_t>(x.includes.size()));
      if (w.append(part_desc(part_t::index), blob) != 0) {
        return -1;
      }
      for (const index_key_rec_t &k : x.keys) {
        writer_t kw(4);
        kw.i4(0, k.attnum);
        kw.b(1, k.desc);
        kw.b(2, k.nulls_first);
        kw.opt_i8(3, k.collation);
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
    }
    return 0;
  });
}

/* Read a table's blob. */
int decode_table(std::string_view blob, table_rec_t &rec) {
  return catch_nomem([&] {
    size_t pos = 0;
    reader_t t;
    if (t.read(blob, pos, part_t::table) != 0) {
      return -1;
    }
    rec.nspoid = t.i8(0);
    rec.name = t.bytes(1);
    rec.storeid = t.i8(2);
    rec.rowno_seq = t.bytes(3);
    rec.next_prefix = static_cast<uint16_t>(t.i2(4));
    int32_t natts = t.i4(5), nindex = t.i4(6);
    if (!t.ok || natts < 0 || nindex < 0) {
      return corrupt("a table part cut short");
    }
    rec.columns.clear();
    for (int32_t i = 0; i < natts; i++) {
      reader_t r;
      if (r.read(blob, pos, part_t::column) != 0) {
        return -1;
      }
      column_rec_t c;
      c.name = r.bytes(0);
      c.type_code = static_cast<uint8_t>(r.i2(1));
      c.typmod = r.i4(2);
      c.not_null = r.b(3);
      c.dropped = r.b(4);
      c.missing = r.opt_bytes(5);
      c.collation = r.opt_i8(6);
      if (!r.ok) {
        return corrupt("a column part cut short");
      }
      rec.columns.push_back(std::move(c));
    }
    rec.indexes.clear();
    for (int32_t i = 0; i < nindex; i++) {
      reader_t r;
      if (r.read(blob, pos, part_t::index) != 0) {
        return -1;
      }
      index_rec_t x;
      x.oid = r.i8(0);
      x.name = r.bytes(1);
      x.prefix = static_cast<uint16_t>(r.i2(2));
      x.unique = r.b(3);
      int32_t nkey = r.i4(4), ninclude = r.i4(5);
      if (!r.ok || nkey < 0 || ninclude < 0) {
        return corrupt("an index part cut short");
      }
      for (int32_t k = 0; k < nkey; k++) {
        reader_t kr;
        if (kr.read(blob, pos, part_t::index_key) != 0) {
          return -1;
        }
        x.keys.push_back({kr.i4(0), kr.b(1), kr.b(2), kr.opt_i8(3)});
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
      rec.indexes.push_back(std::move(x));
    }
    return at_end(blob, pos);
  });
}

/* A namespace's blob. */
int encode_nspace(const nspace_rec_t &rec, std::string &blob) {
  return catch_nomem([&] {
    writer_t w(1);
    w.bytes(0, rec.name);
    blob.clear();
    return w.append(part_desc(part_t::nspace), blob);
  });
}

/* Read a namespace's blob. */
int decode_nspace(std::string_view blob, nspace_rec_t &rec) {
  return catch_nomem([&] {
    reader_t r;
    size_t pos = 0;
    if (r.read(blob, pos, part_t::nspace) != 0) {
      return -1;
    }
    rec.name = r.bytes(0);
    return r.ok ? at_end(blob, pos) : corrupt("a namespace part cut short");
  });
}

/* A collation's blob. */
int encode_collation(const collation_rec_t &rec, std::string &blob) {
  return catch_nomem([&] {
    writer_t w(4);
    w.bytes(0, rec.name);
    w.bytes(1, rec.locale);
    w.b(2, rec.deterministic);
    w.bytes(3, rec.icu_version);
    blob.clear();
    return w.append(part_desc(part_t::collation), blob);
  });
}

/* Read a collation's blob. */
int decode_collation(std::string_view blob, collation_rec_t &rec) {
  return catch_nomem([&] {
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
  });
}

} // namespace sm3

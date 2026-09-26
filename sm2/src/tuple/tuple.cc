/*
 * tuple.cc -- the tuple codec (TUPLE.md, VARLENA.md; why in
 * SM2_DESIGN.md §4, §4.1):
 *
 *   [ header | null bitmap | fixed part | var data ]
 *
 * The header is a flag byte and natts, the row's attribute count,
 * as a varint. A null takes no space: its bit in the null bitmap,
 * set for null, is all it has. The fixed part holds an entry for
 * each non-null attribute, in schema order, end to end with no
 * padding: a fixed-width datum itself, or a variable-width one's
 * slot, where its varlena starts, counted from the tuple's start,
 * 2 bytes or, under the wide flag, 4. The var data holds those
 * varlenas, PostgreSQL's, each in its shortest form, in schema
 * order and end to end; a varlena's header gives its length. Any
 * attribute is found from the null bitmap and the schema's widths,
 * never by walking the data.
 *
 * TOAST and compression are not built yet: a TOAST pointer or a
 * compressed varlena is refused rather than misread.
 */

#include "native.h"
#include "sm2.h"

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace {

constexpr uint8_t has_nulls = 0x01, wide_slots = 0x04;
constexpr uint8_t reserved = 0xFA;

/* The longest datum a 1-byte varlena header holds. */
constexpr size_t short_max = 126;

/*
 * The length of the varlena at the start of v, which may run on
 * past it, from its header, in len; or why it is not a varlena
 * this codec can store or read: no header, one cut short, a
 * compressed one or a TOAST pointer, or a length past v's end.
 */
const char *varlena_extent(std::string_view v, size_t &len) {
  if (v.empty()) {
    return "a varlena without its header";
  }
  uint8_t first = static_cast<uint8_t>(v[0]);
  if (first == static_cast<uint8_t>(sm2::varlena_toast)) {
    return "a TOAST pointer, which sm2 cannot store or read yet";
  }
  if (first & 0x01) {
    len = first >> 1;
  } else {
    if (v.size() < 4) {
      return "a varlena cut short in its 4-byte header";
    }
    uint32_t header = sm2::load_le<uint32_t>(v.substr(0, 4));
    if ((header & 0x03) == 0x02) {
      return "a compressed varlena, which sm2 cannot store or read yet";
    }
    len = header >> 2;
    if (len < 4) {
      return "a varlena whose length is shorter than its header";
    }
  }
  if (len > v.size()) {
    return "a varlena that runs past the tuple's end";
  }
  return nullptr;
}

/* Why v, a whole varlena, is not one the codec can store; or nullptr. */
const char *varlena_problem(std::string_view v) {
  size_t len;
  if (const char *why = varlena_extent(v, len)) {
    return why;
  }
  return len == v.size() ? nullptr
                         : "a varlena whose header does not match its length";
}

/* The shortest varlena length for a datum of n bytes. */
size_t varlena_len(size_t n) { return n + (n <= short_max ? 1 : 4); }

/* Fail with SM_ERR_CORRUPT: the tuple is malformed. */
int corrupt(const std::string &why) {
  return sm1::seterr(sm1::SM_ERR_CORRUPT, "tuple: " + why);
}

/* Fail with SM_ERR_INVALID: the call's arguments are wrong. */
int invalid(const std::string &why) {
  return sm1::seterr(sm1::SM_ERR_INVALID, "tuple: " + why);
}

/* Append v as an unsigned LEB128 varint. */
void put_varint(uint64_t v, std::string &out) {
  do {
    uint8_t byte = v & 0x7F;
    v >>= 7;
    out += static_cast<char>(byte | (v != 0 ? 0x80 : 0));
  } while (v != 0);
}

/* Read a varint of at most 5 bytes from in at pos; false if malformed. */
bool get_varint(std::string_view in, size_t &pos, uint64_t &v) {
  v = 0;
  for (int shift = 0; shift < 35 && pos < in.size(); shift += 7) {
    uint8_t byte = static_cast<uint8_t>(in[pos++]);
    v |= static_cast<uint64_t>(byte & 0x7F) << shift;
    if (!(byte & 0x80)) {
      return true;
    }
  }
  return false;
}

/* The bytes varint v takes. */
size_t varint_len(uint64_t v) {
  size_t n = 1;
  while (v >>= 7) {
    n++;
  }
  return n;
}

/* True if bit i of bitmap is set. */
bool bit(const char *bitmap, size_t i) {
  return (static_cast<uint8_t>(bitmap[i / 8]) >> (i % 8)) & 1;
}

/*
 * Every attribute's width under schema (datum_width), -1 for
 * variable; fails with SM_ERR_INVALID if one is not valid.
 */
int widths(std::span<const sm2::attribute_t> schema,
           std::vector<int32_t> &out) {
  out.clear();
  for (size_t i = 0; i < schema.size(); i++) {
    int32_t w =
        schema[i].type ? sm2::datum_width(schema[i].type, schema[i].typmod) : 0;
    if (w == 0) {
      return invalid("attribute " + std::to_string(i) +
                     " has no type, or a typmod its type refuses");
    }
    out.push_back(w);
  }
  return 0;
}

/* A tuple's parts, located and checked against the schema. */
struct parsed_t {
  std::string_view tuple;
  size_t natts = 0;
  const char *nulls = nullptr; /* the null bitmap, or nullptr */
  size_t fixed = 0;            /* where the fixed part starts */
  size_t slot_width = 2;       /* a variable attribute's entry */
};

/* The width of attribute i's entry in the fixed part. */
size_t entry_width(const parsed_t &t, const std::vector<int32_t> &w, size_t i) {
  return w[i] > 0 ? static_cast<size_t>(w[i]) : t.slot_width;
}

/* The value of the slot at pos: where a varlena starts. */
size_t slot_at(const parsed_t &t, size_t pos) {
  std::string_view s = t.tuple.substr(pos, t.slot_width);
  return t.slot_width == 2 ? sm2::load_le<uint16_t>(s)
                           : sm2::load_le<uint32_t>(s);
}

/*
 * Locate tuple's parts under schema, whose widths are w, and check
 * that they fit together exactly: the header's flags and natts,
 * the bitmap, the fixed part, and each slot, which must point
 * where the varlena before it ends, the first just past the fixed
 * part, the last varlena ending at the tuple's end.
 */
int parse(std::span<const sm2::attribute_t> schema,
          const std::vector<int32_t> &w, std::string_view tuple, parsed_t &t) {
  t.tuple = tuple;
  // The header.
  if (tuple.empty()) {
    return corrupt("empty");
  }
  uint8_t flags = static_cast<uint8_t>(tuple[0]);
  if (flags & reserved) {
    return corrupt("reserved header flags are set");
  }
  size_t pos = 1;
  uint64_t natts;
  if (!get_varint(tuple, pos, natts)) {
    return corrupt("a malformed attribute count");
  }
  if (natts > schema.size()) {
    return corrupt("more attributes than its schema has");
  }
  t.natts = natts;
  // The null bitmap, when there is one.
  if (flags & has_nulls) {
    size_t len = (natts + 7) / 8;
    if (tuple.size() - pos < len) {
      return corrupt("cut short in its null bitmap");
    }
    t.nulls = tuple.data() + pos;
    pos += len;
  }
  // The fixed part, an entry per non-null attribute.
  t.slot_width = flags & wide_slots ? 4 : 2;
  t.fixed = pos;
  size_t fixed = 0;
  for (size_t i = 0; i < natts; i++) {
    if (!(t.nulls && bit(t.nulls, i))) {
      fixed += entry_width(t, w, i);
    }
  }
  if (tuple.size() - pos < fixed) {
    return corrupt("cut short in its fixed part");
  }
  // Each slot must point where the varlenas so far end.
  size_t next = pos + fixed, entry = pos;
  for (size_t i = 0; i < natts; i++) {
    if (t.nulls && bit(t.nulls, i)) {
      continue;
    }
    if (w[i] < 0) {
      if (slot_at(t, entry) != next) {
        return corrupt("a slot that does not point to its varlena");
      }
      size_t len;
      if (const char *why = varlena_extent(tuple.substr(next), len)) {
        return corrupt(why);
      }
      next += len;
    }
    entry += entry_width(t, w, i);
  }
  if (next != tuple.size()) {
    return corrupt("bytes past its last datum");
  }
  return 0;
}

/*
 * Attribute i of a parsed tuple, known non-null and within natts,
 * whose entry is at pos: a fixed-width datum, or the varlena its
 * slot points to.
 */
std::string_view datum_at(const parsed_t &t, const std::vector<int32_t> &w,
                          size_t i, size_t pos) {
  if (w[i] > 0) {
    return t.tuple.substr(pos, w[i]);
  }
  size_t start = slot_at(t, pos), len = 0;
  varlena_extent(t.tuple.substr(start), len);
  return t.tuple.substr(start, len);
}

/* Convert bytes of attribute to Datum representation. */
void bytes_to_datum(const sm2::attribute_t &attr, int32_t w,
                    std::string_view bytes, sm2::Datum &val) {
  if (attr.type && attr.type->byval) {
    val = 0;
    std::memcpy(&val, bytes.data(), w);
  } else {
    val = reinterpret_cast<sm2::Datum>(bytes.data());
  }
}

} // namespace

namespace sm2 {

/* A varlena holding datum, in its shortest form. */
std::string make_varlena(std::string_view datum) {
  std::string v;
  if (datum.size() <= short_max) {
    v += static_cast<char>((datum.size() + 1) << 1 | 0x01);
  } else {
    store_le(static_cast<uint32_t>((datum.size() + 4) << 2), v);
  }
  v += datum;
  return v;
}

/* True if v is a TOAST pointer: its first byte is varlena_toast. */
bool varlena_is_toast(std::string_view v) {
  return !v.empty() && v[0] == varlena_toast;
}

/* The data of an uncompressed varlena, after its header. */
std::string_view varlena_payload(std::string_view v) {
  if (v.empty() || v[0] == varlena_toast) {
    return {};
  }
  if (static_cast<uint8_t>(v[0]) & 0x01) {
    return v.substr(1);
  }
  if (v.size() < 4 || (load_le<uint32_t>(v.substr(0, 4)) & 0x03) != 0) {
    return {};
  }
  return v.substr(4);
}

/* Encode datums and isnull under schema into tuple; see sm2.h. */
int tuple_encode(std::span<const attribute_t> schema,
                 std::span<const Datum> datums, std::span<const bool> isnull,
                 std::string &tuple) {
  return catch_nomem([&] {
    std::vector<int32_t> w;
    if (widths(schema, w) != 0) {
      return -1;
    }
    if (datums.size() != schema.size() || isnull.size() != schema.size()) {
      return invalid("a datum and null flag for every attribute, no more");
    }
    // Check each datum, and size the fixed part and the var data.
    bool nulls = false;
    size_t fixed = 0, nvar = 0, var = 0;
    std::vector<std::string_view> attr_bytes(datums.size());
    for (size_t i = 0; i < datums.size(); i++) {
      if (isnull[i]) {
        nulls = true;
        continue;
      }
      if (schema[i].dropped) {
        return invalid("attribute " + std::to_string(i) +
                       " is dropped, so its datum must be null");
      }
      if (schema[i].type && schema[i].type->byval) {
        attr_bytes[i] =
            std::string_view(reinterpret_cast<const char *>(&datums[i]), w[i]);
        fixed += w[i];
        continue;
      }
      const char *ptr = reinterpret_cast<const char *>(datums[i]);
      if (ptr == nullptr) {
        return invalid("attribute " + std::to_string(i) +
                       " is not null, but its pointer is null");
      }
      if (w[i] > 0) {
        attr_bytes[i] = std::string_view(ptr, w[i]);
        fixed += w[i];
        continue;
      }
      // Variable-width varlena
      uint8_t first = static_cast<uint8_t>(ptr[0]);
      if (first == static_cast<uint8_t>(varlena_toast)) {
        return invalid("attribute " + std::to_string(i) +
                       ": a TOAST pointer, which sm2 cannot store or read yet");
      }
      size_t len = 0;
      if (first & 0x01) {
        len = first >> 1;
      } else {
        uint32_t header = load_le<uint32_t>(std::string_view(ptr, 4));
        if ((header & 0x03) == 0x02) {
          return invalid("attribute " + std::to_string(i) +
                         ": a compressed varlena, which sm2 cannot store or "
                         "read yet");
        }
        len = header >> 2;
        if (len < 4) {
          return invalid("attribute " + std::to_string(i) +
                         ": a varlena whose length is shorter than its header");
        }
      }
      std::string_view v(ptr, len);
      if (const char *why = varlena_problem(v)) {
        return invalid("attribute " + std::to_string(i) + ": " + why);
      }
      attr_bytes[i] = v;
      nvar++;
      var += varlena_len(varlena_payload(v).size());
    }
    // 2-byte slots, unless the tuple is too long for them to reach.
    size_t head =
        1 + varint_len(datums.size()) + (nulls ? (datums.size() + 7) / 8 : 0);
    bool wide = head + fixed + 2 * nvar + var > 0xFFFF;
    size_t slot = wide ? 4 : 2;
    size_t var_start = head + fixed + slot * nvar;
    // The header, then the null bitmap, a set bit for a null.
    std::string out;
    out += static_cast<char>((nulls ? has_nulls : 0) | (wide ? wide_slots : 0));
    put_varint(datums.size(), out);
    if (nulls) {
      size_t at = out.size();
      out.append((datums.size() + 7) / 8, '\0');
      for (size_t i = 0; i < datums.size(); i++) {
        if (isnull[i]) {
          out[at + i / 8] = static_cast<char>(out[at + i / 8] | 1 << (i % 8));
        }
      }
    }
    // The fixed part: each datum, or each varlena's slot.
    size_t next = var_start;
    std::string word;
    for (size_t i = 0; i < datums.size(); i++) {
      if (isnull[i]) {
        continue;
      }
      if (w[i] > 0) {
        out += attr_bytes[i];
        continue;
      }
      if (wide) {
        store_le(static_cast<uint32_t>(next), word);
      } else {
        store_le(static_cast<uint16_t>(next), word);
      }
      out += word;
      next += varlena_len(varlena_payload(attr_bytes[i]).size());
    }
    // The var data, each varlena in its shortest form.
    for (size_t i = 0; i < datums.size(); i++) {
      if (!isnull[i] && w[i] < 0) {
        out += make_varlena(varlena_payload(attr_bytes[i]));
      }
    }
    tuple = std::move(out);
    return 0;
  });
}

/* Encode datums under schema into tuple; see sm2.h. */
int tuple_encode(std::span<const attribute_t> schema,
                 std::span<const datum_t> datums, std::string &tuple) {
  return catch_nomem([&] {
    if (datums.size() != schema.size()) {
      return invalid("a datum for every attribute, no more");
    }
    std::vector<int32_t> w;
    if (widths(schema, w) != 0) {
      return -1;
    }
    std::vector<Datum> d(datums.size());
    auto isnull = std::make_unique<bool[]>(datums.size());
    for (size_t i = 0; i < datums.size(); i++) {
      isnull[i] = datums[i].null;
      if (datums[i].null) {
        d[i] = 0;
        continue;
      }
      if (schema[i].dropped) {
        return invalid("attribute " + std::to_string(i) +
                       " is dropped, so its datum must be null");
      }
      if (w[i] > 0) {
        if (datums[i].bytes.size() != static_cast<size_t>(w[i])) {
          return invalid("attribute " + std::to_string(i) + " must be " +
                         std::to_string(w[i]) + " bytes, not " +
                         std::to_string(datums[i].bytes.size()));
        }
      } else {
        if (const char *why = varlena_problem(datums[i].bytes)) {
          return invalid("attribute " + std::to_string(i) + ": " + why);
        }
      }
      if (schema[i].type && schema[i].type->byval) {
        d[i] = 0;
        std::memcpy(&d[i], datums[i].bytes.data(), datums[i].bytes.size());
      } else {
        d[i] = reinterpret_cast<uintptr_t>(datums[i].bytes.data());
      }
    }
    return tuple_encode(
        schema, d, std::span<const bool>(isnull.get(), datums.size()), tuple);
  });
}

/* Deform every attribute of tuple into datums and isnull; see sm2.h. */
int tuple_deform(std::span<const attribute_t> schema, std::string_view tuple,
                 std::span<Datum> datums, std::span<bool> isnull) {
  return catch_nomem([&] {
    if (datums.size() != schema.size() || isnull.size() != schema.size()) {
      return invalid("deform: output spans must match schema size");
    }
    std::vector<int32_t> w;
    parsed_t t;
    if (widths(schema, w) != 0 || parse(schema, w, tuple, t) != 0) {
      return -1;
    }
    size_t pos = t.fixed;
    for (size_t i = 0; i < schema.size(); i++) {
      if (i >= t.natts) {
        const auto &m = schema[i].missing;
        if (schema[i].dropped || !m) {
          isnull[i] = true;
          datums[i] = 0;
        } else {
          isnull[i] = false;
          bytes_to_datum(schema[i], w[i], *m, datums[i]);
        }
        continue;
      }
      if (t.nulls && bit(t.nulls, i)) {
        isnull[i] = true;
        datums[i] = 0;
        continue;
      }
      std::string_view d = datum_at(t, w, i, pos);
      pos += entry_width(t, w, i);
      if (schema[i].dropped) {
        isnull[i] = true;
        datums[i] = 0;
        continue;
      }
      isnull[i] = false;
      bytes_to_datum(schema[i], w[i], d, datums[i]);
    }
    return 0;
  });
}

/* Every attribute of a tuple, in one pass; see sm2.h. */
int tuple_deform(std::span<const attribute_t> schema, std::string_view tuple,
                 std::vector<std::optional<std::string_view>> &datums) {
  return catch_nomem([&] {
    std::vector<int32_t> w;
    parsed_t t;
    if (widths(schema, w) != 0 || parse(schema, w, tuple, t) != 0) {
      return -1;
    }
    datums.clear();
    size_t pos = t.fixed;
    for (size_t i = 0; i < schema.size(); i++) {
      // Past the row's attributes: the value it was added with.
      if (i >= t.natts) {
        const auto &m = schema[i].missing;
        datums.push_back(!schema[i].dropped && m
                             ? std::optional(std::string_view(*m))
                             : std::nullopt);
        continue;
      }
      if (t.nulls && bit(t.nulls, i)) {
        datums.push_back(std::nullopt);
        continue;
      }
      std::string_view d = datum_at(t, w, i, pos);
      pos += entry_width(t, w, i);
      // A dropped attribute's old bytes are skipped.
      datums.push_back(schema[i].dropped ? std::nullopt : std::optional(d));
    }
    return 0;
  });
}

/* One attribute of a tuple, by ordinal, into val and isnull; see sm2.h. */
int tuple_decode(std::span<const attribute_t> schema, std::string_view tuple,
                 size_t ordinal, Datum &val, bool &isnull) {
  return catch_nomem([&] {
    std::vector<int32_t> w;
    parsed_t t;
    if (ordinal >= schema.size()) {
      return invalid("attribute " + std::to_string(ordinal) +
                     " is past the schema's " + std::to_string(schema.size()));
    }
    if (widths(schema, w) != 0 || parse(schema, w, tuple, t) != 0) {
      return -1;
    }
    val = 0;
    isnull = true;
    const attribute_t &a = schema[ordinal];
    if (a.dropped) {
      return 0;
    }
    if (ordinal >= t.natts) {
      if (a.missing) {
        isnull = false;
        bytes_to_datum(a, w[ordinal], *a.missing, val);
      }
      return 0;
    }
    if (t.nulls && bit(t.nulls, ordinal)) {
      return 0;
    }
    // Its entry follows those of the non-null attributes before it.
    size_t pos = t.fixed;
    for (size_t i = 0; i < ordinal; i++) {
      if (!(t.nulls && bit(t.nulls, i))) {
        pos += entry_width(t, w, i);
      }
    }
    std::string_view d = datum_at(t, w, ordinal, pos);
    isnull = false;
    bytes_to_datum(a, w[ordinal], d, val);
    return 0;
  });
}

/* One attribute of a tuple, by ordinal; see sm2.h. */
int tuple_decode(std::span<const attribute_t> schema, std::string_view tuple,
                 size_t ordinal, std::optional<std::string_view> &datum) {
  return catch_nomem([&] {
    std::vector<int32_t> w;
    parsed_t t;
    if (ordinal >= schema.size()) {
      return invalid("attribute " + std::to_string(ordinal) +
                     " is past the schema's " + std::to_string(schema.size()));
    }
    if (widths(schema, w) != 0 || parse(schema, w, tuple, t) != 0) {
      return -1;
    }
    datum = std::nullopt;
    const attribute_t &a = schema[ordinal];
    if (a.dropped) {
      return 0;
    }
    if (ordinal >= t.natts) {
      if (a.missing) {
        datum = std::string_view(*a.missing);
      }
      return 0;
    }
    if (t.nulls && bit(t.nulls, ordinal)) {
      return 0;
    }
    // Its entry follows those of the non-null attributes before it.
    size_t pos = t.fixed;
    for (size_t i = 0; i < ordinal; i++) {
      if (!(t.nulls && bit(t.nulls, i))) {
        pos += entry_width(t, w, i);
      }
    }
    datum = datum_at(t, w, ordinal, pos);
    return 0;
  });
}

} // namespace sm2

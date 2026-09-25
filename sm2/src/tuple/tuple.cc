/*
 * tuple.cc -- the tuple codec (SM2_DESIGN.md §4, §4.1):
 *
 *   [ header | null bitmap | fixed part | var offsets |
 *     var data ]
 *
 * The header is a flag byte and natts, the row's attribute count,
 * as a varint. A null takes no space: its bit in the null bitmap,
 * set for null, is all it has. The fixed part holds the non-null
 * fixed-width datums in schema order, end to end with no padding;
 * the variable part holds one end offset per non-null
 * variable-width datum, 2 bytes or, under the wide-offsets flag,
 * 4, then their varlenas, PostgreSQL's, each in its shortest form
 * and unpadded. A column is found from the null bitmap and the
 * schema's widths, never by walking the data.
 *
 * TOAST and compression are not built yet: a TOAST pointer or a
 * compressed varlena is refused rather than misread.
 */

#include "native.h"
#include "sm2.h"

#include <cstdint>
#include <string>

namespace {

constexpr uint8_t has_nulls = 0x01, wide_offsets = 0x04;
constexpr uint8_t reserved = 0xFA;

/* The longest datum a 1-byte varlena header holds. */
constexpr size_t short_max = 126;

/*
 * Why v is not a varlena this codec can store or read, or nullptr
 * if it is: an uncompressed one whose header gives its length.
 */
const char *varlena_problem(std::string_view v) {
  if (v.empty()) {
    return "a varlena without its header";
  }
  uint8_t first = static_cast<uint8_t>(v[0]);
  if (first == static_cast<uint8_t>(sm2::varlena_toast)) {
    return "a TOAST pointer, which sm2 cannot store or read yet";
  }
  size_t len;
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
  }
  if (len != v.size()) {
    return "a varlena whose header does not match its length";
  }
  return nullptr;
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
  size_t natts = 0;
  const char *nulls = nullptr; /* the null bitmap, or nullptr */
  std::string_view fixed;      /* the fixed part */
  std::string_view offsets;    /* the variable part's end offsets */
  size_t offset_width = 2;
  std::string_view var; /* the variable part's bytes */
};

/* The end offset of the n-th non-null variable datum. */
size_t var_end(const parsed_t &t, size_t n) {
  std::string_view o = t.offsets.substr(n * t.offset_width, t.offset_width);
  return t.offset_width == 2 ? sm2::load_le<uint16_t>(o)
                             : sm2::load_le<uint32_t>(o);
}

/*
 * Locate tuple's parts under schema, whose widths are w, and check
 * that they fit together exactly: the header's flags and natts,
 * the bitmaps' sizes, the fixed part's length, and the offsets,
 * which must not decrease and must end at the tuple's end.
 */
int parse(std::span<const sm2::attribute_t> schema,
          const std::vector<int32_t> &w, std::string_view tuple, parsed_t &t) {
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
  // The fixed part's length and the number of variable datums.
  size_t fixed = 0, nvar = 0;
  for (size_t i = 0; i < natts; i++) {
    if (t.nulls && bit(t.nulls, i)) {
      continue;
    }
    if (w[i] > 0) {
      fixed += w[i];
    } else {
      nvar++;
    }
  }
  t.offset_width = flags & wide_offsets ? 4 : 2;
  size_t offsets = nvar * t.offset_width;
  if (tuple.size() - pos < fixed + offsets) {
    return corrupt("cut short in its fixed part or offsets");
  }
  t.fixed = tuple.substr(pos, fixed);
  t.offsets = tuple.substr(pos + fixed, offsets);
  t.var = tuple.substr(pos + fixed + offsets);
  // The offsets must climb, and end where the tuple does; each
  // varlena between them must be one this reader can read.
  size_t prev = 0;
  for (size_t n = 0; n < nvar; n++) {
    size_t end = var_end(t, n);
    if (end < prev || end > t.var.size()) {
      return corrupt("a variable offset out of order or range");
    }
    if (const char *why = varlena_problem(t.var.substr(prev, end - prev))) {
      return corrupt(why);
    }
    prev = end;
  }
  if (prev != t.var.size()) {
    return corrupt("bytes past its last datum");
  }
  return 0;
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

/* Encode datums under schema into tuple; see sm2.h. */
int tuple_encode(std::span<const attribute_t> schema,
                 std::span<const datum_t> datums, std::string &tuple) {
  return catch_nomem([&] {
    std::vector<int32_t> w;
    if (widths(schema, w) != 0) {
      return -1;
    }
    if (datums.size() != schema.size()) {
      return invalid("a datum for every attribute, no more");
    }
    // Check each datum, and size the variable data.
    bool nulls = false;
    size_t var = 0;
    for (size_t i = 0; i < datums.size(); i++) {
      if (datums[i].null) {
        nulls = true;
        continue;
      }
      if (schema[i].dropped) {
        return invalid("attribute " + std::to_string(i) +
                       " is dropped, so its datum must be null");
      }
      if (w[i] > 0 && datums[i].bytes.size() != static_cast<size_t>(w[i])) {
        return invalid("attribute " + std::to_string(i) + " must be " +
                       std::to_string(w[i]) + " bytes, not " +
                       std::to_string(datums[i].bytes.size()));
      }
      if (w[i] < 0) {
        if (const char *why = varlena_problem(datums[i].bytes)) {
          return invalid("attribute " + std::to_string(i) + ": " + why);
        }
        var += varlena_len(varlena_payload(datums[i].bytes).size());
      }
    }
    bool wide = var > 0xFFFF;
    // The header.
    std::string out;
    out +=
        static_cast<char>((nulls ? has_nulls : 0) | (wide ? wide_offsets : 0));
    put_varint(datums.size(), out);
    // The null bitmap, a set bit for a null.
    if (nulls) {
      size_t at = out.size();
      out.append((datums.size() + 7) / 8, '\0');
      for (size_t i = 0; i < datums.size(); i++) {
        if (datums[i].null) {
          out[at + i / 8] = static_cast<char>(out[at + i / 8] | 1 << (i % 8));
        }
      }
    }
    // The fixed part, then each variable datum's end offset.
    for (size_t i = 0; i < datums.size(); i++) {
      if (!datums[i].null && w[i] > 0) {
        out += datums[i].bytes;
      }
    }
    size_t end = 0;
    std::string word;
    for (size_t i = 0; i < datums.size(); i++) {
      if (!datums[i].null && w[i] < 0) {
        end += varlena_len(varlena_payload(datums[i].bytes).size());
        if (wide) {
          store_le(static_cast<uint32_t>(end), word);
        } else {
          store_le(static_cast<uint16_t>(end), word);
        }
        out += word;
      }
    }
    // The variable data, each varlena in its shortest form.
    for (size_t i = 0; i < datums.size(); i++) {
      if (!datums[i].null && w[i] < 0) {
        out += make_varlena(varlena_payload(datums[i].bytes));
      }
    }
    tuple = std::move(out);
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
    size_t fixed = 0, nvar = 0, var_start = 0;
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
      std::string_view d;
      if (w[i] > 0) {
        d = t.fixed.substr(fixed, w[i]);
        fixed += w[i];
      } else {
        size_t end = var_end(t, nvar++);
        d = t.var.substr(var_start, end - var_start);
        var_start = end;
      }
      // A dropped attribute's old bytes are skipped.
      datums.push_back(schema[i].dropped ? std::nullopt : std::optional(d));
    }
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
    // Count the non-null datums before it, fixed and variable.
    size_t fixed = 0, nvar = 0;
    for (size_t i = 0; i < ordinal; i++) {
      if (t.nulls && bit(t.nulls, i)) {
        continue;
      }
      if (w[i] > 0) {
        fixed += w[i];
      } else {
        nvar++;
      }
    }
    if (w[ordinal] > 0) {
      datum = t.fixed.substr(fixed, w[ordinal]);
    } else {
      size_t start = nvar == 0 ? 0 : var_end(t, nvar - 1);
      datum = t.var.substr(start, var_end(t, nvar) - start);
    }
    return 0;
  });
}

} // namespace sm2

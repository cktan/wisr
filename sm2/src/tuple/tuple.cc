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
 * 2 bytes, since no tuple is longer than 65535 bytes. The var
 * data holds those
 * varlenas, PostgreSQL's, each in its shortest form, in schema
 * order and end to end; a varlena's header gives its length. Any
 * attribute is found from the null bitmap and the schema's widths,
 * never by walking the data: tup_desc_t holds each attribute's
 * off[], where its entry starts in a row with no nulls, and an
 * entry is off[k] less the widths of the nulls before it.
 *
 * TOAST and compression are not built yet: a TOAST pointer or a
 * compressed varlena is refused rather than misread.
 */

#include "native.h"
#include "sm2.h"

#include <algorithm>
#include <bit>
#include <cstdint>
#include <cstring>
#include <string>

namespace {

/* 0x02 and 0x04, once a TOAST bitmap and wide slots, are retired. */
constexpr uint8_t has_nulls = 0x01;
constexpr uint8_t reserved = 0xFE;

/* A variable attribute's slot: 2 bytes reach any byte of a tuple. */
constexpr size_t slot_width = 2;

/* The longest datum a 1-byte varlena header holds. */
constexpr size_t short_max = 126;

/*
 * The length of the varlena whose header starts v, in len,
 * trusting it past v; or why the header is not one this codec can
 * store or read: none, one cut short, a compressed one or a
 * TOAST pointer.
 */
const char *varlena_header(std::string_view v, size_t &len) {
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
  return nullptr;
}

/*
 * The length of the varlena at the start of v, from its header,
 * in len; or why it is not a varlena this codec can store or
 * read, as varlena_header says, or one whose length is past v's
 * end.
 */
const char *varlena_extent(std::string_view v, size_t &len) {
  if (const char *why = varlena_header(v, len)) {
    return why;
  }
  if (len > v.size()) {
    return "a varlena that runs past the tuple's end";
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

/* Fail with SM_ERR_INVALID if desc has an attribute it refused. */
int check_desc(const sm2::tup_desc_t &desc) {
  if (desc.bad == SIZE_MAX) {
    return 0;
  }
  return invalid("attribute " + std::to_string(desc.bad) +
                 " has no type, or a typmod its type refuses");
}

/* A tuple's parts, located and checked against the schema. */
struct parsed_t {
  std::string_view tuple;
  size_t natts = 0;
  const char *nulls = nullptr; /* the null bitmap, or nullptr */
  size_t fixed = 0;            /* where the fixed part starts */
};

/* The width of attribute i's entry in the fixed part. */
size_t entry_width(const sm2::tup_desc_t &desc, size_t i) {
  int32_t w = desc.width[i];
  return w > 0 ? static_cast<size_t>(w) : slot_width;
}

/* The value of the slot at pos: where a varlena starts. */
size_t slot_at(const parsed_t &t, size_t pos) {
  return sm2::load_le<uint16_t>(t.tuple.substr(pos, slot_width));
}

/*
 * Locate tuple's header and null bitmap under desc, checking only
 * them: the length, the flags, natts, and that the bitmap fits.
 * t.fixed is then where the fixed part starts.
 */
int parse_head(const sm2::tup_desc_t &desc, std::string_view tuple,
               parsed_t &t) {
  t.tuple = tuple;
  // The header.
  if (tuple.empty()) {
    return corrupt("empty");
  }
  if (tuple.size() > sm2::max_tup_len) {
    return corrupt("longer than " + std::to_string(sm2::max_tup_len) +
                   " bytes");
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
  if (natts > desc.schema.size()) {
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
  t.fixed = pos;
  return 0;
}

/*
 * Locate tuple's parts under desc and check that they fit together
 * exactly: the header and bitmap, the fixed part, and each slot,
 * which must point where the varlena before it ends, the first
 * just past the fixed part, the last varlena ending at the tuple's
 * end.
 */
int parse(const sm2::tup_desc_t &desc, std::string_view tuple, parsed_t &t) {
  if (parse_head(desc, tuple, t) != 0) {
    return -1;
  }
  // The fixed part, an entry per non-null attribute.
  size_t pos = t.fixed, natts = t.natts;
  size_t fixed = 0;
  for (size_t i = 0; i < natts; i++) {
    if (!(t.nulls && bit(t.nulls, i))) {
      fixed += entry_width(desc, i);
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
    if (desc.width[i] < 0) {
      if (slot_at(t, entry) != next) {
        return corrupt("a slot that does not point to its varlena");
      }
      size_t len;
      if (const char *why = varlena_extent(tuple.substr(next), len)) {
        return corrupt(why);
      }
      next += len;
    }
    entry += entry_width(desc, i);
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
std::string_view datum_at(const parsed_t &t, const sm2::tup_desc_t &desc,
                          size_t i, size_t pos) {
  if (desc.width[i] > 0) {
    return t.tuple.substr(pos, desc.width[i]);
  }
  size_t start = slot_at(t, pos), len = 0;
  varlena_extent(t.tuple.substr(start), len);
  return t.tuple.substr(start, len);
}

/* Convert bytes of attribute to Datum representation. */
void bytes_to_datum(const sm2::attribute_t &attr, std::string_view bytes,
                    sm2::Datum &val) {
  if (attr.type && attr.type->byval) {
    val = 0;
    std::memcpy(&val, bytes.data(), bytes.size());
  } else {
    val = reinterpret_cast<sm2::Datum>(bytes.data());
  }
}

/*
 * The entry widths the nulls among attributes 0 to k - 1 would
 * have taken: what they save of off[k] (TUPLE.md §6). It visits
 * only the set bits, the bitmap read 64 bits at a time.
 */
size_t null_width(const parsed_t &t, const sm2::tup_desc_t &desc, size_t k) {
  size_t sum = 0;
  if (!t.nulls) {
    return 0;
  }
  for (size_t base = 0; base < k; base += 64) {
    // The bitmap is (natts + 7) / 8 bytes and k < natts, so the
    // bytes read here are within it.
    size_t n = std::min<size_t>(8, (k - base + 7) / 8);
    char word[8] = {};
    std::memcpy(word, t.nulls + base / 8, n);
    uint64_t bits = sm2::load_le<uint64_t>(std::string_view(word, 8));
    if (k - base < 64) {
      bits &= (uint64_t{1} << (k - base)) - 1;
    }
    while (bits != 0) {
      sum += entry_width(desc, base + std::countr_zero(bits));
      bits &= bits - 1;
    }
  }
  return sum;
}

/*
 * Every attribute of a tuple, in one pass: put(i, datum) for each,
 * datum as tup_decode's view gives it. The whole tuple is checked
 * (parse), since the pass reads all of it anyway.
 */
template <class F>
int deform_each(const sm2::tup_desc_t &desc, std::string_view tuple, F &&put) {
  if (check_desc(desc) != 0) {
    return -1;
  }
  const auto &schema = desc.schema;
  parsed_t t;
  if (parse(desc, tuple, t) != 0) {
    return -1;
  }
  size_t pos = t.fixed;
  for (size_t i = 0; i < schema.size(); i++) {
    // Past the row's attributes: the value it was added with.
    if (i >= t.natts) {
      const auto &m = schema[i].missing;
      put(i, !schema[i].dropped && m ? std::optional(std::string_view(*m))
                                     : std::nullopt);
      continue;
    }
    if (t.nulls && bit(t.nulls, i)) {
      put(i, std::nullopt);
      continue;
    }
    std::string_view d = datum_at(t, desc, i, pos);
    pos += entry_width(desc, i);
    // A dropped attribute's old bytes are skipped.
    put(i, schema[i].dropped ? std::nullopt : std::optional(d));
  }
  return 0;
}

/*
 * One attribute of a tuple, by ordinal, as a view: its bytes, or
 * std::nullopt for null, for tup_decode. It reads only what the
 * attribute needs (TUPLE.md §6, §8): the header and bitmap, then
 * the attribute's entry, found at off[ordinal] less the widths of
 * the nulls before it, and for a variable attribute its varlena.
 * Everything it reads is bounds-checked, but the rest of the tuple
 * is not validated; tup_deform and tup_check do that.
 */
int decode_view(const sm2::tup_desc_t &desc, std::string_view tuple,
                size_t ordinal, std::optional<std::string_view> &datum) {
  return sm2::catch_nomem([&] {
    const auto &schema = desc.schema;
    parsed_t t;
    if (ordinal >= schema.size()) {
      return invalid("attribute " + std::to_string(ordinal) +
                     " is past the schema's " + std::to_string(schema.size()));
    }
    // Step 1: a usable schema, then the header and null bitmap only
    // -- flags, natts, and that the bitmap fits -- not the fixed
    // part or the var data.
    if (check_desc(desc) != 0 || parse_head(desc, tuple, t) != 0) {
      return -1;
    }
    datum = std::nullopt;
    const sm2::attribute_t &a = schema[ordinal];
    // A dropped attribute reads as null, whatever the row holds.
    if (a.dropped) {
      return 0;
    }
    // Step 2: the row was written before the attribute was added,
    // so it has no entry; it reads as the missing value, or null.
    if (ordinal >= t.natts) {
      if (a.missing) {
        datum = std::string_view(*a.missing);
      }
      return 0;
    }
    // Step 3: its bit in the null bitmap is set.
    if (t.nulls && bit(t.nulls, ordinal)) {
      return 0;
    }
    // Step 4: its entry is where it would be in a row with no
    // nulls, off[ordinal] from the fixed part's start, less the
    // entry widths the nulls before it do not take.
    size_t pos = t.fixed + desc.off[ordinal] - null_width(t, desc, ordinal);
    // The entry -- a datum, or a 2-byte slot -- must lie within the
    // tuple; a truncated or damaged tuple could place it past.
    if (pos + entry_width(desc, ordinal) > tuple.size()) {
      return corrupt("cut short in its fixed part");
    }
    // Step 5, variable width: the slot must point past its own
    // entry and into the tuple, and the varlena there must be one
    // the codec reads, ending within the tuple.
    if (desc.width[ordinal] < 0) {
      size_t start = slot_at(t, pos), len;
      if (start < pos || start >= tuple.size()) {
        return corrupt("a slot that does not point to its varlena");
      }
      if (const char *why = varlena_extent(tuple.substr(start), len)) {
        return corrupt(why);
      }
    }
    // Step 5: the fixed-width datum itself, or the varlena whole,
    // header included, as a view into tuple.
    datum = datum_at(t, desc, ordinal, pos);
    return 0;
  });
}

} // namespace

namespace sm2 {

/* Lay schema out: each attribute's width, and off[]; see sm2.h. */
tup_desc_t::tup_desc_t(std::span<const attribute_t> s) : schema(s) {
  width.reserve(s.size());
  off.reserve(s.size() + 1);
  off.push_back(0);
  for (size_t i = 0; i < s.size(); i++) {
    int32_t w = s[i].type ? datum_width(s[i].type, s[i].typmod) : 0;
    if (w == 0 && bad == SIZE_MAX) {
      bad = i;
    }
    width.push_back(w);
    off.push_back(off.back() + (w > 0 ? static_cast<size_t>(w) : slot_width));
  }
}

/* The bytes a non-null Datum stands for; see sm2.h. */
int datum_bytes(const type_t *type, int32_t typmod, const Datum &d,
                std::string_view &out) {
  auto bad = [](const char *why) {
    return sm1::seterr(sm1::SM_ERR_INVALID, std::string("datum: ") + why);
  };
  int32_t w = type ? datum_width(type, typmod) : 0;
  if (w == 0) {
    return bad("no type, or a typmod its type refuses");
  }
  // A by-value datum's bytes are the Datum's own, from its start.
  if (type->byval) {
    out = std::string_view(reinterpret_cast<const char *>(&d), w);
    return 0;
  }
  const char *ptr = reinterpret_cast<const char *>(d);
  if (ptr == nullptr) {
    return bad("not null, but its pointer is null");
  }
  if (w > 0) {
    out = std::string_view(ptr, w);
    return 0;
  }
  // A varlena, whose header gives its length: 1 byte, or 4.
  size_t len = 0;
  std::string_view head(ptr, (ptr[0] & 0x01) ? 1 : 4);
  if (const char *why = varlena_header(head, len)) {
    return bad(why);
  }
  out = std::string_view(ptr, len);
  return 0;
}

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
int tup_encode(const tup_desc_t &desc, std::span<const Datum> datums,
               std::span<const bool> isnull, std::string &tuple) {
  return catch_nomem([&] {
    if (check_desc(desc) != 0) {
      return -1;
    }
    const auto &schema = desc.schema;
    const auto &w = desc.width;
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
      // The bytes the Datum stands for, which the tuple stores.
      if (datum_bytes(schema[i].type, schema[i].typmod, datums[i],
                      attr_bytes[i]) != 0) {
        return invalid("attribute " + std::to_string(i) + ": " +
                       std::string(sm1::errmsg()));
      }
      if (w[i] > 0) {
        fixed += w[i];
        continue;
      }
      nvar++;
      var += varlena_len(varlena_payload(attr_bytes[i]).size());
    }
    // A row past the limit is refused, so every slot is 2 bytes.
    size_t head =
        1 + varint_len(datums.size()) + (nulls ? (datums.size() + 7) / 8 : 0);
    size_t var_start = head + fixed + slot_width * nvar;
    if (var_start + var > max_tup_len) {
      return sm1::seterr(sm1::SM_ERR_ROWTOOLONG,
                         "tuple: a row of " + std::to_string(var_start + var) +
                             " bytes is longer than " +
                             std::to_string(max_tup_len));
    }
    // The header, then the null bitmap, a set bit for a null.
    std::string out;
    out += static_cast<char>(nulls ? has_nulls : 0);
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
      store_le(static_cast<uint16_t>(next), word);
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

/* Check that tuple is one the codec could write; see sm2.h. */
int tup_check(const tup_desc_t &desc, std::string_view tuple) {
  return catch_nomem([&] {
    const auto &schema = desc.schema;
    parsed_t t;
    if (check_desc(desc) != 0) {
      return -1;
    }
    // A malformed tuple handed in to be written is the caller's
    // error, not corruption in the store.
    if (parse(desc, tuple, t) != 0) {
      return sm1::seterr(sm1::SM_ERR_INVALID, std::string(sm1::errmsg()));
    }
    if (t.natts != schema.size()) {
      return invalid("an attribute count other than the schema's");
    }
    for (size_t i = 0; i < schema.size(); i++) {
      if (schema[i].dropped && !(t.nulls && bit(t.nulls, i))) {
        return invalid("attribute " + std::to_string(i) +
                       " is dropped, so it must be null");
      }
    }
    return 0;
  });
}

/* Deform every attribute of tuple into datums and isnull; see sm2.h. */
int tup_deform(const tup_desc_t &desc, std::string_view tuple,
               std::span<Datum> datums, std::span<bool> isnull) {
  return catch_nomem([&] {
    const auto &schema = desc.schema;
    if (datums.size() != schema.size() || isnull.size() != schema.size()) {
      return invalid("deform: output spans must match schema size");
    }
    return deform_each(desc, tuple,
                       [&](size_t i, std::optional<std::string_view> d) {
                         isnull[i] = !d;
                         datums[i] = 0;
                         if (d) {
                           bytes_to_datum(schema[i], *d, datums[i]);
                         }
                       });
  });
}

/* One attribute of a tuple, by ordinal, into val and isnull; see sm2.h. */
int tup_decode(const tup_desc_t &desc, std::string_view tuple, size_t ordinal,
               Datum &val, bool &isnull) {
  std::optional<std::string_view> d;
  if (decode_view(desc, tuple, ordinal, d) != 0) {
    return -1;
  }
  val = 0;
  isnull = !d;
  if (d) {
    bytes_to_datum(desc.schema[ordinal], *d, val);
  }
  return 0;
}

} // namespace sm2

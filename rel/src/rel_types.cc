/*
 * rel_types.cc -- datum conversions between Janet values and sm2 types.
 */

#include "rel.h"

#include <cstring>
#include <string>
#include <string_view>

namespace rel {

/* Parse a Janet keyword or symbol into an sm2 type code. */
int parse_type_code(Janet val, uint8_t *code_out) {
  std::string_view name;
  if (janet_checktype(val, JANET_KEYWORD)) {
    name = reinterpret_cast<const char *>(janet_unwrap_keyword(val));
  } else if (janet_checktype(val, JANET_SYMBOL)) {
    name = reinterpret_cast<const char *>(janet_unwrap_symbol(val));
  } else if (janet_checktype(val, JANET_STRING)) {
    name = reinterpret_cast<const char *>(janet_unwrap_string(val));
  } else {
    return -1;
  }

  if (name == "bool") {
    *code_out = 1;
  } else if (name == "int2") {
    *code_out = 2;
  } else if (name == "int4") {
    *code_out = 3;
  } else if (name == "int8") {
    *code_out = 4;
  } else if (name == "float4") {
    *code_out = 5;
  } else if (name == "float8") {
    *code_out = 6;
  } else if (name == "decimal64") {
    *code_out = 17;
  } else if (name == "decimal128") {
    *code_out = 18;
  } else if (name == "text") {
    *code_out = 8;
  } else if (name == "varchar") {
    *code_out = 9;
  } else if (name == "bytea") {
    *code_out = 10;
  } else if (name == "date") {
    *code_out = 11;
  } else if (name == "time") {
    *code_out = 12;
  } else if (name == "timestamp") {
    *code_out = 13;
  } else if (name == "timestamptz") {
    *code_out = 14;
  } else if (name == "interval") {
    *code_out = 15;
  } else if (name == "uuid") {
    *code_out = 16;
  } else {
    return -1;
  }
  return 0;
}

/* Return the canonical name of a type code. */
const char *type_name_of(uint8_t code) {
  switch (code) {
  case 1:
    return "bool";
  case 2:
    return "int2";
  case 3:
    return "int4";
  case 4:
    return "int8";
  case 5:
    return "float4";
  case 6:
    return "float8";

  case 8:
    return "text";
  case 9:
    return "varchar";
  case 10:
    return "bytea";
  case 11:
    return "date";
  case 12:
    return "time";
  case 13:
    return "timestamp";
  case 14:
    return "timestamptz";
  case 15:
    return "interval";
  case 16:
    return "uuid";
  case 17:
    return "decimal64";
  case 18:
    return "decimal128";
  default:
    return "unknown";
  }
}

/* Construct a varlena buffer from raw bytes. */
void make_varlena(const char *data, size_t len, std::vector<uint8_t> &out) {
  if (len <= 126) {
    // 1-byte header for short varlena.
    uint8_t header = static_cast<uint8_t>(((len + 1) << 1) | 1);
    out.resize(1 + len);
    out[0] = header;
    if (len > 0) {
      memcpy(out.data() + 1, data, len);
    }
  } else {
    // 4-byte little-endian header for long varlena.
    uint32_t header = static_cast<uint32_t>((len + 4) << 2);
    out.resize(4 + len);
    memcpy(out.data(), &header, 4);
    memcpy(out.data() + 4, data, len);
  }
}

/* Extract data pointer and length from a varlena. */
bool varlena_extract(const void *data, size_t len, const char **out_data,
                     size_t *out_len) {
  if (len < 1) {
    return false;
  }
  const uint8_t *bytes = static_cast<const uint8_t *>(data);
  if ((bytes[0] & 1) != 0) {
    // 1-byte header.
    size_t total_len = bytes[0] >> 1;
    if (total_len > len || total_len == 0) {
      return false;
    }
    *out_data = reinterpret_cast<const char *>(bytes + 1);
    *out_len = total_len - 1;
    return true;
  } else if ((bytes[0] & 3) == 0) {
    // 4-byte header.
    if (len < 4) {
      return false;
    }
    uint32_t header = 0;
    memcpy(&header, bytes, 4);
    size_t total_len = header >> 2;
    if (total_len > len || total_len < 4) {
      return false;
    }
    *out_data = reinterpret_cast<const char *>(bytes + 4);
    *out_len = total_len - 4;
    return true;
  }
  return false;
}

/* Convert a Janet value to a native sm_datum_t buffer. */
int janet_to_datum(Janet val, const col_def_t &col, std::vector<uint8_t> &buf,
                   sm_datum_t *datum_out) {
  if (janet_checktype(val, JANET_NIL)) {
    if (col.not_null) {
      return -1;
    }
    datum_out->data = nullptr;
    datum_out->len = 0;
    datum_out->null = true;
    return 0;
  }

  datum_out->null = false;

  switch (col.type_code) {
  case 1: { // bool
    if (!janet_checktype(val, JANET_BOOLEAN)) {
      return -1;
    }
    uint8_t b = janet_unwrap_boolean(val) ? 1 : 0;
    buf.resize(1);
    buf[0] = b;
    datum_out->data = buf.data();
    datum_out->len = 1;
    return 0;
  }
  case 2: { // int2
    if (!janet_checktype(val, JANET_NUMBER)) {
      return -1;
    }
    int16_t i2 = static_cast<int16_t>(janet_unwrap_integer(val));
    buf.resize(sizeof(i2));
    memcpy(buf.data(), &i2, sizeof(i2));
    datum_out->data = buf.data();
    datum_out->len = sizeof(i2);
    return 0;
  }
  case 3: { // int4
    if (!janet_checktype(val, JANET_NUMBER)) {
      return -1;
    }
    int32_t i4 = static_cast<int32_t>(janet_unwrap_integer(val));
    buf.resize(sizeof(i4));
    memcpy(buf.data(), &i4, sizeof(i4));
    datum_out->data = buf.data();
    datum_out->len = sizeof(i4);
    return 0;
  }
  case 4: { // int8
    if (!janet_checktype(val, JANET_NUMBER)) {
      return -1;
    }
    int64_t i8 = static_cast<int64_t>(janet_unwrap_number(val));
    buf.resize(sizeof(i8));
    memcpy(buf.data(), &i8, sizeof(i8));
    datum_out->data = buf.data();
    datum_out->len = sizeof(i8);
    return 0;
  }
  case 5: { // float4
    if (!janet_checktype(val, JANET_NUMBER)) {
      return -1;
    }
    float f4 = static_cast<float>(janet_unwrap_number(val));
    buf.resize(sizeof(f4));
    memcpy(buf.data(), &f4, sizeof(f4));
    datum_out->data = buf.data();
    datum_out->len = sizeof(f4);
    return 0;
  }
  case 6: { // float8
    if (!janet_checktype(val, JANET_NUMBER)) {
      return -1;
    }
    double f8 = janet_unwrap_number(val);
    buf.resize(sizeof(f8));
    memcpy(buf.data(), &f8, sizeof(f8));
    datum_out->data = buf.data();
    datum_out->len = sizeof(f8);
    return 0;
  }
  case 8:    // text
  case 9:    // varchar
  case 10: { // bytea
    JanetByteView view;
    if (!janet_bytes_view(val, &view.bytes, &view.len)) {
      return -1;
    }
    make_varlena(reinterpret_cast<const char *>(view.bytes), view.len, buf);
    datum_out->data = buf.data();
    datum_out->len = buf.size();
    return 0;
  }
  default:
    // Handle arbitrary types via string representation if string passed.
    if (janet_checktype(val, JANET_STRING)) {
      JanetString s = janet_unwrap_string(val);
      size_t slen = janet_string_length(s);
      make_varlena(reinterpret_cast<const char *>(s), slen, buf);
      datum_out->data = buf.data();
      datum_out->len = buf.size();
      return 0;
    }
    return -1;
  }
}

/* Convert a native sm_datum_t back to a Janet value. */
Janet datum_to_janet(const sm_datum_t &datum, uint8_t type_code) {
  if (datum.null || datum.data == nullptr) {
    return janet_wrap_nil();
  }

  switch (type_code) {
  case 1: { // bool
    uint8_t b = *static_cast<const uint8_t *>(datum.data);
    return janet_wrap_boolean(b != 0);
  }
  case 2: { // int2
    int16_t v = 0;
    if (datum.len == sizeof(v)) {
      memcpy(&v, datum.data, sizeof(v));
    }
    return janet_wrap_integer(v);
  }
  case 3: { // int4
    int32_t v = 0;
    if (datum.len == sizeof(v)) {
      memcpy(&v, datum.data, sizeof(v));
    }
    return janet_wrap_integer(v);
  }
  case 4: { // int8
    int64_t v = 0;
    if (datum.len == sizeof(v)) {
      memcpy(&v, datum.data, sizeof(v));
    }
    return janet_wrap_number(static_cast<double>(v));
  }
  case 5: { // float4
    float v = 0.0f;
    if (datum.len == sizeof(v)) {
      memcpy(&v, datum.data, sizeof(v));
    }
    return janet_wrap_number(static_cast<double>(v));
  }
  case 6: { // float8
    double v = 0.0;
    if (datum.len == sizeof(v)) {
      memcpy(&v, datum.data, sizeof(v));
    }
    return janet_wrap_number(v);
  }
  case 8:    // text
  case 9:    // varchar
  case 10: { // bytea
    const char *payload = nullptr;
    size_t payload_len = 0;
    if (!varlena_extract(datum.data, datum.len, &payload, &payload_len)) {
      return janet_wrap_nil();
    }
    return janet_wrap_string(
        janet_string(reinterpret_cast<const uint8_t *>(payload), payload_len));
  }
  default:
    // By default, extract varlena payload or treat as raw bytes.
    const char *payload = nullptr;
    size_t payload_len = 0;
    if (varlena_extract(datum.data, datum.len, &payload, &payload_len)) {
      return janet_wrap_string(janet_string(
          reinterpret_cast<const uint8_t *>(payload), payload_len));
    }
    return janet_wrap_string(
        janet_string(static_cast<const uint8_t *>(datum.data), datum.len));
  }
}

/* The width of a by-value type's Datum bytes; 0 if by reference. */
static size_t byval_width(uint8_t code) {
  switch (code) {
  case 1: // bool
    return 1;
  case 2: // int2
    return 2;
  case 3:  // int4
  case 5:  // float4
  case 11: // date
    return 4;
  case 4:  // int8
  case 6:  // float8
  case 12: // time
  case 13: // timestamp
  case 14: // timestamptz
    return 8;
  default:
    return 0;
  }
}

/* Convert a Janet value to a value and null flag; see rel.h. */
int janet_to_value(Janet val, const col_def_t &col, std::vector<uint8_t> &buf,
                   sm_datum_val_t *val_out, bool *isnull_out) {
  sm_datum_t d;
  if (janet_to_datum(val, col, buf, &d) != 0) {
    return -1;
  }
  *val_out = 0;
  *isnull_out = d.null;
  if (d.null) {
    return 0;
  }
  // A by-value type carries its bytes in the Datum itself.
  size_t w = byval_width(col.type_code);
  if (w != 0) {
    if (d.len != w) {
      return -1;
    }
    memcpy(val_out, d.data, w);
    return 0;
  }
  // Only the varlena types are built by reference here; a Datum
  // pointing at anything else would be misread by the encoder.
  if (col.type_code < 8 || col.type_code > 10) {
    return -1;
  }
  *val_out = SM_DATUM_FROM_PTR(d.data);
  return 0;
}

/* View a value as an sm_datum_t; see rel.h. */
sm_datum_t value_to_datum(const sm_datum_val_t &val, bool isnull,
                          uint8_t type_code) {
  if (isnull) {
    return sm_datum_t{nullptr, 0, true};
  }
  size_t w = byval_width(type_code);
  if (w != 0) {
    return sm_datum_t{&val, w, false};
  }
  // A varlena's length is in its 1-byte or 4-byte header.
  const uint8_t *p = static_cast<const uint8_t *>(SM_DATUM_TO_PTR(val));
  size_t len = 0;
  if ((p[0] & 1) != 0) {
    len = p[0] >> 1;
  } else {
    uint32_t header = 0;
    memcpy(&header, p, 4);
    len = header >> 2;
  }
  return sm_datum_t{p, len, false};
}

/* Convert a value back to a Janet value. */
Janet value_to_janet(const sm_datum_val_t &val, bool isnull,
                     uint8_t type_code) {
  return datum_to_janet(value_to_datum(val, isnull, type_code), type_code);
}

} // namespace rel

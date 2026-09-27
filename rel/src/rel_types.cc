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

/* A column's typmod from the numbers after its type; see rel.h. */
int typmod_of(uint8_t code, const std::vector<int32_t> &mods,
              int32_t *typmod_out) {
  *typmod_out = -1;
  if (mods.empty()) {
    return 0;
  }
  switch (code) {
  case 17: // decimal64 (p[, s])
  case 18: // decimal128 (p[, s])
    if (mods.size() > 2) {
      return -1;
    }
    // PostgreSQL's numeric typmod: ((p << 16) | s) + 4.
    *typmod_out = ((mods[0] << 16) | (mods.size() > 1 ? mods[1] : 0)) + 4;
    return 0;
  case 12: // time (p)
  case 13: // timestamp (p)
  case 14: // timestamptz (p)
    if (mods.size() != 1) {
      return -1;
    }
    *typmod_out = mods[0];
    return 0;
  case 9: // varchar (n)
    if (mods.size() != 1) {
      return -1;
    }
    // PostgreSQL's varchar typmod: n + 4.
    *typmod_out = mods[0] + 4;
    return 0;
  default:
    return -1;
  }
}

/* The numbers after a column's type, from its typmod; see rel.h. */
std::vector<int32_t> mods_of(uint8_t code, int32_t typmod) {
  if (typmod < 0) {
    return {};
  }
  switch (code) {
  case 17: // decimal64 (p, s)
  case 18: // decimal128 (p, s)
    return {(typmod - 4) >> 16, (typmod - 4) & 0xFFFF};
  case 12: // time (p)
  case 13: // timestamp (p)
  case 14: // timestamptz (p)
    return {typmod};
  case 9: // varchar (n)
    return {typmod - 4};
  default:
    return {};
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

/* The payload of the varlena at p, its length from its header. */
void varlena_payload(const void *p, const char **out_data, size_t *out_len) {
  const uint8_t *bytes = static_cast<const uint8_t *>(p);
  if ((bytes[0] & 1) != 0) {
    // 1-byte header.
    *out_data = reinterpret_cast<const char *>(bytes + 1);
    *out_len = (bytes[0] >> 1) - 1;
    return;
  }
  // 4-byte little-endian header.
  uint32_t header = 0;
  memcpy(&header, bytes, 4);
  *out_data = reinterpret_cast<const char *>(bytes + 4);
  *out_len = (header >> 2) - 4;
}

/* Convert a Janet value to a value and null flag; see rel.h. */
int janet_to_value(Janet val, const col_def_t &col, std::vector<uint8_t> &buf,
                   sm_datum_t *val_out, bool *isnull_out) {
  *val_out = 0;
  *isnull_out = false;
  if (janet_checktype(val, JANET_NIL)) {
    if (col.not_null) {
      return -1;
    }
    *isnull_out = true;
    return 0;
  }
  switch (col.type_code) {
  case 1: // bool
    if (!janet_checktype(val, JANET_BOOLEAN)) {
      return -1;
    }
    *val_out = SM_DATUM_FROM_BOOL(janet_unwrap_boolean(val));
    return 0;
  case 2: // int2
    if (!janet_checktype(val, JANET_NUMBER)) {
      return -1;
    }
    *val_out = SM_DATUM_FROM_INT16(janet_unwrap_integer(val));
    return 0;
  case 3: // int4
    if (!janet_checktype(val, JANET_NUMBER)) {
      return -1;
    }
    *val_out = SM_DATUM_FROM_INT32(janet_unwrap_integer(val));
    return 0;
  case 4: // int8
    if (!janet_checktype(val, JANET_NUMBER)) {
      return -1;
    }
    *val_out =
        SM_DATUM_FROM_INT64(static_cast<int64_t>(janet_unwrap_number(val)));
    return 0;
  case 5: // float4
    if (!janet_checktype(val, JANET_NUMBER)) {
      return -1;
    }
    *val_out =
        sm_datum_from_float(static_cast<float>(janet_unwrap_number(val)));
    return 0;
  case 6: // float8
    if (!janet_checktype(val, JANET_NUMBER)) {
      return -1;
    }
    *val_out = sm_datum_from_double(janet_unwrap_number(val));
    return 0;
  case 8:    // text
  case 9:    // varchar
  case 10: { // bytea
    // By reference: a varlena in buf, which the value points at.
    JanetByteView view;
    if (!janet_bytes_view(val, &view.bytes, &view.len)) {
      return -1;
    }
    make_varlena(reinterpret_cast<const char *>(view.bytes), view.len, buf);
    *val_out = SM_DATUM_FROM_PTR(buf.data());
    return 0;
  }
  default: {
    // Any other type is written as its canonical text, which the
    // type parses (sm_type_in); a by-reference result is copied
    // into buf, which the value then points at.
    if (!janet_checktype(val, JANET_STRING)) {
      return -1;
    }
    JanetString str = janet_unwrap_string(val);
    void *block = nullptr;
    size_t block_len = 0;
    if (sm_type_in(sm_type(col.type_code), col.typmod,
                   reinterpret_cast<const char *>(str),
                   janet_string_length(str), val_out, &block,
                   &block_len) != 0) {
      return -1;
    }
    if (block != nullptr) {
      const uint8_t *b = static_cast<const uint8_t *>(block);
      buf.assign(b, b + block_len);
      sm_free(block);
      *val_out = SM_DATUM_FROM_PTR(buf.data());
    }
    return 0;
  }
  }
}

/* Convert a value back to a Janet value. */
Janet value_to_janet(const sm_datum_t &val, bool isnull, uint8_t type_code,
                     int32_t typmod) {
  if (isnull) {
    return janet_wrap_nil();
  }
  switch (type_code) {
  case 1: // bool
    return janet_wrap_boolean(SM_DATUM_TO_BOOL(val));
  case 2: // int2
    return janet_wrap_integer(SM_DATUM_TO_INT16(val));
  case 3: // int4
    return janet_wrap_integer(SM_DATUM_TO_INT32(val));
  case 4: // int8
    return janet_wrap_number(static_cast<double>(SM_DATUM_TO_INT64(val)));
  case 5: // float4
    return janet_wrap_number(static_cast<double>(sm_datum_to_float(val)));
  case 6: // float8
    return janet_wrap_number(sm_datum_to_double(val));
  case 8:    // text
  case 9:    // varchar
  case 10: { // bytea
    const char *payload = nullptr;
    size_t len = 0;
    varlena_payload(SM_DATUM_TO_PTR(val), &payload, &len);
    return janet_wrap_string(
        janet_string(reinterpret_cast<const uint8_t *>(payload), len));
  }
  default: {
    // Any other type reads back as its canonical text.
    char *text = nullptr;
    if (sm_type_out(sm_type(type_code), typmod, val, &text) != 0) {
      return janet_wrap_nil();
    }
    Janet out = janet_cstringv(text);
    sm_free(text);
    return out;
  }
  }
}

} // namespace rel

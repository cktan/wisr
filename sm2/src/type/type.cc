/*
 * type.cc -- finding a type by its frozen code, and the errors
 * every type reports through sm1's error state.
 */

#include "native.h"
#include "sm2.h"

#include <string>

namespace sm2 {

/* The type with this code, or nullptr if there is none. */
const type_t *type_by_code(uint8_t code) {
  // Every type built so far, by its frozen code.
  static const type_t *const types[] = {
      &type_bool,      &type_int2,        &type_int4,     &type_int8,
      &type_float4,    &type_float8,      &type_decimal,  &type_text,
      &type_varchar,   &type_bytea,       &type_date,     &type_time,
      &type_timestamp, &type_timestamptz, &type_interval, &type_uuid};
  for (const type_t *t : types) {
    if (t->code == code) {
      return t;
    }
  }
  return nullptr;
}

/* Append s escaped and terminated: 0x00 as 0x00 0xFF, then 0x00 0x00. */
void put_escaped(std::string_view s, std::string &key) {
  for (char c : s) {
    key += c;
    if (c == '\0') {
      key += '\xff';
    }
  }
  key.append(2, '\0');
}

/* Read put_escaped's bytes from the front of key; false if malformed. */
bool get_escaped(std::string_view &key, std::string &s) {
  s.clear();
  for (size_t i = 0; i < key.size(); i++) {
    if (key[i] != '\0') {
      s += key[i];
      continue;
    }
    // A NUL is followed by 0xFF, escaping a NUL, or by 0x00, ending.
    if (i + 1 == key.size()) {
      return false;
    }
    if (key[i + 1] == '\0') {
      key.remove_prefix(i + 2);
      return true;
    }
    if (key[i + 1] != '\xff') {
      return false;
    }
    s += '\0';
    i++;
  }
  return false;
}

/* Fail with SM_ERR_INVALID: a key of type is malformed or cut short. */
int bad_key(const char *type) {
  return sm1::seterr(sm1::SM_ERR_INVALID,
                     std::string("a ") + type + " key is malformed");
}

/* Fail with SM_ERR_INVALID: text is not a valid value of type. */
int bad_input(const char *type, std::string_view text) {
  return sm1::seterr(sm1::SM_ERR_INVALID,
                     std::string("invalid input for type ") + type + ": \"" +
                         std::string(text) + "\"");
}

/* Fail with SM_ERR_INVALID: datum is not len bytes, as type needs. */
int bad_datum(const char *type, std::string_view datum, size_t len) {
  return sm1::seterr(sm1::SM_ERR_INVALID,
                     std::string("a ") + type + " datum is " +
                         std::to_string(len) + " bytes, not " +
                         std::to_string(datum.size()));
}

} // namespace sm2

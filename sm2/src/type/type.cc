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
      &type_bool,      &type_int2,        &type_int4,    &type_int8,
      &type_float4,    &type_float8,      &type_date,    &type_time,
      &type_timestamp, &type_timestamptz, &type_interval};
  for (const type_t *t : types) {
    if (t->code == code) {
      return t;
    }
  }
  return nullptr;
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

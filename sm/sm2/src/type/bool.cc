/*
 * bool.cc -- the bool type (TYPE.md §3.1): one byte, 0 or 1;
 * PostgreSQL prints it "t" or "f".
 */

#include "native.h"
#include "sm2.h"

namespace {

/* Parse "t" or "f", PostgreSQL's canonical bool text. */
int bool_in(std::string_view text, int32_t, std::string &datum) {
  if (text == "t" || text == "f") {
    datum.assign(1, text == "t" ? '\1' : '\0');
    return 0;
  }
  return sm2::bad_input("bool", text);
}

/* Print a bool datum as "t" or "f"; any byte but 0 or 1 is invalid. */
int bool_out(std::string_view datum, int32_t, std::string &text) {
  if (datum.size() != 1) {
    return sm2::bad_datum("bool", datum, 1);
  }
  if (datum[0] != '\0' && datum[0] != '\1') {
    return sm2::bad_input("bool", datum);
  }
  text = datum[0] ? "t" : "f";
  return 0;
}

/* false before true. */
int bool_cmp(std::string_view a, std::string_view b, int32_t) {
  return static_cast<int>(static_cast<unsigned char>(a[0])) -
         static_cast<int>(static_cast<unsigned char>(b[0]));
}

/* A bool's key is its byte: false before true. */
int bool_key(std::string_view datum, int32_t, const sm2::collation_t *,
             std::string &key) {
  if (datum.size() != 1 || (datum[0] != '\0' && datum[0] != '\1')) {
    return sm2::bad_datum("bool", datum, 1);
  }
  key += datum[0];
  return 0;
}

/* Read a bool's key. */
int bool_unkey(std::string_view &key, int32_t, const sm2::collation_t *,
               std::string &datum) {
  if (key.empty() || (key[0] != '\0' && key[0] != '\1')) {
    return sm2::bad_key("bool");
  }
  datum.assign(1, key[0]);
  key.remove_prefix(1);
  return 0;
}

} // namespace

namespace sm2 {

const type_t type_bool = {.code = 1,
                          .name = "bool",
                          .len = 1,
                          .byval = true,
                          .align = 1,
                          .in = bool_in,
                          .out = bool_out,
                          .cmp = bool_cmp,
                          .key_encode = bool_key,
                          .key_decode = bool_unkey};

} // namespace sm2

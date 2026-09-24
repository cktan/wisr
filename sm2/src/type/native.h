/*
 * native.h -- reading and writing a datum's native bytes, and
 * the errors every type reports.
 *
 * A fixed-width datum is PostgreSQL's representation fixed at
 * little-endian (SM2_DESIGN.md §2.1): a memcpy on a little-endian
 * host, a byte swap on a big-endian one.
 */
#ifndef SM2_NATIVE_H
#define SM2_NATIVE_H

#include "sm1.h"

#include <cstdint>
#include <cstring>

#include <bit>
#include <new>
#include <string>
#include <string_view>
#include <type_traits>

namespace sm2 {

/* v with its bytes reversed, for a big-endian host. */
template <typename T> T byte_swap(T v) {
  static_assert(std::is_integral_v<T>, "integers only");
  if constexpr (sizeof(T) == 2) {
    return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(v)));
  } else if constexpr (sizeof(T) == 4) {
    return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(v)));
  } else if constexpr (sizeof(T) == 8) {
    return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(v)));
  } else {
    return v;
  }
}

/* The integer a datum of exactly sizeof(T) bytes holds. */
template <typename T> T load_le(std::string_view datum) {
  T v;
  memcpy(&v, datum.data(), sizeof(T));
  if constexpr (std::endian::native == std::endian::big) {
    v = byte_swap(v);
  }
  return v;
}

/* Replace datum with v's little-endian bytes. */
template <typename T> void store_le(T v, std::string &datum) {
  if constexpr (std::endian::native == std::endian::big) {
    v = byte_swap(v);
  }
  datum.assign(reinterpret_cast<const char *>(&v), sizeof(T));
}

/*
 * f(), or -1 with SM_ERR_NOMEM if it runs out of memory: a type's
 * in and out are sm2 entry points (SM2_DESIGN.md §5). Recording
 * the error allocates nothing.
 */
template <typename F> int catch_nomem(F f) {
  try {
    return f();
  } catch (const std::bad_alloc &) {
    return sm1::seterr(sm1::SM_ERR_NOMEM, "out of memory");
  }
}

/* Fail with SM_ERR_INVALID: text is not a valid value of type. */
int bad_input(const char *type, std::string_view text);

/* Fail with SM_ERR_INVALID: datum is not len bytes, as type needs. */
int bad_datum(const char *type, std::string_view datum, size_t len);

} // namespace sm2

#endif /* SM2_NATIVE_H */

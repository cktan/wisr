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

/*
 * Append u's bytes most significant first; U is unsigned,
 * unsigned __int128 included.
 */
template <typename U> void put_be(U u, std::string &key) {
  for (int i = sizeof(U) - 1; i >= 0; i--) {
    key += static_cast<char>(u >> (8 * i));
  }
}

/* Read put_be's bytes from the front of key; false if too few. */
template <typename U> bool get_be(std::string_view &key, U &u) {
  if (key.size() < sizeof(U)) {
    return false;
  }
  u = 0;
  for (size_t i = 0; i < sizeof(U); i++) {
    u = static_cast<U>(u << 8 | static_cast<unsigned char>(key[i]));
  }
  key.remove_prefix(sizeof(U));
  return true;
}

/*
 * The key of a signed integer (TYPE.md §3.2): its sign bit
 * flipped, big-endian, so memcmp orders it as the integer. U is
 * the unsigned type of the same width.
 */
template <typename U, typename S> void put_key_int(S v, std::string &key) {
  put_be(static_cast<U>(static_cast<U>(v) ^ (U{1} << (8 * sizeof(U) - 1))),
         key);
}

/* Read put_key_int's bytes from the front of key; false if too few. */
template <typename U, typename S>
bool get_key_int(std::string_view &key, S &v) {
  U u;
  if (!get_be(key, u)) {
    return false;
  }
  v = static_cast<S>(static_cast<U>(u ^ (U{1} << (8 * sizeof(U) - 1))));
  return true;
}

/* Fail with SM_ERR_INVALID: a key of type is malformed or cut short. */
int bad_key(const char *type);

/* Fail with SM_ERR_INVALID: datum is not len bytes, as type needs. */
int bad_datum(const char *type, std::string_view datum, size_t len);

/*
 * The key of a datum that is the signed integer S, little-endian:
 * the integers, date, time and the timestamps (TYPE.md §3.2).
 */
template <typename S, typename U>
int int_key(const char *type, std::string_view datum, std::string &key) {
  return catch_nomem([&] {
    if (datum.size() != sizeof(S)) {
      return bad_datum(type, datum, sizeof(S));
    }
    put_key_int<U>(load_le<S>(datum), key);
    return 0;
  });
}

/* Read int_key's bytes back into the little-endian datum. */
template <typename S, typename U>
int int_unkey(const char *type, std::string_view &key, std::string &datum) {
  return catch_nomem([&] {
    S v;
    if (!get_key_int<U>(key, v)) {
      return bad_key(type);
    }
    store_le(v, datum);
    return 0;
  });
}

/*
 * Append s escaped and terminated (TYPE.md §3.6): each 0x00 as
 * 0x00 0xFF, then 0x00 0x00, so the key orders as s and is a prefix
 * of no other.
 */
void put_escaped(std::string_view s, std::string &key);

/* Read put_escaped's bytes from the front of key; false if malformed. */
bool get_escaped(std::string_view &key, std::string &s);

/* Fail with SM_ERR_INVALID: text is not a valid value of type. */
int bad_input(const char *type, std::string_view text);

} // namespace sm2

#endif /* SM2_NATIVE_H */

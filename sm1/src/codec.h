/*
 * codec.h -- fixed-width big-endian integers, the byte order
 * every persisted number uses so that memcmp orders them
 * numerically.
 *
 * The conversions byte-swap the whole word at once (one
 * instruction on x86-64 and AArch64) rather than shifting a
 * byte at a time, and memcpy to and from the buffer, which
 * need not be aligned. They sit on the hot path: every
 * version key is encoded and decoded through them.
 * std::byteswap replaces the builtins once the project moves
 * to C++23.
 */
#ifndef WISR_CODEC_H
#define WISR_CODEC_H

#include <stdint.h>
#include <string.h>

#include <bit>
#include <string>

namespace wisr {

static_assert(std::endian::native == std::endian::little ||
                  std::endian::native == std::endian::big,
              "mixed-endian platforms are not supported");

/* v in big-endian byte order: swapped on a little-endian host. */
inline uint64_t to_be64(uint64_t v) {
  if constexpr (std::endian::native == std::endian::little) {
    return __builtin_bswap64(v);
  }
  return v;
}

/* v in big-endian byte order: swapped on a little-endian host. */
inline uint32_t to_be32(uint32_t v) {
  if constexpr (std::endian::native == std::endian::little) {
    return __builtin_bswap32(v);
  }
  return v;
}

/* Append v to out as 8 big-endian bytes. */
inline void put_be64(std::string &out, uint64_t v) {
  // Swap once, then append the word's bytes as they lie in memory.
  uint64_t be = to_be64(v);
  out.append(reinterpret_cast<const char *>(&be), sizeof(be));
}

/* Append v to out as 4 big-endian bytes. */
inline void put_be32(std::string &out, uint32_t v) {
  // Swap once, then append the word's bytes as they lie in memory.
  uint32_t be = to_be32(v);
  out.append(reinterpret_cast<const char *>(&be), sizeof(be));
}

/* Decode 4 big-endian bytes at p, which need not be aligned. */
inline uint32_t get_be32(const char *p) {
  uint32_t be;
  // Copy the bytes out: p may be unaligned.
  memcpy(&be, p, sizeof(be));
  // Swapping is its own inverse.
  return to_be32(be);
}

/* Decode 8 big-endian bytes at p, which need not be aligned. */
inline uint64_t get_be64(const char *p) {
  uint64_t be;
  // Copy the bytes out: p may be unaligned.
  memcpy(&be, p, sizeof(be));
  // Swapping is its own inverse.
  return to_be64(be);
}

} // namespace wisr

#endif /* WISR_CODEC_H */

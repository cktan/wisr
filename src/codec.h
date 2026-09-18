/*
 * codec.h -- fixed-width big-endian integers, the byte order
 * every persisted number uses so that memcmp orders them
 * numerically.
 */
#ifndef WISR_CODEC_H
#define WISR_CODEC_H

#include <stdint.h>
#include <string>

namespace wisr
{

/* Append v to out as 8 big-endian bytes. */
inline void put_be64(std::string& out, uint64_t v)
{
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>(v >> shift));
    }
}

/* Append v to out as 4 big-endian bytes. */
inline void put_be32(std::string& out, uint32_t v)
{
    for (int shift = 24; shift >= 0; shift -= 8) {
        out.push_back(static_cast<char>(v >> shift));
    }
}

/* Decode 4 big-endian bytes at p. */
inline uint32_t get_be32(const char* p)
{
    uint32_t v = 0;
    for (int i = 0; i < 4; i++) {
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    }
    return v;
}

/* Decode 8 big-endian bytes at p. */
inline uint64_t get_be64(const char* p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v = (v << 8) | static_cast<unsigned char>(p[i]);
    }
    return v;
}

} // namespace wisr

#endif /* WISR_CODEC_H */

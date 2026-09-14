// Network byte order load and store.
//
// Exchange binary protocols are big endian and unaligned: an ITCH Add Order
// puts an 8 byte order reference at offset 11, which is not an 8 byte boundary.
// Reading that with a reinterpret_cast to uint64_t* is undefined behaviour and
// on some targets a fault, so every field goes through memcpy, which the
// compiler turns back into a single load.
#pragma once

#include <cstdint>
#include <cstring>

namespace ltx::wire {

inline std::uint16_t bswap(std::uint16_t v) { return __builtin_bswap16(v); }
inline std::uint32_t bswap(std::uint32_t v) { return __builtin_bswap32(v); }
inline std::uint64_t bswap(std::uint64_t v) { return __builtin_bswap64(v); }

template <typename T>
inline T load_be(const std::uint8_t* p) {
  T v;
  std::memcpy(&v, p, sizeof(T));
  return bswap(v);
}

inline std::uint8_t load_u8(const std::uint8_t* p) { return *p; }

// ITCH timestamps are 48 bit: nanoseconds since midnight.
inline std::uint64_t load_u48(const std::uint8_t* p) {
  return (static_cast<std::uint64_t>(p[0]) << 40) | (static_cast<std::uint64_t>(p[1]) << 32) |
         (static_cast<std::uint64_t>(p[2]) << 24) | (static_cast<std::uint64_t>(p[3]) << 16) |
         (static_cast<std::uint64_t>(p[4]) << 8) | static_cast<std::uint64_t>(p[5]);
}

template <typename T>
inline void store_be(std::uint8_t* p, T v) {
  const T s = bswap(v);
  std::memcpy(p, &s, sizeof(T));
}

inline void store_u8(std::uint8_t* p, std::uint8_t v) { *p = v; }

inline void store_u48(std::uint8_t* p, std::uint64_t v) {
  p[0] = static_cast<std::uint8_t>(v >> 40);
  p[1] = static_cast<std::uint8_t>(v >> 32);
  p[2] = static_cast<std::uint8_t>(v >> 24);
  p[3] = static_cast<std::uint8_t>(v >> 16);
  p[4] = static_cast<std::uint8_t>(v >> 8);
  p[5] = static_cast<std::uint8_t>(v);
}

}  // namespace ltx::wire

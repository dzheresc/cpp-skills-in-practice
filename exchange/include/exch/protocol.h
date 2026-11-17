// Copyright 2026 The Exchange Authors.

// Inbound wire protocol. Skill section 5.3: never put a raw C++ struct on the
// wire; define field order, widths, byte order and versioning explicitly and
// go through encode/decode functions with a fixed byte layout.
//
// All integers little-endian. Byte offsets:
//
//   NewOrder (28 bytes)        Cancel (12 bytes)       Modify (20 bytes)
//   0  type   'N'              0  type   'X'           0  type   'M'
//   1  version (=1)            1  version              1  version
//   2  side   'B'/'S'          2  reserved (0)         2  reserved (0)
//   3  tif    '0' day,'3' IOC  3  reserved (0)         3  reserved (0)
//   4  symbol  char[8]         4  order_id u64         4  order_id u64
//   12 order_id u64                                    12 new price u32
//   20 price  u32 (ticks)                              16 new qty   u32
//   24 qty    u32
//
// Both engines use these same decoders, so the comparison is about the
// engines, not about parsing.
#ifndef INCLUDE_EXCH_PROTOCOL_H_
#define INCLUDE_EXCH_PROTOCOL_H_

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

#include "exch/types.h"

namespace exch::wire {

inline constexpr std::uint8_t kVersion = 1;
inline constexpr std::size_t kNewSize = 28;
inline constexpr std::size_t kCancelSize = 12;
inline constexpr std::size_t kModifySize = 20;
inline constexpr std::size_t kMaxMsgSize = 28;

enum class MsgType : std::uint8_t { kNew = 'N', kCancel = 'X', kModify = 'M' };

struct NewOrder {
  SymbolCode symbol;
  OrderId id;
  Price price;
  Qty qty;
  Side side;
  Tif tif;
};
struct CancelOrder {
  OrderId id;
};
struct ModifyOrder {
  OrderId id;
  Price price;
  Qty qty;
};

// std::byteswap is C++23; this is the C++20 equivalent for unsigned integers.
template <class T>
constexpr T ByteSwap(T v) noexcept {
  if constexpr (sizeof(T) == 1)
    return v;
  else if constexpr (sizeof(T) == 2)
    return __builtin_bswap16(v);
  else if constexpr (sizeof(T) == 4)
    return __builtin_bswap32(v);
  else
    return __builtin_bswap64(v);
}

template <class T>
inline T GetLe(const std::byte* p) noexcept {
  T v;
  std::memcpy(&v, p, sizeof v);  // no alignment assumption
  if constexpr (std::endian::native == std::endian::big) v = ByteSwap(v);
  return v;
}
template <class T>
inline void PutLe(std::byte* p, T v) noexcept {
  if constexpr (std::endian::native == std::endian::big) v = ByteSwap(v);
  std::memcpy(p, &v, sizeof v);
}

// Decoders validate size, type and version before reading any field.
inline bool Decode(std::span<const std::byte> m, NewOrder& o) noexcept {
  if (m.size() != kNewSize || m[0] != std::byte{'N'} ||
      m[1] != std::byte{kVersion})
    return false;
  o.side = static_cast<Side>(m[2]);
  o.tif = static_cast<Tif>(m[3]);
  o.symbol = GetLe<std::uint64_t>(&m[4]);
  o.id = GetLe<std::uint64_t>(&m[12]);
  o.price = GetLe<std::uint32_t>(&m[20]);
  o.qty = GetLe<std::uint32_t>(&m[24]);
  return true;
}
inline bool Decode(std::span<const std::byte> m, CancelOrder& o) noexcept {
  if (m.size() != kCancelSize || m[0] != std::byte{'X'} ||
      m[1] != std::byte{kVersion} || m[2] != std::byte{0} ||
      m[3] != std::byte{0})
    return false;
  o.id = GetLe<std::uint64_t>(&m[4]);
  return true;
}
inline bool Decode(std::span<const std::byte> m, ModifyOrder& o) noexcept {
  if (m.size() != kModifySize || m[0] != std::byte{'M'} ||
      m[1] != std::byte{kVersion} || m[2] != std::byte{0} ||
      m[3] != std::byte{0})
    return false;
  o.id = GetLe<std::uint64_t>(&m[4]);
  o.price = GetLe<std::uint32_t>(&m[12]);
  o.qty = GetLe<std::uint32_t>(&m[16]);
  return true;
}

// Encoders (used by clients, the workload generator and tests).
inline std::size_t Encode(const NewOrder& o, std::byte* out,
                          std::uint8_t version = kVersion) noexcept {
  out[0] = std::byte{'N'};
  out[1] = std::byte{version};
  out[2] = static_cast<std::byte>(o.side);
  out[3] = static_cast<std::byte>(o.tif);
  PutLe(out + 4, o.symbol);
  PutLe(out + 12, o.id);
  PutLe(out + 20, o.price);
  PutLe(out + 24, o.qty);
  return kNewSize;
}
inline std::size_t Encode(const CancelOrder& o, std::byte* out) noexcept {
  out[0] = std::byte{'X'};
  out[1] = std::byte{kVersion};
  out[2] = out[3] = std::byte{0};
  PutLe(out + 4, o.id);
  return kCancelSize;
}
inline std::size_t Encode(const ModifyOrder& o, std::byte* out) noexcept {
  out[0] = std::byte{'M'};
  out[1] = std::byte{kVersion};
  out[2] = out[3] = std::byte{0};
  PutLe(out + 4, o.id);
  PutLe(out + 12, o.price);
  PutLe(out + 16, o.qty);
  return kModifySize;
}

}  // namespace exch::wire

#endif  // INCLUDE_EXCH_PROTOCOL_H_

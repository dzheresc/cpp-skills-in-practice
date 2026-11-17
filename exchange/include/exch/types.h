// Copyright 2026 The Exchange Authors.

// Shared vocabulary for both engines: identifiers, exchange rules, and the
// event record every engine emits. Both engines must produce byte-identical
// event streams for the same input, so everything that defines behaviour
// (limits, reject reasons, event shapes) lives here, not in either engine.
#ifndef INCLUDE_EXCH_TYPES_H_
#define INCLUDE_EXCH_TYPES_H_

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace exch {

using OrderId = std::uint64_t;
// integer ticks: never floating point in a matcher
using Price = std::uint32_t;
using Qty = std::uint32_t;
// 8 ASCII bytes, space padded, packed little-endian
using SymbolCode = std::uint64_t;

enum class Side : std::uint8_t { kBuy = 'B', kSell = 'S' };
enum class Tif : std::uint8_t { kDay = '0', kIoc = '3' };

// ---- Exchange rules (identical for both engines) -------------------------
// Price collar: valid prices are 1..kMaxPrice ticks. Real venues bound prices
// per instrument; we use one bound for all instruments.
inline constexpr Price kMaxPrice = 65535;
inline constexpr Qty kMaxQty = 1'000'000;
inline constexpr std::size_t kMaxSymbols = 16;

enum class RejectReason : std::uint8_t {
  // undecodable message (bad type, size or version)
  kNone = 0,
  kMalformed,
  kUnknownSymbol,
  kBadSide,
  kBadTif,
  kBadPrice,
  // order id 0
  kBadQty,
  kBadOrderId,
  kDuplicateId,   // id already belongs to a live (resting) order
  kBookFull,      // exchange-wide live order limit reached
  kUnknownOrder,  // cancel/modify for an id that is not resting
};

enum class EventType : std::uint8_t {
  kAck,
  kReject,
  kTrade,
  kCancelled,
  kModified
};

// One output record. For Trade: order_id = aggressor (taker), other_id =
// resting order (maker), price = maker's price. For Cancelled: qty = quantity
// removed from the book (also used for the unfilled rest of an IOC).
struct Event {
  EventType type{};
  RejectReason reason{};
  std::uint16_t symbol{};  // index into ExchangeConfig::symbols (0 if unknown)
  Price price{};
  Qty qty{};
  // explicit, zeroed padding: events are compared bytewise
  std::uint32_t reserved{};
  OrderId order_id{};
  OrderId other_id{};
  friend bool operator==(const Event&, const Event&) = default;
};
static_assert(sizeof(Event) == 32);

// Control-plane configuration, read once at start-up (never on the hot path).
struct ExchangeConfig {
  std::vector<std::string> symbols;  // up to kMaxSymbols, <= 8 chars
  std::size_t max_live_orders = 1 << 20;
  std::uint64_t hash_seed = 0x9E3779B97F4A7C15ull;
};

// One resting order in priority order, used by tests to compare whole books.
struct BookEntry {
  std::uint16_t symbol;
  Side side;
  Price price;
  OrderId id;
  Qty qty;
  friend bool operator==(const BookEntry&, const BookEntry&) = default;
};

inline SymbolCode PackSymbol(const std::string& s) {
  SymbolCode c = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    const unsigned char ch =
        i < s.size() ? static_cast<unsigned char>(s[i]) : ' ';
    c |= static_cast<SymbolCode>(ch) << (8 * i);
  }
  return c;
}

}  // namespace exch

#endif  // INCLUDE_EXCH_TYPES_H_

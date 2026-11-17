// Copyright 2026 The Exchange Authors.

// Price ladder: one flat array slot per price tick plus a two-level occupancy
// bitmap. Replaces std::map<Price, std::list<Order>>.
//
// Skill 1.3 / 2.3 / 3.2: an array indexed by price is O(1) with no
// pointer chasing; the bitmap answers "next non-empty level" with at most
// 1 + 16 word reads (bounded worst case, not just a good average), using
// count-leading/trailing-zero instructions instead of tree walks.
#ifndef INCLUDE_EXCH_FAST_LADDER_H_
#define INCLUDE_EXCH_FAST_LADDER_H_

#include <bit>
#include <cstdint>

#include "exch/types.h"

namespace exch::fast {

inline constexpr std::uint32_t kNil = 0;  // pool index 0 is reserved as "null"
// 65536 slots, index == price
inline constexpr std::uint32_t kLadder = kMaxPrice + 1;
inline constexpr Price kNoBid = 0;            // below every valid price
inline constexpr Price kNoAsk = 0xFFFFFFFFu;  // above every valid price

static_assert((kLadder & (kLadder - 1)) == 0 && kLadder == 65536);

// FIFO of resting orders at one price. 16 bytes: four levels per cache line,
// and neighbouring prices (the ones a sweep touches next) are adjacent.
struct Level {
  std::uint32_t head;  // oldest order (first to fill)
  std::uint32_t tail;  // newest order
  std::uint64_t qty;   // total resting quantity (market-data depth)
};
static_assert(sizeof(Level) == 16);

class LevelBitmap {
 public:
  static constexpr std::uint32_t kWords = kLadder / 64;   // 1024
  static constexpr std::uint32_t kSummary = kWords / 64;  // 16

  void Set(std::uint32_t p) noexcept {
    const std::uint32_t w = p >> 6;
    words_[w] |= 1ull << (p & 63);
    summary_[w >> 6] |= 1ull << (w & 63);
  }
  void Clear(std::uint32_t p) noexcept {
    const std::uint32_t w = p >> 6;
    words_[w] &= ~(1ull << (p & 63));
    if (words_[w] == 0) summary_[w >> 6] &= ~(1ull << (w & 63));
  }

  // Highest set index <= p, or kNoBid (0) if none. Requires p < kLadder.
  std::uint32_t HighestAtOrBelow(std::uint32_t p) const noexcept {
    std::uint32_t w = p >> 6;
    const std::uint64_t m = words_[w] & (~0ull >> (63 - (p & 63)));
    if (m) return (w << 6) | (63u - std::countl_zero(m));
    std::uint32_t s = w >> 6;
    std::uint64_t sm = summary_[s] & ((1ull << (w & 63)) - 1);
    for (;;) {
      if (sm) {
        const std::uint32_t ww = (s << 6) | (63u - std::countl_zero(sm));
        return (ww << 6) | (63u - std::countl_zero(words_[ww]));
      }
      if (s == 0) return kNoBid;
      sm = summary_[--s];
    }
  }

  // Lowest set index >= p, or kNoAsk if none.
  std::uint32_t LowestAtOrAbove(std::uint32_t p) const noexcept {
    if (p >= kLadder) [[unlikely]]
      return kNoAsk;
    std::uint32_t w = p >> 6;
    const std::uint64_t m = words_[w] & (~0ull << (p & 63));
    if (m) return (w << 6) | std::countr_zero(m);
    std::uint32_t s = w >> 6;
    std::uint64_t sm =
        (w & 63) == 63 ? 0 : summary_[s] & (~0ull << ((w & 63) + 1));
    for (;;) {
      if (sm) {
        const std::uint32_t ww = (s << 6) | std::countr_zero(sm);
        return (ww << 6) | std::countr_zero(words_[ww]);
      }
      if (++s == kSummary) return kNoAsk;
      sm = summary_[s];
    }
  }

 private:
  std::uint64_t summary_[kSummary];
  std::uint64_t words_[kWords];
};

// One instrument. Hot scalars first, on their own cache line.
struct alignas(64) Book {
  Price best_bid;  // kNoBid when empty
  Price best_ask;  // kNoAsk when empty
  alignas(64) LevelBitmap bid_map;
  LevelBitmap ask_map;
  Level bids[kLadder];
  Level asks[kLadder];
};

}  // namespace exch::fast

#endif  // INCLUDE_EXCH_FAST_LADDER_H_

// Copyright 2026 The Exchange Authors.

// Order id -> pool index. Open addressing, linear probing, backward-shift
// deletion (no tombstones, so probe lengths do not creep up over a long
// session: skill 6.8 soak behaviour). Fixed power-of-two capacity at <= 50%
// load, allocated once.
//
// Skill 1.3 warns that hash tables degrade under adversarial keys. Client
// order ids are attacker-controlled, so the multiplier is a per-instance
// random odd seed (multiply-shift hashing), making collisions unpredictable.
// A venue that assigns its own order ids could index an array directly and
// remove hashing entirely; that is noted as the next step in the README.
#ifndef INCLUDE_EXCH_FAST_ID_MAP_H_
#define INCLUDE_EXCH_FAST_ID_MAP_H_

#include <bit>
#include <cstdint>

#include "exch/fast/ladder.h"
#include "exch/types.h"

namespace exch::fast {

class IdMap {
 public:
  struct Slot {
    OrderId key;  // 0 = empty (0 is never a valid order id)
    std::uint32_t value;
    std::uint32_t pad;
  };
  static_assert(sizeof(Slot) == 16);

  static constexpr std::uint64_t CapacityFor(std::uint64_t max_live) {
    std::uint64_t c = 16;
    while (c < 2 * max_live) c <<= 1;
    return c;
  }

  void Init(Slot* table, std::uint64_t capacity, std::uint64_t seed) noexcept {
    t_ = table;
    mask_ = capacity - 1;
    shift_ = static_cast<unsigned>(64 - std::countr_zero(capacity));
    mul_ = seed | 1;
  }

  std::uint32_t Find(OrderId k) const noexcept {
    // 0 is the empty marker: never a live id
    if (k == 0) [[unlikely]]
      return kNil;
    for (std::uint64_t i = Home(k);; i = (i + 1) & mask_) {
      if (t_[i].key == k) return t_[i].value;
      if (t_[i].key == 0) return kNil;
    }
  }
  bool Contains(OrderId k) const noexcept { return Find(k) != kNil; }

  void Insert(OrderId k, std::uint32_t v) noexcept {  // k must be absent
    std::uint64_t i = Home(k);
    while (t_[i].key != 0) i = (i + 1) & mask_;
    t_[i].key = k;
    t_[i].value = v;
  }

  void Erase(OrderId k) noexcept {  // k must be present
    std::uint64_t i = Home(k);
    while (t_[i].key != k) i = (i + 1) & mask_;
    // Backward shift: pull later entries of the cluster into the hole if
    // the hole lies between their home slot and their current slot.
    for (std::uint64_t j = (i + 1) & mask_; t_[j].key != 0;
         j = (j + 1) & mask_) {
      const std::uint64_t h = Home(t_[j].key);
      if (((j - h) & mask_) >= ((j - i) & mask_)) {
        t_[i] = t_[j];
        i = j;
      }
    }
    t_[i].key = 0;
  }

 private:
  std::uint64_t Home(OrderId k) const noexcept { return (k * mul_) >> shift_; }

  Slot* t_ = nullptr;
  std::uint64_t mask_ = 0;
  std::uint64_t mul_ = 1;
  unsigned shift_ = 64;
};

}  // namespace exch::fast

#endif  // INCLUDE_EXCH_FAST_ID_MAP_H_

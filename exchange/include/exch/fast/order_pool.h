// Copyright 2026 The Exchange Authors.

// Fixed-capacity order pool with an intrusive free list (skill 2.2).
// Orders are addressed by 32-bit index, not pointer: half the size of a
// pointer, so prev/next links fit an Order in 32 bytes (two per cache line).
// Slots have stable identity; nothing moves after start-up.
#ifndef INCLUDE_EXCH_FAST_ORDER_POOL_H_
#define INCLUDE_EXCH_FAST_ORDER_POOL_H_

#include <cstdint>

#include "exch/fast/ladder.h"
#include "exch/types.h"

namespace exch::fast {

struct alignas(32) Order {
  OrderId id;
  Price price;
  Qty qty;
  std::uint32_t next;  // towards the tail (newer); also the free-list link
  std::uint32_t prev;  // towards the head (older)
  std::uint16_t book;
  Side side;
};
static_assert(sizeof(Order) == 32);

class OrderPool {
 public:
  // `slots` must hold capacity + 1 Orders (index 0 is the reserved null).
  void Init(Order* slots, std::uint32_t capacity) noexcept {
    slots_ = slots;
    for (std::uint32_t i = 1; i < capacity; ++i) slots_[i].next = i + 1;
    slots_[capacity].next = kNil;
    free_ = capacity ? 1 : kNil;
  }
  // Caller guarantees availability (the engine enforces max_live_orders,
  // which equals the capacity), so there is no failure branch here.
  std::uint32_t Acquire() noexcept {
    const std::uint32_t i = free_;
    free_ = slots_[i].next;
    return i;
  }
  // LIFO reuse: the most recently freed slot is the one most likely in cache.
  void Release(std::uint32_t i) noexcept {
    slots_[i].next = free_;
    free_ = i;
  }
  Order& operator[](std::uint32_t i) noexcept { return slots_[i]; }
  const Order& operator[](std::uint32_t i) const noexcept { return slots_[i]; }

 private:
  Order* slots_ = nullptr;
  std::uint32_t free_ = kNil;
};

}  // namespace exch::fast

#endif  // INCLUDE_EXCH_FAST_ORDER_POOL_H_

// Copyright 2026 The Exchange Authors.

// Bounded single-producer / single-consumer ring (skill 4.6), with the
// "further step" applied: each side caches the other side's index and only
// reloads the shared atomic when the cached value says full/empty, so the
// common case touches no cache line the other core is writing.
#ifndef BENCH_SPSC_H_
#define BENCH_SPSC_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>

namespace bench {

// x86; see std::hardware_destructive_interference_size
inline constexpr std::size_t kLine = 64;

template <class T, std::size_t N>
class Spsc {
  static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

 public:
  // Producer side. Returns false when full; the caller chooses the
  // backpressure policy (for orders: spin, never drop).
  template <class Fill>
  bool TryPush(Fill&& fill) noexcept {
    const std::size_t t = tail_.load(std::memory_order_relaxed);  // own index
    if (t - head_cache_ == N) {
      // pairs with consumer's release of head_
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t - head_cache_ == N) return false;
    }
    fill(buf_[t & (N - 1)]);
    // Publishes the slot; pairs with the consumer's acquire of tail_.
    tail_.store(t + 1, std::memory_order_release);
    return true;
  }

  // Consumer side. Calls `use` on the front slot in place (no copy out).
  template <class Use>
  bool TryPop(Use&& use) noexcept {
    const std::size_t h = head_.load(std::memory_order_relaxed);  // own index
    if (h == tail_cache_) {
      // pairs with producer's release of tail_
      tail_cache_ = tail_.load(std::memory_order_acquire);
      if (h == tail_cache_) return false;
    }
    use(buf_[h & (N - 1)]);
    // Frees the slot; pairs with the producer's acquire of head_.
    head_.store(h + 1, std::memory_order_release);
    return true;
  }

 private:
  // Producer-owned line, consumer-owned line, then the slots, so the two
  // threads never write the same cache line (no false sharing) [2.5].
  alignas(kLine) std::atomic<std::size_t> tail_{0};
  std::size_t head_cache_ = 0;
  alignas(kLine) std::atomic<std::size_t> head_{0};
  std::size_t tail_cache_ = 0;
  alignas(kLine) T buf_[N];
};

// Variant (additional practice, not from the skill): each slot carries its own
// sequence number, so the consumer polls the slot line itself. When the slot
// becomes visible its payload arrives in the same cache-line transfer,
// instead of one miss for tail_ and a second for the slot. T must expose
// `std::atomic<std::uint64_t> seq` (0-initialised).
template <class T, std::size_t N>
class SeqRing {
  static_assert((N & (N - 1)) == 0, "capacity must be a power of two");

 public:
  template <class Fill>
  bool TryPush(Fill&& fill) noexcept {
    const std::size_t t = tail_;
    if (t - head_cache_ == N) {
      // pairs with consumer's release of head_
      head_cache_ = head_.load(std::memory_order_acquire);
      if (t - head_cache_ == N) return false;
    }
    T& s = buf_[t & (N - 1)];
    fill(s);
    // Publishes the payload; pairs with the consumer's acquire of seq.
    s.seq.store(t + 1, std::memory_order_release);
    tail_ = t + 1;
    return true;
  }
  template <class Use>
  bool TryPop(Use&& use) noexcept {
    const std::size_t h = head_local_;
    T& s = buf_[h & (N - 1)];
    // Pairs with the producer's release store of seq.
    if (s.seq.load(std::memory_order_acquire) != h + 1) return false;
    use(s);
    head_local_ = h + 1;
    // Publish progress to the producer only every 64 slots: it only needs
    // it to detect "full", and fewer stores mean less line ping-pong.
    if ((head_local_ & 63) == 0)
      head_.store(head_local_, std::memory_order_release);
    return true;
  }

 private:
  alignas(kLine) std::size_t tail_ = 0;  // producer-private
  std::size_t head_cache_ = 0;
  alignas(kLine) std::atomic<std::size_t> head_{0};
  alignas(kLine) std::size_t head_local_ = 0;  // consumer-private
  alignas(kLine) T buf_[N];
};

}  // namespace bench

#endif  // BENCH_SPSC_H_

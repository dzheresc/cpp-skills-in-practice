// Copyright 2026 The Exchange Authors.

// Start-up memory: everything the engine will ever touch is reserved, mapped
// and pre-faulted before the first message (skill 2.1, 2.7, 4.10). The hot
// path never calls the allocator and never takes a first-touch page fault.
#ifndef INCLUDE_EXCH_FAST_MEMORY_H_
#define INCLUDE_EXCH_FAST_MEMORY_H_

#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <new>

namespace exch::fast {

struct MemoryOptions {
  bool huge_pages = false;  // madvise(MADV_HUGEPAGE): adopt only on measured
                            // TLB pressure (3.6)
};

// A fixed-size, page-aligned, pre-touched block owned by one engine.
class Region {
 public:
  Region() = default;
  Region(std::size_t bytes, MemoryOptions opt) { Allocate(bytes, opt); }
  Region(const Region&) = delete;
  Region& operator=(const Region&) = delete;
  Region(Region&& o) noexcept : p_(o.p_), n_(o.n_) {
    o.p_ = nullptr;
    o.n_ = 0;
  }
  ~Region() {
    if (p_) ::munmap(p_, n_);
  }  // teardown is off the hot path

  void Allocate(std::size_t bytes, MemoryOptions opt) {
    constexpr std::size_t kHuge = 2u << 20;
    n_ = (bytes + kHuge - 1) & ~(kHuge - 1);
    void* p = ::mmap(nullptr, n_, PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    // start-up only: exceptions are fine here
    if (p == MAP_FAILED) throw std::bad_alloc();
    if (opt.huge_pages) ::madvise(p, n_, MADV_HUGEPAGE);
    // Pre-touch: write every page now so no first-touch fault happens later.
    std::memset(p, 0, n_);
    p_ = static_cast<std::byte*>(p);
  }

  std::byte* data() const noexcept { return p_; }
  std::size_t size() const noexcept { return n_; }

 private:
  std::byte* p_ = nullptr;
  std::size_t n_ = 0;
};

// Bump allocator over a Region, used only during construction.
class Carver {
 public:
  explicit Carver(Region& r) : base_(r.data()), cap_(r.size()) {}
  template <class T>
  T* Take(std::size_t count, std::size_t align = 64) {
    off_ = (off_ + align - 1) & ~(align - 1);
    if (off_ + sizeof(T) * count > cap_) throw std::bad_alloc();
    T* p = reinterpret_cast<T*>(base_ + off_);
    off_ += sizeof(T) * count;
    return p;  // memory is zeroed; T must be implicit-lifetime / zero-valid
  }
  static constexpr std::size_t RoundUp(std::size_t bytes,
                                       std::size_t align = 64) {
    return (bytes + align - 1) & ~(align - 1);
  }

 private:
  std::byte* base_;
  std::size_t cap_;
  std::size_t off_ = 0;
};

// Lock all current and future pages in RAM (needs RLIMIT_MEMLOCK or
// CAP_IPC_LOCK). Returns false if the OS refused; callers report it rather than
// failing.
inline bool LockAllMemory() noexcept {
  return ::mlockall(MCL_CURRENT | MCL_FUTURE) == 0;
}

}  // namespace exch::fast

#endif  // INCLUDE_EXCH_FAST_MEMORY_H_

// Copyright 2026 The Exchange Authors.

// Measurement helpers. Skill 6.7 / 6 additional practice:
//  - rdtsc with lfence fencing on an invariant TSC, calibrated against
//    steady_clock, for per-message timing (steady_clock::now() costs ~20 ns,
//    comparable to what we measure);
//  - every sample stored in a preallocated array, pre-touched, sorted only
//    after the run (no histogram work on the hot path);
//  - report the distribution (P50..P99.99, max), never just the mean.
#ifndef BENCH_BENCH_UTIL_H_
#define BENCH_BENCH_UTIL_H_

#include <pthread.h>
#include <sched.h>
#include <x86intrin.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "exch/fast/engine.h"
#include "exch/naive/exchange.h"
#include "exch/types.h"

namespace bench {

inline std::uint64_t ReadTsc() noexcept {
  _mm_lfence();
  const std::uint64_t t = __rdtsc();
  _mm_lfence();
  return t;
}

inline double TscTicksPerNs() {
  using clk = std::chrono::steady_clock;
  const auto c0 = clk::now();
  const auto t0 = ReadTsc();
  while (clk::now() - c0 < std::chrono::milliseconds(200)) {
  }
  const auto c1 = clk::now();
  const auto t1 = ReadTsc();
  return static_cast<double>(t1 - t0) /
         static_cast<double>(
             std::chrono::duration_cast<std::chrono::nanoseconds>(c1 - c0)
                 .count());
}

inline bool PinToCpu(int cpu) {
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  return pthread_setaffinity_np(pthread_self(), sizeof set, &set) == 0;
}

// Cheap, order-sensitive digest of an event stream so two engines' outputs
// can be compared on multi-million message runs without storing them.
class Digest {
 public:
  void Add(const exch::Event& e) noexcept {
    std::uint64_t w[4];
    std::memcpy(w, &e, sizeof w);
    for (auto x : w) hash_ = (hash_ ^ x) * 0x100000001b3ull;
    ++count_;
  }
  std::uint64_t hash() const { return hash_; }
  std::uint64_t count() const { return count_; }

 private:
  std::uint64_t hash_ = 0xcbf29ce484222325ull;
  std::uint64_t count_ = 0;
};

// Event sinks for the two engines: static (fast) and virtual (naive).
class DigestSink {
 public:
  void OnEvent(const exch::Event& e) noexcept { digest_.Add(e); }
  const Digest& digest() const { return digest_; }

 private:
  Digest digest_;
};

class DigestListener final : public exch::naive::Listener {
 public:
  void OnEvent(const exch::Event& e) override { digest_.Add(e); }
  const Digest& digest() const { return digest_; }

 private:
  Digest digest_;
};

// An engine bundled with its digest sink, behind one OnMessage() call, so
// benchmark loops are written once for both engines.
class NaiveRig {
 public:
  NaiveRig(const exch::ExchangeConfig& c, exch::fast::MemoryOptions /*unused*/)
      : exchange_(c, sink_) {}
  void OnMessage(std::span<const std::byte> m) { exchange_.OnMessage(m); }
  const Digest& digest() const { return sink_.digest(); }

 private:
  DigestListener sink_;
  exch::naive::Exchange exchange_;
};

class FastRig {
 public:
  FastRig(const exch::ExchangeConfig& c, exch::fast::MemoryOptions mem)
      : engine_(c, sink_, mem) {}
  void OnMessage(std::span<const std::byte> m) { engine_.OnMessage(m); }
  const Digest& digest() const { return sink_.digest(); }

 private:
  DigestSink sink_;
  exch::fast::Engine<DigestSink> engine_;
};

struct Summary {
  double p50, p90, p99, p999, p9999, max, mean;
  std::size_t n;
};

inline Summary Summarise(std::vector<std::uint64_t> s, double per_ns) {
  Summary r{};
  r.n = s.size();
  if (s.empty()) return r;
  std::sort(s.begin(), s.end());
  auto q = [&](double p) {
    const auto i =
        std::min(s.size() - 1,
                 static_cast<std::size_t>(p * static_cast<double>(s.size())));
    return static_cast<double>(s[i]) / per_ns;
  };
  double sum = 0;
  for (auto x : s) sum += static_cast<double>(x);
  r.p50 = q(0.50);
  r.p90 = q(0.90);
  r.p99 = q(0.99);
  r.p999 = q(0.999);
  r.p9999 = q(0.9999);
  r.max = static_cast<double>(s.back()) / per_ns;
  r.mean = sum / static_cast<double>(s.size()) / per_ns;
  return r;
}

inline void PrintHeader() {
  std::printf("%-26s %9s %8s %8s %8s %8s %9s %10s %8s\n", "series", "n", "p50",
              "p90", "p99", "p99.9", "p99.99", "max", "mean");
}
inline void PrintRow(const std::string& name, const Summary& s,
                     const char* unit = "ns") {
  std::printf("%-26s %9zu %8.0f %8.0f %8.0f %8.0f %9.0f %10.0f %8.1f  (%s)\n",
              name.c_str(), s.n, s.p50, s.p90, s.p99, s.p999, s.p9999, s.max,
              s.mean, unit);
}

inline const char* GetArg(int argc, char** argv, const char* key,
                          const char* def) {
  for (int i = 1; i + 1 < argc; ++i)
    if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
  return def;
}
inline bool HasFlag(int argc, char** argv, const char* key) {
  for (int i = 1; i < argc; ++i)
    if (std::strcmp(argv[i], key) == 0) return true;
  return false;
}

}  // namespace bench

#endif  // BENCH_BENCH_UTIL_H_

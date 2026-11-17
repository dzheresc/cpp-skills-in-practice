// Copyright 2026 The Exchange Authors.

// Platform jitter baseline ("hiccup meter", in the spirit of cyclictest,
// skill 4.11): spin on the TSC and record every gap between consecutive
// reads. Gaps are time the thread was not running (interrupts, preemption,
// hypervisor steal). Whatever this shows at P99.99/max is a floor no engine
// can beat on this machine, and it explains the far tail of the other runs.
//
//   bench_jitter [--seconds S] [--pin CPU]
#include <cinttypes>
#include <cstdio>
#include <vector>

#include "bench/bench_util.h"

int main(int argc, char** argv) {
  const double seconds = std::stod(bench::GetArg(argc, argv, "--seconds", "5"));
  const int cpu = std::stoi(bench::GetArg(argc, argv, "--pin", "-1"));
  if (cpu >= 0) bench::PinToCpu(cpu);
  const double per_ns = bench::TscTicksPerNs();

  std::vector<std::uint64_t> gaps_over_1us;
  gaps_over_1us.reserve(1 << 20);
  // >1us, >5us, >10us, >20us, >50us, >100us, >1ms, >10ms
  std::uint64_t hist[8] = {};
  const double edges[8] = {1e3, 5e3, 1e4, 2e4, 5e4, 1e5, 1e6, 1e7};

  const auto end =
      bench::ReadTsc() + static_cast<std::uint64_t>(seconds * 1e9 * per_ns);
  std::uint64_t prev = bench::ReadTsc(), loops = 0, stolen = 0;
  while (prev < end) {
    const auto now = bench::ReadTsc();
    const auto gap = now - prev;
    const double ns = static_cast<double>(gap) / per_ns;
    if (ns > 1e3) {
      stolen += gap;
      for (int b = 0; b < 8; ++b)
        if (ns > edges[b]) ++hist[b];
      if (gaps_over_1us.size() < gaps_over_1us.capacity())
        gaps_over_1us.push_back(gap);
    }
    prev = now;
    ++loops;
  }
  std::printf("spun %.1f s (%" PRIu64
              " reads), time lost to gaps > 1us: %.3f%%\n",
              seconds, loops,
              100.0 * static_cast<double>(stolen) / (seconds * 1e9 * per_ns));
  const char* names[8] = {">1us",  ">5us",   ">10us", ">20us",
                          ">50us", ">100us", ">1ms",  ">10ms"};
  for (int b = 0; b < 8; ++b)
    std::printf("  gaps %-7s %8" PRIu64 "  (%.1f/s)\n", names[b], hist[b],
                static_cast<double>(hist[b]) / seconds);
  if (!gaps_over_1us.empty()) {
    auto s = bench::Summarise(gaps_over_1us, per_ns);
    std::printf("  gap size: p50 %.0f ns, p99 %.0f ns, max %.0f ns\n", s.p50,
                s.p99, s.max);
  }
  return 0;
}

// Copyright 2026 The Exchange Authors.

// Single-thread component benchmark: decode + match, one message at a time,
// for the naive and the optimised engine on the identical pre-generated
// stream. Reports throughput and the per-message latency distribution
// (overall and by message type), plus an output digest that must match
// between engines.
//
//   bench_engine --engine naive|fast [--messages N] [--warmup W] [--live L]
//                [--max-live M] [--hugepages] [--mlock]
#include <sys/mman.h>
#include <sys/resource.h>

#include <cinttypes>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "bench/bench_util.h"
#include "exch/fast/engine.h"
#include "exch/naive/exchange.h"
#include "exch/workload.h"

namespace exch {

namespace {

std::int64_t PageFaults() {
  rusage u{};
  getrusage(RUSAGE_SELF, &u);
  return static_cast<std::int64_t>(u.ru_minflt + u.ru_majflt);
}

template <class Rig>
void Run(const char* name, const Workload& w, std::size_t warmup, double per_ns,
         const ExchangeConfig& cfg, fast::MemoryOptions mem) {
  const std::size_t n = w.size() - warmup;

  // 1) throughput: no per-message timer in the loop
  double ns_per_msg;
  std::uint64_t digest, events;
  std::int64_t faults_tp;
  {
    auto rig = std::make_unique<Rig>(cfg, mem);
    for (std::size_t i = 0; i < warmup; ++i) rig->OnMessage(w.Message(i));
    const std::int64_t f0 = PageFaults();
    const auto t0 = bench::ReadTsc();
    for (std::size_t i = warmup; i < w.size(); ++i)
      rig->OnMessage(w.Message(i));
    const auto t1 = bench::ReadTsc();
    faults_tp = PageFaults() - f0;
    ns_per_msg = static_cast<double>(t1 - t0) / per_ns / static_cast<double>(n);
    digest = rig->digest().hash();
    events = rig->digest().count();
  }

  // 2) latency: every message individually timed
  // Value-initialised, so every page is touched before timing starts.
  std::vector<std::uint64_t> all(n);
  std::vector<std::uint64_t> by_type[3];
  for (auto& v : by_type) v.reserve(n);
  {
    auto rig = std::make_unique<Rig>(cfg, mem);
    for (std::size_t i = 0; i < warmup; ++i) rig->OnMessage(w.Message(i));
    for (std::size_t i = warmup; i < w.size(); ++i) {
      const auto m = w.Message(i);
      const auto t0 = bench::ReadTsc();
      rig->OnMessage(m);
      const auto t1 = bench::ReadTsc();
      all[i - warmup] = t1 - t0;
    }
    for (std::size_t i = warmup; i < w.size(); ++i) {
      const auto t = static_cast<char>(w.Message(i)[0]);
      by_type[t == 'N' ? 0 : t == 'X' ? 1 : 2].push_back(all[i - warmup]);
    }
  }

  std::printf("\n== %s ==\n", name);
  std::printf(
      "throughput: %.1f ns/msg mean, %.2f M msg/s; page faults during timed "
      "run: %" PRId64 "\n",
      ns_per_msg, 1e3 / ns_per_msg, faults_tp);
  std::printf("output digest: %016" PRIx64 " over %" PRIu64 " events\n", digest,
              events);
  bench::PrintHeader();
  bench::PrintRow(std::string(name) + " all", bench::Summarise(all, per_ns));
  bench::PrintRow(std::string(name) + " new",
                  bench::Summarise(by_type[0], per_ns));
  bench::PrintRow(std::string(name) + " cancel",
                  bench::Summarise(by_type[1], per_ns));
  bench::PrintRow(std::string(name) + " modify",
                  bench::Summarise(by_type[2], per_ns));
}

}  // namespace

int Main(int argc, char** argv) {
  const std::string engine = bench::GetArg(argc, argv, "--engine", "fast");
  const std::size_t messages =
      std::stoull(bench::GetArg(argc, argv, "--messages", "5000000"));
  const std::size_t warmup =
      std::stoull(bench::GetArg(argc, argv, "--warmup", "1000000"));
  const std::size_t live =
      std::stoull(bench::GetArg(argc, argv, "--live", "40000"));
  const std::size_t max_live = std::stoull(bench::GetArg(
      argc, argv, "--max-live", std::to_string(2 * live).c_str()));
  const bool huge = bench::HasFlag(argc, argv, "--hugepages");

  if (bench::HasFlag(argc, argv, "--mlock"))
    std::printf("mlockall: %s\n",
                fast::LockAllMemory()
                    ? "ok"
                    : "refused (RLIMIT_MEMLOCK); continuing unlocked");

  ExchangeConfig cfg;
  cfg.symbols = DefaultSymbols(8);
  cfg.max_live_orders = max_live;

  WorkloadParams p;
  p.messages = warmup + messages;
  p.target_live = live;
  // generated before timing, same seed for both engines
  const Workload w = Generate(p, cfg.symbols);

  const double per_ns = bench::TscTicksPerNs();
  std::printf(
      "workload: %zu warm-up + %zu measured messages, target live %zu, max "
      "live %zu, TSC %.3f GHz\n",
      warmup, messages, live, max_live, per_ns);

  const fast::MemoryOptions mem{huge};
  if (engine == "naive") {
    Run<bench::NaiveRig>("naive", w, warmup, per_ns, cfg, mem);
  } else {
    Run<bench::FastRig>("fast", w, warmup, per_ns, cfg, mem);
  }
  return 0;
}

}  // namespace exch

int main(int argc, char** argv) { return exch::Main(argc, argv); }

// Copyright 2026 The Exchange Authors.

// End-to-end (in-process) benchmark: a gateway thread receives orders on an
// open-loop schedule and hands them to the matching thread; latency is
// measured from each message's *intended* arrival time to the moment the
// engine has finished it (skill 6.7: avoids coordinated omission, so a
// stalled engine shows up as queueing delay instead of being hidden).
//
//   bench_pipeline --engine naive|fast --queue mutex|spsc [--pin]
//                  [--rate MSGS_PER_SEC] [--messages N] [--warmup W]
//
// "naive" pipeline = naive engine + mutex/condvar queue of heap packets, no
// pinning. "fast"  pipeline = fast engine + SPSC ring of fixed slots,
// busy-poll, pinned threads. Mixed combinations are allowed so each change can
// be measured on its own.
#include <cinttypes>
#include <condition_variable>
#include <cstdio>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "bench/bench_util.h"
#include "bench/spsc.h"
#include "exch/fast/engine.h"
#include "exch/naive/exchange.h"
#include "exch/workload.h"

namespace exch {

namespace {

int g_cpu_gateway = 1, g_cpu_engine = 2;

struct Packet {  // what the naive gateway enqueues
  std::uint64_t intended;
  std::vector<std::byte> bytes;  // heap allocation per message
};

class MutexQueue {  // textbook blocking queue
 public:
  void Push(Packet p) {
    {
      std::lock_guard lk(m_);
      q_.push_back(std::move(p));
    }
    cv_.notify_one();
  }
  Packet Pop() {
    std::unique_lock lk(m_);
    cv_.wait(lk, [&] { return !q_.empty(); });
    Packet p = std::move(q_.front());
    q_.pop_front();
    return p;
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  std::deque<Packet> q_;
};

// One cache line per message, so producer and consumer never share a line.
struct alignas(64) Slot {
  std::atomic<std::uint64_t> seq;  // used only by SeqRing
  std::uint64_t intended;
  std::uint32_t len;
  std::byte data[44];
};
static_assert(sizeof(Slot) == 64);

template <class Ring, class Engine>
void RunRing(Engine& eng, const Workload& w, double rate, bool pin,
             double per_ns, std::vector<std::uint64_t>& lat);

template <class Engine>
void RunPipeline(Engine& eng, const Workload& w, double rate,
                 const std::string& queue, bool pin, double per_ns,
                 std::vector<std::uint64_t>& lat) {
  if (queue == "spsc")
    return RunRing<bench::Spsc<Slot, 1 << 16>>(eng, w, rate, pin, per_ns, lat);
  if (queue == "seq")
    return RunRing<bench::SeqRing<Slot, 1 << 16>>(eng, w, rate, pin, per_ns,
                                                  lat);
  const std::size_t n = w.size();
  const double period = per_ns * 1e9 / rate;
  std::uint64_t start = 0;
  auto schedule = [&](std::size_t i) {
    return start + static_cast<std::uint64_t>(static_cast<double>(i) * period);
  };
  MutexQueue q;
  std::thread consumer([&] {
    if (pin) bench::PinToCpu(g_cpu_engine);
    for (std::size_t i = 0; i < n; ++i) {
      Packet p = q.Pop();
      eng.OnMessage(p.bytes);
      lat[i] = bench::ReadTsc() - p.intended;
    }
  });
  if (pin) bench::PinToCpu(g_cpu_gateway);
  start = bench::ReadTsc() + static_cast<std::uint64_t>(per_ns * 1e6);
  for (std::size_t i = 0; i < n; ++i) {
    const auto t = schedule(i);
    while (bench::ReadTsc() < t) {
    }
    const auto m = w.Message(i);
    q.Push(Packet{t, std::vector<std::byte>(m.begin(), m.end())});
  }
  consumer.join();
}

template <class Ring, class Engine>
void RunRing(Engine& eng, const Workload& w, double rate, bool pin,
             double per_ns, std::vector<std::uint64_t>& lat) {
  const std::size_t n = w.size();
  // TSC ticks between intended arrivals
  const double period = per_ns * 1e9 / rate;
  std::atomic<bool> go{false};
  std::uint64_t start = 0;

  auto schedule = [&](std::size_t i) {
    return start + static_cast<std::uint64_t>(static_cast<double>(i) * period);
  };

  auto ring = std::make_unique<Ring>();
  std::thread consumer([&] {
    if (pin) bench::PinToCpu(g_cpu_engine);
    while (!go.load(std::memory_order_acquire)) _mm_pause();
    for (std::size_t i = 0; i < n;) {
      const bool got = ring->TryPop([&](Slot& s) {
        eng.OnMessage({s.data, s.len});
        lat[i] = bench::ReadTsc() - s.intended;
      });
      // Busy-poll: burns the core, but there is no wake-up latency [5.5].
      if (got) {
        ++i;
      } else {
        _mm_pause();
      }
    }
  });
  if (pin) bench::PinToCpu(g_cpu_gateway);
  start = bench::ReadTsc() + static_cast<std::uint64_t>(per_ns * 1e6);
  go.store(true, std::memory_order_release);
  for (std::size_t i = 0; i < n; ++i) {
    const auto t = schedule(i);
    while (bench::ReadTsc() < t) _mm_pause();
    const auto m = w.Message(i);
    while (!ring->TryPush([&](Slot& s) {
      s.intended = t;
      s.len = static_cast<std::uint32_t>(m.size());
      std::memcpy(s.data, m.data(), m.size());
    })) {
      _mm_pause();  // Backpressure: orders are never dropped, so wait [4.6].
    }
  }
  consumer.join();
}

}  // namespace

int Main(int argc, char** argv) {
  const std::string engine = bench::GetArg(argc, argv, "--engine", "fast");
  const std::string queue =
      bench::GetArg(argc, argv, "--queue", engine == "fast" ? "spsc" : "mutex");
  const bool pin = bench::HasFlag(argc, argv, "--pin");
  const double rate = std::stod(bench::GetArg(argc, argv, "--rate", "1000000"));
  const std::size_t messages =
      std::stoull(bench::GetArg(argc, argv, "--messages", "3000000"));
  const std::size_t warmup =
      std::stoull(bench::GetArg(argc, argv, "--warmup", "500000"));
  g_cpu_gateway = std::stoi(bench::GetArg(argc, argv, "--gateway-cpu", "1"));
  g_cpu_engine = std::stoi(bench::GetArg(argc, argv, "--engine-cpu", "2"));

  ExchangeConfig cfg;
  cfg.symbols = DefaultSymbols(8);
  cfg.max_live_orders = 80'000;
  WorkloadParams p;
  p.messages = warmup + messages;
  p.target_live = 40'000;
  const Workload w = Generate(p, cfg.symbols);

  const double per_ns = bench::TscTicksPerNs();
  // Value-initialised, so every page is touched before timing starts.
  std::vector<std::uint64_t> lat(w.size());
  std::uint64_t digest = 0;

  if (engine == "none") {
    // Transport only: measures the hand-off itself.
    class NullRig {
     public:
      void OnMessage(std::span<const std::byte> /*m*/) {}
    } rig;
    RunPipeline(rig, w, rate, queue, pin, per_ns, lat);
  } else if (engine == "naive") {
    bench::NaiveRig rig(cfg, {});
    RunPipeline(rig, w, rate, queue, pin, per_ns, lat);
    digest = rig.digest().hash();
  } else {
    bench::FastRig rig(cfg, {});
    RunPipeline(rig, w, rate, queue, pin, per_ns, lat);
    digest = rig.digest().hash();
  }

  std::vector<std::uint64_t> measured(
      lat.begin() + static_cast<std::ptrdiff_t>(warmup), lat.end());
  const std::string name = engine + "+" + queue + (pin ? "+pin" : "") + " @" +
                           std::to_string(static_cast<int>(rate / 1000)) +
                           "k/s";
  std::printf("digest %016" PRIx64 "\n", digest);
  bench::PrintHeader();
  bench::PrintRow(name, bench::Summarise(std::move(measured), per_ns));
  return 0;
}

}  // namespace exch

int main(int argc, char** argv) { return exch::Main(argc, argv); }

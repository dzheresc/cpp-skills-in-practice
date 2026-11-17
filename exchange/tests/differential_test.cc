// Copyright 2026 The Exchange Authors.

// Differential test: feed identical message streams to the naive and the
// optimised engine and require identical event streams and identical books.
// Two input sources: the benchmark workload generator (realistic flow) and a
// fully random fuzzer (any price in the collar, tiny books, duplicate ids,
// garbage bytes) that hits corners the realistic flow never reaches.
#include <cinttypes>
#include <cstdio>
#include <random>
#include <vector>

#include "exch/workload.h"
#include "tests/harness.h"

namespace exch {

namespace {

bool CompareEngines(const char* what, const ExchangeConfig& cfg,
                    const Workload& w) {
  test::NaiveHarness a(cfg);
  test::FastHarness b(cfg);
  for (std::size_t i = 0; i < w.size(); ++i) {
    a.events().clear();
    b.events().clear();
    a.Raw(w.Message(i));
    b.Raw(w.Message(i));
    if (a.events() != b.events()) {
      std::fprintf(stderr,
                   "%s: event mismatch at message %zu (%zu vs %zu events)\n",
                   what, i, a.events().size(), b.events().size());
      auto dump = [&](const char* who, const std::vector<Event>& ev) {
        for (auto& e : ev)
          std::fprintf(stderr,
                       "  %s type=%d reason=%d sym=%u id=%" PRIu64
                       " other=%" PRIu64 " px=%u qty=%u\n",
                       who, static_cast<int>(e.type),
                       static_cast<int>(e.reason),
                       static_cast<unsigned>(e.symbol), e.order_id, e.other_id,
                       e.price, e.qty);
      };
      auto m = w.Message(i);
      std::fprintf(stderr, "  msg type=%c len=%zu\n",
                   m.empty() ? '-' : static_cast<char>(m[0]), m.size());
      dump("naive", a.events());
      dump("fast ", b.events());
      return false;
    }
    if ((i % 4096) == 0 || i + 1 == w.size()) {
      if (a.Snapshot() != b.Snapshot() || a.live() != b.live()) {
        std::fprintf(stderr, "%s: book mismatch after message %zu\n", what, i);
        return false;
      }
    }
  }
  return true;
}

Workload Fuzz(std::uint64_t seed, std::size_t n, Price lo, Price hi,
              OrderId id_space) {
  std::mt19937_64 rng(seed);
  auto r = [&](std::uint64_t a, std::uint64_t b) {
    return std::uniform_int_distribution<std::uint64_t>(a, b)(rng);
  };
  const SymbolCode syms[] = {PackSymbol("AAA"), PackSymbol("BBB"),
                             PackSymbol("CCC")};
  Workload w;
  for (std::size_t i = 0; i < n; ++i) {
    const auto k = r(0, 99);
    const OrderId id = r(0, id_space);  // includes 0 and lots of duplicates
    if (k < 55) {
      Price px = static_cast<Price>(r(lo, hi));
      if (r(0, 200) == 0)
        px = static_cast<Price>(r(0, 1) ? 0 : kMaxPrice + r(1, 10));
      const Qty q = r(0, 300) == 0 ? 0 : static_cast<Qty>(r(1, 500));
      const Side side = r(0, 400) == 0 ? static_cast<Side>('Z')
                                       : (r(0, 1) ? Side::kBuy : Side::kSell);
      const Tif tif = r(0, 3) == 0 ? Tif::kIoc : Tif::kDay;
      w.Push(wire::NewOrder{syms[r(0, 2)], id, px, q, side, tif});
    } else if (k < 80) {
      w.Push(wire::CancelOrder{id});
    } else if (k < 97) {
      w.Push(wire::ModifyOrder{id, static_cast<Price>(r(lo, hi)),
                               static_cast<Qty>(r(0, 500))});
    } else {
      std::byte junk[wire::kMaxMsgSize];
      for (auto& b : junk) b = static_cast<std::byte>(r(0, 255));
      junk[0] = static_cast<std::byte>("NXM?"[r(0, 3)]);
      w.PushRaw({junk, static_cast<std::size_t>(r(0, wire::kMaxMsgSize))});
    }
  }
  return w;
}

}  // namespace

int Main() {
  int failures = 0;

  // Realistic flow, several seeds and book depths.
  for (std::uint64_t seed : {1, 2, 3}) {
    ExchangeConfig cfg;
    cfg.symbols = DefaultSymbols(8);
    // below target_live, so BookFull rejects happen too
    cfg.max_live_orders = 20'000;
    WorkloadParams p;
    p.messages = 300'000;
    p.target_live = 25'000;
    p.seed = seed;
    failures += !CompareEngines("workload", cfg, Generate(p, cfg.symbols));
  }

  // Fuzz: narrow ranges (dense crossing), word/summary boundaries, full collar.
  struct Range {
    Price lo, hi;
  };
  const Range ranges[] = {{95, 105}, {60, 70},       {4090, 4100},
                          {1, 200},  {1, kMaxPrice}, {65500, kMaxPrice}};
  std::uint64_t seed = 100;
  for (auto rg : ranges) {
    for (std::size_t max_live :
         {std::size_t{8}, std::size_t{64}, std::size_t{5000}}) {
      ExchangeConfig cfg;
      cfg.symbols = {"AAA", "BBB", "CCC"};
      cfg.max_live_orders = max_live;
      cfg.hash_seed = seed * 0x9E3779B97F4A7C15ull;
      failures += !CompareEngines(
          "fuzz", cfg, Fuzz(seed++, 100'000, rg.lo, rg.hi, max_live * 4));
    }
  }

  if (failures) {
    std::printf("differential_test: %d FAILED\n", failures);
    return 1;
  }
  std::printf(
      "differential_test: naive and fast produced identical events and books "
      "on all inputs\n");
  return 0;
}

}  // namespace exch

int main() { return exch::Main(); }

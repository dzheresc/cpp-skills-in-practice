// Copyright 2026 The Exchange Authors.

// Deterministic synthetic order flow, pre-encoded into one contiguous byte
// stream before any measurement starts. Shape (per message):
//   ~2%  invalid (unknown symbol, bad price/qty, bad version, unknown cancel)
//   rest split between passive limits near the touch, aggressive orders
//   (half IOC), cancels and modifies, with the cancel/modify share scaled so
//   the live book hovers around `target_live` orders. A per-symbol mid price
//   random-walks so the book keeps moving and levels empty and refill.
// Skill 3.3 / 6.8: benchmarks need realistic distributions (branch
// behaviour depends on them) and a book deep enough to matter.
#ifndef INCLUDE_EXCH_WORKLOAD_H_
#define INCLUDE_EXCH_WORKLOAD_H_

#include <algorithm>
#include <cstdint>
#include <random>
#include <span>
#include <string>
#include <vector>

#include "exch/protocol.h"
#include "exch/types.h"

namespace exch {

// A pre-encoded message stream: one contiguous byte buffer plus offsets.
class Workload {
 public:
  void Reserve(std::size_t messages) {
    bytes_.reserve(messages * wire::kMaxMsgSize);
    offsets_.reserve(messages + 1);
  }
  std::size_t size() const { return offsets_.size() - 1; }
  std::span<const std::byte> Message(std::size_t i) const {
    return {bytes_.data() + offsets_[i], offsets_[i + 1] - offsets_[i]};
  }
  template <class M>
  void Push(const M& m) {
    std::byte buf[wire::kMaxMsgSize];
    PushRaw({buf, wire::Encode(m, buf)});
  }
  void PushRaw(std::span<const std::byte> b) {
    bytes_.insert(bytes_.end(), b.begin(), b.end());
    offsets_.push_back(static_cast<std::uint32_t>(bytes_.size()));
  }

 private:
  std::vector<std::byte> bytes_;
  // Message i occupies [offsets_[i], offsets_[i + 1]).
  std::vector<std::uint32_t> offsets_{0};
};

struct WorkloadParams {
  std::size_t messages = 1'000'000;
  std::size_t target_live = 40'000;
  std::uint64_t seed = 42;
  // passive orders sit 1..max_offset ticks from mid
  std::uint32_t max_offset = 200;
  double invalid = 0.02;
};

inline Workload Generate(const WorkloadParams& p,
                         const std::vector<std::string>& symbols) {
  struct Live {
    OrderId id;
    std::uint16_t sym;
    Side side;
    Price price;
    Qty qty;
  };
  std::mt19937_64 rng(p.seed);
  auto uni = [&](double lo, double hi) {
    return std::uniform_real_distribution<double>(lo, hi)(rng);
  };
  auto pick = [&](std::uint64_t n) {
    return std::uniform_int_distribution<std::uint64_t>(0, n - 1)(rng);
  };
  std::geometric_distribution<std::uint32_t> geo(0.15);

  const std::size_t nsym = symbols.size();
  std::vector<SymbolCode> codes;
  for (auto& s : symbols) codes.push_back(PackSymbol(s));
  std::vector<std::int64_t> mid(nsym);
  for (std::size_t s = 0; s < nsym; ++s)
    mid[s] = 10'000 + 1'000 * static_cast<std::int64_t>(s);

  Workload w;
  w.Reserve(p.messages);
  std::vector<Live> live;
  live.reserve(p.target_live * 2);
  OrderId next_id = 1;

  auto clamp_px = [](std::int64_t px) {
    return static_cast<Price>(std::clamp<std::int64_t>(px, 1, kMaxPrice));
  };

  for (std::size_t n = 0; n < p.messages; ++n) {
    const auto s = static_cast<std::uint16_t>(pick(nsym));
    if (uni(0, 1) < 0.01)
      mid[s] = std::clamp<std::int64_t>(mid[s] + (uni(0, 1) < 0.5 ? -1 : 1),
                                        500, 60'000);

    if (uni(0, 1) < p.invalid) {
      switch (pick(5)) {
        case 0:
          w.Push(wire::NewOrder{PackSymbol("NOSUCH"), next_id++, 100, 100,
                                Side::kBuy, Tif::kDay});
          break;
        case 1:
          w.Push(wire::NewOrder{codes[s], next_id++, pick(2) ? 0u : 70'000u,
                                100, Side::kSell, Tif::kDay});
          break;
        case 2:
          w.Push(wire::NewOrder{codes[s], next_id++, clamp_px(mid[s]), 0,
                                Side::kBuy, Tif::kDay});
          break;
        case 3:
          w.Push(wire::CancelOrder{next_id + 1'000'000'000ull});
          break;
        default: {  // wrong protocol version
          std::byte buf[wire::kNewSize];
          wire::Encode(wire::NewOrder{codes[s], next_id++, 100, 100, Side::kBuy,
                                      Tif::kDay},
                       buf, 7);
          w.PushRaw(buf);
        }
      }
      continue;
    }

    const double fill = live.empty()
                            ? 0.0
                            : std::clamp(static_cast<double>(live.size()) /
                                             static_cast<double>(p.target_live),
                                         0.2, 2.0);
    const double w_pass = 0.48, w_aggr = 0.07, w_cxl = 0.37 * fill,
                 w_mod = 0.08 * fill;
    double r = uni(0, w_pass + w_aggr + w_cxl + w_mod);

    // nothing to cancel/modify yet
    if (live.empty() && r >= w_pass + w_aggr) r = 0;

    if (r < w_pass) {
      const Side side = pick(2) ? Side::kBuy : Side::kSell;
      const auto off = 1 + std::min<std::uint32_t>(geo(rng), p.max_offset - 1);
      const Price px =
          clamp_px(side == Side::kBuy ? mid[s] - off : mid[s] + off);
      const Qty q = 100 * (1 + static_cast<Qty>(pick(10)));
      w.Push(wire::NewOrder{codes[s], next_id, px, q, side, Tif::kDay});
      live.push_back({next_id, s, side, px, q});
      ++next_id;
    } else if (r < w_pass + w_aggr) {
      const Side side = pick(2) ? Side::kBuy : Side::kSell;
      const auto off = static_cast<std::int64_t>(pick(4));
      const Price px =
          clamp_px(side == Side::kBuy ? mid[s] + off : mid[s] - off);
      const Qty q = 100 * (1 + static_cast<Qty>(pick(20)));
      const Tif tif = pick(2) ? Tif::kIoc : Tif::kDay;
      w.Push(wire::NewOrder{codes[s], next_id, px, q, side, tif});
      if (tif == Tif::kDay) live.push_back({next_id, s, side, px, q});
      ++next_id;
    } else if (r < w_pass + w_aggr + w_cxl) {
      const auto k = pick(live.size());
      w.Push(wire::CancelOrder{
          live[k].id});  // may already be filled: realistic reject
      live[k] = live.back();
      live.pop_back();
    } else {
      Live& o = live[pick(live.size())];
      if (pick(2)) {
        o.qty = std::max<Qty>(1, o.qty / 2);  // reduce: keeps priority
      } else {
        const auto d = 1 + static_cast<std::int64_t>(pick(3));
        o.price = clamp_px(o.side == Side::kBuy
                               // move away: loses priority
                               ? o.price - d
                               : o.price + d);
      }
      w.Push(wire::ModifyOrder{o.id, o.price, o.qty});
    }
  }
  return w;
}

inline std::vector<std::string> DefaultSymbols(std::size_t n = 8) {
  static const char* names[] = {"AAPL", "MSFT", "NVDA", "AMZN", "GOOG", "META",
                                "TSLA", "JPM",  "V",    "XOM",  "UNH",  "WMT",
                                "MA",   "PG",   "HD",   "KO"};
  return {names, names + std::min<std::size_t>(n, 16)};
}

}  // namespace exch

#endif  // INCLUDE_EXCH_WORKLOAD_H_

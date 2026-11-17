// Copyright 2026 The Exchange Authors.

// Optimised matching engine. Same rules and same event stream as
// exch::naive::Exchange (verified by tests/differential_test.cc).
//
// Critical path (skill 1.2):
//   bytes -> decode -> validate -> match -> rest -> emit
// Everything else (symbol table set-up, memory, snapshots for tests) is
// control plane and runs outside it.
//
// Skill references in comments look like [2.2] = SKILL.md section 2.2.
#ifndef INCLUDE_EXCH_FAST_ENGINE_H_
#define INCLUDE_EXCH_FAST_ENGINE_H_

#include <algorithm>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

#include "exch/fast/id_map.h"
#include "exch/fast/ladder.h"
#include "exch/fast/memory.h"
#include "exch/fast/order_pool.h"
#include "exch/protocol.h"
#include "exch/types.h"

namespace exch::fast {

// Sink: any type with `void OnEvent(const Event&)`. Bound at compile time,
// so emitting is a direct (usually inlined) call, not a vtable dispatch [1.4].
template <class S>
concept EventSink = requires(S& s, const Event& e) { s.OnEvent(e); };

template <EventSink Sink>
class Engine {
 public:
  Engine(const ExchangeConfig& cfg, Sink& sink, MemoryOptions mem = {})
      : sink_(sink) {
    // ---- control plane: validate config, size and pre-fault all memory ----
    if (cfg.symbols.size() > kMaxSymbols || cfg.max_live_orders == 0 ||
        cfg.max_live_orders >= 0xFFFFFFFFull)
      throw std::invalid_argument("bad exchange config");
    nsym_ = static_cast<std::uint32_t>(cfg.symbols.size());
    max_live_ = static_cast<std::uint32_t>(cfg.max_live_orders);
    for (std::uint32_t i = 0; i < kMaxSymbols; ++i)
      symbols_[i] = i < nsym_ ? PackSymbol(cfg.symbols[i]) : 0;

    const std::uint64_t map_cap = IdMap::CapacityFor(max_live_);
    const std::size_t bytes = Carver::RoundUp(sizeof(Book) * nsym_) +
                              Carver::RoundUp(sizeof(Order) * (max_live_ + 1)) +
                              Carver::RoundUp(sizeof(IdMap::Slot) * map_cap) +
                              4096;
    region_.Allocate(bytes, mem);  // [2.1][2.7][4.10] one mapping, pre-touched
    Carver c(region_);
    books_ = c.Take<Book>(nsym_);
    for (std::uint32_t i = 0; i < nsym_; ++i) {
      books_[i].best_bid = kNoBid;
      books_[i].best_ask = kNoAsk;
    }
    pool_.Init(c.Take<Order>(max_live_ + 1), max_live_);
    ids_.Init(c.Take<IdMap::Slot>(map_cap), map_cap, cfg.hash_seed);
  }

  Engine(const Engine&) = delete;
  Engine& operator=(const Engine&) = delete;

  // ---- data plane -------------------------------------------------------
  // Static dispatch on the message type [1.4]; malformed input is the
  // unlikely, out-of-line branch [1.4 additional practice].
  void OnMessage(std::span<const std::byte> m) noexcept {
    if (m.empty()) [[unlikely]]
      return Reject(RejectReason::kMalformed, 0, 0);
    switch (static_cast<wire::MsgType>(m[0])) {
      case wire::MsgType::kNew: {
        wire::NewOrder n;
        if (!wire::Decode(m, n)) [[unlikely]]
          return Reject(RejectReason::kMalformed, 0, 0);
        return OnNew(n);
      }
      case wire::MsgType::kCancel: {
        wire::CancelOrder x;
        if (!wire::Decode(m, x)) [[unlikely]]
          return Reject(RejectReason::kMalformed, 0, 0);
        return OnCancel(x);
      }
      case wire::MsgType::kModify: {
        wire::ModifyOrder x;
        if (!wire::Decode(m, x)) [[unlikely]]
          return Reject(RejectReason::kMalformed, 0, 0);
        return OnModify(x);
      }
      default:
        [[unlikely]] return Reject(RejectReason::kMalformed, 0, 0);
    }
  }

  // ---- inspection (tests / control plane only; allocates) ----------------
  std::size_t live_orders() const noexcept { return live_; }

  std::vector<BookEntry> Snapshot() const {
    std::vector<BookEntry> out;
    for (std::uint16_t s = 0; s < nsym_; ++s) {
      const Book& b = books_[s];
      for (Price p = b.best_bid; p != kNoBid;
           p = p > 1 ? b.bid_map.HighestAtOrBelow(p - 1) : kNoBid)
        for (auto i = b.bids[p].head; i != kNil; i = pool_[i].next)
          out.push_back({s, Side::kBuy, p, pool_[i].id, pool_[i].qty});
      for (Price p = b.best_ask; p != kNoAsk;
           p = b.ask_map.LowestAtOrAbove(p + 1))
        for (auto i = b.asks[p].head; i != kNil; i = pool_[i].next)
          out.push_back({s, Side::kSell, p, pool_[i].id, pool_[i].qty});
    }
    return out;
  }

 private:
  // Small, hot collection: a linear scan over 16 contiguous u64 beats
  // hashing a std::string [1.3]. One cache line pair, no allocation.
  std::uint32_t FindSymbol(SymbolCode code) const noexcept {
    for (std::uint32_t i = 0; i < nsym_; ++i)
      if (symbols_[i] == code) return i;
    return kMaxSymbols;
  }

  // Rejects are rare: keep them out of line so the hot code stays small.
  [[gnu::cold, gnu::noinline]] void Reject(RejectReason r, OrderId id,
                                           std::uint16_t sym) noexcept {
    Event e{};
    e.type = EventType::kReject;
    e.reason = r;
    e.order_id = id;
    e.symbol = sym;
    sink_.OnEvent(e);
  }

  void Emit(EventType t, std::uint16_t sym, OrderId id, OrderId other, Price px,
            Qty q) noexcept {
    Event e{};
    e.type = t;
    e.symbol = sym;
    e.order_id = id;
    e.other_id = other;
    e.price = px;
    e.qty = q;
    sink_.OnEvent(e);
  }

  // Range checks as one unsigned compare each: 0 wraps to the maximum.
  static bool ValidPrice(Price p) noexcept { return p - 1 < kMaxPrice; }
  static bool ValidQty(Qty q) noexcept { return q - 1 < kMaxQty; }

  void OnNew(const wire::NewOrder& n) noexcept {
    const std::uint32_t s = FindSymbol(n.symbol);
    if (s == kMaxSymbols) [[unlikely]]
      return Reject(RejectReason::kUnknownSymbol, n.id, 0);
    const auto sym = static_cast<std::uint16_t>(s);
    if (n.side != Side::kBuy && n.side != Side::kSell) [[unlikely]]
      return Reject(RejectReason::kBadSide, n.id, sym);
    if (n.tif != Tif::kDay && n.tif != Tif::kIoc) [[unlikely]]
      return Reject(RejectReason::kBadTif, n.id, sym);
    if (!ValidPrice(n.price)) [[unlikely]]
      return Reject(RejectReason::kBadPrice, n.id, sym);
    if (!ValidQty(n.qty)) [[unlikely]]
      return Reject(RejectReason::kBadQty, n.id, sym);
    if (n.id == 0) [[unlikely]]
      return Reject(RejectReason::kBadOrderId, n.id, sym);
    if (ids_.Contains(n.id)) [[unlikely]]
      return Reject(RejectReason::kDuplicateId, n.id, sym);
    if (live_ >= max_live_) [[unlikely]]
      return Reject(RejectReason::kBookFull, n.id, sym);

    Emit(EventType::kAck, sym, n.id, 0, n.price, n.qty);
    Book& b = books_[s];
    const Qty rem = n.side == Side::kBuy
                        ? Match<Side::kBuy>(b, sym, n.id, n.price, n.qty)
                        : Match<Side::kSell>(b, sym, n.id, n.price, n.qty);
    if (rem == 0) return;
    if (n.tif == Tif::kIoc)
      return Emit(EventType::kCancelled, sym, n.id, 0, n.price, rem);

    const std::uint32_t i = pool_.Acquire();
    Order& o = pool_[i];
    o.id = n.id;
    o.price = n.price;
    o.qty = rem;
    o.book = sym;
    o.side = n.side;
    ids_.Insert(n.id, i);
    ++live_;
    n.side == Side::kBuy ? Link<Side::kBuy>(b, i) : Link<Side::kSell>(b, i);
  }

  void OnCancel(const wire::CancelOrder& x) noexcept {
    const std::uint32_t i = ids_.Find(x.id);
    if (i == kNil) [[unlikely]]
      return Reject(RejectReason::kUnknownOrder, x.id, 0);
    Order& o = pool_[i];
    Emit(EventType::kCancelled, o.book, o.id, 0, o.price, o.qty);
    Book& b = books_[o.book];
    o.side == Side::kBuy ? Unlink<Side::kBuy>(b, i) : Unlink<Side::kSell>(b, i);
    ids_.Erase(x.id);
    pool_.Release(i);
    --live_;
  }

  void OnModify(const wire::ModifyOrder& x) noexcept {
    const std::uint32_t i = ids_.Find(x.id);
    if (i == kNil) [[unlikely]]
      return Reject(RejectReason::kUnknownOrder, x.id, 0);
    Order& o = pool_[i];
    if (!ValidPrice(x.price)) [[unlikely]]
      return Reject(RejectReason::kBadPrice, x.id, o.book);
    if (!ValidQty(x.qty)) [[unlikely]]
      return Reject(RejectReason::kBadQty, x.id, o.book);
    Emit(EventType::kModified, o.book, o.id, 0, x.price, x.qty);
    Book& b = books_[o.book];

    // Same price, not larger: reduce in place and keep time priority.
    if (x.price == o.price && x.qty <= o.qty) {
      Level& lv = (o.side == Side::kBuy ? b.bids : b.asks)[o.price];
      lv.qty -= o.qty - x.qty;
      o.qty = x.qty;
      return;
    }
    // Loses priority: take it out, re-enter as an aggressor, rest the
    // remainder at the back. The pool slot and id mapping are reused.
    const Side side = o.side;
    const std::uint16_t sym = o.book;
    side == Side::kBuy ? Unlink<Side::kBuy>(b, i) : Unlink<Side::kSell>(b, i);
    const Qty rem = side == Side::kBuy
                        ? Match<Side::kBuy>(b, sym, x.id, x.price, x.qty)
                        : Match<Side::kSell>(b, sym, x.id, x.price, x.qty);
    if (rem == 0) {
      ids_.Erase(x.id);
      pool_.Release(i);
      --live_;
      return;
    }
    Order& r = pool_[i];
    r.price = x.price;
    r.qty = rem;
    side == Side::kBuy ? Link<Side::kBuy>(b, i) : Link<Side::kSell>(b, i);
  }

  // Sweep the opposite side. The side is a template parameter, so each
  // instantiation is straight-line code with no side branches inside the
  // loop [3.3]. The "is the book empty" test is folded into the price
  // compare via sentinels (kNoAsk above all prices, kNoBid below).
  template <Side S>
  Qty Match(Book& b, std::uint16_t sym, OrderId taker, Price limit,
            Qty qty) noexcept {
    constexpr bool kBuy = S == Side::kBuy;
    Level* opp = kBuy ? b.asks : b.bids;
    LevelBitmap& map = kBuy ? b.ask_map : b.bid_map;
    Price& best = kBuy ? b.best_ask : b.best_bid;

    while (qty != 0 && (kBuy ? best <= limit : best >= limit)) {
      Level& lv = opp[best];
      std::uint32_t i = lv.head;
      while (i != kNil) {
        Order& o = pool_[i];
        const Qty f = std::min(qty, o.qty);
        Emit(EventType::kTrade, sym, taker, o.id, best, f);
        qty -= f;
        o.qty -= f;
        lv.qty -= f;
        if (o.qty != 0) break;  // taker exhausted, maker partially filled
        const std::uint32_t next = o.next;
        ids_.Erase(o.id);
        pool_.Release(i);
        --live_;
        i = next;
        if (qty == 0) break;
      }
      lv.head = i;
      if (i != kNil) {
        pool_[i].prev = kNil;
        break;  // level still has liquidity: qty == 0
      }
      lv.tail = kNil;
      map.Clear(best);
      best = kBuy ? map.LowestAtOrAbove(best + 1)
                  : (best > 1 ? map.HighestAtOrBelow(best - 1) : kNoBid);
    }
    return qty;
  }

  // Appends at the tail of the price level (time priority).
  template <Side S>
  void Link(Book& b, std::uint32_t i) noexcept {
    Order& o = pool_[i];
    Level& lv = (S == Side::kBuy ? b.bids : b.asks)[o.price];
    o.next = kNil;
    o.prev = lv.tail;
    if (lv.tail != kNil) {
      pool_[lv.tail].next = i;
    } else {
      lv.head = i;
      if constexpr (S == Side::kBuy) {
        b.bid_map.Set(o.price);
        if (o.price > b.best_bid) b.best_bid = o.price;
      } else {
        b.ask_map.Set(o.price);
        if (o.price < b.best_ask) b.best_ask = o.price;
      }
    }
    lv.tail = i;
    lv.qty += o.qty;
  }

  // O(1) removal from anywhere in the level's FIFO.
  template <Side S>
  void Unlink(Book& b, std::uint32_t i) noexcept {
    Order& o = pool_[i];
    Level& lv = (S == Side::kBuy ? b.bids : b.asks)[o.price];
    if (o.prev != kNil)
      pool_[o.prev].next = o.next;
    else
      lv.head = o.next;
    if (o.next != kNil)
      pool_[o.next].prev = o.prev;
    else
      lv.tail = o.prev;
    lv.qty -= o.qty;
    if (lv.head != kNil) return;
    if constexpr (S == Side::kBuy) {
      b.bid_map.Clear(o.price);
      if (b.best_bid == o.price)
        b.best_bid =
            o.price > 1 ? b.bid_map.HighestAtOrBelow(o.price - 1) : kNoBid;
    } else {
      b.ask_map.Clear(o.price);
      if (b.best_ask == o.price)
        b.best_ask = b.ask_map.LowestAtOrAbove(o.price + 1);
    }
  }

  // Hot members first; the big arrays live in region_.
  Sink& sink_;
  Book* books_ = nullptr;
  OrderPool pool_;
  IdMap ids_;
  std::uint32_t live_ = 0;
  std::uint32_t max_live_ = 0;
  std::uint32_t nsym_ = 0;
  SymbolCode symbols_[kMaxSymbols]{};
  Region region_;
};

}  // namespace exch::fast

#endif  // INCLUDE_EXCH_FAST_ENGINE_H_

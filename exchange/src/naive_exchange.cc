// Copyright 2026 The Exchange Authors.

#include <algorithm>
#include <list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "exch/naive/exchange.h"

namespace exch::naive {

namespace {
std::string TrimSymbol(SymbolCode code) {
  std::string s;
  for (int i = 0; i < 8; ++i) {
    const char c = static_cast<char>((code >> (8 * i)) & 0xFF);
    if (c == ' ') break;
    s.push_back(c);
  }
  return s;
}
Event RejectEvent(RejectReason r, OrderId id, std::uint16_t sym) {
  Event e;
  e.type = EventType::kReject;
  e.reason = r;
  e.order_id = id;
  e.symbol = sym;
  return e;
}
}  // namespace

Exchange::Exchange(const ExchangeConfig& cfg, Listener& listener)
    : listener_(listener),
      max_live_(cfg.max_live_orders),
      symbol_names_(cfg.symbols) {
  for (std::size_t i = 0; i < cfg.symbols.size(); ++i) {
    auto b = std::make_unique<OrderBook>();
    b->symbol = cfg.symbols[i];
    b->index = static_cast<std::uint16_t>(i);
    books_.emplace(cfg.symbols[i], std::move(b));
  }
}

void Exchange::OnMessage(std::span<const std::byte> msg) {
  std::vector<Event> events;
  try {
    if (msg.empty()) throw RejectError(RejectReason::kMalformed, 0, 0);
    switch (static_cast<wire::MsgType>(msg[0])) {
      case wire::MsgType::kNew: {
        wire::NewOrder m{};
        if (!wire::Decode(msg, m))
          throw RejectError(RejectReason::kMalformed, 0, 0);
        events = HandleNew(m);
        break;
      }
      case wire::MsgType::kCancel: {
        wire::CancelOrder m{};
        if (!wire::Decode(msg, m))
          throw RejectError(RejectReason::kMalformed, 0, 0);
        events = HandleCancel(m);
        break;
      }
      case wire::MsgType::kModify: {
        wire::ModifyOrder m{};
        if (!wire::Decode(msg, m))
          throw RejectError(RejectReason::kMalformed, 0, 0);
        events = HandleModify(m);
        break;
      }
      default:
        throw RejectError(RejectReason::kMalformed, 0, 0);
    }
  } catch (const RejectError& r) {
    events.push_back(RejectEvent(r.reason, r.order_id, r.symbol));
  }
  for (const auto& e : events) listener_.OnEvent(e);
}

std::vector<Event> Exchange::HandleNew(const wire::NewOrder& m) {
  const std::string sym = TrimSymbol(m.symbol);
  auto bit = books_.find(sym);
  if (bit == books_.end())
    throw RejectError(RejectReason::kUnknownSymbol, m.id, 0);
  OrderBook& book = *bit->second;
  const auto idx = book.index;

  if (m.side != Side::kBuy && m.side != Side::kSell)
    throw RejectError(RejectReason::kBadSide, m.id, idx);
  if (m.tif != Tif::kDay && m.tif != Tif::kIoc)
    throw RejectError(RejectReason::kBadTif, m.id, idx);
  if (m.price == 0 || m.price > kMaxPrice)
    throw RejectError(RejectReason::kBadPrice, m.id, idx);
  if (m.qty == 0 || m.qty > kMaxQty)
    throw RejectError(RejectReason::kBadQty, m.id, idx);
  if (m.id == 0) throw RejectError(RejectReason::kBadOrderId, m.id, idx);
  if (orders_.count(m.id))
    throw RejectError(RejectReason::kDuplicateId, m.id, idx);
  if (orders_.size() >= max_live_)
    throw RejectError(RejectReason::kBookFull, m.id, idx);

  std::vector<Event> out;
  Event ack;
  ack.type = EventType::kAck;
  ack.symbol = idx;
  ack.order_id = m.id;
  ack.price = m.price;
  ack.qty = m.qty;
  out.push_back(ack);

  Order order{m.id, m.side, m.price, m.qty, sym};
  Match(book, order, out);
  if (order.qty > 0) {
    if (m.tif == Tif::kIoc) {
      Event c;
      c.type = EventType::kCancelled;
      c.symbol = idx;
      c.order_id = m.id;
      c.price = m.price;
      c.qty = order.qty;
      out.push_back(c);
    } else {
      Rest(book, std::move(order));
    }
  }
  return out;
}

std::vector<Event> Exchange::HandleCancel(const wire::CancelOrder& m) {
  auto it = orders_.find(m.id);
  if (it == orders_.end())
    throw RejectError(RejectReason::kUnknownOrder, m.id, 0);
  const Locator loc = it->second;
  Event c;
  c.type = EventType::kCancelled;
  c.symbol = loc.book->index;
  c.order_id = m.id;
  c.price = loc.price;
  c.qty = loc.it->qty;
  Remove(loc);
  return {c};
}

std::vector<Event> Exchange::HandleModify(const wire::ModifyOrder& m) {
  auto it = orders_.find(m.id);
  if (it == orders_.end())
    throw RejectError(RejectReason::kUnknownOrder, m.id, 0);
  Locator loc = it->second;
  const auto idx = loc.book->index;
  if (m.price == 0 || m.price > kMaxPrice)
    throw RejectError(RejectReason::kBadPrice, m.id, idx);
  if (m.qty == 0 || m.qty > kMaxQty)
    throw RejectError(RejectReason::kBadQty, m.id, idx);

  std::vector<Event> out;
  Event mod;
  mod.type = EventType::kModified;
  mod.symbol = idx;
  mod.order_id = m.id;
  mod.price = m.price;
  mod.qty = m.qty;
  out.push_back(mod);

  if (m.price == loc.price && m.qty <= loc.it->qty) {
    loc.it->qty = m.qty;  // reduce in place: keeps time priority
    return out;
  }
  // Price change or size increase: loses priority, may trade.
  Order order = *loc.it;
  order.price = m.price;
  order.qty = m.qty;
  Remove(loc);
  Match(*loc.book, order, out);
  if (order.qty > 0) Rest(*loc.book, std::move(order));
  return out;
}

void Exchange::Match(OrderBook& book, Order& taker, std::vector<Event>& out) {
  auto sweep = [&](auto& opposite, auto crosses) {
    while (taker.qty > 0 && !opposite.empty()) {
      auto level = opposite.begin();
      if (!crosses(level->first)) break;
      auto& queue = level->second;
      while (taker.qty > 0 && !queue.empty()) {
        Order& maker = queue.front();
        const Qty fill = std::min(taker.qty, maker.qty);
        Event t;
        t.type = EventType::kTrade;
        t.symbol = book.index;
        t.order_id = taker.id;
        t.other_id = maker.id;
        t.price = maker.price;
        t.qty = fill;
        out.push_back(t);
        taker.qty -= fill;
        maker.qty -= fill;
        if (maker.qty == 0) {
          orders_.erase(maker.id);
          queue.pop_front();
        }
      }
      if (queue.empty()) opposite.erase(level);
    }
  };
  if (taker.side == Side::kBuy)
    sweep(book.asks, [&](Price p) { return p <= taker.price; });
  else
    sweep(book.bids, [&](Price p) { return p >= taker.price; });
}

void Exchange::Rest(OrderBook& book, Order order) {
  const OrderId id = order.id;
  const Side side = order.side;
  const Price price = order.price;
  std::list<Order>::iterator it;
  if (side == Side::kBuy) {
    auto& q = book.bids[price];
    q.push_back(std::move(order));
    it = std::prev(q.end());
  } else {
    auto& q = book.asks[price];
    q.push_back(std::move(order));
    it = std::prev(q.end());
  }
  orders_[id] = Locator{&book, side, price, it};
}

void Exchange::Remove(const Locator& loc) {
  const OrderId id = loc.it->id;
  if (loc.side == Side::kBuy) {
    auto lvl = loc.book->bids.find(loc.price);
    lvl->second.erase(loc.it);
    if (lvl->second.empty()) loc.book->bids.erase(lvl);
  } else {
    auto lvl = loc.book->asks.find(loc.price);
    lvl->second.erase(loc.it);
    if (lvl->second.empty()) loc.book->asks.erase(lvl);
  }
  orders_.erase(id);
}

std::vector<BookEntry> Exchange::Snapshot() const {
  std::vector<BookEntry> out;
  for (std::size_t s = 0; s < symbol_names_.size(); ++s) {
    const OrderBook& b = *books_.at(symbol_names_[s]);
    for (const auto& [px, q] : b.bids)
      for (const auto& o : q)
        out.push_back({b.index, Side::kBuy, px, o.id, o.qty});
    for (const auto& [px, q] : b.asks)
      for (const auto& o : q)
        out.push_back({b.index, Side::kSell, px, o.id, o.qty});
  }
  return out;
}

}  // namespace exch::naive

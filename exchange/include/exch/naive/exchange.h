// Copyright 2026 The Exchange Authors.

// Naive exchange: the "first version a competent developer writes".
// Idiomatic, correct, readable C++ using standard node-based containers,
// a virtual listener, exceptions for rejects and a per-message event vector.
// It is the behavioural reference and the performance baseline.
#ifndef INCLUDE_EXCH_NAIVE_EXCHANGE_H_
#define INCLUDE_EXCH_NAIVE_EXCHANGE_H_

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "exch/protocol.h"
#include "exch/types.h"

namespace exch::naive {

class Listener {
 public:
  virtual ~Listener() = default;
  virtual void OnEvent(const Event& e) = 0;
};

struct Order {
  OrderId id;
  Side side;
  Price price;
  Qty qty;
  std::string symbol;
};

struct OrderBook {
  std::string symbol;
  std::uint16_t index;
  // best (highest) first
  std::map<Price, std::list<Order>, std::greater<Price>> bids;
  std::map<Price, std::list<Order>> asks;  // best (lowest) first
};

class RejectError : public std::runtime_error {
 public:
  RejectError(RejectReason r, OrderId id, std::uint16_t sym)
      : std::runtime_error("order rejected"),
        reason(r),
        order_id(id),
        symbol(sym) {}
  RejectReason reason;
  OrderId order_id;
  std::uint16_t symbol;
};

class Exchange {
 public:
  Exchange(const ExchangeConfig& cfg, Listener& listener);

  void OnMessage(std::span<const std::byte> msg);

  std::size_t live_orders() const { return orders_.size(); }
  // every resting order, priority order
  std::vector<BookEntry> Snapshot() const;

 private:
  struct Locator {
    OrderBook* book;
    Side side;
    Price price;
    std::list<Order>::iterator it;
  };

  std::vector<Event> HandleNew(const wire::NewOrder& m);
  std::vector<Event> HandleCancel(const wire::CancelOrder& m);
  std::vector<Event> HandleModify(const wire::ModifyOrder& m);

  void Match(OrderBook& book, Order& taker, std::vector<Event>& out);
  void Rest(OrderBook& book, Order order);
  void Remove(const Locator& loc);

  std::unordered_map<std::string, std::unique_ptr<OrderBook>> books_;
  std::unordered_map<OrderId, Locator> orders_;
  Listener& listener_;
  std::size_t max_live_;
  std::vector<std::string> symbol_names_;
};

}  // namespace exch::naive

#endif  // INCLUDE_EXCH_NAIVE_EXCHANGE_H_

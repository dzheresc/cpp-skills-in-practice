// Copyright 2026 The Exchange Authors.

// Hand-written matching scenarios, run against BOTH engines.
#include <cstdio>

#include "tests/harness.h"

namespace exch {
using test::Send;
using wire::CancelOrder;
using wire::ModifyOrder;
using wire::NewOrder;

namespace {

const SymbolCode kAaa = PackSymbol("AAA");
const SymbolCode kBbb = PackSymbol("BBB");

ExchangeConfig MakeConfig(std::size_t max_live = 1000) {
  ExchangeConfig c;
  c.symbols = {"AAA", "BBB"};
  c.max_live_orders = max_live;
  return c;
}

NewOrder Buy(OrderId id, Price px, Qty q, Tif t = Tif::kDay,
             SymbolCode s = kAaa) {
  return {s, id, px, q, Side::kBuy, t};
}
NewOrder Sell(OrderId id, Price px, Qty q, Tif t = Tif::kDay,
              SymbolCode s = kAaa) {
  return {s, id, px, q, Side::kSell, t};
}

bool IsTrade(const Event& e, OrderId taker, OrderId maker, Price px, Qty q) {
  return e.type == EventType::kTrade && e.order_id == taker &&
         e.other_id == maker && e.price == px && e.qty == q;
}
bool IsReject(const Event& e, RejectReason r) {
  return e.type == EventType::kReject && e.reason == r;
}

template <class H>
void PriceTimePriority() {
  H h(MakeConfig());
  Send(h, Sell(1, 101, 100));
  Send(h, Sell(2, 100, 100));
  Send(h, Sell(3, 100, 100));  // same price as 2, later
  auto ev = Send(h, Buy(10, 101, 250));
  EXCH_EXPECT(ev.size() == 4);
  EXCH_EXPECT(ev[0].type == EventType::kAck);
  EXCH_EXPECT(IsTrade(ev[1], 10, 2, 100, 100));  // best price first
  EXCH_EXPECT(IsTrade(ev[2], 10, 3, 100, 100));  // then time
  // then next level, at maker price
  EXCH_EXPECT(IsTrade(ev[3], 10, 1, 101, 50));
  auto snap = h.Snapshot();
  EXCH_EXPECT(snap.size() == 1 && snap[0].id == 1 && snap[0].qty == 50);
  EXCH_EXPECT(h.live() == 1);
}

template <class H>
void IocAndRestingRemainder() {
  H h(MakeConfig());
  Send(h, Sell(1, 100, 100));
  auto ev = Send(h, Buy(2, 100, 300, Tif::kIoc));
  EXCH_EXPECT(ev.size() == 3);
  EXCH_EXPECT(IsTrade(ev[1], 2, 1, 100, 100));
  EXCH_EXPECT(ev[2].type == EventType::kCancelled && ev[2].qty == 200);
  EXCH_EXPECT(h.live() == 0);
  ev = Send(h, Buy(3, 99, 300));  // day order rests
  EXCH_EXPECT(ev.size() == 1 && h.live() == 1);
  ev = Send(h, Sell(4, 98, 100, Tif::kIoc));  // crosses at resting price 99
  EXCH_EXPECT(ev.size() == 2 && IsTrade(ev[1], 4, 3, 99, 100));
}

template <class H>
void CancelAndModify() {
  H h(MakeConfig());
  Send(h, Buy(1, 100, 100));
  Send(h, Buy(2, 100, 100));
  Send(h, Buy(3, 100, 100));
  // reduce keeps priority
  auto ev = Send(h, ModifyOrder{1, 100, 50});
  EXCH_EXPECT(ev.size() == 1 && ev[0].type == EventType::kModified);
  // increase loses priority
  Send(h, ModifyOrder{2, 100, 200});
  ev = Send(h, CancelOrder{3});
  EXCH_EXPECT(ev.size() == 1 && ev[0].type == EventType::kCancelled &&
              ev[0].qty == 100 && ev[0].price == 100);
  ev = Send(h, Sell(9, 100, 1000, Tif::kIoc));
  EXCH_EXPECT(ev.size() == 4);
  EXCH_EXPECT(IsTrade(ev[1], 9, 1, 100, 50));
  EXCH_EXPECT(IsTrade(ev[2], 9, 2, 100, 200));
  EXCH_EXPECT(ev[3].type == EventType::kCancelled && ev[3].qty == 750);
  // modify into a cross trades immediately
  Send(h, Sell(20, 105, 100));
  Send(h, Buy(21, 104, 100));
  ev = Send(h, ModifyOrder{21, 105, 150});
  EXCH_EXPECT(ev.size() == 2 && ev[0].type == EventType::kModified &&
              IsTrade(ev[1], 21, 20, 105, 100));
  auto snap = h.Snapshot();
  EXCH_EXPECT(snap.size() == 1 && snap[0].id == 21 && snap[0].qty == 50 &&
              snap[0].price == 105);
  // unknown order
  EXCH_EXPECT(
      IsReject(Send(h, CancelOrder{999})[0], RejectReason::kUnknownOrder));
  EXCH_EXPECT(IsReject(Send(h, ModifyOrder{999, 1, 1})[0],
                       RejectReason::kUnknownOrder));
  EXCH_EXPECT(
      IsReject(Send(h, ModifyOrder{21, 0, 1})[0], RejectReason::kBadPrice));
  EXCH_EXPECT(
      IsReject(Send(h, ModifyOrder{21, 10, 0})[0], RejectReason::kBadQty));
}

template <class H>
void Validation() {
  H h(MakeConfig(2));
  EXCH_EXPECT(IsReject(
      Send(h, NewOrder{PackSymbol("ZZZ"), 1, 100, 1, Side::kBuy, Tif::kDay})[0],
      RejectReason::kUnknownSymbol));
  EXCH_EXPECT(IsReject(
      Send(h, NewOrder{kAaa, 1, 100, 1, static_cast<Side>('Q'), Tif::kDay})[0],
      RejectReason::kBadSide));
  EXCH_EXPECT(IsReject(
      Send(h, NewOrder{kAaa, 1, 100, 1, Side::kBuy, static_cast<Tif>('9')})[0],
      RejectReason::kBadTif));
  EXCH_EXPECT(IsReject(Send(h, Buy(1, 0, 1))[0], RejectReason::kBadPrice));
  EXCH_EXPECT(
      IsReject(Send(h, Buy(1, kMaxPrice + 1, 1))[0], RejectReason::kBadPrice));
  EXCH_EXPECT(IsReject(Send(h, Buy(1, 1, 0))[0], RejectReason::kBadQty));
  EXCH_EXPECT(
      IsReject(Send(h, Buy(1, 1, kMaxQty + 1))[0], RejectReason::kBadQty));
  EXCH_EXPECT(IsReject(Send(h, Buy(0, 1, 1))[0], RejectReason::kBadOrderId));
  Send(h, Buy(1, 1, 1));
  EXCH_EXPECT(IsReject(Send(h, Buy(1, 2, 1))[0], RejectReason::kDuplicateId));
  Send(h, Buy(2, kMaxPrice, 1, Tif::kDay, kBbb));
  EXCH_EXPECT(IsReject(Send(h, Buy(3, 5, 1))[0], RejectReason::kBookFull));
  // malformed
  std::byte junk[3] = {std::byte{'N'}, std::byte{1}, std::byte{0}};
  h.events().clear();
  h.Raw(junk);
  EXCH_EXPECT(h.events().size() == 1 &&
              IsReject(h.events()[0], RejectReason::kMalformed));
  h.events().clear();
  h.Raw({});
  EXCH_EXPECT(h.events().size() == 1 &&
              IsReject(h.events()[0], RejectReason::kMalformed));
}

template <class H>
void LadderEdges() {
  // Exercise bitmap word boundaries and both ends of the price collar.
  H h(MakeConfig());
  const Price prices[] = {1,    2,    63,    64,    65,   4095,
                          4096, 4097, 30000, 65534, 65535};
  OrderId id = 1;
  for (Price p : prices) Send(h, Sell(id++, p, 10));
  auto ev = Send(h, Buy(id++, kMaxPrice, 10 * 11 + 5));
  EXCH_EXPECT(ev.size() == 1 + 11 + 0 + 0 || ev.size() == 1 + 11);
  for (std::size_t i = 0; i < 11; ++i)
    EXCH_EXPECT(IsTrade(ev[1 + i], id - 1, i + 1, prices[i], 10));
  EXCH_EXPECT(h.live() == 1);  // remainder 5 rests at 65535
  for (Price p : prices) Send(h, Buy(id++, p, 10));
  // best bid is now 65535 (5 + 10); sweep down
  ev = Send(h, Sell(id++, 1, 1000, Tif::kIoc));
  EXCH_EXPECT(ev.size() == 1 + 12 + 1);
  EXCH_EXPECT(ev[1].price == 65535 && ev[2].price == 65535 &&
              ev[12].price == 1);
  EXCH_EXPECT(h.live() == 0);
}

template <class H>
void RunAll() {
  PriceTimePriority<H>();
  IocAndRestingRemainder<H>();
  CancelAndModify<H>();
  Validation<H>();
  LadderEdges<H>();
}

}  // namespace

int Main() {
  RunAll<test::NaiveHarness>();
  RunAll<test::FastHarness>();
  if (test::g_failures) {
    std::printf("scenario_test: %d FAILED\n", test::g_failures);
    return 1;
  }
  std::printf("scenario_test: all passed (naive + fast)\n");
  return 0;
}

}  // namespace exch

int main() { return exch::Main(); }

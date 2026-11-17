// Copyright 2026 The Exchange Authors.

#ifndef TESTS_HARNESS_H_
#define TESTS_HARNESS_H_

#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

#include "exch/fast/engine.h"
#include "exch/naive/exchange.h"
#include "exch/protocol.h"

namespace test {

// Records every event an engine emits.
class Recorder {
 public:
  void OnEvent(const exch::Event& e) { events_.push_back(e); }
  std::vector<exch::Event>& events() { return events_; }

 private:
  std::vector<exch::Event> events_;
};

class NaiveRecorder final : public exch::naive::Listener {
 public:
  void OnEvent(const exch::Event& e) override { recorder_.OnEvent(e); }
  std::vector<exch::Event>& events() { return recorder_.events(); }

 private:
  Recorder recorder_;
};

// Uniform front end over both engines, so every test runs against each.
class NaiveHarness {
 public:
  explicit NaiveHarness(const exch::ExchangeConfig& c) : exchange_(c, rec_) {}
  std::vector<exch::Event>& events() { return rec_.events(); }
  void Raw(std::span<const std::byte> m) { exchange_.OnMessage(m); }
  std::vector<exch::BookEntry> Snapshot() const { return exchange_.Snapshot(); }
  std::size_t live() const { return exchange_.live_orders(); }

 private:
  NaiveRecorder rec_;
  exch::naive::Exchange exchange_;
};

class FastHarness {
 public:
  explicit FastHarness(const exch::ExchangeConfig& c) : engine_(c, rec_) {}
  std::vector<exch::Event>& events() { return rec_.events(); }
  void Raw(std::span<const std::byte> m) { engine_.OnMessage(m); }
  std::vector<exch::BookEntry> Snapshot() const { return engine_.Snapshot(); }
  std::size_t live() const { return engine_.live_orders(); }

 private:
  Recorder rec_;
  exch::fast::Engine<Recorder> engine_;
};

template <class H, class M>
std::vector<exch::Event> Send(H& h, const M& m) {
  std::byte buf[exch::wire::kMaxMsgSize];
  const auto n = exch::wire::Encode(m, buf);
  h.events().clear();
  h.Raw({buf, n});
  return h.events();
}

inline int g_failures = 0;
#define EXCH_EXPECT(cond)                                               \
  do {                                                                  \
    if (!(cond)) {                                                      \
      std::fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, \
                   __LINE__, #cond);                                    \
      ++test::g_failures;                                               \
    }                                                                   \
  } while (0)

}  // namespace test

#endif  // TESTS_HARNESS_H_

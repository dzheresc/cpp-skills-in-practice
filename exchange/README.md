# Exchange: naive vs low-latency matching engine

A small exchange (binary order-entry protocol, multi-instrument limit order
books, price-time priority matching) written twice:

* `exch::naive::Exchange` — the first version a competent C++ developer writes:
  `std::map<Price, std::list<Order>>`, `std::unordered_map`, a virtual listener,
  exceptions for rejects, a `std::vector<Event>` per message.
* `exch::fast::Engine<Sink>` — the same rules rebuilt by applying the
  [low-latency-cpp skill](https://github.com/dzheresc/low-latency-cpp)
  (`SKILL.md`). Comments cite skill sections as `[2.2]`.

Both produce **identical event streams and identical books** for any input;
the differential test checks this on realistic flow and on a fuzzer, and every
benchmark prints an output digest that must match across engines (it does).

## Exchange rules (shared by both)

| Message | Wire layout (little-endian, versioned, see `protocol.h`) |
|---|---|
| New `'N'` 28 B | side, TIF (Day / IOC), 8-char symbol, order id, price (ticks), qty |
| Cancel `'X'` 12 B | order id |
| Modify `'M'` 20 B | order id, new price, new qty |

* Price-time priority; trades print at the resting order's price.
* IOC remainder is cancelled; Day remainder rests.
* Modify: same price and qty not larger → reduce in place, keeps priority;
  otherwise loses priority, re-enters as an aggressor (may trade), rests at the back.
* Validation order: symbol, side, TIF, price (1..65535 collar), qty (1..1e6),
  id 0, duplicate live id, exchange-wide live-order limit. Malformed bytes → reject.
* Events: Ack, Reject(reason), Trade(taker, maker, px, qty), Cancelled(qty), Modified.

## Which skill drove which optimisation

| # | Naive | Optimised | Skill |
|---|---|---|---|
| 1 | `std::map<Price, std::list<Order>>` per side (tree + list nodes, pointer chasing) | Flat `Level[65536]` indexed by price + two-level occupancy bitmap; next best level found with `countl_zero`/`countr_zero` in ≤ 17 word reads (bounded worst case) | 1.3, 2.3, 3.2 |
| 2 | Heap node per order, `std::string` symbol inside each order | Fixed pool of 32-byte `Order`s (two per cache line), 32-bit indices instead of pointers, LIFO free list (hot slot reuse), no allocation after start-up | 2.1, 2.2, 2.3 |
| 3 | `std::unordered_map<OrderId, Locator>` (node-based, allocates, chained) | Open-addressing id map, ≤ 50 % load, backward-shift deletion (no tombstone creep in long sessions), seeded multiply-shift hash because client ids are attacker-controlled | 1.3 (worst case), 2 add'l practice, 6.8 (soak) |
| 4 | `unordered_map<std::string, …>` symbol lookup, builds a `std::string` per message | Linear scan over ≤ 16 packed `uint64_t` codes | 1.3 (small hot collections) |
| 5 | Virtual `Listener::OnEvent` | Sink is a template parameter constrained by a concept: direct, inlinable call | 1.4 |
| 6 | Rejects via `throw`; `std::vector<Event>` built and returned per message | Rejects are `[[unlikely]]` branches into a `[[gnu::cold, gnu::noinline]]` function; events emitted straight to the sink | 0.4, 1.4 add'l, 2.1 |
| 7 | Side handled by runtime `if` throughout | `Match<Side>`, `Link<Side>`, `Unlink<Side>` instantiated per side; "book empty" folded into the price compare via sentinels (`kNoAsk` above every price, `kNoBid` below) | 3.3 |
| 8 | Memory comes from the allocator on demand | One `mmap` region sized at start-up, pre-touched (0 page faults during the run, naive takes 2 728 on the deep book); `mlockall` available (`--mlock`); `MADV_HUGEPAGE` available but **not adopted** (no measured gain, see results) | 2.7, 4.10, 3.6 |
| 9 | — | Explicit wire format with encode/decode functions, version and size checks; shared by both engines so the comparison is about matching, not parsing | 5.3 |
| 10 | Gateway → engine via `std::mutex` + `condition_variable` + `std::deque` of heap-allocated packets | Bounded SPSC ring of 64-byte slots, acquire/release indices on separate lines, each side caches the other's index, busy-poll consumer, producer spins when full (orders are never dropped) | 4.2, 4.6, 2.5, 5.5 |
| 11 | — | `SeqRing`: sequence number inside each slot so the payload arrives with the same cache-line transfer that signals it (halves the hand-off here). *Not from the skill.* | additional |

### Measurement practice (also from the skill)

* Distributions, not means: P50 … P99.99 and max for every run [6.2].
* Per-message timing with fenced `rdtsc`, calibrated against `steady_clock`;
  samples go into a preallocated, pre-touched array, sorted after the run [6.7].
* Pipeline benchmark is **open-loop**: messages have an intended arrival time and
  latency is measured from it, so a stalled engine shows up as queueing delay
  instead of being hidden (coordinated omission) [6.7].
* Micro/component (`bench_engine`) and in-process end-to-end (`bench_pipeline`) levels [6.3].
* Platform jitter baseline (`bench_jitter`, a cyclictest-style hiccup meter)
  to explain the far tail [4.11].
* One change at a time: the pipeline matrix toggles queue, pinning and engine
  independently; compiler flags (`-march=native`) and huge pages measured
  separately [6.10, 3.8, 3.6].
* Both engines built with identical flags (`-O2`). ASan + UBSan run on the tests
  (`-DEXCH_SANITIZE=ON`); sanitizer builds are never used for numbers [6.10].

### Considered and not applied

* SIMD [3.4]: matching walks a FIFO and touches one level at a time; there is
  no wide data-parallel loop on the hot path worth vectorising.
* Kernel bypass / DPDK / RDMA [5.5, 5.6]: no network in scope; the gateway is in-process.
* NUMA placement [3.5]: single-socket 4-vCPU VM.
* RT scheduling, `isolcpus`, IRQ steering [4.7–4.9]: need host control we don't have.
* CPU pinning [3.7]: implemented (`--pin`), but on this VM it did **not**
  improve results, so the skill's "pin only on evidence" rule says don't rely on it here.

## Results

Cloud VM (Firecracker, 4 vCPU Xeon @ 2.3 GHz, GCC 13.3, `-O2`). Shared,
virtualised hardware: the skill says to benchmark on dedicated machines, so
read the absolute numbers as indicative and the ratios as the result. Raw output
of every run is in `results/`; reproduce with `./run_benchmarks.sh`.

**Platform floor** (`bench_jitter`): the thread loses the CPU ~1 340 times a
second for ~12 µs (P99 gap 81 µs, max 3.8 ms). Anything at P99.99 or beyond in
the per-message numbers is dominated by this, for both engines.

### Engine only, single thread (`bench_engine`, 5 M measured messages, 3 runs; range shown)

Book of ~40 k live orders across 8 instruments:

| | throughput | P50 | P90 | P99 | P99.9 |
|---|---|---|---|---|---|
| naive | 462–490 ns/msg (2.0–2.2 M msg/s) | 249–348 ns | 1.10–1.19 µs | 1.68–1.87 µs | 3.5–4.5 µs |
| fast  | 46–48 ns/msg (21–22 M msg/s) | 92–139 ns | 168–243 ns | 310–430 ns | 451–563 ns |
| **gain** | **~10×** | ~2.5× | ~5.5× | ~4.8× | ~8× |

Deep book, ~1 M live orders (working set well beyond L2):

| | throughput | P50 | P90 | P99 | P99.9 | page faults |
|---|---|---|---|---|---|---|
| naive | 883–970 ns/msg (~1.0 M msg/s) | 0.97–1.11 µs | 1.61–1.86 µs | 2.3–3.0 µs | 14–17 µs | 2 728 |
| fast  | 104–135 ns/msg (7.4–9.6 M msg/s) | 212–256 ns | 402–469 ns | 665–763 ns | 1.04–1.23 µs | 0 |
| **gain** | **~8×** | ~4.3× | ~3.9× | ~3.6× | ~13× | |

Per-message latency includes ~20 ns of fenced-`rdtsc` overhead and the fences
stop consecutive messages overlapping, which is why P50 sits above the
throughput figure. `-march=native` and `MADV_HUGEPAGE` were within run-to-run
noise in both books, so neither is adopted.

### Gateway thread → matching thread, open-loop (`bench_pipeline`, 1 M measured, 3 runs)

At 200 k msg/s:

| pipeline | P50 | P90 |
|---|---|---|
| hand-off only, mutex + condvar | 6.6–7.8 µs | 12–15 µs |
| hand-off only, SPSC ring | 330–540 ns | 650–710 ns |
| hand-off only, SeqRing | 254–276 ns | 310–347 ns |
| **naive engine + mutex queue** | **7.9–9.3 µs** | **14–17 µs** |
| naive engine + SeqRing, pinned | 1.05–1.32 µs | 2.2–3.0 µs |
| fast engine + mutex queue | 7.6–8.3 µs | 14–15 µs |
| fast engine + SPSC, pinned | 713–857 ns | 0.91–1.15 µs |
| **fast engine + SeqRing, pinned** | **361–417 ns** | **539–593 ns** |

At 1 M msg/s the mutex queue cannot keep up even with no engine at all
(latency climbs to hundreds of ms as the queue grows: skill 6.8, queueing delay
explodes near capacity), so the naive pipeline is saturated, while the fast
pipeline holds P50 ≈ 0.46–0.6 µs. The best 1 M msg/s tails came from the
*unpinned* SPSC run (P99 10–13 µs, P99.9 33–45 µs); P99 and beyond in this
benchmark are dominated by multi-millisecond vCPU stalls (see the jitter
floor) and vary several-fold between identical runs.

**Takeaways.** The engine work buys ~8–10× throughput and 4–13× at the
tail percentiles we can trust here. End to end, the hand-off dominated:
replacing the mutex/condvar queue alone is worth ~10× at P50, and putting the
fast engine behind the best ring gives ~20× at P50 and P90 versus the naive pipeline.

## Build, test, run

```sh
cmake -S . -B build -G Ninja && ninja -C build
./build/scenario_test && ./build/differential_test
./build/bench_engine --engine fast --live 40000
./build/bench_pipeline --engine fast --queue seq --pin --rate 200000
./run_benchmarks.sh            # everything, 3 repetitions, into results/
```

Options: `-DEXCH_NATIVE=ON` (`-march=native`), `-DEXCH_SANITIZE=ON` (ASan + UBSan).

## Layout

```
include/exch/types.h, protocol.h     shared rules, events, wire format
include/exch/naive/exchange.h        naive engine (+ src/naive_exchange.cc)
include/exch/fast/                   engine.h, ladder.h, order_pool.h, id_map.h, memory.h
include/exch/workload.h              deterministic synthetic order flow
tests/                               scenario tests (run on both), differential test + fuzzer
bench/                               engine, pipeline and jitter benchmarks, SPSC rings
```

## Style

Follows the Google C++ Style Guide: `.h`/`.cc`, path-based header guards,
include ordering, `CamelCase` functions and `kEnumerators`, `snake_case`
accessors, trailing-underscore members, no `using namespace`, project-prefixed
macros, `clang-format --style=Google` (`.clang-format`), naming checked with
`clang-tidy` (`.clang-tidy`). `cpplint` is clean except five
`whitespace/braces` false positives on `std::byte{'N'}` brace initialisers.
One deliberate deviation: the naive engine throws exceptions (Google style
forbids them) because that is part of what the naive version represents; the
fast engine and everything else are exception-free on the data path.

## Next steps

* Exchange-assigned order ids: index the pool directly and drop hashing.
* Journal and market-data publisher on their own threads behind SPSC rings
  (skill 1.2: move non-critical work off the path).
* Run `run_benchmarks.sh` on dedicated, isolated hardware to get trustworthy P99.9+.

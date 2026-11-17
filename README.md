# C++ skills in practice

Challenges that apply the [low-latency-cpp skill](https://github.com/dzheresc/low-latency-cpp)
to real code, with a naive baseline and a measured, optimised version side by side.

| Project | What it is |
|---|---|
| [`exchange/`](exchange/) | Exchange with order book and matching engine: naive STL version vs. low-latency version, identical behaviour (differential-tested), benchmarks and a skill-to-optimisation map. |

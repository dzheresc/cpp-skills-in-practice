# C++ skills in practice

Challenges that apply the [low-latency-cpp skill](https://github.com/dzheresc/low-latency-cpp)
to real code, with a naive baseline and a measured, optimised version side by side.

| Project | What it is |
|---|---|
| [`exchange/`](exchange/) | Exchange with order book and matching engine: naive STL version vs. low-latency version, identical behaviour (differential-tested), benchmarks and a skill-to-optimisation map. |

## Skills

| Skill | What it is |
|---|---|
| [`skills/oracle-by-design/`](skills/oracle-by-design/SKILL.md) | Oracle schema, SQL and PL/SQL design and tuning, after Tom Kyte's *Effective Oracle by Design*. |
| [`skills/postgres-by-design/`](skills/postgres-by-design/SKILL.md) | The same principles adapted to PostgreSQL (MVCC/vacuum, prepared statements, FK indexes, PITR). |

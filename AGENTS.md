# order-book-engine

L3 limit order book + matching engine in C++, fed via AF_XDP kernel-bypass
networking. Target: sub-microsecond p99.9 tick-to-trade latency. Portfolio
project for quant/HFT firm applications (Jane Street, Citadel Securities,
HRT, Voloridge) — treat correctness and measured latency as the two things
that matter; nothing else is the point of this project.

## Build

- `cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build`
- Release builds only for anything you benchmark — Debug numbers are meaningless here.
- Compiler: clang++ preferred (better -march=native codegen visibility for this kind of code); note in the PR if you switch to g++ for a specific reason.

## Hard rules for the hot path (order_book.hpp, price_level.hpp, memory_pool.hpp, spsc_ring_buffer.hpp, xdp_socket.hpp, signals.hpp, level_ladder.hpp)

- No `malloc`/`new`/`std::vector::push_back` growth on any path an incoming order can take. Preallocate everything.
- No `virtual` calls, no `std::function` with heap-allocating captures. Raw function pointers or templates only.
- Prices are `Price` (int64 ticks), never `float`/`double`. Do not introduce floating point on the matching path for any reason.
  Signals (`signals.hpp`) follow the same rule: fixed-point int64 scaled by `kSignalScale`.
- Any new hot-path struct gets `alignas(64)` considered explicitly — say in the PR/commit message whether you added it and why, or why it wasn't needed.

## Price-level container (Phase 4 swap done)

Price levels are a `LevelLadder` (`level_ladder.hpp`): a flat bitmap-indexed
window over the hot range, with a `std::map` fallback for the long tail.
The swap was validated by the Phase 0-3 tests running unchanged, plus
`tests/ladder_tests.cpp`, which diffs it against the old map book
(`tests/support/map_order_book.hpp`) after every op. Any future change to
the container follows the same rule: keep that reference and those tests
unchanged, and make them pass.

## Testing before any latency claim

- Correctness first: property-based tests on the matching engine — quantity
  conservation (sum of fills + remaining resting qty == sum of order qty in),
  price-time priority never violated, no phantom fills. Put these in
  `tests/` and run them on every change to order_book.hpp/price_level.hpp
  before touching anything performance-related.
- Only after tests are green: benchmark. Use RDTSC or
  `clock_gettime(CLOCK_MONOTONIC)` around each pipeline stage, accumulate
  into an HDR histogram, report p50/p99/p99.9/p99.99 — never just a mean.
- `perf stat` / `perf record` require running outside this sandbox with
  elevated privileges on the actual dev machine (WSL2/Linux) — don't try to
  run these in a restricted container and report a "latency number" from it.

## AF_XDP / eBPF work

- Requires root / CAP_NET_ADMIN and a real (or loopback-capable) NIC path —
  this only runs correctly on the WSL2 Linux environment, same as the
  net-suite project's XDP filter. Flag clearly in any PR if a change to
  this layer hasn't actually been run with real privileges yet, don't
  assume it works because it compiles.

## What "done" looks like for a milestone

Every milestone PR should include: the invariant tests passing, an updated
latency histogram (or explicit note that this milestone doesn't touch the
hot path), and one sentence on what's still a placeholder vs. production-real.
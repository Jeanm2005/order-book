# order-book-engine

An L3 limit order book and matching engine in C++, fed over a kernel-bypass
(AF_XDP) network path. Portfolio project aimed at market-making/HFT shops
(Jane Street, Citadel Securities, HRT, Voloridge) — the two things that
matter here are correctness and measured latency, not feature breadth.

[![CI](https://github.com/Jeanm2005/order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/Jeanm2005/order-book/actions/workflows/ci.yml)

**Status:** Phase 0 (correctness) and Phase 1 (synthetic feed + replay) are
done and covered by CI. AF_XDP ingestion, the signal layer, and latency
benchmarking aren't built yet — see [Roadmap](#roadmap).

## What it does

A single-process, single-symbol order book that:

- Ingests order/cancel messages over a zero-copy AF_XDP socket path
  (kernel-bypass networking), not the regular socket API. *(planned —
  Phase 2, replay-only for now)*
- Maintains a price-time-priority limit order book with no allocation,
  no locks, and no virtual dispatch on the hot path.
- Matches incoming orders against resting liquidity and emits fills.
- Computes real-time market-microstructure signals (microprice, order
  book imbalance) on top of the book state. *(planned — Phase 3)*
- Measures its own tick-to-trade latency end-to-end via a histogram,
  reporting p50/p99/p99.9/p99.99 — tail latency is what actually gets
  cared about, not the average. *(planned — Phase 4)*

## Non-goals

- **Not a trading strategy or backtester.** No PnL logic, no position
  management, no signal-driven order generation. The signal layer is
  read-only analytics on top of the book, full stop.
- **Not DPDK.** Using AF_XDP instead — see [Design notes](#design-notes).
- **Not exchange-accurate.** The feed protocol is a synthetic,
  ITCH-inspired binary format for replay/testing, not a certified
  implementation of any real exchange's wire protocol.

## Architecture

```
NIC -> XDP/eBPF (kernel) -> AF_XDP zero-copy ring -> userspace poller
    -> feed parser (synthetic ITCH-style binary protocol)
    -> order book core (per-symbol, lock-free single-writer)
    -> matching engine (price-time priority)
    -> signal layer (microprice / order book imbalance)
    -> market data publisher (shared-mem ring for consumers)
    -> replay/record subsystem (same code path as live)
```

| File | Component |
|---|---|
| `include/order.hpp` | `Order` — the order record, cache-line aligned |
| `include/price_level.hpp` | `PriceLevel` — intrusive FIFO queue of orders resting at one price |
| `include/memory_pool.hpp` | `MemoryPool` — fixed-capacity slab allocator for `Order` |
| `include/order_id_map.hpp` | `OrderIdMap` — fixed-capacity `OrderId -> Order*` index backing cancels |
| `include/order_book.hpp` | `OrderBook` — the matching engine, price-time priority |
| `include/spsc_ring_buffer.hpp` | `SpscRingBuffer` — lock-free single-producer/consumer ring for the market-data publish path |
| `include/feed_message.hpp`, `include/feed_replay.hpp` | synthetic feed wire format + replay engine |
| `src/main.cpp` | CLI: generate a synthetic feed, replay it through the book |

The matching path (`order_book.hpp`, `price_level.hpp`, `memory_pool.hpp`,
`spsc_ring_buffer.hpp`) runs under a few hard constraints: no heap
allocation once the pools are sized at startup, no virtual dispatch,
prices are fixed-point 64-bit ticks — never floating point — and every
hot struct gets evaluated for 64-byte cache-line alignment.

## Design notes

**AF_XDP instead of DPDK.** DPDK requires binding the NIC driver
exclusively to userspace (UIO/VFIO), which is awkward on commodity
hardware and doesn't map cleanly onto WSL2. AF_XDP gets zero-copy packet
delivery into userspace while the NIC stays under normal kernel control,
and it extends directly from prior XDP/eBPF work rather than starting a
new networking stack from scratch.

**`std::map` price levels, for now.** The order book currently keys price
levels with `std::map`. That's a known placeholder, not an oversight —
the intended structure is a flat/sparse array over the hot range around
best bid/ask, with the map as a fallback for the long tail. The swap is
deliberately deferred to its own step in Phase 4, validated against the
existing test suite unchanged, instead of getting bundled into feature
work before the matching logic itself was proven correct.

## Getting started

Requires CMake 3.20+ and a C++20 compiler (clang++ preferred).

```bash
git clone https://github.com/Jeanm2005/order-book.git
cd order-book
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

Generate a synthetic order flow log and replay it through the book:

```bash
./build/order_book_main generate feed.dat 200000
./build/order_book_main replay feed.dat
```

## Testing

```bash
cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
cmake --build build-debug
ctest --test-dir build-debug --output-on-failure
```

Debug builds run under ASan/UBSan. The suite covers quantity
conservation, price-time priority, no phantom/over-fills, and replay
determinism, via randomized property tests plus a few targeted regression
tests. Never benchmark a Debug build — only Release numbers mean anything
for latency.

## Roadmap

- [x] **Phase 0 — correctness.** Order book core and matching engine,
      pure userspace, no networking.
- [x] **Phase 1 — synthetic feed + replay.** Wire format, generator, and
      a replay engine that runs the same matching path a live feed will.
- [ ] **Phase 2 — AF_XDP ingestion.** Wire the feed into a real (or
      loopback) AF_XDP socket path.
- [ ] **Phase 3 — signal layer.** Microprice, order book imbalance at
      multiple depths.
- [ ] **Phase 4 — latency measurement.** Swap the price-level container
      for the flat/sparse array, instrument the pipeline, report
      p50/p99/p99.9/p99.99 off a Release build.
- [ ] **Phase 5 — writeup.** The latency numbers, the design tradeoffs,
      and the why behind each decision.

# order-book-engine

An L3 limit order book and matching engine in C++, fed over a kernel-bypass
(AF_XDP) network path. Portfolio project aimed at market-making/HFT shops
(Jane Street, Citadel Securities, HRT, Voloridge) — the two things that
matter here are correctness and measured latency, not feature breadth.

[![CI](https://github.com/Jeanm2005/order-book/actions/workflows/ci.yml/badge.svg)](https://github.com/Jeanm2005/order-book/actions/workflows/ci.yml)

**Status:** Phase 0 (correctness), Phase 1 (synthetic feed + replay),
Phase 2 (AF_XDP ingestion), and Phase 3 (signal layer) are done. Phase 2 is opt-in (`-DENABLE_AF_XDP=ON`,
default off — CI stays on Phase 0/1) and has been verified under real
privileges on the WSL2 dev box: `scripts/xdp_loopback_test.sh` delivers a
2000-message synthetic feed over a veth pair through a real `bind()`ed
AF_XDP socket and produces an identical book state (fills, traded qty,
best bid/ask, resting qty) to file-replay, with zero kernel-reported drops.
Phase 4 (flat-array price levels + latency measurement) is done: book +
signals run at 97 ns p50 / 429 ns p99.9 per message on the dev box — see
[Latency](#latency-phase-4).

## What it does

A single-process, single-symbol order book that:

- Ingests order/cancel messages over an AF_XDP socket path (kernel-bypass
  networking), not the regular socket API. *(Phase 2, opt-in build behind
  `-DENABLE_AF_XDP=ON`, verified via loopback test under real privileges;
  file-replay remains the default, CI-covered path)*
- Maintains a price-time-priority limit order book with no allocation,
  no locks, and no virtual dispatch on the hot path.
- Matches incoming orders against resting liquidity and emits fills.
- Computes real-time market-microstructure signals (microprice, order
  book imbalance at depths 1/3/5) on top of the book state, in
  fixed-point integer arithmetic. *(Phase 3 — see [Signals](#signals))*
- Measures its own tick-to-trade latency end-to-end via a histogram,
  reporting p50/p99/p99.9/p99.99 — tail latency is what actually gets
  cared about, not the average. *(Phase 4 — `order_book_bench`, see
  [Latency](#latency-phase-4))*

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
| `include/signals.hpp` | Phase 3: `BookSignals` + `compute_signals()` — microprice, order book imbalance at multiple depths |
| `include/level_ladder.hpp` | Phase 4: `LevelLadder` — one side's price levels: flat bitmap-indexed window over the hot range, `std::map` tail |
| `include/latency_histogram.hpp`, `include/tsc_clock.hpp` | Phase 4: fixed-size HDR-style histogram, fenced/calibrated RDTSC stamps |
| `bench/latency_bench.cpp` | Phase 4: `order_book_bench` — per-stage latency histograms over a replayed feed |
| `tests/support/map_order_book.hpp` | Phase 4: the pre-swap `std::map` book, kept as test oracle and bench baseline |
| `src/main.cpp` | CLI: generate/replay a synthetic feed, `record-size`, `xdp-listen` (opt-in build) |
| `ebpf/xdp_redirect.c` | Phase 2: XDP program, redirects the feed's UDP port into an `XSKMAP` |
| `include/xdp_socket.hpp`, `src/xdp_socket.cpp` | Phase 2: raw AF_XDP socket (UMEM/ring setup, RX poll loop) |
| `include/xdp_listen_cmd.hpp`, `src/xdp_listen_cmd.cpp` | Phase 2: `xdp-listen` CLI command, wires `XdpSocket` RX frames into `apply_message()` |
| `scripts/xdp_loopback_test.sh` | Phase 2: veth + socat loopback correctness test (manual, not CI) |

The matching path (`order_book.hpp`, `price_level.hpp`, `memory_pool.hpp`,
`spsc_ring_buffer.hpp`, `xdp_socket.hpp`'s RX poll loop, and
`signals.hpp`'s `compute_signals()`) runs under
a few hard constraints: no heap allocation once the pools/rings are sized
at startup, no virtual dispatch, prices are fixed-point 64-bit ticks —
never floating point — and every hot struct gets evaluated for 64-byte
cache-line alignment.

## Design notes

**AF_XDP instead of DPDK.** DPDK requires binding the NIC driver
exclusively to userspace (UIO/VFIO), which is awkward on commodity
hardware and doesn't map cleanly onto WSL2. AF_XDP gets zero-copy packet
delivery into userspace while the NIC stays under normal kernel control,
and it extends directly from prior XDP/eBPF work rather than starting a
new networking stack from scratch.

**Flat hot window, `std::map` tail.** Each side's price levels live in a
`LevelLadder`: a flat array of 1024 consecutive ticks (32 KB per side)
plus an occupancy bitmap, so finding the next best level is a `clz`/`ctz`
scan over 64-bit words instead of a tree walk. Prices outside the window
go to a `std::map` tail. The window follows the top of book: an insert
beyond the window's better edge re-centers it, and levels that fall out
move to the tail. Inserts on the worse side just join the tail. Each
price lives in exactly one place. Only the tail and re-centers can
allocate. Phases 0–3 shipped on a plain `std::map`, and the swap came
later as its own step: the Phase 0–3 test suites passed against the new
container with the test code unchanged. `tests/ladder_tests.cpp` then
runs a 64-tick window against the old map book (kept as
`tests/support/map_order_book.hpp`) in lock-step on drifting, jumping
flows, and compares fills, full depth, best bid/ask, and signals after
every operation. Those flows force the tail, re-center, and migration
paths, which the default window never reaches with the 90–110 test
prices. `PriceLevel` is not `alignas(64)`: at 32 bytes, adjacent levels
share a line, which helps the depth scan, and the book has a single
writer, so there's no false sharing to prevent.

## Signals

`include/signals.hpp` computes read-only analytics from the book after each
update; nothing here feeds back into matching. Every value is a fixed-point
`int64` in units of 1e-6 (of a tick for prices, of the unit interval for
imbalance). The no-floating-point rule applies here too, since this is a
tick-to-trade pipeline stage. Intermediates are `__int128`, so the only
range limit is |price| < ~9.2e12 ticks.

- **Microprice**: size-weighted top-of-book mid,
  `(Pb·Qa + Pa·Qb) / (Qa + Qb)`. Heavier bid size pulls it toward the ask.
  This is the model-free version commonly called microprice, not Stoikov's
  (2018) adjusted microprice, which needs a fitted model of queue dynamics.
  Floored, so it's deterministic for any price sign.
- **Order book imbalance** at depths 1, 3, 5 (price levels per side):
  `(Qb − Qa) / (Qb + Qa)` over the cumulative qty of the best N levels, in
  [−1, 1]. Truncates toward zero, so swapping sides negates it exactly.
  A one-sided book gives ±1; an empty book gives 0.

`BookSignals` is `alignas(64)` and exactly 64 bytes (static-asserted), so
one snapshot is one cache line. That's the intended slot size for the
SPSC publish ring. To fit, it has no separate valid flag: a side is empty
iff its best-level qty is 0 (the book never keeps an empty level).

The signal layer reads the book only through `OrderBook::top_levels()`,
a read-only L2 depth view. The Phase 4 container swap preserved it, and
`tests/signal_tests.cpp` was part of the unchanged suite that swap was
validated against. Those tests check the formulas against hand-computed values.
They also check that, after every operation of randomized flow, the
book's signals exactly equal signals derived from an independently
tracked model of resting orders. On top of that, a mirrored book (sides
swapped, prices negated) fed the same flow must produce mirrored signals
and an identical fill sequence, which also verifies that the matching
engine is side-symmetric.

## Latency (Phase 4)

`order_book_bench` replays a feed from memory and stamps every message
around each pipeline stage:

| stage | what's inside the stamps |
|---|---|
| `add` / `cancel` | `apply_message()`: match + rest, or cancel |
| `signals` | `compute_signals()` on the updated book |
| `total` | message in hand → signals out |
| `overhead` | two back-to-back stamps: the measurement floor, reported rather than subtracted |

- **Stamps:** fenced RDTSC (`lfence; rdtsc; lfence` to open,
  `rdtscp; lfence` to close), calibrated against `CLOCK_MONOTONIC`.
- **Histogram:** a fixed-size, integer-only HDR-style histogram. Values
  are exact below 256 ticks and within 1/128 above that, and each
  quantile is reported as its bucket's upper bound, so a reported p99.9
  is never lower than the true one.
- **Warm-up:** a warm-up pass runs on a throwaway book first. The timed
  pass then uses a fresh book, so it replays the identical state sequence.
- **Baseline:** `--book map` runs the pre-swap `std::map` book on the
  same feed as the before/after baseline.
- **Not measured:** NIC → userspace (AF_XDP RX) and market-data publish.

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++
cmake --build build
./build/order_book_main generate wide.feed 400000 42 wide   # ~hundreds of live levels, drifting mid
./build/order_book_bench wide.feed --cpu 2                  # flat ladder
./build/order_book_bench wide.feed --cpu 2 --book map       # pre-swap baseline
```

There are two feed profiles:
- **`narrow`** (the default): 11 price levels, 95–105. This is the map's
  best case, because an 11-node tree stays in L1.
- **`wide`**: the mid drifts, and orders rest up to 300 ticks from it,
  mostly near the touch.

### Results

WSL2 dev box, Release, clang++ 21, `-O3 -march=native`, pinned to CPU 2
(`--cpu 2`, not an isolated core). Feed: `wide`, 400,000 messages,
seed 42. Stamp clock: invariant TSC at 3686 MHz. All values in ns.
Single run per book.

**Flat ladder (current):**

| stage | count | min | p50 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add | 219,727 | 22 | 50 | 196 | 333 | 807 | 23,153 |
| cancel | 180,273 | 20 | 47 | 205 | 342 | 540 | 13,357 |
| signals | 400,000 | 42 | 47 | 101 | 114 | 253 | 78,275 |
| **total** | 400,000 | 68 | **97** | **270** | **429** | 5,520 | 78,324 |
| overhead | 1,000,000 | 13 | 14 | 15 | 16 | 16 | 10,438 |

**`std::map` book (pre-Phase-4 baseline), same feed:**

| stage | count | min | p50 | p99 | p99.9 | p99.99 | max |
|---|---:|---:|---:|---:|---:|---:|---:|
| add | 219,727 | 22 | 64 | 209 | 405 | 1,267 | 34,934 |
| cancel | 180,273 | 23 | 70 | 242 | 444 | 1,206 | 127,505 |
| signals | 400,000 | 33 | 37 | 100 | 165 | 344 | 43,736 |
| **total** | 400,000 | 60 | **109** | **323** | **540** | 8,680 | 129,334 |
| overhead | 1,000,000 | 13 | 15 | 16 | 17 | 17 | 42,475 |

What the numbers say:

- **The swap pays off where it should.** Median add is 22% faster
  (50 vs 64 ns) and median cancel 33% faster (47 vs 70 ns). Tails
  improve too: add p99.9 is 333 vs 405 ns and cancel p99.9 is 342 vs
  444 ns. Both books produce identical fills (27,259).
- **End to end, p99.9 is 429 ns vs 540 ns.** That covers book +
  signals per message, and it's under the project's sub-µs p99.9
  target for this part of the pipeline. It is *not* full
  tick-to-trade: NIC → userspace isn't in it.
- **The ladder is slower at the signals median:** 47 vs 37 ns.
  Walking the top 5 levels by bitmap scan costs more than 5 in-order
  steps through a small, cache-hot tree. It has the better tail
  (p99.9 114 vs 165 ns). This is the obvious next target: e.g.,
  maintain depth aggregates incrementally instead of re-walking.
- **Treat p99.99 and max as environment, not code, until shown
  otherwise.** The book work is the same at p99.9 and p99.99, yet
  total jumps from 429 ns to 5.5 µs, and the max reaches 78 µs. That
  pattern fits interrupts, VM exits, or scheduler preemption on a
  non-isolated core under WSL2's hypervisor. I haven't verified that
  (it needs `perf` / an isolated core on bare Linux). At this count,
  p99.99 also rests on only ~20–40 samples per stage.
- The timing overhead (14 ns p50 for two back-to-back stamps) is
  included in every row, not subtracted.

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
conservation, price-time priority, no phantom/over-fills, replay
determinism, matching-engine side symmetry, signal correctness,
ladder-vs-map equivalence, and latency-histogram accuracy, via randomized property tests plus a few targeted regression
tests. Never benchmark a Debug build — only Release numbers mean anything
for latency.

## AF_XDP ingestion (Phase 2, opt-in)

Requires `libbpf-dev` and `clang` (for the eBPF object), and, to actually
run, root/`CAP_NET_ADMIN` plus a real or loopback-capable (veth) NIC path —
this only works on the WSL2/Linux dev box, not in CI or a sandboxed
container. See `AGENTS.md` "AF_XDP / eBPF work".

```bash
cmake -B build-xdp -DCMAKE_BUILD_TYPE=Release -DENABLE_AF_XDP=ON -DCMAKE_CXX_COMPILER=clang++
cmake --build build-xdp
sudo ./scripts/xdp_loopback_test.sh   # veth + socat loopback correctness check
```

The loopback script generates a feed, replays it via the file path for an
expected result, then re-delivers the same messages over a veth pair (one
`FeedMessage` per UDP datagram, via `socat`) into `xdp-listen`, and prints
both results for comparison. Verified passing under real privileges on the
WSL2 dev box: identical fills/traded-qty/best-bid/best-ask/resting-qty,
zero kernel-reported drops (`XDP_STATISTICS`).

Two real bugs surfaced only by running this for real, not by compiling it:
`bind()` returned a bare `EINVAL` until `XDP_UMEM_COMPLETION_RING`/
`XDP_TX_RING` were registered (never mmap'd — this socket is still RX-only
in *use*), even though the AF_XDP docs suggest an RX-only ring pair should
be enough; and veth's native XDP mode wouldn't bind either, so the program
attaches in forced generic/SKB mode, same as the kernel's own `xskxceiver`
selftest does for veth-pair testing. Neither is discoverable by reading
the code — see `include/xdp_socket.hpp` and `src/xdp_socket.cpp` for the
detail.

## Roadmap

- [x] **Phase 0 — correctness.** Order book core and matching engine,
      pure userspace, no networking.
- [x] **Phase 1 — synthetic feed + replay.** Wire format, generator, and
      a replay engine that runs the same matching path a live feed will.
- [x] **Phase 2 — AF_XDP ingestion.** Wire the feed into a real (or
      loopback) AF_XDP socket path. Opt-in build (`-DENABLE_AF_XDP=ON`);
      loopback test passed under real privileges on the WSL2 dev box.
- [x] **Phase 3 — signal layer.** Microprice, order book imbalance at
      depths 1/3/5, fixed-point, one cache line per snapshot.
- [x] **Phase 4 — latency measurement.** Flat bitmap-indexed price
      levels (differential-tested against the map book), per-stage
      HDR-style histograms, pinned Release numbers from the dev box:
      97 ns p50 / 429 ns p99.9 per message, book + signals.
- [ ] **Phase 5 — writeup.** The latency numbers, the design tradeoffs,
      and the why behind each decision.

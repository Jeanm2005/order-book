# order-book-engine

An L3 limit order book and matching engine in C++, fed over a kernel-bypass
(AF_XDP) network path, built as a portfolio project demonstrating the
systems-programming depth that quant/HFT firms screen for.

**Target audience for this project:** Jane Street, Citadel Securities, HRT,
Voloridge, and similar market-making/HFT shops. Every design decision below
is made with "would this hold up in an interview with one of these firms"
as the bar — not "does it work," but "does it reflect real understanding
of why HFT systems are built this way."

## What this is

A single-process, single-symbol-sharded order book that:

1. Ingests order/cancel/execute messages over a zero-copy AF_XDP socket
   path (kernel-bypass networking), not the regular socket API.
2. Maintains a price-time-priority limit order book with no allocation,
   no locks, and no virtual dispatch on the hot path.
3. Matches incoming orders against resting liquidity and emits fills.
4. Computes real-time market-microstructure signals (microprice, order
   book imbalance) on top of the book state — this is what separates the
   project from "yet another fast matching engine" and ties it to actual
   market microstructure theory, not just low-latency C++.
5. Measures its own tick-to-trade latency end-to-end via an HDR histogram,
   reporting p50/p99/p99.9/p99.99 — because tail latency, not average
   latency, is what HFT actually cares about.

## What this deliberately is NOT

- **Not a trading strategy or backtester.** No PnL logic, no position
  management, no signal-driven order generation. The signal layer is
  read-only analytics on top of the book, full stop. Scope creep here
  dilutes the actual story ("I built a fast, correct matching engine and
  understand the microstructure around it") into a much bigger, weaker
  project.
- **Not DPDK.** Deliberately using AF_XDP instead — see "Why AF_XDP" below.
- **Not exchange-accurate.** The feed protocol is a synthetic ITCH-style
  format for replay/testing, not a certified implementation of any real
  exchange's wire protocol.

## Architecture

```
NIC -> XDP/eBPF (kernel) -> AF_XDP zero-copy ring -> userspace poller
    -> feed parser (ITCH-style binary protocol)
    -> order book core (per-symbol, lock-free single-writer)
    -> matching engine (price-time priority)
    -> signal layer (microprice / order book imbalance)
    -> market data publisher (shared-mem ring for consumers)
    -> replay/record subsystem (same code path as live)
```

See `include/` for the current core data structures (`Order`, `PriceLevel`,
`OrderBook`, `MemoryPool`, `SpscRingBuffer`). See `CLAUDE.md` for hot-path
coding rules and the current known placeholder in `order_book.hpp`
(std::map price levels — a deliberate correctness-first choice, not yet
the intended flat/sparse-array structure).

## Why AF_XDP instead of DPDK

DPDK requires binding the NIC driver exclusively to userspace (UIO/VFIO),
which is awkward on commodity hardware and doesn't map cleanly onto WSL2.
AF_XDP gets zero-copy packet delivery into userspace while the NIC stays
under normal kernel control, and — specific to this project's author —
extends directly from prior XDP/eBPF work on the net-suite project rather
than starting a new networking stack from scratch.

## Roadmap

- [x] **Phase 0 — correctness.** Order book core + matching engine, pure
      userspace, no networking. Nothing else matters until this phase is
      solid.
    - `Order` / `PriceLevel` / `MemoryPool` / `OrderBook` (std::map
      placeholder container) / `SpscRingBuffer`.
    - Fixed-capacity `OrderId -> Order*` index (open addressing, no
      heap allocation after construction) so cancel works by id like a
      real system, not by raw pointer.
    - CMake: Release (`-O3 -march=native`) and Debug (ASan/UBSan) configs.
    - Property-based tests: quantity conservation, price-time priority
      never violated, no phantom/over-fills, cancel/re-cancel edge cases,
      a targeted regression test for the pool-slot-reuse linked-list bug.
    - **Done.** All tests green on Debug (sanitized) and Release builds.
- [x] **Phase 1 — synthetic feed + replay.**
    - `FeedMessage`: fixed-size, type-tagged (`AddOrder`/`CancelOrder`)
      binary record, host-native layout, trivially copyable — the exact
      shape Phase 2's AF_XDP RX path will `reinterpret_cast` frames as.
      No Execute message type: executions are the book's own *output*
      (`Fill`), not something an inbound feed carries.
    - Feed generator (`order_book_main generate <file> <count> [seed]`):
      mostly Add orders over a tight price range plus cancels referencing
      earlier ids, written to a magic-tagged binary log file. Offline
      tooling, not hot-path-constrained.
    - `apply_message()`: turns one `FeedMessage` into an `OrderBook` call.
      This *is* the hot-path piece — allocation-free, reused unchanged by
      the replay engine now and by the live AF_XDP path in Phase 2.
    - Replay engine (`order_book_main replay <file>`) driving
      parse -> `apply_message` -> fill callback from a recorded log,
      reporting fill count, traded qty, and final book state.
    - Tests (`tests/feed_replay_tests.cpp`): file round-trip
      (save/load equality, bad-magic/missing-file handling), and replay
      determinism (same 3000-message log replayed into two independent
      books produces byte-identical fill sequences and matching resting
      qty).
    - **Done.** All tests green on Debug (sanitized) and Release; demoed
      end-to-end on a 200k-message generated log.
- [ ] **Phase 2 — AF_XDP ingestion.**
    - AF_XDP socket setup: UMEM registration, fill/completion/RX/TX rings.
    - Minimal XDP/eBPF program redirecting the feed's UDP port into the
      AF_XDP socket via `XDP_REDIRECT` — extends the net-suite project's
      XDP work rather than starting fresh.
    - Zero-copy consumption off the RX ring, reinterpreting frames as
      `FeedMessage` and calling the *same* `apply_message()` from Phase 1
      — no new parsing code.
    - Loopback sender for testing without a real counterparty feed.
    - Requires root/CAP_NET_ADMIN and a real WSL2 NIC/loopback path —
      cannot be built or verified in a sandbox. Any code here stays
      flagged as "compiles, not yet run with real privileges" until
      it's actually been run on the real box.
    - Validation: live loopback run must produce fills identical to
      replaying the same message sequence from Phase 1.
- [ ] **Phase 3 — signal layer.** Read-only analytics on top of book
      state — no PnL, no position management, no signal-driven order
      generation (see "What this deliberately is NOT").
    - Microprice: size-weighted mid from best bid/ask price and
      top-of-book quantity.
    - Order book imbalance at multiple depths (top-N level qty sum per
      side). Fine to build on the current std::map-backed book — not
      hot-path-critical enough to block on the Phase 4 container swap.
    - Tests: hand-computed expected values for constructed book states;
      property test that microprice always falls within [best bid, best
      ask].
- [ ] **Phase 4 — latency measurement.** Only after Phase 0-3 tests are
      green — never claim a latency number off untested code.
    - **Container swap (the placeholder CLAUDE.md flags):** replace
      `std::map<Price, PriceLevel>` with a flat/sparse array over the
      hot range around best bid/ask, map fallback for the long tail.
      Dedicated step; re-run the unchanged Phase 0-3 tests afterward to
      prove the swap didn't change behavior.
    - Instrumentation: RDTSC or `clock_gettime(CLOCK_MONOTONIC)` at each
      pipeline stage boundary (receive -> parse -> book mutate -> match
      -> fill -> signal -> publish), gated behind a compile-time flag so
      it never taxes the uninstrumented hot path.
    - HDR histogram (or the minimal log-bucketed equivalent already used
      in tests) accumulating per-stage and end-to-end tick-to-trade
      latency.
    - Benchmark driver replaying a large Phase 1 log at full speed,
      reporting p50/p99/p99.9/p99.99 — never a bare mean.
    - Release builds only, run on the real WSL2 box with `perf
      stat`/`perf record` for deeper profiling — not reproducible in a
      sandbox.
    - Iterate: pool sizing, `alignas(64)` audit across hot structs,
      false-sharing check on the ring buffer's head/tail, branch removal.
    - Deliverable: a checked-in latency report, before/after the
      container swap.
- [ ] **Phase 5 — writeup.** What actually gets read in an application,
      more than the code itself.
    - Architecture decisions and why (AF_XDP vs DPDK, map-then-array,
      raw function pointers vs `std::function`, etc.).
    - The latency numbers with methodology: what was measured, how,
      on what hardware, Release build confirmed.
    - Explicit "production-real vs. placeholder" accounting, project-wide
      (synthetic protocol vs. real exchange wire format, single-symbol
      vs. sharded, no persistence/recovery, no risk checks).

## Build

See `CLAUDE.md` for build commands and hot-path conventions once the
build system exists.
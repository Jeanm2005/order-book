# Order book engine: design writeup

An L3 limit order book and price-time matching engine in C++20, fed over an
AF_XDP kernel-bypass path, with fixed-point microstructure signals and a
market-data publisher. This is the Phase 5 writeup: what was built, why
each decision went the way it did, how it was verified, and what the
latency numbers do and don't say.

The project optimizes for two things only: **correctness** and **measured
latency**. Every section below is organized around one of the two.

## 1. The pipeline

```
NIC ─► XDP program (eBPF, kernel) ─► AF_XDP RX ring (UMEM, mmap'd)
    ─► XdpSocket::poll()            userspace, one thread, busy-poll
    ─► apply_message()              40-byte FeedMessage
    ─► OrderBook                    match → rest / cancel, fills via fn pointer
    ─► compute_signals()            microprice, imbalance @ 1/3/5
    ─► MdPublisher::publish()       SPSC ring ─► consumer thread
```

File replay (`order_book_main replay`) enters at `apply_message()`, the same
function the network path calls, so every correctness test of the book is
also a test of the live path's logic. Everything from `poll()` to
`publish()` runs on one thread with no locks. The only cross-thread handoff
is the SPSC ring at the end.

## 2. Design decisions and why

### Single writer, single symbol

One thread owns the book. There are no locks, no atomics on the matching
path and no false sharing to design around, so the book's structs are laid
out for the depth scan, not for concurrency. Scaling to many symbols means
one book per core with the feed sharded by symbol (or one AF_XDP queue per
shard), not a shared book behind a lock.

### Fixed point everywhere

Prices are `int64` ticks. Signals are `int64` scaled by 10⁶. Intermediate
products go through `__int128`, so the only range limit is on the result
(|price| < ~9.2e12 ticks). Two details matter:

- **Microprice** is computed as `Pb + (Pa − Pb)·Qb / (Qa + Qb)`, not the
  textbook `(Pb·Qa + Pa·Qb)/(Qa+Qb)`. The numerator is then never
  negative, so integer truncation is a true floor and the result is
  deterministic for any price sign.
- **Imbalance** `(Qb − Qa)/(Qb + Qa)` truncates toward zero, which makes it
  exactly antisymmetric. The mirrored-book test depends on that.

There is no float anywhere on the path, so there's no rounding mode, no
denormal and no "equal within epsilon" in any test. Every signal comparison
in the suite is exact.

### Orders, levels, and memory

- `Order` is `alignas(64)`: one order per cache line, allocated from a
  fixed `MemoryPool` slab with a LIFO free list, so a just-freed (still
  cached) slot is reused first.
- `PriceLevel` is an intrusive doubly linked FIFO, 32 bytes, and
  deliberately **not** cache-line aligned. With a single writer there's no
  false sharing to prevent, and two levels per line helps the top-of-book
  scan.
- Fills go out through a raw function pointer: no virtual dispatch and no
  `std::function` capture.

### Cancel lookup: `OrderIdMap`

This is an open-addressing hash table (splitmix64 finalizer, linear
probing) sized to at least 2× the pool, so load stays ≤ 0.5.

The first version used tombstones for deletion. Tombstones never became
Empty again, so over a long run the table lost all its Empty slots and
every lookup for a missing id scanned the whole table. At the default size
that's 131,072 slots, and a cancel for an already-filled order is exactly
such a lookup. The 400k-message benchmark was too short to show it; a
trading day wouldn't be.

The fix is **backward-shift deletion**: erase pulls later entries of the
cluster back into the hole unless their home slot lies cyclically in
`(hole, j]`. The table only ever holds Empty and Occupied slots, so a miss
stops within `size() + 1` probes no matter how long the map has churned.
`tests/order_id_map_tests.cpp` bounds that directly, using a probe-count
diagnostic rather than timing, and fails on the old code.

### Price levels: bitmap ladder + sorted-array tail

Each side is a `LevelLadder`:

- **Hot window:** a flat array of 1024 consecutive ticks (32 KB per side,
  L2-resident), plus a 1024-bit occupancy bitmap. "Next best level" is one
  masked word and a `ctz`/`clz`, instead of the pointer-chasing tree walk
  of the `std::map` it replaced.
- **Recenter policy:** the window follows the top of book. An insert
  beyond the window's better edge re-anchors it; inserts beyond the worse
  edge go to the tail. `tests/ladder_tests.cpp` pins this policy, since
  the differential tests can't see it (a tail level behaves identically,
  just slower).
- **Tail:** originally a `std::map`, which allocated on every tail insert
  and recenter, against the hot-path rule. It's now a sorted array
  allocated once. Its capacity is the order pool's capacity, which makes
  overflow impossible rather than unlikely: every level holds at least one
  resting order, so a side can't have more levels than the pool has slots.
  It's stored worst-first with the best level at the back, because new
  tail levels usually land just past the window's worse edge, and that
  keeps the shift short. A recenter's evictions are staged in a
  preallocated scratch area and merged in O(n + k).

The trade-off: an insert deep in a large tail shifts up to the tail's size
in 32-byte levels, where the map did an O(log n) node insert. That is the
price of never allocating, and it only applies far from the touch.

### Refusals

`add_order` refuses, before matching and without touching book state:

- a **duplicate live id**, which would otherwise trade and then rest as a
  second order that a cancel can't tell apart;
- an add when the **pool is full**.

Both are counted in `OrderBook::stats()` and printed by `replay` and
`xdp-listen`. A duplicate check costs one extra hash lookup per add. That's
paid deliberately, because the alternative is a book that can silently
corrupt its own cancel index.

### Market-data publish

`MdPublisher` puts one 64-byte `BookSignals` snapshot in each slot of an
SPSC ring. `BookSignals` is exactly one cache line, static-asserted, which
is why it has no separate valid flag: an empty side is `qty == 0`.

`publish()` **never blocks the book thread**. If the consumer is a full
ring behind, the snapshot is dropped and counted. This is safe because a
snapshot is the full signal state, not a delta: a slow consumer sees
conflated updates, never wrong ones, and the drop counter says it's too
slow. The producer's counters sit on their own cache line, away from the
consumer-written tail index.

### Networking: AF_XDP, not DPDK

DPDK takes the NIC away from the kernel (UIO/VFIO), which is awkward on
commodity hardware and doesn't map onto WSL2. AF_XDP delivers frames into a
userspace-mapped ring while the NIC stays under the kernel, and an XDP
program (`ebpf/xdp_redirect.c`) redirects only the feed's UDP port. All
other traffic passes through normally.

The socket uses raw syscalls against `<linux/if_xdp.h>`, not libxdp, for
explicit control. Two things only showed up by running it with real
privileges:

1. `bind()` returned a bare `EINVAL` until the TX and completion rings were
   registered, even though the socket is RX-only.
2. veth's native XDP mode wouldn't bind, so the program attaches in generic
   (SKB) mode. The kernel's own AF_XDP selftests do the same over veth.

Over veth, the socket runs in copy mode. Zero-copy on a real NIC's driver
hasn't been exercised.

## 3. How it's verified

Correctness comes first; nothing performance-related changes until these
pass (AGENTS.md).

| What | How |
|---|---|
| Quantity conservation, price-time priority, no phantom or over-fills | Randomized property tests on the book (`order_book_tests`) |
| Replay determinism | Same log, twice, identical fills and final state (`feed_replay_tests`) |
| Matching is side-symmetric | A mirrored book (sides swapped, prices negated) must produce mirrored signals and an identical fill sequence (`signal_tests`) |
| Signals are right | Hand-computed values, plus after every op, equality with signals from an independently tracked model (`signal_tests`) |
| Container swap changed nothing | Ladder book vs the preserved `std::map` book in lock-step, 64-tick window, flows that force tail/recenter/migration; fills, full depth and signals compared after every op (`ladder_tests`). That reference and those tests are never edited. |
| Hot path never allocates | Global `operator new` replaced with a counter; 1M messages of match/rest/cancel + signals must allocate 0 times (`no_alloc_tests`). The old map tail failed this: 64,239 allocations on one 200k flow. |
| Cancel index stays fast | Probe chains bounded by live size under long churn; differential against `std::unordered_map` (`order_id_map_tests`) |
| Refusals are harmless | Duplicate ids and a full pool leave depth unchanged and emit no fills (`book_guard_tests`) |
| Publish path | FIFO, no torn 64-byte slots, exact published/dropped accounting across real threads; clean under ThreadSanitizer (`publisher_tests`, CI `tsan` job) |
| Histogram is honest | Bucket geometry checked exhaustively; quantiles vs exact order statistics (`histogram_tests`) |
| AF_XDP == file replay | `scripts/xdp_loopback_test.sh`: veth pair, one `FeedMessage` per UDP datagram, book state diffed line by line against replay, zero kernel drops, every snapshot consumed. Exits non-zero on any mismatch; runs in CI under sudo. |

The tests also get mutation checks: a deliberately broken backward-shift
condition, tail merge, tail range extraction and tail `best()` must each
make a suite fail. They do, by check failure or by hang, which the 120 s
ctest timeout turns into a failure.

Debug builds run under ASan and UBSan.

## 4. Latency

### Method

- **Stamps:** fenced RDTSC (`lfence; rdtsc; lfence` to open, `rdtscp;
  lfence` to close), converted to ns with an exact integer ratio
  calibrated against `CLOCK_MONOTONIC`.
- **Histogram:** fixed-size, integer-only, HDR-style. Values are exact
  below 256 ticks and within 1/128 above. Each quantile reports its
  bucket's upper bound, so a reported p99.9 is never optimistic. No means
  are reported anywhere.
- **Run shape:** a warm-up pass on a throwaway book, then a timed pass on
  a fresh book, so it replays the identical state sequence. Stamp overhead
  (two back-to-back stamps) is reported as its own row, not subtracted.
- **Stages:** `add`/`cancel` (apply_message), `signals`, `publish`, and
  `total` (message in hand → snapshot published). `xdp-listen`
  additionally reports `rx->pub`: from `poll()` starting the batch a frame
  arrived in to that frame's snapshot being published.

### Results

The measured dev-box numbers (WSL2, clang 21, `-O3 -march=native`, pinned,
wide feed, 400k messages) are in the README's [Latency](../README.md#latency-phase-4)
section. Headline: **97 ns p50 / 429 ns p99.9** for book + signals, against
540 ns p99.9 for the `std::map` book on the same feed.

Those numbers predate four changes, each of which touches a measured stage:

- the backward-shift `OrderIdMap` (cancel and fill path);
- the sorted-array tail (add/cancel, when outside the window);
- the duplicate-id check (one lookup per add);
- the new `publish` stage, now included in `total`.

They need one re-run on the dev box: `./scripts/bench_report.sh 2 3`
prints the README tables directly. The `rx->pub` NIC-to-publish figure also
needs a run of the loopback script on that machine. Numbers from a CI
runner or a cloud container are not the project's numbers (AGENTS.md).

### Reading the tail

On the dev box, `total` goes from 429 ns at p99.9 to 5.5 µs at p99.99, and
the max reaches 78 µs, while the book does the same work at both
percentiles. That pattern fits interrupts, VM exits or scheduler preemption
on a non-isolated core under WSL2's hypervisor. It hasn't been proven. That
needs `perf` and an isolated core (`isolcpus`, `nohz_full`) on bare Linux.
At 400k messages, p99.99 also rests on only 20 to 40 samples per stage.

## 5. What's production-real vs placeholder

**Production-real** for a single-writer, single-symbol book:

- the matching engine;
- the preallocated containers (pool, id map, ladder, tail);
- fixed-point signals;
- the conflating publisher;
- the AF_XDP RX path (generic mode, copy mode).

**Placeholder, by design:**

- **The feed format.** It's synthetic, host-endian and fixed-size, not an
  exchange protocol. Real ITCH or OUCH would need a real parser in front
  of `apply_message()`.
- **Order types.** Limit orders and cancels only: no modify, IOC or
  market orders.
- **The publish consumer.** It drains and counts; no downstream protocol.

**Not yet exercised:**

- AF_XDP zero-copy on a real NIC driver;
- NIC hardware timestamps, so `rx->pub` starts at userspace, not the wire;
- isolated-core `perf` runs to explain the p99.99 tail.

## 6. Next, if this continued

1. **Incremental depth aggregates for signals.** The ladder's signals
   median (47 ns) is slower than the map's (37 ns): re-walking the top 5
   levels by bitmap costs more than 5 steps through a tiny, cache-hot
   tree. Maintaining top-N depth sums as orders arrive and leave would
   make `compute_signals` O(1). Worth doing only with a dev-box run to
   prove it.
2. **Hardware RX timestamps** (SO_TIMESTAMPING or XDP RX metadata), so the
   end-to-end number starts at the NIC.
3. **Zero-copy AF_XDP on a real NIC**, and one queue plus one book per
   core for multiple symbols.

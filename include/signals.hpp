#pragma once
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include "order.hpp"
#include "order_book.hpp"

// Phase 3 signal layer: read-only market-microstructure analytics computed
// from book state. Never feeds back into matching.
//
// No floating point, same as the matching path (AGENTS.md): every signal is
// a fixed-point int64 in units of 1/kSignalScale — of a tick for prices, of
// the unit interval for imbalance. Intermediates go through __int128 so the
// only range constraint is on the result: |price| * kSignalScale must fit in
// int64, i.e. |price| < ~9.2e12 ticks.

inline constexpr std::int64_t kSignalScale = 1'000'000;

// Depths (in price levels, per side) at which order book imbalance is
// reported. Must be strictly ascending; the deepest one sets how many
// levels compute_signals() pulls from the book.
inline constexpr std::size_t kImbalanceDepths[] = {1, 3, 5};
inline constexpr std::size_t kNumImbalanceDepths = std::size(kImbalanceDepths);
inline constexpr std::size_t kMaxSignalDepth = kImbalanceDepths[kNumImbalanceDepths - 1];

// Size-weighted mid of the top of book — what's usually called "microprice"
// in practice: (Pb*Qa + Pa*Qb) / (Qa + Qb). Heavier bid size pulls it toward
// the ask. (Not Stoikov's 2018 adjusted microprice, which needs a fitted
// model of queue dynamics; this is the model-free first-order version.)
//
// Written as Pb + spread*Qb/(Qa+Qb) so the division only ever sees a non-
// negative numerator: truncation == floor, deterministic for any sign of Pb.
// Preconditions: bid < ask (a resting book is never crossed), both qty > 0.
constexpr std::int64_t microprice_scaled(Price bid, Quantity bid_qty, Price ask, Quantity ask_qty) {
    __int128 offset = static_cast<__int128>(ask - bid) * bid_qty * kSignalScale / (bid_qty + ask_qty);
    return static_cast<std::int64_t>(static_cast<__int128>(bid) * kSignalScale + offset);
}

// Order book imbalance (Qb - Qa) / (Qb + Qa), in [-kSignalScale, kSignalScale].
// C++ division truncates toward zero, so this is exactly antisymmetric:
// imbalance(a, b) == -imbalance(b, a). One side empty -> +/-kSignalScale
// (fully one-sided); both empty -> 0.
constexpr std::int64_t imbalance_scaled(Quantity bid_qty, Quantity ask_qty) {
    Quantity total = bid_qty + ask_qty;
    if (total == 0) return 0;
    return static_cast<std::int64_t>(static_cast<__int128>(bid_qty - ask_qty) * kSignalScale / total);
}

// Exactly one cache line: signals are produced once per book update and are
// the natural payload for the SPSC market-data publish ring, where 64-byte
// slots keep the producer writing slot i+1 off the line the consumer is
// reading slot i from. Fitting in 64 bytes is why there's no separate
// "valid" flag or stored mid: a side is empty iff its qty is 0 (the book
// never keeps an empty level), and mid is derived.
struct alignas(64) BookSignals {
    Price    best_bid = 0;          // meaningless when bid_qty == 0
    Price    best_ask = 0;          // meaningless when ask_qty == 0
    Quantity bid_qty  = 0;          // resting qty at best bid, 0 == no bids
    Quantity ask_qty  = 0;          // resting qty at best ask, 0 == no asks
    std::int64_t microprice = 0;    // scaled; 0 unless two_sided()
    std::int64_t imbalance[kNumImbalanceDepths] = {}; // scaled, per kImbalanceDepths

    bool has_bid() const { return bid_qty > 0; }
    bool has_ask() const { return ask_qty > 0; }
    bool two_sided() const { return has_bid() && has_ask(); }

    // Exact: (bid + ask) * kSignalScale / 2, and kSignalScale is even.
    std::int64_t mid_scaled() const {
        return two_sided() ? best_bid * kSignalScale + (best_ask - best_bid) * (kSignalScale / 2) : 0;
    }
};
static_assert(sizeof(BookSignals) == 64, "BookSignals is meant to fill exactly one cache line");

// Recomputes every signal from the current book state. O(kMaxSignalDepth)
// level reads per side, no allocation — this is a tick-to-trade pipeline
// stage (it runs after matching on each update), so hot-path rules apply.
template <std::size_t Cap>
inline void compute_signals(const OrderBook<Cap>& ob, BookSignals& out) {
    static_assert([] {
        for (std::size_t i = 1; i < kNumImbalanceDepths; ++i)
            if (kImbalanceDepths[i] <= kImbalanceDepths[i - 1]) return false;
        return kImbalanceDepths[0] > 0;
    }(), "kImbalanceDepths must be positive and strictly ascending");

    LevelQty bids[kMaxSignalDepth];
    LevelQty asks[kMaxSignalDepth];
    std::size_t nb = ob.top_levels(Side::Buy, bids, kMaxSignalDepth);
    std::size_t na = ob.top_levels(Side::Sell, asks, kMaxSignalDepth);

    out.best_bid = nb ? bids[0].price : 0;
    out.bid_qty  = nb ? bids[0].qty : 0;
    out.best_ask = na ? asks[0].price : 0;
    out.ask_qty  = na ? asks[0].qty : 0;
    out.microprice = (nb && na) ? microprice_scaled(out.best_bid, out.bid_qty, out.best_ask, out.ask_qty) : 0;

    Quantity cum_bid = 0, cum_ask = 0;
    std::size_t d = 0;
    for (std::size_t level = 0; level < kMaxSignalDepth; ++level) {
        if (level < nb) cum_bid += bids[level].qty;
        if (level < na) cum_ask += asks[level].qty;
        if (level + 1 == kImbalanceDepths[d]) {
            out.imbalance[d++] = imbalance_scaled(cum_bid, cum_ask);
        }
    }
}

// --- Reporting (not hot path): stdio formatting for the CLI. ---------------

// Prints a scaled fixed-point value as a decimal, e.g. 101500000 -> "101.500000".
inline void print_scaled(std::int64_t v) {
    std::uint64_t mag = v < 0 ? 0 - static_cast<std::uint64_t>(v) : static_cast<std::uint64_t>(v);
    std::printf("%s%llu.%06llu", v < 0 ? "-" : "",
                static_cast<unsigned long long>(mag / kSignalScale),
                static_cast<unsigned long long>(mag % kSignalScale));
}

// Shared by `replay` and `xdp-listen` so the loopback test's two outputs
// stay the same shape.
inline void print_signals(const BookSignals& s) {
    std::printf("mid: ");
    if (s.two_sided()) print_scaled(s.mid_scaled()); else std::printf("(n/a)");
    std::printf("\nmicroprice: ");
    if (s.two_sided()) print_scaled(s.microprice); else std::printf("(n/a)");
    std::printf("\n");
    for (std::size_t d = 0; d < kNumImbalanceDepths; ++d) {
        std::printf("imbalance@%zu: ", kImbalanceDepths[d]);
        print_scaled(s.imbalance[d]);
        std::printf("\n");
    }
}

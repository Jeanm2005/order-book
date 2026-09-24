// Phase 3 tests: signal layer (microprice, order book imbalance at multiple
// depths) and the OrderBook::top_levels() depth view it reads from.
//
// Three layers of checking:
//   1. The pure fixed-point formulas against hand-computed values.
//   2. Small deterministic books against hand-computed signals.
//   3. Randomized: after every add/cancel, the book's signals must equal
//      signals computed from an independently tracked model of what should
//      be resting, AND must mirror a second book fed the same flow with
//      sides swapped and prices negated (which also checks the matching
//      engine itself is side-symmetric: identical fills, mirrored book).
//
// Same self-contained CHECK harness as the Phase 0/1 tests.

#include "signals.hpp"
#include "order_book.hpp"
#include <cstdio>
#include <cstdint>
#include <map>
#include <random>
#include <unordered_map>
#include <vector>

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", expr, file, line);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

constexpr std::int64_t S = kSignalScale;

// ---------------------------------------------------------------------
// Independent model of resting orders, driven by the fill callback.
// ---------------------------------------------------------------------
struct ModelOrder {
    Side side;
    Price price;
    Quantity remaining;
    bool canceled = false;
};

struct Model {
    std::unordered_map<OrderId, ModelOrder> orders;
    std::vector<Fill> fills;
};

Model* g_model = nullptr;
Model* g_mirror_model = nullptr;

void apply_fill(Model* m, const Fill& f) {
    if (!m) return;
    m->fills.push_back(f);
    m->orders[f.resting_id].remaining -= f.qty;
    m->orders[f.incoming_id].remaining -= f.qty;
}

void record_fill(const Fill& f) { apply_fill(g_model, f); }
void record_mirror_fill(const Fill& f) { apply_fill(g_mirror_model, f); }

// Signals as they *should* be, computed from the model rather than from the
// book: aggregate resting qty per price, take the best kMaxSignalDepth
// levels per side, run the same (separately tested) formulas.
BookSignals reference_signals(const Model& m) {
    std::map<Price, Quantity, std::greater<Price>> bids;
    std::map<Price, Quantity> asks;
    for (const auto& [id, o] : m.orders) {
        if (o.canceled || o.remaining <= 0) continue;
        if (o.side == Side::Buy) bids[o.price] += o.remaining;
        else asks[o.price] += o.remaining;
    }

    BookSignals s;
    if (!bids.empty()) { s.best_bid = bids.begin()->first; s.bid_qty = bids.begin()->second; }
    if (!asks.empty()) { s.best_ask = asks.begin()->first; s.ask_qty = asks.begin()->second; }
    if (s.two_sided()) s.microprice = microprice_scaled(s.best_bid, s.bid_qty, s.best_ask, s.ask_qty);

    for (std::size_t d = 0; d < kNumImbalanceDepths; ++d) {
        Quantity qb = 0, qa = 0;
        std::size_t n = 0;
        for (auto it = bids.begin(); it != bids.end() && n < kImbalanceDepths[d]; ++it, ++n) qb += it->second;
        n = 0;
        for (auto it = asks.begin(); it != asks.end() && n < kImbalanceDepths[d]; ++it, ++n) qa += it->second;
        s.imbalance[d] = imbalance_scaled(qb, qa);
    }
    return s;
}

bool same_signals(const BookSignals& a, const BookSignals& b) {
    if (a.bid_qty != b.bid_qty || a.ask_qty != b.ask_qty) return false;
    if (a.has_bid() && a.best_bid != b.best_bid) return false;
    if (a.has_ask() && a.best_ask != b.best_ask) return false;
    if (a.microprice != b.microprice) return false;
    for (std::size_t d = 0; d < kNumImbalanceDepths; ++d)
        if (a.imbalance[d] != b.imbalance[d]) return false;
    return true;
}

// Invariants any valid signal snapshot must satisfy, regardless of flow.
void check_signal_invariants(const BookSignals& s) {
    if (s.two_sided()) {
        CHECK(s.best_bid < s.best_ask); // resting book is never crossed
        CHECK(s.microprice >= s.best_bid * S);
        CHECK(s.microprice <= s.best_ask * S);
        CHECK(s.mid_scaled() > s.best_bid * S && s.mid_scaled() < s.best_ask * S);
    } else {
        CHECK(s.microprice == 0);
    }
    for (std::size_t d = 0; d < kNumImbalanceDepths; ++d) {
        CHECK(s.imbalance[d] >= -S && s.imbalance[d] <= S);
        if (!s.has_bid() && s.has_ask()) CHECK(s.imbalance[d] == -S);
        if (s.has_bid() && !s.has_ask()) CHECK(s.imbalance[d] == S);
        if (!s.has_bid() && !s.has_ask()) CHECK(s.imbalance[d] == 0);
    }
}

} // namespace

// ---------------------------------------------------------------------
// 1. Pure formulas, hand-computed.
// ---------------------------------------------------------------------
static void test_formulas() {
    // 100 x 30 / 102 x 10: bid-heavy pulls toward ask. 100 + 2*30/40 = 101.5
    CHECK(microprice_scaled(100, 30, 102, 10) == 101'500'000);
    // Ask-heavy: 100 + 2*10/40 = 100.5
    CHECK(microprice_scaled(100, 10, 102, 30) == 100'500'000);
    // Equal size == mid.
    CHECK(microprice_scaled(100, 7, 101, 7) == 100'500'000);
    // Non-terminating: 100 + 1*1/3 = 100.333333 (floored).
    CHECK(microprice_scaled(100, 1, 101, 2) == 100'333'333);
    // Negative prices floor the same way (offset is always non-negative).
    CHECK(microprice_scaled(-101, 1, -100, 2) == -100'666'667);

    CHECK(imbalance_scaled(30, 10) == 500'000);
    CHECK(imbalance_scaled(10, 30) == -500'000);
    CHECK(imbalance_scaled(1, 2) == -333'333);
    CHECK(imbalance_scaled(2, 1) == 333'333); // exact antisymmetry under truncation
    CHECK(imbalance_scaled(5, 0) == S);
    CHECK(imbalance_scaled(0, 5) == -S);
    CHECK(imbalance_scaled(0, 0) == 0);
    CHECK(imbalance_scaled(9, 9) == 0);

    // Range: large prices/quantities must not overflow the intermediates.
    constexpr Price big = 9'000'000'000'000; // ~9e12 ticks, near the documented limit
    constexpr Quantity bigq = 4'000'000'000'000'000'000;
    CHECK(microprice_scaled(big, bigq, big + 2, bigq) == (big + 1) * S);
    CHECK(imbalance_scaled(bigq, bigq / 4 * 3) == 142'857); // (1 - .75)/(1.75)
}

// ---------------------------------------------------------------------
// 2. Deterministic books.
// ---------------------------------------------------------------------
static void test_top_levels_and_depth_imbalance() {
    OrderBook<64> ob(nullptr);

    // Bids: 100x10 (two orders 4+6), 99x5, 97x20, 96x1, 95x3, 94x100 (6th level, beyond depth 5)
    ob.add_order(1, Side::Buy, 100, 4, 1);
    ob.add_order(2, Side::Buy, 100, 6, 2);
    ob.add_order(3, Side::Buy, 99, 5, 3);
    ob.add_order(4, Side::Buy, 97, 20, 4);
    ob.add_order(5, Side::Buy, 96, 1, 5);
    ob.add_order(6, Side::Buy, 95, 3, 6);
    ob.add_order(7, Side::Buy, 94, 100, 7);
    // Asks: 102x30, 104x10 (only two levels)
    ob.add_order(8, Side::Sell, 102, 30, 8);
    ob.add_order(9, Side::Sell, 104, 10, 9);

    LevelQty lv[8];
    std::size_t n = ob.top_levels(Side::Buy, lv, 8);
    CHECK(n == 6);
    if (n == 6) {
        CHECK(lv[0].price == 100 && lv[0].qty == 10); // same-price orders aggregated
        CHECK(lv[1].price == 99 && lv[1].qty == 5);
        CHECK(lv[2].price == 97 && lv[2].qty == 20);
        CHECK(lv[5].price == 94 && lv[5].qty == 100);
    }
    CHECK(ob.top_levels(Side::Buy, lv, 2) == 2); // respects max_levels
    CHECK(ob.top_levels(Side::Sell, lv, 8) == 2);
    CHECK(lv[0].price == 102 && lv[1].price == 104); // asks best (lowest) first

    BookSignals s;
    compute_signals(ob, s);
    CHECK(s.best_bid == 100 && s.bid_qty == 10);
    CHECK(s.best_ask == 102 && s.ask_qty == 30);
    CHECK(s.mid_scaled() == 101 * S);
    CHECK(s.microprice == microprice_scaled(100, 10, 102, 30)); // 100.5
    CHECK(s.microprice == 100'500'000);
    CHECK(s.imbalance[0] == imbalance_scaled(10, 30));             // depth 1
    CHECK(s.imbalance[1] == imbalance_scaled(10 + 5 + 20, 40));    // depth 3, asks run out at 2
    CHECK(s.imbalance[2] == imbalance_scaled(10 + 5 + 20 + 1 + 3, 40)); // depth 5, excludes 94x100
    check_signal_invariants(s);

    // A cross that consumes the whole best ask level moves every signal.
    ob.add_order(10, Side::Buy, 102, 30, 10);
    compute_signals(ob, s);
    CHECK(s.best_bid == 100 && s.best_ask == 104 && s.ask_qty == 10);
    CHECK(s.imbalance[0] == imbalance_scaled(10, 10) && s.imbalance[0] == 0);
    check_signal_invariants(s);
}

static void test_one_sided_and_empty() {
    OrderBook<16> ob(nullptr);
    BookSignals s;

    compute_signals(ob, s);
    CHECK(!s.has_bid() && !s.has_ask());
    check_signal_invariants(s);

    ob.add_order(1, Side::Sell, 50, 5, 1);
    compute_signals(ob, s);
    CHECK(!s.has_bid() && s.has_ask() && s.best_ask == 50);
    check_signal_invariants(s); // imbalance == -1 at every depth

    CHECK(ob.cancel_order(1));
    compute_signals(ob, s);
    CHECK(!s.has_bid() && !s.has_ask());
    check_signal_invariants(s);
}

// ---------------------------------------------------------------------
// 3. Randomized: model equivalence + mirror symmetry after every op.
// ---------------------------------------------------------------------
static void test_signals_random(std::uint64_t seed, int num_ops) {
    Model model, mirror_model;
    g_model = &model;
    g_mirror_model = &mirror_model;
    OrderBook<4096> ob(&record_fill);
    OrderBook<4096> mirror(&record_mirror_fill);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_dist(90, 110); // wider than 5 levels/side
    std::uniform_int_distribution<int> qty_dist(1, 20);
    std::uniform_int_distribution<int> action_dist(0, 99);

    std::vector<OrderId> ids;
    OrderId next_id = 1;
    int mismatches = 0;

    for (int i = 0; i < num_ops; ++i) {
        if (ids.empty() || action_dist(rng) < 70) {
            OrderId id = next_id++;
            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            Side mside = side == Side::Buy ? Side::Sell : Side::Buy;
            Price px = price_dist(rng);
            Quantity qty = qty_dist(rng);
            model.orders[id] = ModelOrder{side, px, qty};
            mirror_model.orders[id] = ModelOrder{mside, -px, qty};
            ob.add_order(id, side, px, qty, static_cast<Nanos>(i));
            mirror.add_order(id, mside, -px, qty, static_cast<Nanos>(i));
            ids.push_back(id);
        } else {
            std::uniform_int_distribution<std::size_t> pick(0, ids.size() - 1);
            OrderId id = ids[pick(rng)];
            bool a = ob.cancel_order(id);
            bool b = mirror.cancel_order(id);
            CHECK(a == b);
            if (a) model.orders[id].canceled = true;
            if (b) mirror_model.orders[id].canceled = true;
        }

        BookSignals s, ms;
        compute_signals(ob, s);
        compute_signals(mirror, ms);
        check_signal_invariants(s);

        // Book-derived signals == model-derived signals. Count rather than
        // CHECK per op so one divergence doesn't print thousands of lines.
        if (!same_signals(s, reference_signals(model))) ++mismatches;

        // Mirror: bid side of one is the negated ask side of the other.
        CHECK(s.bid_qty == ms.ask_qty && s.ask_qty == ms.bid_qty);
        if (s.has_bid()) CHECK(s.best_bid == -ms.best_ask);
        if (s.has_ask()) CHECK(s.best_ask == -ms.best_bid);
        for (std::size_t d = 0; d < kNumImbalanceDepths; ++d)
            CHECK(s.imbalance[d] == -ms.imbalance[d]); // exact: truncation is symmetric
        if (s.two_sided()) {
            // Exact value is spread*S split into two floored parts, so the
            // mirrored microprices sum to 0 or -1 (one unit of floor loss).
            std::int64_t sum = s.microprice + ms.microprice;
            CHECK(sum == 0 || sum == -1);
            CHECK(s.mid_scaled() == -ms.mid_scaled());
        }
    }

    CHECK(mismatches == 0);

    // Matching engine symmetry: identical fill sequence, prices negated.
    CHECK(model.fills.size() == mirror_model.fills.size());
    if (model.fills.size() == mirror_model.fills.size()) {
        for (std::size_t i = 0; i < model.fills.size(); ++i) {
            const Fill& f = model.fills[i];
            const Fill& g = mirror_model.fills[i];
            CHECK(f.resting_id == g.resting_id && f.incoming_id == g.incoming_id &&
                  f.qty == g.qty && f.price == -g.price);
        }
    }

    g_model = nullptr;
    g_mirror_model = nullptr;
}

int main() {
    test_formulas();
    test_top_levels_and_depth_imbalance();
    test_one_sided_and_empty();

    std::uint64_t seeds[] = {1, 2, 3, 42, 1337, 90210, 777, 20260923};
    for (std::uint64_t seed : seeds) {
        test_signals_random(seed, 2000);
    }

    if (g_failures == 0) {
        std::puts("ALL TESTS PASSED");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(S) FAILED\n", g_failures);
    return 1;
}

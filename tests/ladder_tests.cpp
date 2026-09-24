// Phase 4 container-swap tests: the flat-ladder OrderBook (LevelLadder hot
// window + map tail) must be observationally identical to the std::map
// OrderBook it replaced, preserved verbatim as tests/support/
// map_order_book.hpp.
//
// The Phase 0-3 suites run unchanged against the new container, but their
// prices (90..110) never leave the default 1024-tick window, so they never
// touch the tail, recentering, or window<->tail migration. These tests
// use a 64-tick window and flows built to hit exactly those paths, and
// check after EVERY op: identical fill stream, cancel results, best bid/ask,
// full depth (every level, both sides), and total resting qty.

#include "order_book.hpp"
#include "signals.hpp"
#include "support/map_order_book.hpp"
#include <cstdio>
#include <cstdint>
#include <random>
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

std::vector<Fill> g_ladder_fills;
std::vector<Fill> g_map_fills;
void ladder_fill(const Fill& f) { g_ladder_fills.push_back(f); }
void map_fill(const Fill& f) { g_map_fills.push_back(f); }

bool same_fill(const Fill& a, const Fill& b) {
    return a.resting_id == b.resting_id && a.incoming_id == b.incoming_id &&
           a.price == b.price && a.qty == b.qty;
}

constexpr std::size_t kPool = 4096;
constexpr std::size_t kWindow = 64;
constexpr std::size_t kMaxDepth = 4096; // more than any test can create: compares the FULL book

// Runs the ladder book and the map reference in lock-step. Every op is
// applied to both and the observable state compared immediately after, so
// a failure points at the exact op that diverged.
struct Harness {
    OrderBook<kPool, kWindow> ladder{&ladder_fill};
    MapOrderBook<kPool> ref{&map_fill};
    int op = 0;
    int divergent_ops = 0;
    LevelQty a[kMaxDepth];
    LevelQty b[kMaxDepth];

    Harness() { g_ladder_fills.clear(); g_map_fills.clear(); }

    void add(OrderId id, Side side, Price px, Quantity qty) {
        ladder.add_order(id, side, px, qty, static_cast<Nanos>(op));
        ref.add_order(id, side, px, qty, static_cast<Nanos>(op));
        compare();
    }

    bool cancel(OrderId id) {
        bool x = ladder.cancel_order(id);
        bool y = ref.cancel_order(id);
        if (x != y) ++divergent_ops;
        compare();
        return x;
    }

    bool same_side(Side side) {
        std::size_t n = ladder.top_levels(side, a, kMaxDepth);
        std::size_t m = ref.top_levels(side, b, kMaxDepth);
        if (n != m) return false;
        for (std::size_t i = 0; i < n; ++i)
            if (a[i].price != b[i].price || a[i].qty != b[i].qty) return false;
        return true;
    }

    void compare() {
        ++op;
        bool ok = g_ladder_fills.size() == g_map_fills.size();
        for (std::size_t i = 0; ok && i < g_ladder_fills.size(); ++i)
            ok = same_fill(g_ladder_fills[i], g_map_fills[i]);

        Price p1 = 0, p2 = 0;
        bool h1 = ladder.best_bid(p1), h2 = ref.best_bid(p2);
        ok = ok && h1 == h2 && (!h1 || p1 == p2);
        h1 = ladder.best_ask(p1); h2 = ref.best_ask(p2);
        ok = ok && h1 == h2 && (!h1 || p1 == p2);

        ok = ok && same_side(Side::Buy) && same_side(Side::Sell);
        ok = ok && ladder.total_resting_qty() == ref.total_resting_qty();

        BookSignals s1, s2;
        compute_signals(ladder, s1);
        compute_signals(ref, s2);
        ok = ok && s1.microprice == s2.microprice;
        for (std::size_t d = 0; d < kNumImbalanceDepths; ++d)
            ok = ok && s1.imbalance[d] == s2.imbalance[d];

        if (!ok) {
            if (divergent_ops == 0) std::fprintf(stderr, "first divergence at op %d\n", op);
            ++divergent_ops;
        }
        // Keep the fill logs short: once compared equal they carry no info.
        if (ok) { g_ladder_fills.clear(); g_map_fills.clear(); }
    }
};

Price best_or(OrderBook<kPool, kWindow>& ob, Side side, Price fallback) {
    Price p;
    bool have = side == Side::Buy ? ob.best_bid(p) : ob.best_ask(p);
    return have ? p : fallback;
}

} // namespace

// ---------------------------------------------------------------------
// Directed: window drains while the tail holds a BETTER price than where
// the window re-anchors. best(), depth order, and matching must all come
// from the tail first.
// ---------------------------------------------------------------------
static void test_tail_better_than_window() {
    Harness h;
    h.add(1, Side::Buy, 1000, 5); // anchors bid window around 1000
    h.add(2, Side::Buy, 900, 5);  // worse side, outside window -> tail
    CHECK(h.cancel(1));           // window now empty, tail holds 900
    h.add(3, Side::Buy, 10, 5);   // empty window -> re-anchors around 10; 900 stays in tail

    Price bb = 0;
    CHECK(h.ladder.best_bid(bb) && bb == 900);
    LevelQty lv[4];
    CHECK(h.ladder.top_levels(Side::Buy, lv, 4) == 2);
    CHECK(lv[0].price == 900 && lv[1].price == 10);

    h.add(4, Side::Sell, 5, 8);   // sweeps 900 (tail) fully, then 3 of 10 (window)
    CHECK(h.divergent_ops == 0);
    CHECK(h.ladder.best_bid(bb) && bb == 10);
}

// ---------------------------------------------------------------------
// Directed: a better-side insert recenters the window and evicts the
// levels that fall out into the tail; a later empty-window recenter pulls
// the ones back in that fit the new range, leaving the rest in the tail.
// Mirrored for asks.
// ---------------------------------------------------------------------
static void test_recenter_evict_and_migrate() {
    Harness h;
    h.add(1, Side::Buy, 100, 1); // window [68, 132)
    h.add(2, Side::Buy, 80, 2);
    h.add(3, Side::Buy, 70, 3);
    h.add(4, Side::Buy, 140, 4); // beyond better edge -> window [108, 172); 100/80/70 -> tail
    CHECK(h.cancel(4));          // window empty; best must come from tail (100)
    Price bb = 0;
    CHECK(h.ladder.best_bid(bb) && bb == 100);
    h.add(5, Side::Buy, 105, 5); // empty window -> [73, 137): 100, 80 migrate in, 70 stays tail

    LevelQty lv[8];
    CHECK(h.ladder.top_levels(Side::Buy, lv, 8) == 4);
    CHECK(lv[0].price == 105 && lv[1].price == 100 && lv[2].price == 80 && lv[3].price == 70);
    CHECK(lv[2].qty == 2 && lv[3].qty == 3);

    // Asks: better side is DOWN.
    h.add(10, Side::Sell, 500, 1); // window [468, 532)
    h.add(11, Side::Sell, 520, 2);
    h.add(12, Side::Sell, 530, 3);
    h.add(13, Side::Sell, 440, 4); // below window -> recenter [408, 472); 500/520/530 -> tail
    h.add(14, Side::Sell, 600, 5); // worse side -> tail
    CHECK(h.cancel(13));
    Price ba = 0;
    CHECK(h.ladder.best_ask(ba) && ba == 500);

    // A buy sweeping every ask level across tail + window, then every bid.
    h.add(20, Side::Buy, 1000, 100);
    h.add(21, Side::Sell, 1, 200);
    CHECK(h.divergent_ops == 0);
}

// ---------------------------------------------------------------------
// Randomized differential: a drifting mid with occasional large jumps,
// order offsets wider than the window, and occasional sweeping orders.
// ---------------------------------------------------------------------
static void test_differential_random(std::uint64_t seed, int num_ops, int jump_size) {
    Harness h;
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> drift(-3, 3);
    std::uniform_int_distribution<int> offset(0, 150); // > 2x the 64-tick window
    std::uniform_int_distribution<int> qty(1, 20);
    std::uniform_int_distribution<int> jump(-jump_size, jump_size);

    std::vector<OrderId> ids;
    OrderId next_id = 1;
    Price mid = 100'000;

    for (int i = 0; i < num_ops; ++i) {
        mid += drift(rng);
        if (pct(rng) < 1) mid += jump(rng);

        int action = pct(rng);
        if (ids.empty() || action < 60) {
            // Passive-ish: rests at mid -/+ offset, sometimes crosses.
            Side side = pct(rng) < 50 ? Side::Buy : Side::Sell;
            Price px = side == Side::Buy ? mid - offset(rng) + 2 : mid + offset(rng) - 2;
            h.add(next_id, side, px, qty(rng));
            ids.push_back(next_id++);
        } else if (action < 65) {
            // Aggressive sweep through several levels of the opposite side.
            Side side = pct(rng) < 50 ? Side::Buy : Side::Sell;
            Price touch = best_or(h.ladder, side == Side::Buy ? Side::Sell : Side::Buy, mid);
            Price px = side == Side::Buy ? touch + 20 : touch - 20;
            h.add(next_id, side, px, qty(rng) * 10);
            ids.push_back(next_id++);
        } else {
            std::uniform_int_distribution<std::size_t> pick(0, ids.size() - 1);
            h.cancel(ids[pick(rng)]);
        }
    }

    CHECK(h.divergent_ops == 0);
}

// ---------------------------------------------------------------------
// Directed, on LevelLadder itself: the recenter POLICY. Correctness can't
// see it (a tail level behaves identically), but if top of book stopped
// following into the flat window, the whole point of the swap is lost.
// ---------------------------------------------------------------------
static void test_window_follows_top_of_book() {
    Order o[8] = {};
    for (auto& x : o) x.quantity = 1;

    LevelLadder<true, 64> bids;
    bids.level_for_insert(100).push_back(&o[0]);
    CHECK(bids.in_hot_window(100));
    bids.level_for_insert(10).push_back(&o[1]);   // worse side: tail, no recenter
    CHECK(bids.in_hot_window(100) && !bids.in_hot_window(10));
    bids.level_for_insert(200).push_back(&o[2]);  // better side: window follows
    CHECK(bids.in_hot_window(200) && !bids.in_hot_window(100));
    CHECK(bids.best() && bids.best()->price == 200);

    LevelLadder<false, 64> asks;
    asks.level_for_insert(500).push_back(&o[3]);
    asks.level_for_insert(900).push_back(&o[4]);  // worse side for asks is UP
    CHECK(asks.in_hot_window(500) && !asks.in_hot_window(900));
    asks.level_for_insert(300).push_back(&o[5]);  // better side for asks is DOWN
    CHECK(asks.in_hot_window(300) && !asks.in_hot_window(500));
    CHECK(asks.best() && asks.best()->price == 300);
}

int main() {
    test_window_follows_top_of_book();
    test_tail_better_than_window();
    test_recenter_evict_and_migrate();

    std::uint64_t seeds[] = {1, 2, 3, 42, 1337, 90210, 777, 20260923};
    for (std::uint64_t seed : seeds) {
        test_differential_random(seed, 3000, 60);    // jumps ~ one window
        test_differential_random(seed, 3000, 2000);  // jumps far past it
    }

    if (g_failures == 0) {
        std::puts("ALL TESTS PASSED");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(S) FAILED\n", g_failures);
    return 1;
}

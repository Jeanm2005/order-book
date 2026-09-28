// Orders the book refuses: a duplicate live id, and an add when the order
// pool is full. Either must leave the book exactly as it was (no fills,
// same depth, same resting qty), be counted in stats(), and never corrupt
// later operations. Ids of filled or canceled orders stay reusable.

#include "order_book.hpp"
#include <cstdio>
#include <cstdint>
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

std::vector<Fill> g_fills;
void record_fill(const Fill& f) { g_fills.push_back(f); }

template <typename Book>
std::vector<LevelQty> depth(const Book& ob, Side side) {
    std::vector<LevelQty> out(64);
    out.resize(ob.top_levels(side, out.data(), out.size()));
    return out;
}

template <typename Book>
bool same_depth(const Book& ob, const std::vector<LevelQty>& bids, const std::vector<LevelQty>& asks) {
    auto b = depth(ob, Side::Buy);
    auto a = depth(ob, Side::Sell);
    if (b.size() != bids.size() || a.size() != asks.size()) return false;
    for (std::size_t i = 0; i < b.size(); ++i)
        if (b[i].price != bids[i].price || b[i].qty != bids[i].qty) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (a[i].price != asks[i].price || a[i].qty != asks[i].qty) return false;
    return true;
}

} // namespace

static void test_duplicate_live_id_rejected() {
    g_fills.clear();
    OrderBook<64> ob(&record_fill);
    CHECK(ob.add_order(1, Side::Buy, 100, 10, 0));
    CHECK(ob.add_order(2, Side::Sell, 105, 7, 1));
    auto bids = depth(ob, Side::Buy);
    auto asks = depth(ob, Side::Sell);

    // Same id as a resting bid, and marketable: must not trade.
    CHECK(!ob.add_order(1, Side::Buy, 110, 5, 2));
    // Same id as the resting ask.
    CHECK(!ob.add_order(2, Side::Buy, 99, 3, 3));
    CHECK(g_fills.empty());
    CHECK(same_depth(ob, bids, asks));
    CHECK(ob.total_resting_qty() == 17);
    CHECK(ob.stats().duplicate_id == 2);
    CHECK(ob.stats().pool_exhausted == 0);

    // The original order is still the one a cancel removes.
    CHECK(ob.cancel_order(1));
    CHECK(!ob.cancel_order(1));
    CHECK(ob.total_resting_qty() == 7);
}

static void test_ids_reusable_after_fill_or_cancel() {
    g_fills.clear();
    OrderBook<64> ob(&record_fill);
    CHECK(ob.add_order(1, Side::Sell, 100, 5, 0));
    CHECK(ob.add_order(2, Side::Buy, 100, 5, 1)); // fills 1 fully, 2 never rests
    CHECK(g_fills.size() == 1);
    CHECK(ob.add_order(1, Side::Buy, 90, 3, 2));  // 1 is gone: reusable
    CHECK(ob.add_order(2, Side::Buy, 91, 3, 3));  // 2 never rested: reusable
    CHECK(ob.cancel_order(1));
    CHECK(ob.add_order(1, Side::Buy, 92, 4, 4));  // canceled: reusable
    CHECK(ob.stats().duplicate_id == 0);
    CHECK(ob.total_resting_qty() == 7);
}

static void test_pool_exhaustion_counted_and_harmless() {
    g_fills.clear();
    OrderBook<8> ob(&record_fill);
    for (OrderId id = 1; id <= 8; ++id) CHECK(ob.add_order(id, Side::Buy, 100 - static_cast<Price>(id), 1, id));
    auto bids = depth(ob, Side::Buy);
    auto asks = depth(ob, Side::Sell);

    // Pool full: even a marketable order is refused before matching.
    CHECK(!ob.add_order(9, Side::Sell, 1, 100, 9));
    CHECK(!ob.add_order(10, Side::Buy, 50, 1, 10));
    CHECK(g_fills.empty());
    CHECK(same_depth(ob, bids, asks));
    CHECK(ob.stats().pool_exhausted == 2);
    CHECK(ob.stats().duplicate_id == 0);

    // Free one slot and the book accepts again; the refused ids were never
    // entered, so they are free too.
    CHECK(ob.cancel_order(8));
    CHECK(ob.add_order(9, Side::Sell, 99, 1, 11)); // crosses best bid 99 (id 1)
    CHECK(g_fills.size() == 1 && g_fills[0].resting_id == 1 && g_fills[0].price == 99);
    CHECK(ob.total_resting_qty() == 6);
}

int main() {
    test_duplicate_live_id_rejected();
    test_ids_reusable_after_fill_or_cancel();
    test_pool_exhaustion_counted_and_harmless();

    if (g_failures == 0) {
        std::puts("ALL TESTS PASSED");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(S) FAILED\n", g_failures);
    return 1;
}

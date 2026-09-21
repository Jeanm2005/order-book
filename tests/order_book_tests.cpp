// Phase 0 correctness tests (see README.md roadmap): quantity conservation,
// price-time priority, no phantom fills. Pure userspace, no networking —
// nothing else matters until this is solid.
//
// No external test framework: a small self-contained CHECK harness plus a
// randomized property test. Run via `ctest` or the binary directly; nonzero
// exit code means a check failed.

#include "order_book.hpp"
#include <cstdio>
#include <cstdint>
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

struct TestState {
    Quantity submitted = 0;
    Quantity filled = 0;
    bool canceled = false;
};

struct TestContext {
    std::vector<Fill> fills;
    std::unordered_map<OrderId, TestState> orders;
};

TestContext* g_ctx = nullptr;

// Raw function pointer target required by OrderBook::FillHandler (no
// captures allowed) — routes into whichever TestContext the running test
// installed. Tests run sequentially, single-threaded, so a single global
// pointer is enough.
void record_fill(const Fill& f) {
    if (!g_ctx) return;
    g_ctx->fills.push_back(f);

    CHECK(f.qty > 0);

    auto& resting = g_ctx->orders[f.resting_id];
    auto& incoming = g_ctx->orders[f.incoming_id];
    CHECK(resting.filled + f.qty <= resting.submitted);   // no phantom/over-fill
    CHECK(incoming.filled + f.qty <= incoming.submitted);
    resting.filled += f.qty;
    incoming.filled += f.qty;
}

template <std::size_t Cap>
void submit(OrderBook<Cap>& ob, TestContext& ctx, OrderId id, Side side,
            Price price, Quantity qty, Nanos ts) {
    ctx.orders[id] = TestState{qty, 0, false};
    ob.add_order(id, side, price, qty, ts);
}

// Sums (submitted - filled) over every non-canceled order the test tracked.
// Cross-checked against OrderBook::total_resting_qty() — the book's own
// internal accounting — to catch lost or duplicated quantity.
Quantity expected_resting(const TestContext& ctx) {
    Quantity total = 0;
    for (const auto& [id, st] : ctx.orders) {
        if (st.canceled) continue;
        Quantity remaining = st.submitted - st.filled;
        CHECK(remaining >= 0);
        total += remaining;
    }
    return total;
}

} // namespace

// ---------------------------------------------------------------------
// Deterministic: price-time priority within a level must never be violated.
// ---------------------------------------------------------------------
static void test_price_time_priority() {
    TestContext ctx;
    g_ctx = &ctx;
    OrderBook<64> ob(&record_fill);

    submit(ob, ctx, 1, Side::Buy, 100, 5, 1);
    submit(ob, ctx, 2, Side::Buy, 100, 5, 2);
    submit(ob, ctx, 3, Side::Buy, 100, 5, 3);

    // Incoming sell for 12 must hit id1 fully, then id2 fully, then id3
    // partially — strictly submission order, never id2 before id1 exhausted.
    submit(ob, ctx, 4, Side::Sell, 100, 12, 4);

    CHECK(ctx.fills.size() == 3);
    if (ctx.fills.size() == 3) {
        CHECK(ctx.fills[0].resting_id == 1 && ctx.fills[0].qty == 5);
        CHECK(ctx.fills[1].resting_id == 2 && ctx.fills[1].qty == 5);
        CHECK(ctx.fills[2].resting_id == 3 && ctx.fills[2].qty == 2);
    }
    CHECK(ob.total_resting_qty() == expected_resting(ctx));

    g_ctx = nullptr;
}

// ---------------------------------------------------------------------
// Deterministic: non-marketable orders must rest untouched, no phantom fill.
// ---------------------------------------------------------------------
static void test_no_cross_when_not_marketable() {
    TestContext ctx;
    g_ctx = &ctx;
    OrderBook<64> ob(&record_fill);

    submit(ob, ctx, 1, Side::Buy, 100, 10, 1);
    submit(ob, ctx, 2, Side::Sell, 105, 10, 2); // above best bid, must not cross

    CHECK(ctx.fills.empty());
    CHECK(ob.total_resting_qty() == 20);
    CHECK(expected_resting(ctx) == 20);

    g_ctx = nullptr;
}

// ---------------------------------------------------------------------
// Regression test for the PriceLevel::remove() doubly-linked-list bug:
// removing a non-tail node must fix the *next* node's prev pointer, and
// removing the tail must update PriceLevel::tail. The pool recycles freed
// slots LIFO, so a stale pointer left behind by a broken remove() would
// point at whatever order gets allocated next — this forces that reuse
// deterministically and checks the unrelated order it lands on stays intact.
// ---------------------------------------------------------------------
static void test_cancel_relinks_correctly_after_pool_reuse() {
    TestContext ctx;
    g_ctx = &ctx;
    OrderBook<8> ob(&record_fill); // small pool -> deterministic LIFO slot reuse

    submit(ob, ctx, 10, Side::Buy, 100, 5, 1); // A
    submit(ob, ctx, 11, Side::Buy, 100, 5, 2); // B
    submit(ob, ctx, 12, Side::Buy, 100, 5, 3); // C
    submit(ob, ctx, 14, Side::Buy, 100, 5, 4); // E

    CHECK(ob.cancel_order(11)); // cancel B (middle) -> frees B's pool slot, list: A->C->E
    ctx.orders[11].canceled = true;

    // D is the very next allocation: deterministically reuses B's freed slot.
    submit(ob, ctx, 13, Side::Buy, 90, 7, 5); // D, different price level

    CHECK(ob.cancel_order(12)); // cancel C -> exercises the next->prev relink fix
    ctx.orders[12].canceled = true;
    // If remove() were still buggy, C->prev would be the stale pointer into
    // B's old (now D's) memory, and this cancel would corrupt D's ->next.

    submit(ob, ctx, 20, Side::Sell, 100, 5, 6); // should fill A only
    submit(ob, ctx, 21, Side::Sell, 90, 7, 7);  // should fill E fully, then D partially

    CHECK(ctx.fills.size() == 3);
    if (ctx.fills.size() == 3) {
        CHECK(ctx.fills[0].resting_id == 10 && ctx.fills[0].qty == 5 && ctx.fills[0].price == 100);
        CHECK(ctx.fills[1].resting_id == 14 && ctx.fills[1].qty == 5 && ctx.fills[1].price == 100);
        CHECK(ctx.fills[2].resting_id == 13 && ctx.fills[2].qty == 2 && ctx.fills[2].price == 90);
    }

    CHECK(ob.total_resting_qty() == 5); // D has 5 left resting, nothing else
    CHECK(ob.total_resting_qty() == expected_resting(ctx));

    g_ctx = nullptr;
}

// ---------------------------------------------------------------------
// Deterministic: canceling an id twice, or an id that already fully
// filled, must fail cleanly (no double release, no crash).
// ---------------------------------------------------------------------
static void test_cancel_unknown_or_already_gone() {
    TestContext ctx;
    g_ctx = &ctx;
    OrderBook<64> ob(&record_fill);

    submit(ob, ctx, 1, Side::Buy, 100, 5, 1);
    CHECK(ob.cancel_order(1));
    CHECK(!ob.cancel_order(1));  // already canceled
    CHECK(!ob.cancel_order(99)); // never existed

    submit(ob, ctx, 2, Side::Buy, 100, 5, 2);
    submit(ob, ctx, 3, Side::Sell, 100, 5, 3); // fully fills id2
    CHECK(!ob.cancel_order(2));  // already fully filled, no longer resting

    g_ctx = nullptr;
}

// ---------------------------------------------------------------------
// Randomized property test: quantity conservation and no phantom fills
// across many random add/cancel sequences.
// submitted == filled + canceled-away + still-resting, for every order,
// and the book's own total_resting_qty() must match the independently
// tracked sum.
// ---------------------------------------------------------------------
static void test_quantity_conservation_random(std::uint64_t seed, int num_ops) {
    TestContext ctx;
    g_ctx = &ctx;
    OrderBook<4096> ob(&record_fill);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_dist(95, 105);
    std::uniform_int_distribution<int> qty_dist(1, 20);
    std::uniform_int_distribution<int> action_dist(0, 99); // <70 add, else cancel

    std::vector<OrderId> submitted_ids;
    OrderId next_id = 1;

    for (int i = 0; i < num_ops; ++i) {
        if (submitted_ids.empty() || action_dist(rng) < 70) {
            OrderId id = next_id++;
            Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            submit(ob, ctx, id, side, static_cast<Price>(price_dist(rng)),
                   static_cast<Quantity>(qty_dist(rng)), static_cast<Nanos>(i));
            submitted_ids.push_back(id);
        } else {
            std::uniform_int_distribution<std::size_t> pick(0, submitted_ids.size() - 1);
            OrderId id = submitted_ids[pick(rng)];
            if (ob.cancel_order(id)) {
                ctx.orders[id].canceled = true;
            }
        }
    }

    CHECK(ob.total_resting_qty() == expected_resting(ctx));

    g_ctx = nullptr;
}

int main() {
    test_price_time_priority();
    test_no_cross_when_not_marketable();
    test_cancel_relinks_correctly_after_pool_reuse();
    test_cancel_unknown_or_already_gone();

    // Multiple seeds/lengths for broader coverage of the random-op space.
    std::uint64_t seeds[] = {1, 2, 3, 42, 1337, 90210, 777, 20260921};
    for (std::uint64_t seed : seeds) {
        test_quantity_conservation_random(seed, 2000);
    }

    if (g_failures == 0) {
        std::puts("ALL TESTS PASSED");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(S) FAILED\n", g_failures);
    return 1;
}

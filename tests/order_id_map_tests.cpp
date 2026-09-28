// OrderIdMap: the OrderId -> Order* index behind every cancel and every
// fill-driven removal. Two properties:
//   1. It agrees with a reference std::unordered_map under long random
//      insert/erase/find churn (erase moves entries around, so this is the
//      check that nothing becomes unfindable).
//   2. Probe chains stay bounded by the live population no matter how long
//      the map has been churning. A miss (e.g. a cancel for an order that
//      already filled) must stop at an empty slot within size() + 1 probes.
//      Tombstone deletion broke this: empty slots were never restored, so
//      after enough churn every miss scanned the whole table.

#include "order_id_map.hpp"
#include <cstdint>
#include <cstdio>
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

// Distinct fake pointers per id; the map never dereferences them.
Order* fake_ptr(OrderId id) {
    return reinterpret_cast<Order*>(static_cast<std::uintptr_t>(id) * 64 + 64);
}

// Ids far from anything the churn tests insert.
constexpr OrderId kMissBase = OrderId{1} << 62;

} // namespace

// Steady state: a fixed live population, oldest erased as each new id
// arrives (FIFO churn, like resting orders filling or canceling in turn),
// for many times the table's capacity. Every miss must stay short.
template <std::size_t Cap>
static void test_churn_keeps_misses_short(std::size_t live, std::size_t churn_ops) {
    static OrderIdMap<Cap> m; // static: keep large tables off the stack
    std::vector<OrderId> ring(live);
    OrderId next = 1;
    for (std::size_t i = 0; i < live; ++i) {
        ring[i] = next;
        m.insert(next, fake_ptr(next));
        ++next;
    }

    std::size_t worst_miss = 0;
    for (std::size_t op = 0; op < churn_ops; ++op) {
        std::size_t slot = op % live;
        CHECK(m.erase(ring[slot]));
        ring[slot] = next;
        m.insert(next, fake_ptr(next));
        ++next;

        if (op % 97 == 0) {
            for (OrderId k = 0; k < 16; ++k) {
                OrderId miss = kMissBase + op * 16 + k;
                CHECK(m.find(miss) == nullptr);
                std::size_t p = m.probe_length(miss);
                if (p > worst_miss) worst_miss = p;
            }
        }
    }
    CHECK(m.size() == live);
    CHECK(worst_miss <= live + 1);
    if (worst_miss > live + 1) {
        std::fprintf(stderr, "  capacity %zu, %zu live: worst miss probed %zu slots\n",
                     Cap, live, worst_miss);
    }
    for (OrderId id : ring) CHECK(m.find(id) == fake_ptr(id));
}

// Random insert/erase/find against std::unordered_map. Small capacity so
// clusters wrap around the end of the table and erase has to shift
// entries across the wrap.
template <std::size_t Cap>
static void test_vs_reference(std::uint64_t seed, std::size_t max_live, std::size_t ops) {
    OrderIdMap<Cap> m;
    std::unordered_map<OrderId, Order*> ref;
    std::vector<OrderId> live;
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    // Small id universe on purpose: lots of reinsertion of recently erased ids.
    std::uniform_int_distribution<OrderId> id_dist(1, Cap * 4);

    for (std::size_t op = 0; op < ops; ++op) {
        int r = pct(rng);
        if (r < 45 && live.size() < max_live) {
            OrderId id = id_dist(rng);
            if (ref.count(id)) continue; // book never inserts a live id twice
            m.insert(id, fake_ptr(id));
            ref[id] = fake_ptr(id);
            live.push_back(id);
        } else if (r < 85 && !live.empty()) {
            std::size_t k = std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng);
            OrderId id = live[k];
            CHECK(m.erase(id));
            ref.erase(id);
            live[k] = live.back();
            live.pop_back();
        } else {
            OrderId id = id_dist(rng);
            auto it = ref.find(id);
            Order* want = it == ref.end() ? nullptr : it->second;
            CHECK(m.find(id) == want);
            if (!want) CHECK(!m.erase(id));
        }

        CHECK(m.size() == ref.size());
        if (op % 1024 == 0) {
            for (const auto& [id, ptr] : ref) CHECK(m.find(id) == ptr);
        }
    }
    for (const auto& [id, ptr] : ref) CHECK(m.find(id) == ptr);
}

// Erase-then-reinsert of the same id, and erasing from the middle of a
// cluster, with a hand-checkable table.
static void test_small_edges() {
    OrderIdMap<8> m;
    for (OrderId id = 1; id <= 4; ++id) m.insert(id, fake_ptr(id));
    CHECK(m.size() == 4);
    CHECK(!m.erase(99));
    CHECK(m.erase(2));
    CHECK(!m.erase(2));
    CHECK(m.find(2) == nullptr);
    for (OrderId id : {OrderId{1}, OrderId{3}, OrderId{4}}) CHECK(m.find(id) == fake_ptr(id));
    m.insert(2, fake_ptr(2));
    for (OrderId id = 1; id <= 4; ++id) CHECK(m.find(id) == fake_ptr(id));
    for (OrderId id = 1; id <= 4; ++id) CHECK(m.erase(id));
    CHECK(m.size() == 0);
    for (OrderId id = 1; id <= 4; ++id) {
        CHECK(m.find(id) == nullptr);
        CHECK(m.probe_length(id) == 1); // empty table: first slot is empty
    }
}

int main() {
    test_small_edges();
    test_churn_keeps_misses_short<1024>(100, 50'000);
    // Same shape as OrderBook<1 << 16>'s map: 2^17 slots.
    test_churn_keeps_misses_short<1 << 17>(2'000, 600'000);
    for (std::uint64_t seed : {1ull, 42ull, 20260928ull}) {
        test_vs_reference<16>(seed, 8, 200'000);
        test_vs_reference<256>(seed, 128, 200'000);
        test_vs_reference<4096>(seed, 2048, 200'000);
    }

    if (g_failures == 0) {
        std::puts("ALL TESTS PASSED");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(S) FAILED\n", g_failures);
    return 1;
}

// AGENTS.md hot-path rule, checked mechanically: once an OrderBook is
// constructed, nothing an incoming order or cancel does may allocate —
// matching, resting, cancels, the tail, window recenters, and the signal
// computation that follows each update.
//
// Global operator new/delete are replaced with counting versions. Every
// flow is generated up front; the counter is armed only around the replay.
// A 64-tick window and a drifting, jumping mid force the tail and recenter
// paths, which the default 1024-tick window rarely reaches.
//
// Works under the Debug build's ASan too: ASan interposes malloc, and the
// replaced operator new sits above it.

#include "feed_message.hpp"
#include "feed_replay.hpp"
#include "order_book.hpp"
#include "signals.hpp"
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <random>
#include <vector>

namespace {

bool g_armed = false;
std::uint64_t g_allocs = 0;

} // namespace

void* operator new(std::size_t n) {
    if (g_armed) ++g_allocs;
    if (void* p = std::malloc(n ? n : 1)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) { return operator new(n); }
void* operator new(std::size_t n, std::align_val_t a) {
    if (g_armed) ++g_allocs;
    std::size_t al = static_cast<std::size_t>(a);
    if (void* p = std::aligned_alloc(al, (n + al - 1) / al * al)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n, std::align_val_t a) { return operator new(n, a); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }
void operator delete(void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::align_val_t) noexcept { std::free(p); }
void operator delete(void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t, std::align_val_t) noexcept { std::free(p); }

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", expr, file, line);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

std::uint64_t g_fills = 0;
void count_fill(const Fill&) { ++g_fills; }

// Drifting mid with occasional jumps of several windows, orders up to
// `depth` ticks from the mid, cancels aimed at ids believed live.
std::vector<FeedMessage> make_flow(std::uint64_t seed, std::size_t n, int depth) {
    std::vector<FeedMessage> msgs;
    msgs.reserve(n);
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> off(0, depth);
    std::uniform_int_distribution<int> jump(-400, 400);
    std::uniform_int_distribution<int> qty(1, 20);
    std::vector<OrderId> live;
    OrderId next = 1;
    Price mid = 1'000'000;
    for (std::size_t i = 0; i < n; ++i) {
        FeedMessage m{};
        m.ts = i;
        if (pct(rng) < 10) mid += pct(rng) < 50 ? -1 : 1;
        if (pct(rng) == 0) mid += jump(rng);
        if (live.empty() || pct(rng) < 55) {
            m.type = MsgType::AddOrder;
            m.id = next++;
            m.side = pct(rng) < 50 ? Side::Buy : Side::Sell;
            int o = off(rng);
            // A few marketable orders so matching sweeps several levels.
            if (pct(rng) < 3) o = -o / 4;
            m.price = m.side == Side::Buy ? mid - o : mid + 1 + o;
            m.qty = qty(rng);
            live.push_back(m.id);
        } else {
            std::size_t k = std::uniform_int_distribution<std::size_t>(0, live.size() - 1)(rng);
            m.type = MsgType::CancelOrder;
            m.id = live[k];
            live[k] = live.back();
            live.pop_back();
        }
        msgs.push_back(m);
    }
    return msgs;
}

template <typename Book>
std::uint64_t replay_counting(Book& book, const std::vector<FeedMessage>& msgs) {
    BookSignals sig;
    g_allocs = 0;
    g_armed = true;
    for (const FeedMessage& m : msgs) {
        apply_message(book, m);
        compute_signals(book, sig);
    }
    g_armed = false;
    return g_allocs;
}

} // namespace

static void test_no_alloc_small_window() {
    for (std::uint64_t seed : {1ull, 7ull, 42ull, 20260928ull}) {
        std::vector<FeedMessage> msgs = make_flow(seed, 200'000, 300);
        auto book = std::make_unique<OrderBook<1 << 16, 64>>(&count_fill);
        std::uint64_t allocs = replay_counting(*book, msgs);
        CHECK(allocs == 0);
        if (allocs) std::fprintf(stderr, "  seed %llu: %llu allocations on the hot path\n",
                                 static_cast<unsigned long long>(seed), static_cast<unsigned long long>(allocs));
    }
}

static void test_no_alloc_default_book() {
    std::vector<FeedMessage> msgs = make_flow(99, 200'000, 2'000);
    auto book = std::make_unique<OrderBook<>>(&count_fill);
    CHECK(replay_counting(*book, msgs) == 0);
}

// Sanity: the counter actually sees allocations when armed.
static void test_counter_works() {
    g_allocs = 0;
    g_armed = true;
    std::vector<int>* v = new std::vector<int>(10);
    g_armed = false;
    delete v;
    CHECK(g_allocs >= 1);
}

int main() {
    test_counter_works();
    test_no_alloc_small_window();
    test_no_alloc_default_book();
    std::printf("fills observed: %llu\n", static_cast<unsigned long long>(g_fills));
    CHECK(g_fills > 0);

    if (g_failures == 0) {
        std::puts("ALL TESTS PASSED");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(S) FAILED\n", g_failures);
    return 1;
}

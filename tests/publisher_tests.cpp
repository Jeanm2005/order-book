// Market-data publish path: MdPublisher over SpscRingBuffer.
//   1. Single-threaded ring semantics: FIFO, capacity - 1 usable slots,
//      a full ring drops (and counts) instead of blocking or overwriting.
//   2. Two threads, the real shape: the producer publishes a long numbered
//      sequence while a consumer drains it. Every received snapshot must be
//      whole (all fields from the same publish, i.e. no torn 64-byte slot),
//      in strictly increasing order, and published == received with
//      published + dropped == attempts.
//   3. The book pipeline end to end: the last snapshot the consumer sees
//      equals compute_signals() on the final book.

#include "feed_message.hpp"
#include "feed_replay.hpp"
#include "md_publisher.hpp"
#include "order_book.hpp"
#include "signals.hpp"
#include <atomic>
#include <cstdio>
#include <cstdint>
#include <memory>
#include <random>
#include <thread>
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

// Every field derived from one sequence number, so a torn read (fields
// from two different publishes) is detectable.
BookSignals numbered(std::int64_t n) {
    BookSignals s;
    s.best_bid = n;
    s.best_ask = n + 1;
    s.bid_qty = n * 3 + 1;
    s.ask_qty = n * 5 + 2;
    s.microprice = n * 7;
    for (std::size_t d = 0; d < kNumImbalanceDepths; ++d) s.imbalance[d] = n * 11 + static_cast<std::int64_t>(d);
    return s;
}

bool is_whole(const BookSignals& s) {
    const std::int64_t n = s.best_bid;
    if (s.best_ask != n + 1 || s.bid_qty != n * 3 + 1 || s.ask_qty != n * 5 + 2 || s.microprice != n * 7) return false;
    for (std::size_t d = 0; d < kNumImbalanceDepths; ++d)
        if (s.imbalance[d] != n * 11 + static_cast<std::int64_t>(d)) return false;
    return true;
}

bool same_signals(const BookSignals& a, const BookSignals& b) {
    if (a.best_bid != b.best_bid || a.best_ask != b.best_ask || a.bid_qty != b.bid_qty ||
        a.ask_qty != b.ask_qty || a.microprice != b.microprice) return false;
    for (std::size_t d = 0; d < kNumImbalanceDepths; ++d)
        if (a.imbalance[d] != b.imbalance[d]) return false;
    return true;
}

} // namespace

static void test_single_thread_semantics() {
    auto pub = std::make_unique<MdPublisher<8>>();
    BookSignals out;
    CHECK(!pub->poll(out));
    CHECK(pub->usable_slots() == 7);
    for (std::int64_t i = 0; i < 7; ++i) CHECK(pub->publish(numbered(i)));
    CHECK(!pub->publish(numbered(7)));      // full: dropped, not overwritten
    CHECK(pub->published() == 7 && pub->dropped() == 1);
    for (std::int64_t i = 0; i < 7; ++i) {
        CHECK(pub->poll(out));
        CHECK(is_whole(out) && out.best_bid == i);
    }
    CHECK(!pub->poll(out));
    // Wraps around the ring correctly.
    for (std::int64_t i = 100; i < 150; ++i) {
        CHECK(pub->publish(numbered(i)));
        CHECK(pub->poll(out) && out.best_bid == i && is_whole(out));
    }
}

template <std::size_t Ring>
static void test_two_threads(std::int64_t count, bool retry_when_full) {
    auto pub = std::make_unique<MdPublisher<Ring>>();
    std::atomic<bool> done{false};
    std::int64_t received = 0, last = -1;
    bool order_ok = true, whole_ok = true;

    std::thread consumer([&] {
        BookSignals s;
        for (;;) {
            if (pub->poll(s)) {
                ++received;
                if (!is_whole(s)) whole_ok = false;
                if (s.best_bid <= last) order_ok = false;
                last = s.best_bid;
            } else if (done.load(std::memory_order_acquire)) {
                if (!pub->poll(s)) return;
                ++received;
                if (!is_whole(s)) whole_ok = false;
                if (s.best_bid <= last) order_ok = false;
                last = s.best_bid;
            }
        }
    });

    std::uint64_t attempts = 0;
    for (std::int64_t i = 0; i < count; ++i) {
        ++attempts;
        if (retry_when_full) {
            while (!pub->publish(numbered(i))) ++attempts;
        } else {
            pub->publish(numbered(i));
        }
    }
    done.store(true, std::memory_order_release);
    consumer.join();

    CHECK(whole_ok);
    CHECK(order_ok);
    CHECK(static_cast<std::uint64_t>(received) == pub->published());
    CHECK(pub->published() + pub->dropped() == attempts);
    if (retry_when_full) {
        CHECK(received == count);          // nothing lost when the producer retries
        CHECK(last == count - 1);
    }
}

std::uint64_t g_fills = 0;
void count_fill(const Fill&) { ++g_fills; }

static void test_book_pipeline_last_snapshot() {
    std::vector<FeedMessage> msgs;
    std::mt19937_64 rng(5);
    std::uniform_int_distribution<int> pct(0, 99), px(95, 105), q(1, 20);
    for (std::size_t i = 0; i < 50'000; ++i) {
        FeedMessage m{};
        m.ts = i;
        if (i < 10 || pct(rng) < 80) {
            m.type = MsgType::AddOrder;
            m.id = i + 1;
            m.side = pct(rng) < 50 ? Side::Buy : Side::Sell;
            m.price = px(rng);
            m.qty = q(rng);
        } else {
            m.type = MsgType::CancelOrder;
            m.id = std::uniform_int_distribution<std::uint64_t>(1, i)(rng);
        }
        msgs.push_back(m);
    }

    auto book = std::make_unique<OrderBook<1 << 16>>(&count_fill);
    auto pub = std::make_unique<MdPublisher<1 << 10>>();
    std::atomic<bool> done{false};
    BookSignals last_seen;
    std::uint64_t received = 0;
    std::thread consumer([&] {
        BookSignals s;
        for (;;) {
            if (pub->poll(s)) { last_seen = s; ++received; }
            else if (done.load(std::memory_order_acquire)) {
                while (pub->poll(s)) { last_seen = s; ++received; }
                return;
            }
        }
    });

    BookSignals sig;
    for (const FeedMessage& m : msgs) {
        apply_message(*book, m);
        compute_signals(*book, sig);
        while (!pub->publish(sig)) {} // this test wants every snapshot delivered
    }
    done.store(true, std::memory_order_release);
    consumer.join();

    BookSignals final_sig;
    compute_signals(*book, final_sig);
    CHECK(received == msgs.size());
    CHECK(same_signals(last_seen, final_sig));
}

int main() {
    test_single_thread_semantics();
    test_two_threads<64>(2'000'000, true);
    test_two_threads<64>(2'000'000, false);
    test_two_threads<4096>(2'000'000, false);
    test_book_pipeline_last_snapshot();

    if (g_failures == 0) {
        std::puts("ALL TESTS PASSED");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(S) FAILED\n", g_failures);
    return 1;
}

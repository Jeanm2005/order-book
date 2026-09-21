// Phase 1 tests: feed message round-trip through the file format, and
// replay determinism (same log replayed twice -> identical fill sequence
// and identical final book state).

#include "feed_replay.hpp"
#include "order_book.hpp"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>
#include <unistd.h>

namespace {

int g_failures = 0;

void check(bool cond, const char* expr, const char* file, int line) {
    if (!cond) {
        std::fprintf(stderr, "CHECK FAILED: %s (%s:%d)\n", expr, file, line);
        ++g_failures;
    }
}

#define CHECK(cond) check((cond), #cond, __FILE__, __LINE__)

bool same_message(const FeedMessage& a, const FeedMessage& b) {
    return a.type == b.type && a.side == b.side && a.id == b.id &&
           a.price == b.price && a.qty == b.qty && a.ts == b.ts;
}

std::vector<FeedMessage> make_sample_messages() {
    return {
        FeedMessage{MsgType::AddOrder, Side::Buy, 1, 100, 10, 1},
        FeedMessage{MsgType::AddOrder, Side::Sell, 2, 105, 7, 2},
        FeedMessage{MsgType::CancelOrder, Side::Buy, 1, 0, 0, 3},
        FeedMessage{MsgType::AddOrder, Side::Buy, 3, 104, 3, 4},
    };
}

std::vector<FeedMessage> generate_feed(std::uint64_t seed, std::size_t num_messages) {
    std::vector<FeedMessage> msgs;
    msgs.reserve(num_messages);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_dist(95, 105);
    std::uniform_int_distribution<int> qty_dist(1, 20);
    std::uniform_int_distribution<int> action_dist(0, 99);

    std::vector<OrderId> seen_ids;
    OrderId next_id = 1;

    for (std::size_t i = 0; i < num_messages; ++i) {
        FeedMessage msg{};
        msg.ts = static_cast<Nanos>(i);
        if (seen_ids.empty() || action_dist(rng) < 80) {
            msg.type = MsgType::AddOrder;
            msg.id = next_id++;
            msg.side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
            msg.price = static_cast<Price>(price_dist(rng));
            msg.qty = static_cast<Quantity>(qty_dist(rng));
            seen_ids.push_back(msg.id);
        } else {
            std::uniform_int_distribution<std::size_t> pick(0, seen_ids.size() - 1);
            msg.type = MsgType::CancelOrder;
            msg.id = seen_ids[pick(rng)];
        }
        msgs.push_back(msg);
    }
    return msgs;
}

std::filesystem::path unique_temp_path(const char* label) {
    auto dir = std::filesystem::temp_directory_path();
    auto name = std::string("order_book_test_") + label + "_" +
                std::to_string(static_cast<unsigned long long>(::getpid())) + ".feed";
    return dir / name;
}

} // namespace

// ---------------------------------------------------------------------
// Round trip: save a message sequence to a file, load it back, expect an
// exact match (header magic/count and every field of every record).
// ---------------------------------------------------------------------
static void test_feed_file_round_trip() {
    auto path = unique_temp_path("roundtrip");
    std::vector<FeedMessage> original = make_sample_messages();

    CHECK(save_feed_file(path.string(), original));

    std::vector<FeedMessage> loaded;
    CHECK(load_feed_file(path.string(), loaded));

    CHECK(loaded.size() == original.size());
    if (loaded.size() == original.size()) {
        for (std::size_t i = 0; i < original.size(); ++i) {
            CHECK(same_message(original[i], loaded[i]));
        }
    }

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------
// Loading a nonexistent file or one with a bad magic must fail cleanly,
// not crash or return a partially-populated vector.
// ---------------------------------------------------------------------
static void test_feed_file_bad_input() {
    std::vector<FeedMessage> out;
    CHECK(!load_feed_file("/nonexistent/path/does_not_exist.feed", out));
    CHECK(out.empty());

    auto path = unique_temp_path("badmagic");
    {
        std::FILE* f = std::fopen(path.string().c_str(), "wb");
        CHECK(f != nullptr);
        if (f) {
            FeedFileHeader bad_hdr;
            std::memcpy(bad_hdr.magic, "GARBAGE1", 8);
            bad_hdr.count = 0;
            std::fwrite(&bad_hdr, sizeof(bad_hdr), 1, f);
            std::fclose(f);
        }
    }
    CHECK(!load_feed_file(path.string(), out));
    CHECK(out.empty());

    std::error_code ec;
    std::filesystem::remove(path, ec);
}

// ---------------------------------------------------------------------
// Replay determinism: the same log replayed into two independent
// OrderBook instances must produce identical fill sequences and
// identical final resting quantity. This is what "replay runs the exact
// same matching code path as live" (README) needs to be true for.
// ---------------------------------------------------------------------
static void test_replay_determinism() {
    std::vector<FeedMessage> msgs = generate_feed(20260921, 3000);

    std::vector<Fill> fills_a, fills_b;

    // OrderBook::FillHandler is a raw function pointer (no captures), so
    // both runs route through one static sink pointer selected before each
    // replay — tests run single-threaded and sequentially, so this is safe.
    static std::vector<Fill>* g_sink = nullptr;
    struct Local {
        static void on_fill(const Fill& f) { g_sink->push_back(f); }
    };

    OrderBook<4096> ob_a(&Local::on_fill);
    g_sink = &fills_a;
    replay(ob_a, msgs);

    OrderBook<4096> ob_b(&Local::on_fill);
    g_sink = &fills_b;
    replay(ob_b, msgs);

    CHECK(fills_a.size() == fills_b.size());
    if (fills_a.size() == fills_b.size()) {
        for (std::size_t i = 0; i < fills_a.size(); ++i) {
            CHECK(fills_a[i].resting_id == fills_b[i].resting_id);
            CHECK(fills_a[i].incoming_id == fills_b[i].incoming_id);
            CHECK(fills_a[i].price == fills_b[i].price);
            CHECK(fills_a[i].qty == fills_b[i].qty);
        }
    }
    CHECK(ob_a.total_resting_qty() == ob_b.total_resting_qty());
    CHECK(!fills_a.empty()); // sanity: this feed shape should actually produce crosses
}

int main() {
    test_feed_file_round_trip();
    test_feed_file_bad_input();
    test_replay_determinism();

    if (g_failures == 0) {
        std::puts("ALL TESTS PASSED");
        return 0;
    }
    std::fprintf(stderr, "%d CHECK(S) FAILED\n", g_failures);
    return 1;
}

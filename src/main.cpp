#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "feed_message.hpp"
#include "feed_replay.hpp"
#include "order_book.hpp"
#include "signals.hpp"
#include "xdp_listen_cmd.hpp"

namespace {

// Generates a synthetic order-flow log: mostly Add orders spread over a
// tight price range (so there's plenty of crossing/matching on replay),
// with occasional cancels referencing an id seen earlier in the stream
// (which may or may not still be resting by the time the cancel lands —
// that's a legitimate no-op path the book already handles).
std::vector<FeedMessage> generate_feed(std::uint64_t seed, std::size_t num_messages) {
    std::vector<FeedMessage> msgs;
    msgs.reserve(num_messages);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> price_dist(95, 105);
    std::uniform_int_distribution<int> qty_dist(1, 20);
    std::uniform_int_distribution<int> action_dist(0, 99); // <80 add, else cancel

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

// Wider, more book-like flow for benchmarking the price-level container
// (Phase 4): the mid drifts, and resting orders spread up to `depth` ticks
// from it with most of them near the touch (min of two uniforms), so the
// book holds hundreds of live levels instead of the narrow profile's 11.
// Cancels only target ids still believed live, so most of them hit.
std::vector<FeedMessage> generate_wide_feed(std::uint64_t seed, std::size_t num_messages, int depth = 300) {
    std::vector<FeedMessage> msgs;
    msgs.reserve(num_messages);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99);
    std::uniform_int_distribution<int> drift(-1, 1);
    std::uniform_int_distribution<int> offset(0, depth);
    std::uniform_int_distribution<int> qty_dist(1, 20);

    std::vector<OrderId> live;
    OrderId next_id = 1;
    Price mid = 100'000;

    for (std::size_t i = 0; i < num_messages; ++i) {
        FeedMessage msg{};
        msg.ts = static_cast<Nanos>(i);
        if (pct(rng) < 10) mid += drift(rng);

        if (live.empty() || pct(rng) < 55) {
            msg.type = MsgType::AddOrder;
            msg.id = next_id++;
            msg.side = pct(rng) < 50 ? Side::Buy : Side::Sell;
            int off = std::min(offset(rng), offset(rng));
            msg.price = msg.side == Side::Buy ? mid - off : mid + 1 + off;
            msg.qty = static_cast<Quantity>(qty_dist(rng));
            live.push_back(msg.id);
        } else {
            std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
            std::size_t k = pick(rng);
            msg.type = MsgType::CancelOrder;
            msg.id = live[k];
            live[k] = live.back();
            live.pop_back();
        }
        msgs.push_back(msg);
    }
    return msgs;
}

// Fill accounting for the replay report. Raw function pointer target
// (OrderBook::FillHandler allows no captures), so state lives in globals —
// fine for a single-threaded CLI driver.
std::uint64_t g_fill_count = 0;
Quantity g_traded_qty = 0;

void report_fill(const Fill& f) {
    ++g_fill_count;
    g_traded_qty += f.qty;
}

int run_generate(const std::string& out_path, std::size_t count, std::uint64_t seed, bool wide) {
    std::vector<FeedMessage> msgs = wide ? generate_wide_feed(seed, count) : generate_feed(seed, count);
    if (!save_feed_file(out_path, msgs)) {
        std::fprintf(stderr, "failed to write feed file: %s\n", out_path.c_str());
        return 1;
    }
    std::printf("generated %zu messages (seed=%llu, profile=%s) -> %s\n",
                msgs.size(), static_cast<unsigned long long>(seed), wide ? "wide" : "narrow", out_path.c_str());
    return 0;
}

int run_replay(const std::string& in_path) {
    std::vector<FeedMessage> msgs;
    if (!load_feed_file(in_path, msgs)) {
        std::fprintf(stderr, "failed to read feed file: %s\n", in_path.c_str());
        return 1;
    }

    OrderBook<1 << 16> ob(&report_fill);
    replay(ob, msgs);

    std::printf("replayed %zu messages from %s\n", msgs.size(), in_path.c_str());
    std::printf("fills: %llu, traded qty: %lld\n",
                static_cast<unsigned long long>(g_fill_count), static_cast<long long>(g_traded_qty));

    Price bb, ba;
    if (ob.best_bid(bb)) std::printf("best bid: %lld\n", static_cast<long long>(bb));
    else std::printf("best bid: (none)\n");
    if (ob.best_ask(ba)) std::printf("best ask: %lld\n", static_cast<long long>(ba));
    else std::printf("best ask: (none)\n");
    std::printf("resting qty: %lld\n", static_cast<long long>(ob.total_resting_qty()));

    BookSignals signals;
    compute_signals(ob, signals);
    print_signals(signals);
    return 0;
}

// Prints sizeof(FeedMessage)/sizeof(FeedFileHeader) so tooling (e.g.
// scripts/xdp_loopback_test.sh, which has to slice a feed file into
// per-record UDP datagrams) never has to hardcode struct sizes that could
// silently drift out of sync with feed_message.hpp.
int run_record_size() {
    std::printf("%zu %zu\n", sizeof(FeedMessage), sizeof(FeedFileHeader));
    return 0;
}

void print_usage(const char* argv0) {
    std::fprintf(stderr,
        "usage:\n"
        "  %s generate <output.feed> <num_messages> [seed] [narrow|wide]\n"
        "  %s replay <input.feed>\n"
        "  %s record-size\n",
        argv0, argv0, argv0);
#ifdef HAVE_AF_XDP
    std::fprintf(stderr, "  %s xdp-listen <ifname> <queue_id> [bpf_obj_path]\n", argv0);
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    std::string cmd = argv[1];
    if (cmd == "generate" && argc >= 4) {
        std::string out_path = argv[2];
        std::size_t count = static_cast<std::size_t>(std::strtoull(argv[3], nullptr, 10));
        std::uint64_t seed = argc >= 5 ? std::strtoull(argv[4], nullptr, 10) : 42;
        bool wide = argc >= 6 && std::string(argv[5]) == "wide";
        if (argc >= 6 && !wide && std::string(argv[5]) != "narrow") {
            print_usage(argv[0]);
            return 1;
        }
        return run_generate(out_path, count, seed, wide);
    }
    if (cmd == "replay" && argc >= 3) {
        return run_replay(argv[2]);
    }
    if (cmd == "record-size") {
        return run_record_size();
    }
#ifdef HAVE_AF_XDP
    if (cmd == "xdp-listen") {
        return run_xdp_listen(argc, argv);
    }
#endif

    print_usage(argv[0]);
    return 1;
}

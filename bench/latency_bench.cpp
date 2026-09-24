// Phase 4 latency benchmark: per-stage, per-message latency of the
// matching pipeline over a replayed feed, as HDR-style histograms.
//
//   order_book_bench <feed.dat> [--book ladder|map] [--cpu N]
//
// Stages, each stamped separately for every message:
//   add     apply_message() for an AddOrder   (match + rest)
//   cancel  apply_message() for a CancelOrder
//   signals compute_signals() after the update
//   total   message in hand -> signals out (add/cancel + signals)
//
// What this does NOT measure: NIC -> userspace (the AF_XDP RX path), and
// market-data publish. The feed is already in memory; this isolates the
// book + signal cost from the network. `--book map` runs the pre-Phase-4
// std::map book (tests/support/map_order_book.hpp) on the same feed, for
// a before/after on the container swap.
//
// Run it on the real dev box, Release build, ideally pinned to an isolated
// core — see AGENTS.md. Numbers from a Debug build or a restricted
// container are not the project's latency numbers.

#include <sched.h>

#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "feed_message.hpp"
#include "feed_replay.hpp"
#include "latency_histogram.hpp"
#include "order_book.hpp"
#include "signals.hpp"
#include "support/map_order_book.hpp"
#include "tsc_clock.hpp"

namespace {

constexpr std::size_t kPool = 1 << 16; // same capacity as `order_book_main replay`

std::uint64_t g_fills = 0;
void count_fill(const Fill&) { ++g_fills; }

struct StageHistograms {
    LatencyHistogram add, cancel, signals, total;
};

// Consumes every computed signal so the compiler can't discard the stage.
volatile std::int64_t g_sink = 0;

template <typename Book>
void run_pass(Book& book, const std::vector<FeedMessage>& msgs, StageHistograms* h) {
    BookSignals sig;
    std::int64_t acc = 0;
    for (const FeedMessage& msg : msgs) {
        std::uint64_t t0 = stamp_begin();
        apply_message(book, msg);
        std::uint64_t t1 = stamp_end();
        compute_signals(book, sig);
        std::uint64_t t2 = stamp_end();
        acc ^= sig.microprice ^ sig.imbalance[0];

        if (h) {
            (msg.type == MsgType::AddOrder ? h->add : h->cancel).record(t1 - t0);
            h->signals.record(t2 - t1);
            h->total.record(t2 - t0);
        }
    }
    g_sink = acc;
}

// Cost of the measurement itself: two back-to-back stamps. Every stage
// number below includes roughly this much; it's reported, not subtracted.
LatencyHistogram measure_overhead(std::size_t n) {
    LatencyHistogram h;
    for (std::size_t i = 0; i < n; ++i) {
        std::uint64_t a = stamp_begin();
        std::uint64_t b = stamp_end();
        h.record(b - a);
    }
    return h;
}

void print_row(const char* name, const LatencyHistogram& h, const StampCalibration& cal) {
    auto ns = [&](std::uint64_t ticks) { return static_cast<unsigned long long>(cal.to_ns(ticks)); };
    std::printf("%-9s %10llu %8llu %8llu %8llu %8llu %8llu %10llu\n", name,
                static_cast<unsigned long long>(h.count()),
                ns(h.min()), ns(h.quantile_ppm(500'000)), ns(h.quantile_ppm(990'000)),
                ns(h.quantile_ppm(999'000)), ns(h.quantile_ppm(999'900)), ns(h.max()));
}

template <typename Book>
int bench(const std::vector<FeedMessage>& msgs, const char* book_name, const StampCalibration& cal) {
    // Warm-up pass on a throwaway book: code, branch predictors, feed pages.
    // The timed pass then gets a fresh book so it replays the exact same
    // state sequence — the pool/id-map are value-initialized at construction
    // (already paged in), so no first-touch faults land inside the timing.
    {
        auto warm = std::make_unique<Book>(&count_fill);
        run_pass(*warm, msgs, nullptr);
    }
    g_fills = 0;

    auto book = std::make_unique<Book>(&count_fill);
    auto hist = std::make_unique<StageHistograms>();
    run_pass(*book, msgs, hist.get());

    LatencyHistogram overhead = measure_overhead(1'000'000);

    std::printf("book: %s | messages: %zu | fills: %llu | stamp clock: %llu MHz\n",
                book_name, msgs.size(), static_cast<unsigned long long>(g_fills),
                static_cast<unsigned long long>(cal.mhz()));
    std::printf("%-9s %10s %8s %8s %8s %8s %8s %10s   (ns)\n",
                "stage", "count", "min", "p50", "p99", "p99.9", "p99.99", "max");
    print_row("add", hist->add, cal);
    print_row("cancel", hist->cancel, cal);
    print_row("signals", hist->signals, cal);
    print_row("total", hist->total, cal);
    print_row("overhead", overhead, cal);
    return 0;
}

void usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s <feed.dat> [--book ladder|map] [--cpu N]\n", argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    std::string feed_path = argv[1];
    std::string book = "ladder";
    int cpu = -1;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--book") && i + 1 < argc) book = argv[++i];
        else if (!std::strcmp(argv[i], "--cpu") && i + 1 < argc) cpu = std::atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }
    if (book != "ladder" && book != "map") { usage(argv[0]); return 1; }

#ifndef NDEBUG
    std::fprintf(stderr, "WARNING: not a Release build (NDEBUG unset) — these numbers mean nothing.\n");
#endif

    if (cpu >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(cpu, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0) {
            std::perror("sched_setaffinity");
            return 1;
        }
    }

    std::vector<FeedMessage> msgs;
    if (!load_feed_file(feed_path, msgs)) {
        std::fprintf(stderr, "failed to read feed file: %s\n", feed_path.c_str());
        return 1;
    }

    StampCalibration cal = calibrate_stamps();
    std::printf("cpu: %s | ladder window: %zu ticks/side\n",
                cpu >= 0 ? std::to_string(cpu).c_str() : "unpinned", kDefaultLadderWindow);

    return book == "map" ? bench<MapOrderBook<kPool>>(msgs, "map (pre-Phase-4)", cal)
                         : bench<OrderBook<kPool>>(msgs, "ladder", cal);
}

// Phase 4 latency benchmark: per-stage, per-message latency of the
// matching pipeline over a replayed feed, as HDR-style histograms.
//
//   order_book_bench <feed.dat> [--book ladder|map] [--cpu N] [--consumer-cpu M]
//
// Stages, each stamped separately for every message:
//   add     apply_message() for an AddOrder   (match + rest)
//   cancel  apply_message() for a CancelOrder
//   signals compute_signals() after the update
//   publish MdPublisher::publish() of the snapshot into the SPSC ring
//   total   message in hand -> snapshot published (add/cancel + signals + publish)
//
// A consumer thread (pinned with --consumer-cpu) drains the ring for the
// whole run, so publish sees a live consumer's cache-line traffic, not an
// idle ring. What this does NOT measure: NIC -> userspace (the AF_XDP RX
// path; `xdp-listen` reports that end to end) and the consumer's receive
// time. The feed is already in memory; this isolates the book + signals +
// publish cost from the network. `--book map` runs the pre-Phase-4
// std::map book (tests/support/map_order_book.hpp) on the same feed, for
// a before/after on the container swap.
//
// Run it on the real dev box, Release build, ideally pinned to an isolated
// core — see AGENTS.md. Numbers from a Debug build or a restricted
// container are not the project's latency numbers.

#include <sched.h>
#include <unistd.h>

#include <atomic>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "feed_message.hpp"
#include "feed_replay.hpp"
#include "latency_histogram.hpp"
#include "latency_report.hpp"
#include "md_publisher.hpp"
#include "order_book.hpp"
#include "signals.hpp"
#include "support/map_order_book.hpp"
#include "tsc_clock.hpp"

namespace {

constexpr std::size_t kPool = 1 << 16; // same capacity as `order_book_main replay`
constexpr std::size_t kRing = 1 << 12;  // 4096 x 64 B snapshots = 256 KB

using Publisher = MdPublisher<kRing>;

std::uint64_t g_fills = 0;
void count_fill(const Fill&) { ++g_fills; }

struct StageHistograms {
    LatencyHistogram add, cancel, signals, publish, total;
};

// Consumes every computed signal so the compiler can't discard the stage.
volatile std::int64_t g_sink = 0;

template <typename Book>
void run_pass(Book& book, Publisher& pub, const std::vector<FeedMessage>& msgs, StageHistograms* h) {
    BookSignals sig;
    std::int64_t acc = 0;
    for (const FeedMessage& msg : msgs) {
        std::uint64_t t0 = stamp_begin();
        apply_message(book, msg);
        std::uint64_t t1 = stamp_end();
        compute_signals(book, sig);
        std::uint64_t t2 = stamp_end();
        pub.publish(sig);
        std::uint64_t t3 = stamp_end();
        acc ^= sig.microprice ^ sig.imbalance[0];

        if (h) {
            (msg.type == MsgType::AddOrder ? h->add : h->cancel).record(t1 - t0);
            h->signals.record(t2 - t1);
            h->publish.record(t3 - t2);
            h->total.record(t3 - t0);
        }
    }
    g_sink = acc;
}

bool pin_to_cpu(int cpu) {
    if (cpu < 0) return true;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set) == 0;
}

// Drains the ring until told to stop, then drains what's left. Spins: a
// market-data consumer on its own core would, and sleeping would let the
// ring fill and turn publish into the (cheaper) drop path.
struct Consumer {
    Publisher& pub;
    std::atomic<bool> stop{false};
    std::uint64_t received = 0;
    std::int64_t sink = 0;

    void run(int cpu) {
        if (!pin_to_cpu(cpu)) std::perror("consumer sched_setaffinity");
        BookSignals s;
        for (;;) {
            if (pub.poll(s)) {
                ++received;
                sink ^= s.microprice;
            } else if (stop.load(std::memory_order_acquire)) {
                while (pub.poll(s)) ++received;
                return;
            }
        }
    }
};

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

template <typename Book>
int bench(const std::vector<FeedMessage>& msgs, const char* book_name, const StampCalibration& cal, int consumer_cpu) {
    auto pub = std::make_unique<Publisher>();
    Consumer consumer{*pub};
    std::thread consumer_thread([&] { consumer.run(consumer_cpu); });

    // Warm-up pass on a throwaway book: code, branch predictors, feed pages.
    // The timed pass then gets a fresh book so it replays the exact same
    // state sequence — the pool/id-map are value-initialized at construction
    // (already paged in), so no first-touch faults land inside the timing.
    {
        auto warm = std::make_unique<Book>(&count_fill);
        run_pass(*warm, *pub, msgs, nullptr);
    }
    g_fills = 0;
    const std::uint64_t pub_before = pub->published(), drop_before = pub->dropped();

    auto book = std::make_unique<Book>(&count_fill);
    auto hist = std::make_unique<StageHistograms>();
    run_pass(*book, *pub, msgs, hist.get());
    const std::uint64_t published = pub->published() - pub_before;
    const std::uint64_t dropped = pub->dropped() - drop_before;

    consumer.stop.store(true, std::memory_order_release);
    consumer_thread.join();

    LatencyHistogram overhead = measure_overhead(1'000'000);

    std::printf("book: %s | messages: %zu | fills: %llu | stamp clock: %llu MHz\n",
                book_name, msgs.size(), static_cast<unsigned long long>(g_fills),
                static_cast<unsigned long long>(cal.mhz()));
    std::printf("md publish (timed pass): %llu published, %llu dropped (ring full) | consumer received %llu over both passes\n",
                static_cast<unsigned long long>(published), static_cast<unsigned long long>(dropped),
                static_cast<unsigned long long>(consumer.received));
    print_latency_header();
    print_latency_row("add", hist->add, cal);
    print_latency_row("cancel", hist->cancel, cal);
    print_latency_row("signals", hist->signals, cal);
    print_latency_row("publish", hist->publish, cal);
    print_latency_row("total", hist->total, cal);
    print_latency_row("overhead", overhead, cal);
    return 0;
}

void usage(const char* argv0) {
    std::fprintf(stderr, "usage: %s <feed.dat> [--book ladder|map] [--cpu N] [--consumer-cpu M]\n", argv0);
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) { usage(argv[0]); return 1; }

    std::string feed_path = argv[1];
    std::string book = "ladder";
    int cpu = -1;
    int consumer_cpu = -1;
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--book") && i + 1 < argc) book = argv[++i];
        else if (!std::strcmp(argv[i], "--cpu") && i + 1 < argc) cpu = std::atoi(argv[++i]);
        else if (!std::strcmp(argv[i], "--consumer-cpu") && i + 1 < argc) consumer_cpu = std::atoi(argv[++i]);
        else { usage(argv[0]); return 1; }
    }
    if (book != "ladder" && book != "map") { usage(argv[0]); return 1; }

#ifndef NDEBUG
    std::fprintf(stderr, "WARNING: not a Release build (NDEBUG unset) — these numbers mean nothing.\n");
#endif

    // A pinned producer with an unpinned consumer would hand the consumer
    // the producer's one-core affinity (threads inherit it), and the two
    // would spin on one core. Default the consumer to the next core.
    if (cpu >= 0 && consumer_cpu < 0) {
        long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
        if (ncpu > 1) consumer_cpu = static_cast<int>((cpu + 1) % ncpu);
    }
    if (!pin_to_cpu(cpu)) {
        std::perror("sched_setaffinity");
        return 1;
    }

    std::vector<FeedMessage> msgs;
    if (!load_feed_file(feed_path, msgs)) {
        std::fprintf(stderr, "failed to read feed file: %s\n", feed_path.c_str());
        return 1;
    }

    StampCalibration cal = calibrate_stamps();
    std::printf("cpu: %s | consumer cpu: %s | ladder window: %zu ticks/side\n",
                cpu >= 0 ? std::to_string(cpu).c_str() : "unpinned",
                consumer_cpu >= 0 ? std::to_string(consumer_cpu).c_str() : "unpinned",
                kDefaultLadderWindow);

    return book == "map" ? bench<MapOrderBook<kPool>>(msgs, "map (pre-Phase-4)", cal, consumer_cpu)
                         : bench<OrderBook<kPool>>(msgs, "ladder", cal, consumer_cpu);
}

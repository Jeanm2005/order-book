#include "xdp_listen_cmd.hpp"

#ifdef HAVE_AF_XDP
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "feed_message.hpp"
#include "feed_replay.hpp"
#include "order_book.hpp"
#include "xdp_socket.hpp"

namespace {

// Ethernet(14) + IPv4-no-options(20) + UDP(8): xdp_redirect_feed (see
// ebpf/xdp_redirect.c) only redirects packets with ihl==5, so every frame
// XdpSocket::poll() hands back is guaranteed to have exactly this many
// header bytes before the FeedMessage payload. This project's feed is
// synthetic/host-native by design (see feed_message.hpp) — a real wire
// protocol would need a real parser instead of this fixed offset.
constexpr std::size_t kHeaderBytes = 14 + 20 + 8;

volatile std::sig_atomic_t g_stop = 0;
void on_signal(int) { g_stop = 1; }

std::uint64_t g_fill_count = 0;
Quantity g_traded_qty = 0;

void report_fill(const Fill& f) {
    ++g_fill_count;
    g_traded_qty += f.qty;
}

} // namespace

int run_xdp_listen(int argc, char** argv) {
    if (argc < 4) {
        std::fprintf(stderr, "usage: %s xdp-listen <ifname> <queue_id> [bpf_obj_path]\n", argv[0]);
        return 1;
    }

    std::string ifname = argv[2];
    unsigned queue_id = static_cast<unsigned>(std::strtoul(argv[3], nullptr, 10));
#ifndef XDP_BPF_OBJ_PATH
#define XDP_BPF_OBJ_PATH ""
#endif
    std::string bpf_obj_path = (argc >= 5) ? argv[4] : XDP_BPF_OBJ_PATH;

    XdpSocket xsk(ifname, queue_id, bpf_obj_path);
    if (!xsk.open()) {
        std::fprintf(stderr, "xdp-listen: failed to open AF_XDP socket on %s queue %u\n",
                     ifname.c_str(), queue_id);
        return 1;
    }

    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);

    OrderBook<1 << 16> ob(&report_fill);

    std::printf("xdp-listen: bound to %s queue %u (bpf obj: %s), ctrl+C to stop\n",
                ifname.c_str(), queue_id, bpf_obj_path.c_str());

    while (!g_stop) {
        xsk.poll([&ob](const std::byte* data, std::uint32_t len) {
            if (len < kHeaderBytes + sizeof(FeedMessage)) return; // truncated/malformed frame
            FeedMessage msg;
            // memcpy, not reinterpret_cast-and-dereference: the payload
            // starts at a fixed 42-byte offset into the UMEM frame, which
            // isn't 8-byte aligned, so a direct FeedMessage* dereference
            // would be an unaligned access (works on x86 in practice, but
            // is UB per the standard). memcpy into a stack-local, properly
            // aligned FeedMessage sidesteps that; at -O3 with a 40-byte
            // fixed size the compiler inlines this to a couple of loads,
            // not a libc call — no allocation, still hot-path-clean.
            std::memcpy(&msg, data + kHeaderBytes, sizeof(msg));
            apply_message(ob, msg);
        });
    }

    std::printf("fills: %llu, traded qty: %lld\n",
                static_cast<unsigned long long>(g_fill_count), static_cast<long long>(g_traded_qty));

    // Same fields run_replay() prints (see main.cpp) — the loopback script
    // diffs both by hand, so this side needs to report exactly the same
    // shape or "should match exactly" isn't actually checkable.
    Price bb, ba;
    if (ob.best_bid(bb)) std::printf("best bid: %lld\n", static_cast<long long>(bb));
    else std::printf("best bid: (none)\n");
    if (ob.best_ask(ba)) std::printf("best ask: %lld\n", static_cast<long long>(ba));
    else std::printf("best ask: (none)\n");
    std::printf("resting qty: %lld\n", static_cast<long long>(ob.total_resting_qty()));

    // Kernel-tracked drop counters, queried before close() invalidates the
    // socket fd — the authoritative source for *why* the frame count came
    // up short of what was sent, instead of guessing.
    struct xdp_statistics stats {};
    if (xsk.get_stats(stats)) {
        std::printf("xdp stats: rx_dropped=%llu rx_invalid_descs=%llu rx_ring_full=%llu "
                    "rx_fill_ring_empty_descs=%llu tx_invalid_descs=%llu tx_ring_empty_descs=%llu\n",
                    static_cast<unsigned long long>(stats.rx_dropped),
                    static_cast<unsigned long long>(stats.rx_invalid_descs),
                    static_cast<unsigned long long>(stats.rx_ring_full),
                    static_cast<unsigned long long>(stats.rx_fill_ring_empty_descs),
                    static_cast<unsigned long long>(stats.tx_invalid_descs),
                    static_cast<unsigned long long>(stats.tx_ring_empty_descs));
    }

    xsk.close();
    return 0;
}
#endif // HAVE_AF_XDP

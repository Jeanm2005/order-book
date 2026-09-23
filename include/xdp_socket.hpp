#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

#include <linux/if_xdp.h>

// Raw AF_XDP ingestion socket: direct syscalls against the kernel AF_XDP
// ABI (<linux/if_xdp.h>), no libxdp/xsk.h dependency — this machine has
// libbpf-dev but not libxdp-dev, and the project already prefers explicit
// control over hidden library behavior on anything feed-adjacent.
//
// RX-only in *use* — nothing is transmitted back out over this socket —
// but XDP_TX_RING/XDP_UMEM_COMPLETION_RING are still registered (never
// mmap'd) because bind() returns a bare EINVAL without them on this
// kernel, despite the AF_XDP docs suggesting an RX-only ring pair should
// be sufficient. Confirmed by hand, not a defensive guess — see the git
// history for this file.
//
// Setup (BPF load/attach, UMEM alloc, ring registration, veth/queue bind)
// happens once in open() and may allocate/syscall freely. poll() is the
// only method called per packet and stays allocation-free — same hot-path
// rule as order_book.hpp (see AGENTS.md "Hard rules for the hot path").
// This header should be added to that hot-path file list once this lands.
//
// Requires CAP_NET_ADMIN/root and a real or loopback-capable NIC (veth for
// WSL2 dev/test) — see AGENTS.md "AF_XDP / eBPF work". Verified end-to-end
// via scripts/xdp_loopback_test.sh under real privileges on the WSL2 dev
// box: identical book state to file-replay, zero kernel-reported drops.
class XdpSocket {
public:
    // frame_count must be a power of two (mask-based ring indexing).
    // frame_size defaults to 2048: comfortably larger than any real frame
    // this project ever puts on the wire (sizeof(FeedMessage) == 40).
    XdpSocket(std::string ifname, unsigned queue_id, std::string bpf_obj_path,
              std::size_t frame_count = 4096, std::size_t frame_size = 2048);
    ~XdpSocket();

    XdpSocket(const XdpSocket&) = delete;
    XdpSocket& operator=(const XdpSocket&) = delete;

    // Loads ebpf/xdp_redirect.o, attaches it to ifname, creates the UMEM +
    // FILL/RX rings, binds to queue_id (zero-copy first, falls back to
    // copy mode — see AGENTS.md Phase 2 plan note on veth not supporting
    // zero-copy), and registers this socket into the program's XSKMAP.
    // Returns false (with a message on stderr) on any failure.
    bool open();

    // Detaches the XDP program, unmaps rings/UMEM, closes the socket.
    // Safe to call more than once; the destructor calls it too.
    void close();

    // Kernel-tracked drop counters for this socket (rx_dropped,
    // rx_invalid_descs, rx_ring_full, rx_fill_ring_empty_descs, ...) — the
    // only way to tell *why* a frame count came up short of what was sent,
    // as opposed to guessing. Not hot-path: call it at shutdown for a
    // report, not per-packet. Returns false if the socket isn't open.
    bool get_stats(struct xdp_statistics& out) const;

    static constexpr std::size_t kMaxBatch = 64;

    // Drains up to kMaxBatch completed RX descriptors. For each, calls
    // on_frame(data, len) where data/len is the *whole* Ethernet frame
    // (AF_XDP hands back the raw frame from ctx->data, not just the UDP
    // payload) — the caller is responsible for skipping past its own
    // headers, same as xdp_listen_cmd.cpp does. Recycles every drained
    // frame back to the FILL ring before returning. No allocation; the
    // only template so the frame handler can be a plain lambda rather
    // than a heap-capturing std::function (AGENTS.md hot-path rule).
    template <typename FrameHandler>
    std::size_t poll(FrameHandler&& on_frame) {
        std::uint32_t cons = rx_.consumer->load(std::memory_order_relaxed);
        std::uint32_t prod = rx_.producer->load(std::memory_order_acquire);
        std::uint32_t available = prod - cons;
        if (available == 0) return 0;
        if (available > kMaxBatch) available = kMaxBatch;

        const auto* descs = static_cast<const xdp_desc*>(rx_.descs);
        std::uint64_t recycle[kMaxBatch];

        for (std::uint32_t i = 0; i < available; ++i) {
            const xdp_desc& d = descs[(cons + i) & rx_.mask];
            const std::byte* frame = static_cast<const std::byte*>(umem_area_) + d.addr;
            on_frame(frame, d.len);
            recycle[i] = d.addr;
        }
        rx_.consumer->store(cons + available, std::memory_order_release);

        auto* fill_descs = static_cast<std::uint64_t*>(fill_.descs);
        std::uint32_t fprod = fill_.producer->load(std::memory_order_relaxed);
        for (std::uint32_t i = 0; i < available; ++i) {
            fill_descs[(fprod + i) & fill_.mask] = recycle[i];
        }
        fill_.producer->store(fprod + available, std::memory_order_release);

        return available;
    }

private:
    struct Ring {
        void* map_base = nullptr;
        std::size_t map_len = 0;
        std::atomic<std::uint32_t>* producer = nullptr;
        std::atomic<std::uint32_t>* consumer = nullptr;
        void* descs = nullptr; // xdp_desc* for rx, std::uint64_t* for fill
        std::uint32_t mask = 0;
    };

    bool setup_umem_and_rings();
    bool load_and_attach_bpf();
    void unmap_ring(Ring& ring);

    std::string ifname_;
    unsigned queue_id_;
    std::string bpf_obj_path_;
    std::size_t frame_count_;
    std::size_t frame_size_;

    unsigned ifindex_ = 0;
    int fd_ = -1;
    void* umem_area_ = nullptr;
    std::size_t umem_size_ = 0;

    Ring rx_;
    Ring fill_;

    struct bpf_object* bpf_obj_ = nullptr; // opaque: keeps <bpf/libbpf.h> out of this header
    bool attached_ = false;
};

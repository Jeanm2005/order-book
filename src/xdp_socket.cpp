// Setup/teardown for XdpSocket. Everything in this file runs once at
// startup or shutdown, never per-packet, so it's allowed to allocate,
// syscall, and fail loudly — unlike include/xdp_socket.hpp's poll().
//
// Verified end-to-end under real privileges via scripts/xdp_loopback_test.sh
// on the WSL2 dev box — see README.md's Phase 2 section for the two real
// bugs that surfaced only by actually running this (not by compiling it).
#include "xdp_socket.hpp"

#include <cerrno>
#include <cstdio>
#include <cstring>

#include <linux/if_link.h>
#include <net/if.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

namespace {

bool is_power_of_two(std::size_t n) {
    return n != 0 && (n & (n - 1)) == 0;
}

} // namespace

XdpSocket::XdpSocket(std::string ifname, unsigned queue_id, std::string bpf_obj_path,
                      std::size_t frame_count, std::size_t frame_size)
    : ifname_(std::move(ifname)),
      queue_id_(queue_id),
      bpf_obj_path_(std::move(bpf_obj_path)),
      frame_count_(frame_count),
      frame_size_(frame_size) {}

XdpSocket::~XdpSocket() { close(); }

bool XdpSocket::load_and_attach_bpf() {
    ifindex_ = if_nametoindex(ifname_.c_str());
    if (ifindex_ == 0) {
        std::fprintf(stderr, "xdp_socket: invalid interface '%s': %s\n",
                     ifname_.c_str(), std::strerror(errno));
        return false;
    }

    bpf_obj_ = bpf_object__open_file(bpf_obj_path_.c_str(), nullptr);
    if (!bpf_obj_ || libbpf_get_error(bpf_obj_)) {
        std::fprintf(stderr, "xdp_socket: failed to open BPF object '%s'\n", bpf_obj_path_.c_str());
        bpf_obj_ = nullptr;
        return false;
    }

    if (bpf_object__load(bpf_obj_)) {
        std::fprintf(stderr, "xdp_socket: failed to load BPF object into kernel: %s\n",
                     std::strerror(errno));
        return false;
    }

    struct bpf_program* prog = bpf_object__find_program_by_name(bpf_obj_, "xdp_redirect_feed");
    if (!prog) {
        std::fprintf(stderr, "xdp_socket: program 'xdp_redirect_feed' not found in %s\n",
                     bpf_obj_path_.c_str());
        return false;
    }

    int prog_fd = bpf_program__fd(prog);
    if (prog_fd < 0) {
        std::fprintf(stderr, "xdp_socket: failed to get program fd\n");
        return false;
    }

    // Forced generic/SKB mode, not the flags=0 "let the kernel pick"
    // default: veth's *native* XDP mode carries extra per-queue setup
    // requirements a plain single-queue veth doesn't satisfy, which is
    // exactly why the kernel's own AF_XDP selftests (xdpxceiver) force SKB
    // mode when testing over veth pairs. Confirmed by hand on this
    // project's WSL2 box: flags=0 attached fine but bind() then failed
    // with EINVAL — forcing SKB mode is the fix, not a defensive guess.
    if (bpf_xdp_attach(ifindex_, prog_fd, XDP_FLAGS_SKB_MODE, nullptr) < 0) {
        std::fprintf(stderr, "xdp_socket: failed to attach XDP program to %s: %s\n",
                     ifname_.c_str(), std::strerror(errno));
        return false;
    }
    attached_ = true;
    return true;
}

bool XdpSocket::setup_umem_and_rings() {
    umem_size_ = frame_count_ * frame_size_;
    umem_area_ = mmap(nullptr, umem_size_, PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (umem_area_ == MAP_FAILED) {
        std::fprintf(stderr, "xdp_socket: umem mmap failed: %s\n", std::strerror(errno));
        umem_area_ = nullptr;
        return false;
    }

    fd_ = socket(AF_XDP, SOCK_RAW, 0);
    if (fd_ < 0) {
        std::fprintf(stderr, "xdp_socket: socket(AF_XDP) failed: %s\n", std::strerror(errno));
        return false;
    }

    struct xdp_umem_reg umem_reg {};
    umem_reg.addr = reinterpret_cast<std::uint64_t>(umem_area_);
    umem_reg.len = umem_size_;
    umem_reg.chunk_size = static_cast<std::uint32_t>(frame_size_);
    umem_reg.headroom = 0;
    if (setsockopt(fd_, SOL_XDP, XDP_UMEM_REG, &umem_reg, sizeof(umem_reg)) != 0) {
        std::fprintf(stderr, "xdp_socket: XDP_UMEM_REG failed: %s\n", std::strerror(errno));
        return false;
    }

    int ring_size = static_cast<int>(frame_count_);
    if (setsockopt(fd_, SOL_XDP, XDP_UMEM_FILL_RING, &ring_size, sizeof(ring_size)) != 0) {
        std::fprintf(stderr, "xdp_socket: XDP_UMEM_FILL_RING failed: %s\n", std::strerror(errno));
        return false;
    }
    if (setsockopt(fd_, SOL_XDP, XDP_RX_RING, &ring_size, sizeof(ring_size)) != 0) {
        std::fprintf(stderr, "xdp_socket: XDP_RX_RING failed: %s\n", std::strerror(errno));
        return false;
    }
    // Registered but never mmap'd/used: this socket is RX-only and nothing
    // will ever land in either ring. Confirmed by hand on this project's
    // WSL2 box that omitting these makes bind() fail with a bare EINVAL —
    // every real AF_XDP implementation (including the kernel's own
    // xskxceiver selftest, which specifically exercises veth pairs)
    // registers all four rings on a non-shared UMEM socket regardless of
    // which direction it actually uses, contrary to what the docs suggest
    // is optional. Matching that is the fix, not a defensive guess.
    if (setsockopt(fd_, SOL_XDP, XDP_UMEM_COMPLETION_RING, &ring_size, sizeof(ring_size)) != 0) {
        std::fprintf(stderr, "xdp_socket: XDP_UMEM_COMPLETION_RING failed: %s\n", std::strerror(errno));
        return false;
    }
    if (setsockopt(fd_, SOL_XDP, XDP_TX_RING, &ring_size, sizeof(ring_size)) != 0) {
        std::fprintf(stderr, "xdp_socket: XDP_TX_RING failed: %s\n", std::strerror(errno));
        return false;
    }

    struct xdp_mmap_offsets off {};
    socklen_t off_len = sizeof(off);
    if (getsockopt(fd_, SOL_XDP, XDP_MMAP_OFFSETS, &off, &off_len) != 0) {
        std::fprintf(stderr, "xdp_socket: XDP_MMAP_OFFSETS failed: %s\n", std::strerror(errno));
        return false;
    }

    // FILL ring: descriptor type is a bare __u64 (frame address).
    fill_.map_len = off.fr.desc + frame_count_ * sizeof(std::uint64_t);
    fill_.map_base = mmap(nullptr, fill_.map_len, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_POPULATE, fd_, XDP_UMEM_PGOFF_FILL_RING);
    if (fill_.map_base == MAP_FAILED) {
        std::fprintf(stderr, "xdp_socket: FILL ring mmap failed: %s\n", std::strerror(errno));
        fill_.map_base = nullptr;
        return false;
    }
    fill_.producer = reinterpret_cast<std::atomic<std::uint32_t>*>(
        static_cast<std::byte*>(fill_.map_base) + off.fr.producer);
    fill_.consumer = reinterpret_cast<std::atomic<std::uint32_t>*>(
        static_cast<std::byte*>(fill_.map_base) + off.fr.consumer);
    fill_.descs = static_cast<std::byte*>(fill_.map_base) + off.fr.desc;
    fill_.mask = static_cast<std::uint32_t>(frame_count_ - 1);

    // RX ring: descriptor type is struct xdp_desc { addr, len, options }.
    rx_.map_len = off.rx.desc + frame_count_ * sizeof(xdp_desc);
    rx_.map_base = mmap(nullptr, rx_.map_len, PROT_READ | PROT_WRITE,
                         MAP_SHARED | MAP_POPULATE, fd_, XDP_PGOFF_RX_RING);
    if (rx_.map_base == MAP_FAILED) {
        std::fprintf(stderr, "xdp_socket: RX ring mmap failed: %s\n", std::strerror(errno));
        rx_.map_base = nullptr;
        return false;
    }
    rx_.producer = reinterpret_cast<std::atomic<std::uint32_t>*>(
        static_cast<std::byte*>(rx_.map_base) + off.rx.producer);
    rx_.consumer = reinterpret_cast<std::atomic<std::uint32_t>*>(
        static_cast<std::byte*>(rx_.map_base) + off.rx.consumer);
    rx_.descs = static_cast<std::byte*>(rx_.map_base) + off.rx.desc;
    rx_.mask = static_cast<std::uint32_t>(frame_count_ - 1);

    // A bad queue_id gives bind() a bare EINVAL with no indication of why —
    // check the device actually advertises that RX queue first so a
    // misconfigured queue_id fails with a message that says so.
    {
        std::string queue_path = "/sys/class/net/" + ifname_ + "/queues/rx-" + std::to_string(queue_id_);
        struct stat st{};
        if (stat(queue_path.c_str(), &st) != 0) {
            std::fprintf(stderr,
                "xdp_socket: %s has no RX queue %u (missing %s) — check `ls /sys/class/net/%s/queues/`\n",
                ifname_.c_str(), queue_id_, queue_path.c_str(), ifname_.c_str());
            return false;
        }
    }

    // Bind: try zero-copy first, fall back to forced copy mode — veth (the
    // only NIC path on WSL2) doesn't support zero-copy, so in practice this
    // always ends up on the XDP_COPY branch during dev/test (see AGENTS.md
    // Phase 2 plan note); a real zero-copy-capable driver would take the
    // first branch instead.
    struct sockaddr_xdp sxdp {};
    sxdp.sxdp_family = AF_XDP;
    sxdp.sxdp_ifindex = ifindex_;
    sxdp.sxdp_queue_id = queue_id_;

    sxdp.sxdp_flags = XDP_ZEROCOPY;
    if (bind(fd_, reinterpret_cast<struct sockaddr*>(&sxdp), sizeof(sxdp)) != 0) {
        sxdp.sxdp_flags = XDP_COPY;
        if (bind(fd_, reinterpret_cast<struct sockaddr*>(&sxdp), sizeof(sxdp)) != 0) {
            std::fprintf(stderr, "xdp_socket: bind(%s, queue %u) failed in both zero-copy and copy mode: %s\n",
                         ifname_.c_str(), queue_id_, std::strerror(errno));
            return false;
        }
    }

    // Register this socket into the BPF program's XSKMAP for our queue, so
    // xdp_redirect_feed's bpf_redirect_map() has somewhere to send frames.
    struct bpf_map* map = bpf_object__find_map_by_name(bpf_obj_, "xsks_map");
    if (!map) {
        std::fprintf(stderr, "xdp_socket: map 'xsks_map' not found in BPF object\n");
        return false;
    }
    int map_fd = bpf_map__fd(map);
    if (map_fd < 0) {
        std::fprintf(stderr, "xdp_socket: failed to get xsks_map fd\n");
        return false;
    }
    if (bpf_map_update_elem(map_fd, &queue_id_, &fd_, BPF_ANY) != 0) {
        std::fprintf(stderr, "xdp_socket: failed to register socket in xsks_map: %s\n",
                     std::strerror(errno));
        return false;
    }

    // Prime the FILL ring with every frame so the kernel has somewhere to
    // land the first wave of packets. One-time startup cost, not hot path.
    auto* fill_descs = static_cast<std::uint64_t*>(fill_.descs);
    for (std::size_t i = 0; i < frame_count_; ++i) {
        fill_descs[i & fill_.mask] = i * frame_size_;
    }
    fill_.producer->store(static_cast<std::uint32_t>(frame_count_), std::memory_order_release);

    return true;
}

bool XdpSocket::get_stats(struct xdp_statistics& out) const {
    if (fd_ < 0) return false;
    socklen_t len = sizeof(out);
    return getsockopt(fd_, SOL_XDP, XDP_STATISTICS, &out, &len) == 0;
}

bool XdpSocket::open() {
    if (!is_power_of_two(frame_count_)) {
        std::fprintf(stderr, "xdp_socket: frame_count (%zu) must be a power of two\n", frame_count_);
        return false;
    }
    if (!load_and_attach_bpf()) return false;
    if (!setup_umem_and_rings()) return false;
    return true;
}

void XdpSocket::unmap_ring(Ring& ring) {
    if (ring.map_base) {
        munmap(ring.map_base, ring.map_len);
        ring = Ring{};
    }
}

void XdpSocket::close() {
    unmap_ring(rx_);
    unmap_ring(fill_);

    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
    }
    if (umem_area_) {
        munmap(umem_area_, umem_size_);
        umem_area_ = nullptr;
    }
    if (attached_) {
        bpf_xdp_detach(ifindex_, 0, nullptr);
        attached_ = false;
    }
    if (bpf_obj_) {
        bpf_object__close(bpf_obj_);
        bpf_obj_ = nullptr;
    }
}

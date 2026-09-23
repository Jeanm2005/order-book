#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/in.h>
#include <linux/udp.h>

SEC("license")
char _license[] = "GPL";

// Keyed by RX queue index -> AF_XDP socket fd for that queue. Userspace
// populates this (bpf_map_update_elem) after binding its XSK, in
// XdpSocket::open() (src/xdp_socket.cpp).
struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

#define FEED_UDP_PORT 9999

SEC("xdp")
int xdp_redirect_feed(struct xdp_md *ctx) {
    void *data_end = (void *)(long)ctx->data_end;
    void *data     = (void *)(long)ctx->data;

    struct ethhdr *eth = data;
    if ((void *)(eth + 1) > data_end)
        return XDP_PASS;

    if (eth->h_proto != bpf_htons(ETH_P_IP))
        return XDP_PASS;

    struct iphdr *iph = (void *)(eth + 1);
    if ((void *)(iph + 1) > data_end)
        return XDP_PASS;

    if (iph->protocol != IPPROTO_UDP)
        return XDP_PASS;

    if (iph->ihl != 5) // no IP options — see file header note
        return XDP_PASS;

    struct udphdr *udph = (void *)(iph + 1);
    if ((void *)(udph + 1) > data_end)
        return XDP_PASS;

    if (bpf_ntohs(udph->dest) != FEED_UDP_PORT)
        return XDP_PASS;

    // Fall back to XDP_PASS (not drop) if no socket is registered for this
    // queue yet — a portfolio/dev box shouldn't silently blackhole traffic
    // just because userspace hasn't attached.
    __u32 queue_id = ctx->rx_queue_index;
    return bpf_redirect_map(&xsks_map, queue_id, XDP_PASS);
}

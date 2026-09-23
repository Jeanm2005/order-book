#!/usr/bin/env bash
# Phase 2 correctness bridge: proves AF_XDP ingestion produces the same book
# state as file-replay for an identical generated FeedMessage sequence, over
# a veth pair, injecting each record as its own UDP datagram via socat (one
# frame == one FeedMessage, matching xdp_listen_cmd.cpp's fixed-offset
# payload assumption). This is NOT a ctest target and does NOT run in CI —
# it needs root (veth creation, XDP program load) and a real kernel network
# stack, neither of which a hosted runner or this sandbox has. See AGENTS.md
# "AF_XDP / eBPF work".
#
# Usage: sudo ./scripts/xdp_loopback_test.sh   (build with -DENABLE_AF_XDP=ON first)
set -euo pipefail

if [[ $EUID -ne 0 ]]; then
    echo "must run as root (creates a veth pair and loads an XDP program)" >&2
    exit 1
fi

BUILD_DIR="${BUILD_DIR:-build-xdp}"
BIN="$BUILD_DIR/order_book_main"
NUM_MESSAGES="${NUM_MESSAGES:-2000}"
UDP_PORT=9999
VETH_A=obveth0
VETH_B=obveth1
NS=obveth_ns
IP_A=10.99.0.1
IP_B=10.99.0.2

if [[ ! -x "$BIN" ]]; then
    echo "missing $BIN — configure with -DENABLE_AF_XDP=ON and build first" >&2
    exit 1
fi
if ! command -v socat >/dev/null; then
    echo "socat not found — install it (apt install socat)" >&2
    exit 1
fi

FEED_FILE="$(mktemp --suffix=.feed)"
LISTEN_OUT="$(mktemp)"

# Runs on every exit path, including a `set -e` abort partway through — so
# xdp-listen's stdout/stderr always gets shown, even if something upstream
# (a stray dd/socat failure, an already-dead listener) fails first. Losing
# that diagnostic silently is exactly what happened before this was added.
cleanup() {
    local status=$?
    local listen_status=0
    if [[ -n "${LISTEN_PID:-}" ]]; then
        kill -INT "$LISTEN_PID" 2>/dev/null || true
        wait "$LISTEN_PID" 2>/dev/null
        listen_status=$?
    fi
    if [[ -f "$LISTEN_OUT" ]]; then
        echo "--- xdp-listen output ---"
        cat "$LISTEN_OUT"
    fi
    if { [[ "$status" -ne 0 ]] || [[ "$listen_status" -ne 0 ]]; } && command -v dmesg >/dev/null; then
        echo "--- dmesg tail (kernel-side context on failure) ---"
        dmesg | tail -n 30 || true
    fi
    ip netns del "$NS" >/dev/null 2>&1 || true
    ip link del "$VETH_A" >/dev/null 2>&1 || true
    rm -f "$FEED_FILE" "$LISTEN_OUT"
    exit "$status"
}
trap cleanup EXIT

read -r RECORD_BYTES HEADER_BYTES <<< "$("$BIN" record-size)"
echo "[*] sizeof(FeedMessage)=$RECORD_BYTES sizeof(FeedFileHeader)=$HEADER_BYTES (queried, not hardcoded)"

echo "[*] generating synthetic feed ($NUM_MESSAGES messages)"
"$BIN" generate "$FEED_FILE" "$NUM_MESSAGES"

echo "[*] expected result (file replay):"
EXPECTED="$("$BIN" replay "$FEED_FILE")"
echo "$EXPECTED"

echo "[*] setting up veth pair $VETH_A <-> $VETH_B (peer in netns $NS)"
# Idempotent: a previous run's cleanup can leave this namespace/link behind
# (observed: a benign race between the last `ip netns exec socat` exiting
# and `ip netns del` running) — clear any leftover state before creating
# fresh state instead of failing on "File exists".
ip netns del "$NS" >/dev/null 2>&1 || true
ip link del "$VETH_A" >/dev/null 2>&1 || true
ip netns add "$NS"
ip link add "$VETH_A" type veth peer name "$VETH_B"
ip link set "$VETH_B" netns "$NS"
ip addr add "$IP_A/24" dev "$VETH_A"
ip link set "$VETH_A" up
ip netns exec "$NS" ip addr add "$IP_B/24" dev "$VETH_B"
ip netns exec "$NS" ip link set "$VETH_B" up
ip netns exec "$NS" ip link set lo up

LISTEN_CMD=("$BIN" xdp-listen "$VETH_A" 0)
if [[ "${STRACE:-0}" == "1" ]]; then
    if ! command -v strace >/dev/null; then
        echo "STRACE=1 requested but strace isn't installed (apt-get install -y strace)" >&2
        exit 1
    fi
    LISTEN_CMD=(strace -f -tt -e trace=socket,setsockopt,getsockopt,bind,mmap "${LISTEN_CMD[@]}")
fi

echo "[*] starting xdp-listen on $VETH_A (queue 0)${STRACE:+ [strace]}"
"${LISTEN_CMD[@]}" > "$LISTEN_OUT" 2>&1 &
LISTEN_PID=$!
sleep 1

echo "[*] injecting $NUM_MESSAGES frames via socat, one FeedMessage per UDP datagram"
# One long-lived socat process, not $NUM_MESSAGES spawned ones: 2000
# separate `dd | socat | ip netns exec` process pipelines back-to-back was
# bursty enough to overflow the interface's receive backlog under WSL2's
# scheduling before packets ever reached the XDP hook (confirmed by hand:
# xdp_statistics showed zero drops at the AF_XDP layer while the actual
# ingested count still fell short — the loss was upstream of it, i.e.
# exactly what an overflowed backlog looks like). socat's `-b` forces each
# read/write to be exactly one record, so this still puts one FeedMessage
# per UDP datagram, it just does it as one paced stream instead of a burst
# of independent processes.
#
# `tail -c +N`, not `dd bs=1`: dd with bs=1 writes to the pipe one byte at
# a time, and socat -u sends one UDP datagram per underlying read() rather
# than accumulating up to -b bytes first — so bs=1 produced a burst of
# mostly 1-byte datagrams instead of clean $RECORD_BYTES-byte ones
# (confirmed by hand: received FeedMessage fields came back full of
# garbage, consistent with single stray bytes landing in struct fields).
# tail reads/writes in normal-sized chunks, so socat's reads reliably get
# more than $RECORD_BYTES bytes available and -b correctly slices them.
tail -c +"$((HEADER_BYTES + 1))" "$FEED_FILE" \
    | ip netns exec "$NS" socat -u -b "$RECORD_BYTES" - UDP-SENDTO:"$IP_A":$UDP_PORT

sleep 1
echo "[*] done injecting — xdp-listen output follows (via cleanup), compare against the expected block above"
echo "    (fills/traded-qty/best-bid/best-ask/resting-qty should match exactly)"

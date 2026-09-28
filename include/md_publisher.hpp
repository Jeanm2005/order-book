#pragma once
#include <cstddef>
#include <cstdint>
#include "signals.hpp"
#include "spsc_ring_buffer.hpp"

// Market-data publish stage: the book thread hands every BookSignals
// snapshot to one consumer thread over an SPSC ring (one 64-byte snapshot
// per slot, see signals.hpp).
//
// publish() never blocks the book thread. If the consumer has fallen a
// full ring behind, the snapshot is dropped and counted. A snapshot is the
// full signal state, not a delta, so the next one supersedes anything
// dropped; a consumer that falls behind sees conflated updates, never
// wrong ones. The drop count is the signal that the consumer is too slow.
//
// Hot path (AGENTS.md): no allocation (the ring is a fixed array inside
// this object), no locks, no virtual calls. Producer calls publish(), one
// other thread calls poll(); the counters are producer-owned and are read
// by the producer, or after the threads have joined.
template <std::size_t Capacity>
class MdPublisher {
public:
    bool publish(const BookSignals& s) {
        if (ring_.try_push(s)) {
            ++published_;
            return true;
        }
        ++dropped_;
        return false;
    }

    // Consumer side. Returns false if nothing is waiting.
    bool poll(BookSignals& out) { return ring_.try_pop(out); }

    std::uint64_t published() const { return published_; }
    std::uint64_t dropped() const { return dropped_; }

    // One slot is kept empty to tell full from empty.
    static constexpr std::size_t usable_slots() { return Capacity - 1; }

private:
    SpscRingBuffer<BookSignals, Capacity> ring_;
    // Own cache line: the ring's last member is the consumer-written tail.
    alignas(64) std::uint64_t published_ = 0;
    std::uint64_t dropped_ = 0;
};

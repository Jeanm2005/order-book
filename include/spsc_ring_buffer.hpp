#pragma once
#include <atomic>
#include <array>
#include <cstddef>

template <typename T, std::size_t Capacity>
class SpscRingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    bool try_push(const T& item) {
        std::size_t head = head_.load(std::memory_order_relaxed);
        std::size_t next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full — producer's job to decide backpressure policy
        }
        buffer_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool try_pop(T& out) {
        std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = buffer_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        return true;
    }

private:
    static constexpr std::size_t mask_ = Capacity - 1;

    std::array<T, Capacity> buffer_;

    alignas(64) std::atomic<std::size_t> head_{0}; // producer-owned
    alignas(64) std::atomic<std::size_t> tail_{0}; // consumer-owned
};
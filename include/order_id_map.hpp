#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include "order.hpp"

// Fixed-capacity open-addressing hash map from OrderId -> Order*.
// Backing storage is sized once at construction and never grows or
// allocates again: insert/find/erase are all O(1) amortized with no
// malloc/new on the path an incoming order or cancel takes.
template <std::size_t Capacity>
class OrderIdMap {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    OrderIdMap() : slots_(Capacity) {}

    void insert(OrderId id, Order* ptr) {
        std::size_t idx = index_of(id);
        std::size_t insert_at = kNoSlot;
        for (std::size_t probes = 0; probes < Capacity; ++probes) {
            State st = slots_[idx].state;
            if (st == State::Empty) {
                if (insert_at == kNoSlot) insert_at = idx;
                break;
            }
            if (st == State::Tombstone && insert_at == kNoSlot) {
                insert_at = idx; // reclaim so a long-running table doesn't fill with tombstones
            }
            idx = (idx + 1) & mask_;
        }
        slots_[insert_at] = Slot{id, ptr, State::Occupied};
        ++size_;
    }

    Order* find(OrderId id) const {
        std::size_t idx = index_of(id);
        for (std::size_t probes = 0; probes < Capacity; ++probes) {
            const Slot& s = slots_[idx];
            if (s.state == State::Empty) return nullptr;
            if (s.state == State::Occupied && s.id == id) return s.ptr;
            idx = (idx + 1) & mask_;
        }
        return nullptr;
    }

    bool erase(OrderId id) {
        std::size_t idx = index_of(id);
        for (std::size_t probes = 0; probes < Capacity; ++probes) {
            Slot& s = slots_[idx];
            if (s.state == State::Empty) return false;
            if (s.state == State::Occupied && s.id == id) {
                s.state = State::Tombstone;
                s.ptr = nullptr;
                --size_;
                return true;
            }
            idx = (idx + 1) & mask_;
        }
        return false;
    }

    std::size_t size() const { return size_; }

private:
    enum class State : std::uint8_t { Empty, Occupied, Tombstone };

    struct Slot {
        OrderId id = 0;
        Order* ptr = nullptr;
        State state = State::Empty;
    };

    // splitmix64 finalizer: cheap, decent avalanche for sequential/sparse ids alike.
    static std::size_t index_of(OrderId id) {
        std::uint64_t x = id;
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27; x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<std::size_t>(x) & mask_;
    }

    static constexpr std::size_t kNoSlot = static_cast<std::size_t>(-1);
    static constexpr std::size_t mask_ = Capacity - 1;
    std::vector<Slot> slots_;
    std::size_t size_ = 0;
};

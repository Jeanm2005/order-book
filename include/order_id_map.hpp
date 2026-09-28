#pragma once
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "order.hpp"

// Fixed-capacity open-addressing hash map from OrderId -> Order*.
// Backing storage is sized once at construction and never grows or
// allocates again: insert/find/erase are all O(1) amortized with no
// malloc/new on the path an incoming order or cancel takes.
//
// Linear probing with backward-shift deletion, no tombstones. erase()
// closes the gap by pulling later entries of the same cluster back toward
// their home slot, so the table only ever holds Empty and Occupied slots.
// That keeps every probe chain bounded by the live population: a lookup
// stops at the first Empty slot, and there are always Capacity - size()
// of them. (Tombstones never turned back into Empty slots, so a long
// enough run of insert/erase left none, and every miss scanned the whole
// table — tests/order_id_map_tests.cpp is the regression.)
template <std::size_t Capacity>
class OrderIdMap {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

public:
    OrderIdMap() : slots_(Capacity) {}

    // Precondition: id is not already present (the book never rests the
    // same id twice). OrderBook sizes Capacity >= 2x its order pool, so the
    // table is never full; every loop is still capped at Capacity probes so
    // a broken invariant fails loudly instead of spinning.
    void insert(OrderId id, Order* ptr) {
        std::size_t idx = index_of(id);
        for (std::size_t probes = 0; probes < Capacity; ++probes) {
            if (!slots_[idx].occupied) {
                slots_[idx] = Slot{id, ptr, true};
                ++size_;
                return;
            }
            idx = (idx + 1) & mask_;
        }
        assert(false && "OrderIdMap full");
    }

    Order* find(OrderId id) const {
        std::size_t idx = index_of(id);
        for (std::size_t probes = 0; probes < Capacity && slots_[idx].occupied; ++probes) {
            if (slots_[idx].id == id) return slots_[idx].ptr;
            idx = (idx + 1) & mask_;
        }
        return nullptr;
    }

    bool erase(OrderId id) {
        std::size_t hole = index_of(id);
        for (std::size_t probes = 0;; ++probes) {
            if (probes == Capacity || !slots_[hole].occupied) return false;
            if (slots_[hole].id == id) break;
            hole = (hole + 1) & mask_;
        }

        // Backward shift: walk the rest of the cluster. An entry can move
        // into the hole only if its home slot is not cyclically inside
        // (hole, j] — otherwise moving it would put it before its home,
        // where find() would never look.
        std::size_t j = hole;
        for (std::size_t probes = 1; probes < Capacity; ++probes) {
            j = (j + 1) & mask_;
            if (!slots_[j].occupied) break;
            std::size_t home = index_of(slots_[j].id);
            bool stays = hole <= j ? (hole < home && home <= j)
                                   : (hole < home || home <= j);
            if (stays) continue;
            slots_[hole] = slots_[j];
            hole = j;
        }
        slots_[hole] = Slot{};
        --size_;
        return true;
    }

    std::size_t size() const { return size_; }

    // Diagnostic only: number of slots find(id) inspects before it returns.
    // Lets tests bound probe chains directly instead of timing them.
    std::size_t probe_length(OrderId id) const {
        std::size_t idx = index_of(id);
        std::size_t probes = 1;
        while (probes <= Capacity && slots_[idx].occupied && slots_[idx].id != id) {
            idx = (idx + 1) & mask_;
            ++probes;
        }
        return probes;
    }

private:
    struct Slot {
        OrderId id = 0;
        Order* ptr = nullptr;
        bool occupied = false;
    };

    // splitmix64 finalizer: cheap, decent avalanche for sequential/sparse ids alike.
    static std::size_t index_of(OrderId id) {
        std::uint64_t x = id;
        x ^= x >> 30; x *= 0xbf58476d1ce4e5b9ULL;
        x ^= x >> 27; x *= 0x94d049bb133111ebULL;
        x ^= x >> 31;
        return static_cast<std::size_t>(x) & mask_;
    }

    static constexpr std::size_t mask_ = Capacity - 1;
    std::vector<Slot> slots_;
    std::size_t size_ = 0;
};

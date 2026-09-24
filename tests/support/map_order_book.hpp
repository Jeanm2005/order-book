#pragma once
// TEST/BENCH-ONLY reference: the std::map-backed OrderBook exactly as it was
// before the Phase 4 container swap (commit b024b83), renamed MapOrderBook.
// It's the oracle the flat-ladder OrderBook is differential-tested against
// (tests/ladder_tests.cpp), and the "before" side of the container
// comparison in bench/latency_bench.cpp. Not used by the engine itself.
#include <algorithm>
#include <map>
#include "order.hpp"
#include "price_level.hpp"
#include "memory_pool.hpp"
#include "order_id_map.hpp"
#include "order_book.hpp" // Fill, LevelQty

template <std::size_t PoolCapacity = 1 << 16>
class MapOrderBook {
public:
    using FillHandler = void(*)(const Fill&); // raw function pointer: no vtable, no heap capture

    explicit MapOrderBook(FillHandler on_fill) : on_fill_(on_fill) {}

    void add_order(OrderId id, Side side, Price price, Quantity qty, Nanos ts) {
        Order* o = pool_.allocate(Order{id, side, price, qty, ts, nullptr, nullptr});
        if (!o) return; // pool exhausted — counted as a hard error upstream, never thrown here

        match(o);
        if (o->quantity > 0) {
            // remainder rests on the book
            if (o->side == Side::Buy) {
                bids_[o->price].push_back(o);
            } else {
                asks_[o->price].push_back(o);
            }
            id_map_.insert(o->id, o);
        } else {
            pool_.release(o);
        }
    }

    // Cancels a resting order by id. Returns false if the id is unknown
    // (already filled, already canceled, or never existed).
    bool cancel_order(OrderId id) {
        Order* o = id_map_.find(id);
        if (!o) return false;

        bool erased = (o->side == Side::Buy) ? remove_from_book(bids_, o)
                                              : remove_from_book(asks_, o);
        if (!erased) return false; // should not happen if id_map_ is consistent

        id_map_.erase(id);
        pool_.release(o);
        return true;
    }

    bool best_bid(Price& out) const {
        if (bids_.empty()) return false;
        out = bids_.begin()->first; // bids_ is ordered descending — see Compare below
        return true;
    }

    bool best_ask(Price& out) const {
        if (asks_.empty()) return false;
        out = asks_.begin()->first; // asks_ is ordered ascending
        return true;
    }

    // Copies up to `max_levels` of the best price levels on `side` (best
    // first) into caller-owned `out`, returns how many were written. Read-
    // only, no allocation — this is the signal layer's (Phase 3) view into
    // depth. Part of the contract the Phase 4 container swap must preserve.
    std::size_t top_levels(Side side, LevelQty* out, std::size_t max_levels) const {
        return side == Side::Buy ? copy_top(bids_, out, max_levels)
                                 : copy_top(asks_, out, max_levels);
    }

    // Diagnostic only — walks every price level. Not on the hot path;
    // for tests/tooling, never call this from a path an incoming order takes.
    Quantity total_resting_qty() const {
        Quantity total = 0;
        for (const auto& [price, level] : bids_) total += level.total_qty;
        for (const auto& [price, level] : asks_) total += level.total_qty;
        return total;
    }

private:
    template <typename Levels>
    static std::size_t copy_top(const Levels& levels, LevelQty* out, std::size_t max_levels) {
        std::size_t n = 0;
        for (auto it = levels.begin(); it != levels.end() && n < max_levels; ++it, ++n) {
            out[n] = LevelQty{it->first, it->second.total_qty};
        }
        return n;
    }

    template <typename Levels>
    static bool remove_from_book(Levels& levels, Order* o) {
        auto it = levels.find(o->price);
        if (it == levels.end()) return false;
        it->second.remove(o);
        if (it->second.empty()) levels.erase(it);
        return true;
    }

    // Match an incoming order against the resting book, price-time priority,
    // emitting Fill events and decrementing qty in place until either the
    // incoming order is filled or the book has nothing left to match against.
    void match(Order* incoming) {
        if (incoming->side == Side::Buy) {
            while (incoming->quantity > 0 && !asks_.empty()) {
                auto best = asks_.begin();
                if (best->first > incoming->price) break; // no crossing price left
                match_against_level(incoming, best->second, best->first);
                if (best->second.empty()) asks_.erase(best);
            }
        } else {
            while (incoming->quantity > 0 && !bids_.empty()) {
                auto best = bids_.begin();
                if (best->first < incoming->price) break;
                match_against_level(incoming, best->second, best->first);
                if (best->second.empty()) bids_.erase(best);
            }
        }
    }

    void match_against_level(Order* incoming, PriceLevel& level, Price trade_price) {
        while (incoming->quantity > 0 && !level.empty()) {
            Order* resting = level.head;
            Quantity traded = std::min(incoming->quantity, resting->quantity);

            incoming->quantity -= traded;
            level.reduce_head(traded);

            if (on_fill_) on_fill_(Fill{resting->id, incoming->id, trade_price, traded});

            if (resting->quantity == 0) {
                Order* dead = resting;
                level.remove(dead);
                id_map_.erase(dead->id);
                pool_.release(dead);
            }
        }
    }

    struct DescendingPrice { bool operator()(Price a, Price b) const { return a > b; } };

    // Smallest power of two >= 2*PoolCapacity, so the id map keeps a load
    // factor <= 0.5 even at full pool occupancy (short probe chains).
    static constexpr std::size_t id_map_capacity() {
        std::size_t n = 1;
        while (n < 2 * PoolCapacity) n <<= 1;
        return n;
    }

    std::map<Price, PriceLevel, DescendingPrice> bids_; // best bid first
    std::map<Price, PriceLevel>                  asks_; // best ask first

    MemoryPool<Order, PoolCapacity> pool_;
    OrderIdMap<id_map_capacity()> id_map_;
    FillHandler on_fill_;
};
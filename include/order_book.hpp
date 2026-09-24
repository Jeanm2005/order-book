#pragma once
#include <algorithm>
#include <cstddef>
#include "order.hpp"
#include "price_level.hpp"
#include "level_ladder.hpp"
#include "memory_pool.hpp"
#include "order_id_map.hpp"

struct Fill {
    OrderId  resting_id;
    OrderId  incoming_id;
    Price    price;   // trade prints at the resting order's price (price-time priority)
    Quantity qty;
};

// One aggregated price level as seen from outside the book (L2 view).
struct LevelQty {
    Price    price;
    Quantity qty; // always > 0: empty levels are erased, never left resting
};

// Default hot-range width per side, in ticks. 1024 levels x 32-byte
// PriceLevel = 32 KB per side: the whole hot range of both sides fits in L2
// alongside the order pool's hot slots. Anything further than ~512 ticks
// from top of book falls back to the map tail (see level_ladder.hpp).
inline constexpr std::size_t kDefaultLadderWindow = 1024;

template <std::size_t PoolCapacity = 1 << 16, std::size_t WindowSize = kDefaultLadderWindow>
class OrderBook {
public:
    using FillHandler = void(*)(const Fill&); // raw function pointer: no vtable, no heap capture

    explicit OrderBook(FillHandler on_fill) : on_fill_(on_fill) {}

    void add_order(OrderId id, Side side, Price price, Quantity qty, Nanos ts) {
        Order* o = pool_.allocate(Order{id, side, price, qty, ts, nullptr, nullptr});
        if (!o) return; // pool exhausted — counted as a hard error upstream, never thrown here

        match(o);
        if (o->quantity > 0) {
            // remainder rests on the book
            if (o->side == Side::Buy) {
                bids_.level_for_insert(o->price).push_back(o);
            } else {
                asks_.level_for_insert(o->price).push_back(o);
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
        const PriceLevel* lvl = bids_.best();
        if (!lvl) return false;
        out = lvl->price;
        return true;
    }

    bool best_ask(Price& out) const {
        const PriceLevel* lvl = asks_.best();
        if (!lvl) return false;
        out = lvl->price;
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
        auto add = [&total](const PriceLevel& level) { total += level.total_qty; };
        bids_.for_each_level(add);
        asks_.for_each_level(add);
        return total;
    }

private:
    template <typename Ladder>
    static std::size_t copy_top(const Ladder& levels, LevelQty* out, std::size_t max_levels) {
        std::size_t n = 0;
        levels.for_each_best(max_levels, [&](const PriceLevel& level) {
            out[n++] = LevelQty{level.price, level.total_qty};
        });
        return n;
    }

    template <typename Ladder>
    static bool remove_from_book(Ladder& levels, Order* o) {
        PriceLevel* level = levels.find(o->price);
        if (!level) return false;
        level->remove(o);
        if (level->empty()) levels.erase_level(level);
        return true;
    }

    // Match an incoming order against the resting book, price-time priority,
    // emitting Fill events and decrementing qty in place until either the
    // incoming order is filled or the book has nothing left to match against.
    void match(Order* incoming) {
        if (incoming->side == Side::Buy) {
            while (incoming->quantity > 0) {
                PriceLevel* best = asks_.best();
                if (!best || best->price > incoming->price) break; // no crossing price left
                match_against_level(incoming, *best, best->price);
                if (best->empty()) asks_.erase_level(best);
            }
        } else {
            while (incoming->quantity > 0) {
                PriceLevel* best = bids_.best();
                if (!best || best->price < incoming->price) break;
                match_against_level(incoming, *best, best->price);
                if (best->empty()) bids_.erase_level(best);
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

    // Smallest power of two >= 2*PoolCapacity, so the id map keeps a load
    // factor <= 0.5 even at full pool occupancy (short probe chains).
    static constexpr std::size_t id_map_capacity() {
        std::size_t n = 1;
        while (n < 2 * PoolCapacity) n <<= 1;
        return n;
    }

    LevelLadder<true, WindowSize>  bids_; // best = highest price
    LevelLadder<false, WindowSize> asks_; // best = lowest price

    MemoryPool<Order, PoolCapacity> pool_;
    OrderIdMap<id_map_capacity()> id_map_;
    FillHandler on_fill_;
};
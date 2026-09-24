#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <type_traits>
#include <utility>
#include "order.hpp"
#include "price_level.hpp"

// Price levels for ONE side of the book (Phase 4 replacement for the
// std::map-only placeholder).
//
// Hot range: a flat array of WindowSize consecutive ticks [base_, base_ +
// WindowSize), plus a bitmap of which slots hold a non-empty level, so
// "next best level" is a ctz/clz scan over 64-bit words instead of a tree
// walk. Long tail: anything outside the window lives in a std::map.
//
// Invariant: a given price lives in exactly one place — the window if it's
// in [base_, base_ + WindowSize), otherwise the tail. recenter() keeps this
// true by migrating levels both ways whenever base_ moves.
//
// Recenter policy: the window follows the top of book. An insert outside
// the window on the *better* side (above it for bids, below it for asks),
// or any insert while the window is empty, re-anchors the window centered
// on that price. Inserts outside on the *worse* side just go to the tail —
// that's the long tail the map is for. Only the tail (and a recenter that
// evicts into it) can allocate; everything in the hot range is flat.
//
// Prices are assumed to be at least WindowSize ticks away from the int64
// limits (base_ arithmetic would overflow otherwise).
template <bool IsBid, std::size_t WindowSize>
class LevelLadder {
    static_assert(WindowSize >= 64 && (WindowSize & (WindowSize - 1)) == 0,
                  "WindowSize must be a power of two >= 64 (one bitmap word minimum)");

public:
    bool empty() const { return window_count_ == 0 && tail_.empty(); }

    // Best level on this side, or nullptr if the side is empty.
    PriceLevel* best() { return const_cast<PriceLevel*>(std::as_const(*this).best()); }
    const PriceLevel* best() const {
        const PriceLevel* w = best_idx_ >= 0 ? &levels_[static_cast<std::size_t>(best_idx_)] : nullptr;
        if (tail_.empty()) return w;
        const PriceLevel* t = &tail_.begin()->second;
        if (!w) return t;
        return better(t->price, w->price) ? t : w;
    }

    // Existing level at `price`, or nullptr.
    PriceLevel* find(Price price) {
        if (in_window(price)) {
            std::size_t idx = index_of(price);
            return test_bit(idx) ? &levels_[idx] : nullptr;
        }
        auto it = tail_.find(price);
        return it == tail_.end() ? nullptr : &it->second;
    }

    // Level at `price`, created empty if it doesn't exist yet. The caller
    // must push at least one order into it before the next erase_level()/
    // best() call — an empty level is never left behind.
    PriceLevel& level_for_insert(Price price) {
        if (!in_window(price) && (window_count_ == 0 || beyond_better_edge(price))) {
            recenter(price - static_cast<Price>(WindowSize / 2));
        }
        if (in_window(price)) {
            std::size_t idx = index_of(price);
            if (!test_bit(idx)) {
                levels_[idx] = PriceLevel{price, 0, nullptr, nullptr};
                set_bit(idx);
                ++window_count_;
                if (best_idx_ < 0 || better_idx(static_cast<std::ptrdiff_t>(idx), best_idx_)) {
                    best_idx_ = static_cast<std::ptrdiff_t>(idx);
                }
            }
            return levels_[idx];
        }
        auto [it, inserted] = tail_.try_emplace(price);
        if (inserted) it->second.price = price;
        return it->second;
    }

    // Removes a level that has just become empty.
    void erase_level(PriceLevel* level) {
        Price price = level->price;
        if (in_window(price)) {
            std::size_t idx = index_of(price);
            clear_bit(idx);
            --window_count_;
            if (static_cast<std::ptrdiff_t>(idx) == best_idx_) {
                best_idx_ = next_worse(best_idx_);
            }
        } else {
            tail_.erase(price);
        }
    }

    // Visits up to `max_levels` levels best-first, merging window and tail
    // (both are already sorted best-first; the tail can hold better prices
    // than the window after an empty-window recenter, so merge, don't concat).
    template <typename F>
    void for_each_best(std::size_t max_levels, F&& f) const {
        std::ptrdiff_t idx = best_idx_;
        if (tail_.empty()) { // common case: pure bitmap walk, no merge compares
            for (std::size_t n = 0; n < max_levels && idx >= 0; ++n) {
                f(levels_[static_cast<std::size_t>(idx)]);
                idx = next_worse(idx);
            }
            return;
        }
        auto it = tail_.begin();
        for (std::size_t n = 0; n < max_levels; ++n) {
            bool have_w = idx >= 0;
            bool have_t = it != tail_.end();
            if (!have_w && !have_t) return;
            if (have_w && (!have_t || better(levels_[static_cast<std::size_t>(idx)].price, it->first))) {
                f(levels_[static_cast<std::size_t>(idx)]);
                idx = next_worse(idx);
            } else {
                f(it->second);
                ++it;
            }
        }
    }

    // Visits every level, unordered. Diagnostics only (walks the window).
    template <typename F>
    void for_each_level(F&& f) const {
        for (std::size_t w = 0; w < kWords; ++w) {
            for (std::uint64_t word = bits_[w]; word; word &= word - 1) {
                f(levels_[w * 64 + static_cast<std::size_t>(std::countr_zero(word))]);
            }
        }
        for (const auto& [price, level] : tail_) f(level);
    }

    // True if `price` currently maps to the flat window rather than the map
    // tail. Lets tests pin the recenter *policy*, which is a performance
    // property the differential tests can't see (the tail is equally correct).
    bool in_hot_window(Price price) const { return in_window(price); }

private:
    static constexpr std::size_t kWords = WindowSize / 64;
    using TailCompare = std::conditional_t<IsBid, std::greater<Price>, std::less<Price>>;

    static bool better(Price a, Price b) { return IsBid ? a > b : a < b; }
    // Index order == price order, so "better" by index follows the same rule.
    static bool better_idx(std::ptrdiff_t a, std::ptrdiff_t b) { return IsBid ? a > b : a < b; }

    bool in_window(Price p) const { return anchored_ && p >= base_ && p < base_ + static_cast<Price>(WindowSize); }
    bool beyond_better_edge(Price p) const {
        return IsBid ? p >= base_ + static_cast<Price>(WindowSize) : p < base_;
    }
    std::size_t index_of(Price p) const { return static_cast<std::size_t>(p - base_); }

    bool test_bit(std::size_t i) const { return (bits_[i >> 6] >> (i & 63)) & 1u; }
    void set_bit(std::size_t i) { bits_[i >> 6] |= std::uint64_t{1} << (i & 63); }
    void clear_bit(std::size_t i) { bits_[i >> 6] &= ~(std::uint64_t{1} << (i & 63)); }

    // Highest set index <= i, or -1.
    std::ptrdiff_t scan_down(std::ptrdiff_t i) const {
        if (i < 0) return -1;
        std::size_t w = static_cast<std::size_t>(i) >> 6;
        std::uint64_t word = bits_[w] & (~std::uint64_t{0} >> (63 - (static_cast<std::size_t>(i) & 63)));
        for (;;) {
            if (word) return static_cast<std::ptrdiff_t>(w * 64 + 63 - static_cast<std::size_t>(std::countl_zero(word)));
            if (w == 0) return -1;
            word = bits_[--w];
        }
    }

    // Lowest set index >= i, or -1.
    std::ptrdiff_t scan_up(std::ptrdiff_t i) const {
        if (i >= static_cast<std::ptrdiff_t>(WindowSize)) return -1;
        std::size_t w = static_cast<std::size_t>(i) >> 6;
        std::uint64_t word = bits_[w] & (~std::uint64_t{0} << (static_cast<std::size_t>(i) & 63));
        for (;;) {
            if (word) return static_cast<std::ptrdiff_t>(w * 64 + static_cast<std::size_t>(std::countr_zero(word)));
            if (++w == kWords) return -1;
            word = bits_[w];
        }
    }

    std::ptrdiff_t next_worse(std::ptrdiff_t idx) const { return IsBid ? scan_down(idx - 1) : scan_up(idx + 1); }
    std::ptrdiff_t best_in_window() const {
        return IsBid ? scan_down(static_cast<std::ptrdiff_t>(WindowSize) - 1) : scan_up(0);
    }

    // Moves the window to [new_base, new_base + WindowSize), preserving the
    // one-home-per-price invariant. Off the common path: only runs when the
    // top of book leaves the window or the window drains.
    void recenter(Price new_base) {
        if (!anchored_) {
            anchored_ = true;
            base_ = new_base;
            best_idx_ = -1;
            return;
        }

        const Price new_end = new_base + static_cast<Price>(WindowSize);
        const Price delta = new_base - base_;

        // 1. Evict window levels that fall outside the new range into the tail.
        for (std::size_t w = 0; w < kWords; ++w) {
            for (std::uint64_t word = bits_[w]; word; word &= word - 1) {
                std::size_t idx = w * 64 + static_cast<std::size_t>(std::countr_zero(word));
                Price p = levels_[idx].price;
                if (p < new_base || p >= new_end) {
                    tail_.emplace(p, levels_[idx]);
                    clear_bit(idx);
                    --window_count_;
                }
            }
        }

        // 2. Shift survivors to their new index, in place. Walking in the
        //    direction of the shift means a slot is always vacated before
        //    anything is written to it.
        if (delta > 0) {
            for (std::size_t idx = 0; idx < WindowSize; ++idx) {
                if (!test_bit(idx)) continue;
                std::size_t to = idx - static_cast<std::size_t>(delta);
                levels_[to] = levels_[idx];
                clear_bit(idx);
                set_bit(to);
            }
        } else if (delta < 0) {
            for (std::size_t idx = WindowSize; idx-- > 0;) {
                if (!test_bit(idx)) continue;
                std::size_t to = idx + static_cast<std::size_t>(-delta);
                levels_[to] = levels_[idx];
                clear_bit(idx);
                set_bit(to);
            }
        }
        base_ = new_base;

        // 3. Pull tail levels that now fall inside the window into it.
        const Price lo = new_base, hi = new_end - 1;
        auto first = tail_.lower_bound(IsBid ? hi : lo);
        auto last = tail_.upper_bound(IsBid ? lo : hi);
        for (auto it = first; it != last; ++it) {
            std::size_t idx = index_of(it->first);
            levels_[idx] = it->second;
            set_bit(idx);
            ++window_count_;
        }
        tail_.erase(first, last);

        best_idx_ = best_in_window();
    }

    Price base_ = 0;
    bool anchored_ = false;
    std::size_t window_count_ = 0;   // non-empty levels currently in the window
    std::ptrdiff_t best_idx_ = -1;   // best non-empty window slot, -1 if window empty
    std::array<std::uint64_t, kWords> bits_{};
    std::array<PriceLevel, WindowSize> levels_{};
    std::map<Price, PriceLevel, TailCompare> tail_;
};

#pragma once

#include <slick-trader/utils/order_types.hpp>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace slick::trader {

// One instrument's price levels, aggregated from order-by-order updates in the
// exchange's own wire units, remembering which levels changed since the last flush.
// Converting to price_t only when a level is published keeps the per-update cost to
// two hash lookups.
//
// Each update subtracts what the order was (`prev_*`) and adds what it is, so the view
// needs no order map of its own. A level stays in the map with quantity 0 until it is
// flushed, so its removal is reported once and then forgotten.
//
// The best price of each side is kept as the levels change. Only a best level
// emptying costs a look over the side's levels, and only when the best price is next
// asked for.
class LevelBook {
public:
    // What best() returns for a side with no orders.
    static constexpr int64_t NO_PRICE = std::numeric_limits<int64_t>::max();

    struct Level {
        int64_t qty = 0;
        uint32_t orders = 0;
        bool dirty = false;
    };

    // A level as reported: its price and what now rests there (0 = gone).
    struct LevelChange {
        Side side;
        int64_t price;
        int64_t qty;
        uint32_t orders;
    };

    // An order was `prev_qty` at `prev_price` on `prev_side` and is now `qty` at `price`
    // on `side`. A quantity of 0 means "not on the book".
    void apply(Side prev_side, int64_t prev_price, int64_t prev_qty, Side side, int64_t price, int64_t qty) {
        if (prev_qty > 0) {
            adjust(prev_side, prev_price, -prev_qty, -1);
        }
        if (qty > 0) {
            adjust(side, price, qty, +1);
        }
    }

    [[nodiscard]] bool dirty() const noexcept { return !dirty_.empty(); }

    // The side's best price (the highest bid, the lowest offer), or NO_PRICE.
    [[nodiscard]] int64_t best(Side side) noexcept {
        Best& best = side == Side::BUY ? best_bid_ : best_ask_;
        if (best.stale) {
            best.stale = false;
            best.price = NO_PRICE;
            for (const auto& [price, level] : side == Side::BUY ? bids_ : asks_) {
                if (level.qty > 0 && (best.price == NO_PRICE || better(side, price, best.price))) {
                    best.price = price;
                }
            }
        }
        return best.price;
    }

    // Calls `f(LevelChange)` for each level changed since the last flush, then forgets
    // the levels that emptied.
    template <typename F>
    void flush(F&& f) {
        for (const auto& [side, price] : dirty_) {
            auto& levels = side == Side::BUY ? bids_ : asks_;
            auto it = levels.find(price);
            if (it == levels.end()) {
                continue;
            }
            f(LevelChange{side, price, it->second.qty, it->second.orders});
            if (it->second.qty <= 0) {
                levels.erase(it);
            } else {
                it->second.dirty = false;
            }
        }
        dirty_.clear();
    }

    // Calls `f(LevelChange)` for every non-empty level: a snapshot.
    template <typename F>
    void for_each_level(F&& f) const {
        for (const auto& [price, level] : bids_) {
            if (level.qty > 0) {
                f(LevelChange{Side::BUY, price, level.qty, level.orders});
            }
        }
        for (const auto& [price, level] : asks_) {
            if (level.qty > 0) {
                f(LevelChange{Side::SELL, price, level.qty, level.orders});
            }
        }
    }

    [[nodiscard]] size_t level_count() const noexcept { return bids_.size() + asks_.size(); }

private:
    struct Best {
        int64_t price = NO_PRICE;
        bool stale = false;   // the best level emptied: the side has to be looked over
    };

    static bool better(Side side, int64_t price, int64_t than) noexcept {
        return side == Side::BUY ? price > than : price < than;
    }

    void adjust(Side side, int64_t price, int64_t qty_delta, int32_t order_delta) {
        auto& level = (side == Side::BUY ? bids_ : asks_)[price];
        level.qty += qty_delta;
        level.orders = static_cast<uint32_t>(static_cast<int64_t>(level.orders) + order_delta);
        if (!level.dirty) {
            level.dirty = true;
            dirty_.emplace_back(side, price);
        }
        Best& best = side == Side::BUY ? best_bid_ : best_ask_;
        if (level.qty <= 0) {
            best.stale |= price == best.price;
        } else if (!best.stale && (best.price == NO_PRICE || better(side, price, best.price))) {
            best.price = price;
        }
    }

    std::unordered_map<int64_t, Level> bids_;
    std::unordered_map<int64_t, Level> asks_;
    std::vector<std::pair<Side, int64_t>> dirty_;
    Best best_bid_;
    Best best_ask_;
};

} // namespace slick::trader

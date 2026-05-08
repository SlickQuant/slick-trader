#pragma once

#include <slick-trader/core/position.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <slick-trader/utils/types.hpp>
#include <slick/logger.hpp>
#include <unordered_map>
#include <string>

namespace slick::trader {

// PnL snapshot for a strategy
struct PnLSnapshot {
    std::string strategy_id;
    price_t realized_pnl{0};    // Total realized PnL
    price_t unrealized_pnl{0};  // Total unrealized PnL
    price_t total_pnl{0};       // realized + unrealized
    uint64_t timestamp_ns{0};   // Snapshot timestamp

    double get_realized() const { return from_price_t(realized_pnl); }
    double get_unrealized() const { return from_price_t(unrealized_pnl); }
    double get_total() const { return from_price_t(total_pnl); }
};

// Single-writer PnL calculator
// Tracks mark prices and calculates PnL using PositionTracker data
// NOTE: Single-writer only (called from StrategyManager thread)
class PnLCalculator {
public:
    explicit PnLCalculator(PositionTracker& position_tracker)
        : position_tracker_(position_tracker) {}

    ~PnLCalculator() = default;

    // Update mark price from a single-price source (fills, trades).
    // Stores the same price as both bid and ask.
    void update_mark_price(const std::string& instrument, price_t price, uint64_t timestamp_ns);

    // Update bid and ask from a top-of-book event.
    // Used by calculate_pnl for directional pricing (long→bid, short→ask).
    void update_top_of_book(const std::string& instrument, price_t bid, price_t ask, uint64_t timestamp_ns);

    // Calculate PnL for a specific strategy
    PnLSnapshot calculate_pnl(const std::string& strategy_id, uint64_t timestamp_ns) const;

    // Calculate PnL for all strategies
    std::unordered_map<std::string, PnLSnapshot> calculate_all_pnl(uint64_t timestamp_ns) const;

    // Get current mark price (mid of bid/ask if available, else last single price)
    price_t get_mark_price(const std::string& instrument) const;

    // Check if mark price exists
    bool has_mark_price(const std::string& instrument) const;

private:
    struct MarkPrice {
        price_t price{0};
        uint64_t timestamp_ns{0};
    };

    PositionTracker& position_tracker_;
    std::unordered_map<std::string, MarkPrice> bid_prices_;
    std::unordered_map<std::string, MarkPrice> ask_prices_;
};

} // namespace slick::trader

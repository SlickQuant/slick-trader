#pragma once

#include <slick-trader/core/position.hpp>
#include <slick-trader/events/order_event.hpp>
#include <slick/logger.hpp>
#include <unordered_map>
#include <string>

namespace slick::trader {

// Single-writer position tracking with FIFO accounting
// Tracks positions per strategy and instrument
// NOTE: Single-writer only (called from StrategyManager thread)
// Readers use lock-free position_update_queue for consumption
class PositionTracker {
public:
    PositionTracker() = default;
    ~PositionTracker() = default;

    // Update position from a fill event
    // Uses FIFO accounting to calculate realized PnL
    void on_fill(const std::string& strategy_id, const OrderFillEvent& fill);

    // Get current position for strategy/instrument (single-reader safe)
    Position get_position(const std::string& strategy_id, std::string_view instrument) const;

    // Get all positions for a strategy (single-reader safe)
    std::unordered_map<std::string, Position> get_all_positions(const std::string& strategy_id) const;

    // Get all positions across all strategies (single-reader safe)
    std::unordered_map<std::string, std::unordered_map<std::string, Position>> get_all_positions() const;

    // Clear positions for a strategy
    void clear_strategy_positions(const std::string& strategy_id);

    // Clear all positions
    void clear_all();

private:
    // FIFO lot for position tracking
    struct Lot {
        qty_t quantity;
        price_t price;
        uint64_t timestamp;

        Lot(qty_t q, price_t p, uint64_t ts)
            : quantity(q), price(p), timestamp(ts) {}
    };

    // Position with FIFO lots for realized PnL calculation
    struct PositionWithLots {
        Position position;
        std::vector<Lot> lots;  // FIFO queue of entry lots

        PositionWithLots() = default;
        explicit PositionWithLots(const std::string& inst) : position(inst) {}
    };

    // Apply fill to position using FIFO accounting
    void apply_fill(PositionWithLots& pos, bool is_buy, qty_t fill_qty, price_t fill_price, uint64_t timestamp);

    // Calculate new average price when adding to position
    price_t calculate_new_avg_price(price_t current_avg, qty_t current_qty,
                                     price_t fill_price, qty_t fill_qty) const;

    // Positions: strategy_id -> instrument -> PositionWithLots
    std::unordered_map<std::string, std::unordered_map<std::string, PositionWithLots>> positions_;
};

} // namespace slick::trader

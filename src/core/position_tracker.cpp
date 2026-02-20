#include <slick-trader/core/position_tracker.hpp>
#include <algorithm>

namespace slick::trader {

void PositionTracker::on_fill(const std::string& strategy_id, const OrderFillEvent& fill) {
    // Single-writer: called only from StrategyManager thread

    const std::string instrument(fill.instrument);
    auto& strategy_positions = positions_[strategy_id];
    auto& pos_with_lots = strategy_positions[instrument];

    if (pos_with_lots.position.instrument.empty()) {
        pos_with_lots.position.instrument = instrument;
    }

    const bool is_buy = (fill.side == Side::BUY);
    const qty_t fill_qty = fill.fill_quantity;
    const price_t fill_price = fill.fill_price;
    const uint64_t timestamp = fill.timestamp_ns;

    apply_fill(pos_with_lots, is_buy, fill_qty, fill_price, timestamp);
    pos_with_lots.position.last_update_time = timestamp;

    LOG_DEBUG("PositionTracker: {} {} fill: {} {}@{} | pos: {} avg: {} realized_pnl: {}",
              strategy_id, instrument,
              is_buy ? "BUY" : "SELL",
              from_qty_t(fill_qty), from_price_t(fill_price),
              from_qty_t(pos_with_lots.position.quantity),
              from_price_t(pos_with_lots.position.avg_price),
              from_price_t(pos_with_lots.position.realized_pnl));
}

void PositionTracker::apply_fill(PositionWithLots& pos, bool is_buy, qty_t fill_qty,
                                   price_t fill_price, uint64_t timestamp) {
    Position& p = pos.position;
    std::vector<Lot>& lots = pos.lots;

    const bool position_is_long = p.is_long();
    const bool position_is_short = p.is_short();
    const bool position_is_flat = p.is_flat();

    // Case 1: Opening new position or adding to existing position in same direction
    if (position_is_flat || (is_buy && position_is_long) || (!is_buy && position_is_short)) {
        // Add to position
        const qty_t signed_fill = is_buy ? fill_qty : -fill_qty;

        if (!position_is_flat) {
            p.avg_price = calculate_new_avg_price(p.avg_price, p.quantity, fill_price, signed_fill);
        } else {
            p.avg_price = fill_price;
        }

        p.quantity += signed_fill;
        lots.emplace_back(fill_qty, fill_price, timestamp);
        return;
    }

    // Case 2: Reducing or reversing position (FIFO accounting for realized PnL)
    qty_t remaining_qty = fill_qty;

    while (remaining_qty > 0 && !lots.empty()) {
        Lot& oldest_lot = lots.front();
        const qty_t close_qty = std::min(remaining_qty, oldest_lot.quantity);

        // Calculate realized PnL for this lot closure
        const double pnl_per_unit = is_buy
            ? from_price_t(oldest_lot.price) - from_price_t(fill_price)  // Closing short
            : from_price_t(fill_price) - from_price_t(oldest_lot.price); // Closing long

        const double realized = pnl_per_unit * from_qty_t(close_qty);
        p.realized_pnl += to_price_t(realized);

        // Update position quantity
        const qty_t signed_close = is_buy ? close_qty : -close_qty;
        p.quantity += signed_close;

        // Deduct from lot
        oldest_lot.quantity -= close_qty;
        if (oldest_lot.quantity == 0) {
            lots.erase(lots.begin());
        }

        remaining_qty -= close_qty;
    }

    // If we still have remaining quantity, we're reversing the position
    if (remaining_qty > 0) {
        const qty_t signed_remaining = is_buy ? remaining_qty : -remaining_qty;
        p.quantity += signed_remaining;
        p.avg_price = fill_price;
        lots.emplace_back(remaining_qty, fill_price, timestamp);
    }

    // Update avg_price if position is now flat
    if (p.is_flat()) {
        p.avg_price = 0;
    }
}

price_t PositionTracker::calculate_new_avg_price(price_t current_avg, qty_t current_qty,
                                                   price_t fill_price, qty_t fill_qty) const {
    const double total_cost = from_price_t(current_avg) * std::abs(from_qty_t(current_qty))
                            + from_price_t(fill_price) * std::abs(from_qty_t(fill_qty));
    const double total_qty = std::abs(from_qty_t(current_qty)) + std::abs(from_qty_t(fill_qty));

    return to_price_t(total_cost / total_qty);
}

Position PositionTracker::get_position(const std::string& strategy_id, const std::string& instrument) const {
    // Single-reader safe (called from same thread)

    auto strategy_it = positions_.find(strategy_id);
    if (strategy_it == positions_.end()) {
        return Position(instrument);
    }

    auto pos_it = strategy_it->second.find(instrument);
    if (pos_it == strategy_it->second.end()) {
        return Position(instrument);
    }

    return pos_it->second.position;
}

std::unordered_map<std::string, Position> PositionTracker::get_all_positions(const std::string& strategy_id) const {

    std::unordered_map<std::string, Position> result;

    auto strategy_it = positions_.find(strategy_id);
    if (strategy_it == positions_.end()) {
        return result;
    }

    for (const auto& [instrument, pos_with_lots] : strategy_it->second) {
        result.emplace(instrument, pos_with_lots.position);
    }

    return result;
}

std::unordered_map<std::string, std::unordered_map<std::string, Position>>
PositionTracker::get_all_positions() const {

    std::unordered_map<std::string, std::unordered_map<std::string, Position>> result;

    for (const auto& [strategy_id, strategy_positions] : positions_) {
        auto& strategy_result = result[strategy_id];
        for (const auto& [instrument, pos_with_lots] : strategy_positions) {
            strategy_result.emplace(instrument, pos_with_lots.position);
        }
    }

    return result;
}

void PositionTracker::clear_strategy_positions(const std::string& strategy_id) {
    positions_.erase(strategy_id);
    LOG_INFO("PositionTracker: Cleared positions for strategy {}", strategy_id);
}

void PositionTracker::clear_all() {
    positions_.clear();
    LOG_INFO("PositionTracker: Cleared all positions");
}

} // namespace slick::trader

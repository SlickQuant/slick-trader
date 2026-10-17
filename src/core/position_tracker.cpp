#include <slick/logger.hpp>
#include <slick-trader/core/position_tracker.hpp>
#include <algorithm>
#include <cmath>

namespace slick::trader {

void PositionTracker::attach_store(PositionStore& store) {
    store_ = &store;
    for (std::size_t broker = 0; broker < journaled_.size(); ++broker) {
        journaled_[broker] = store.journaled(static_cast<BrokerId>(broker));
    }
    store.for_each([this](uint32_t slot, std::string_view strategy_id, const Position& position,
                          std::span<const PositionLot> lots) {
        auto& pos = positions_[std::string(strategy_id)][position.instrument];
        pos.position = position;
        pos.lots.assign(lots.begin(), lots.end());
        pos.store_slot = slot;
        // The multiplier the position was last valued with, until the instrument's
        // definition says it again.
        contract_multipliers_[position.instrument] = position.multiplier;
        LOG_INFO("PositionTracker: restored {} {} pos: {} avg: {} realized_pnl: {} ({} open lot(s))",
                 strategy_id, position.instrument, from_qty_t(position.quantity), from_price_t(position.avg_price),
                 from_price_t(position.realized_pnl), lots.size());
    });
}

void PositionTracker::persist(const std::string& strategy_id, PositionWithLots& pos, const OrderFillEvent* fill) {
    if (store_) {
        pos.store_slot = store_->save(pos.store_slot, strategy_id, pos.position, pos.lots,
                                      fill ? fill->broker_id : BrokerId::UNKNOWN, fill ? fill->journal_seq : 0);
    }
}

void PositionTracker::fill_dispatched(const OrderFillEvent& fill) noexcept {
    const auto broker = static_cast<std::size_t>(fill.broker_id);
    if (fill.journal_seq == 0 || broker >= journaled_.size() || fill.journal_seq <= journaled_[broker]) {
        return;
    }
    journaled_[broker] = fill.journal_seq;
    if (store_) {
        store_->mark_journaled(fill.broker_id, fill.journal_seq);
    }
}

void PositionTracker::on_fill(const std::string& strategy_id, const OrderFillEvent& fill) {
    // Single-writer: called only from StrategyManager thread

    const std::string instrument(fill.instrument);
    // The fill says what a contract is worth: it may come before the instrument's definition
    // does - replayed from a broker's journal at startup - and PnL realized now stays booked.
    if (fill.contract_multiplier > 0.0 && fill.contract_multiplier != get_contract_multiplier(instrument)) {
        set_contract_multiplier(instrument, fill.contract_multiplier);
    }
    auto& strategy_positions = positions_[strategy_id];
    auto& pos_with_lots = strategy_positions[instrument];

    if (pos_with_lots.position.instrument.empty()) {
        pos_with_lots.position.instrument = instrument;
        pos_with_lots.position.multiplier = get_contract_multiplier(instrument);
    }

    const bool is_buy = (fill.side == Side::BUY);
    const qty_t fill_qty = fill.fill_quantity;
    const price_t fill_price = fill.fill_price;
    const uint64_t timestamp = fill.timestamp_ns;

    // Use fee from event if provided (e.g. Coinbase), else compute from fallback FeeStructure
    const price_t fee = (fill.fee_amount > 0)
        ? fill.fee_amount
        : compute_fee(fill_price, fill_qty, fee_structure_);

    apply_fill(pos_with_lots, is_buy, fill_qty, fill_price, timestamp, fee);
    pos_with_lots.position.last_update_time = timestamp;
    persist(strategy_id, pos_with_lots, &fill);

    LOG_INFO("PositionTracker: {} {} fill: {} {}@{} fee:{} | pos: {} avg: {} realized_pnl: {} realized_fees: {} open_fees: {}",
             strategy_id, instrument,
             is_buy ? "BUY" : "SELL",
             from_qty_t(fill_qty), from_price_t(fill_price), from_price_t(fee),
             from_qty_t(pos_with_lots.position.quantity),
             from_price_t(pos_with_lots.position.avg_price),
             from_price_t(pos_with_lots.position.realized_pnl),
             from_price_t(pos_with_lots.position.realized_fees),
             from_price_t(pos_with_lots.position.open_fees));
}

void PositionTracker::apply_fill(PositionWithLots& pos, bool is_buy, qty_t fill_qty,
                                   price_t fill_price, uint64_t timestamp, price_t fee_amount) {
    // Zero and negative prices are real: calendar spreads trade through zero, and so have
    // outright futures. Only the null sentinel is not a price.
    if (fill_price == NULL_PRICE) {
        LOG_ERROR("PositionTracker: fill has no fill_price — skipping P&L update");
        return;
    }
    Position& p = pos.position;
    std::vector<Lot>& lots = pos.lots;

    const bool position_is_long = p.is_long();
    const bool position_is_short = p.is_short();
    const bool position_is_flat = p.is_flat();

    // Case 1: Opening new position or adding to existing position in same direction
    // Entry fee goes into open_fees (deducted from unrealized until the lot is closed)
    if (position_is_flat || (is_buy && position_is_long) || (!is_buy && position_is_short)) {
        const qty_t signed_fill = is_buy ? fill_qty : -fill_qty;

        if (!position_is_flat) {
            p.avg_price = calculate_new_avg_price(p.avg_price, p.quantity, fill_price, signed_fill);
        } else {
            p.avg_price = fill_price;
        }

        p.quantity += signed_fill;
        p.open_fees += fee_amount;  // entry fee reduces unrealized until position is closed
        lots.push_back(Lot{fill_qty, fill_price, timestamp});
        return;
    }

    // Case 2: Reducing or reversing position (FIFO accounting for realized PnL)
    // For each lot closed: transfer its proportional entry fee + exit fee to realized_fees
    qty_t remaining_qty = fill_qty;

    while (remaining_qty > 0 && !lots.empty()) {
        Lot& oldest_lot = lots.front();
        const qty_t close_qty = std::min(remaining_qty, oldest_lot.quantity);

        // Transfer proportional entry fee from open_fees to realized_fees
        // (must use p.quantity BEFORE updating it this iteration)
        const qty_t current_qty = std::abs(p.quantity);
        const price_t entry_fee_transfer = (current_qty > 0)
            ? to_price_t(from_price_t(p.open_fees) * from_qty_t(close_qty) / from_qty_t(current_qty))
            : p.open_fees;

        // Proportional share of exit fee for this lot's portion of the fill
        const price_t exit_fee_portion = (fill_qty > 0)
            ? to_price_t(from_price_t(fee_amount) * from_qty_t(close_qty) / from_qty_t(fill_qty))
            : 0;

        p.realized_fees += entry_fee_transfer + exit_fee_portion;
        p.open_fees     -= entry_fee_transfer;

        // Calculate realized PnL for this lot closure
        const double pnl_per_unit = is_buy
            ? from_price_t(oldest_lot.price) - from_price_t(fill_price)  // Closing short
            : from_price_t(fill_price) - from_price_t(oldest_lot.price); // Closing long

        p.realized_pnl += to_price_t(pnl_per_unit * from_qty_t(close_qty) * p.multiplier);

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
    // All old lots consumed → old open_fees fully moved to realized; new position starts fresh
    if (remaining_qty > 0) {
        const qty_t signed_remaining = is_buy ? remaining_qty : -remaining_qty;
        p.quantity += signed_remaining;
        p.avg_price = fill_price;
        lots.push_back(Lot{remaining_qty, fill_price, timestamp});

        // Entry fee for the reversed position portion
        p.open_fees = (fill_qty > 0)
            ? to_price_t(from_price_t(fee_amount) * from_qty_t(remaining_qty) / from_qty_t(fill_qty))
            : 0;
    }

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

bool PositionTracker::set_contract_multiplier(std::string_view instrument, double multiplier) {
    if (!std::isfinite(multiplier) || multiplier <= 0.0) {
        LOG_ERROR("PositionTracker: rejecting contract multiplier {} for {}", multiplier, instrument);
        return false;
    }

    const std::string key(instrument);
    contract_multipliers_[key] = multiplier;

    // Positions already held revalue from now on; what they realized before stays as booked.
    for (auto& [strategy_id, strategy_positions] : positions_) {
        auto it = strategy_positions.find(key);
        if (it != strategy_positions.end() && it->second.position.multiplier != multiplier) {
            it->second.position.multiplier = multiplier;
            persist(strategy_id, it->second);
        }
    }
    return true;
}

double PositionTracker::get_contract_multiplier(std::string_view instrument) const {
    auto it = contract_multipliers_.find(std::string(instrument));
    return it == contract_multipliers_.end() ? 1.0 : it->second;
}

Position PositionTracker::get_position(const std::string& strategy_id, std::string_view instrument) const {
    // Single-reader safe (called from same thread)

    auto strategy_it = positions_.find(strategy_id);
    if (strategy_it == positions_.end()) {
        return Position(instrument);
    }

    auto pos_it = strategy_it->second.find(std::string(instrument));
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
    if (store_) {
        store_->erase_strategy(strategy_id);
    }
    LOG_INFO("PositionTracker: Cleared positions for strategy {}", strategy_id);
}

void PositionTracker::clear_all() {
    positions_.clear();
    if (store_) {
        store_->clear();
    }
    LOG_INFO("PositionTracker: Cleared all positions");
}

} // namespace slick::trader

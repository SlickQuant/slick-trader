#include <slick-trader/core/pnl_calculator.hpp>
#include <chrono>

namespace slick::trader {

void PnLCalculator::update_mark_price(const std::string& instrument, price_t price, uint64_t timestamp_ns) {
    mark_prices_[instrument] = {price, timestamp_ns};
}

price_t PnLCalculator::get_mark_price(const std::string& instrument) const {
    auto it = mark_prices_.find(instrument);
    if (it == mark_prices_.end()) {
        return 0;
    }
    return it->second.price;
}

bool PnLCalculator::has_mark_price(const std::string& instrument) const {
    return mark_prices_.find(instrument) != mark_prices_.end();
}

PnLSnapshot PnLCalculator::calculate_pnl(const std::string& strategy_id, uint64_t timestamp_ns) const {
    PnLSnapshot snapshot;
    snapshot.strategy_id = strategy_id;
    snapshot.timestamp_ns = timestamp_ns;

    auto positions = position_tracker_.get_all_positions(strategy_id);

    price_t total_realized = 0;
    price_t total_unrealized = 0;

    for (const auto& [instrument, position] : positions) {
        // Add realized PnL
        total_realized += position.realized_pnl;

        // Calculate unrealized PnL if we have a mark price
        if (!position.is_flat()) {
            auto mark_it = mark_prices_.find(instrument);
            if (mark_it != mark_prices_.end()) {
                const price_t mark_price = mark_it->second.price;
                const double unrealized = position.get_unrealized_pnl(mark_price);
                total_unrealized += to_price_t(unrealized);
            }
        }
    }

    snapshot.realized_pnl = total_realized;
    snapshot.unrealized_pnl = total_unrealized;
    snapshot.total_pnl = total_realized + total_unrealized;

    return snapshot;
}

std::unordered_map<std::string, PnLSnapshot>
PnLCalculator::calculate_all_pnl(uint64_t timestamp_ns) const {
    std::unordered_map<std::string, PnLSnapshot> result;

    auto all_positions = position_tracker_.get_all_positions();

    for (const auto& [strategy_id, positions] : all_positions) {
        result.emplace(strategy_id, calculate_pnl(strategy_id, timestamp_ns));
    }

    return result;
}

} // namespace slick::trader

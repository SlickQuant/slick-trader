#include <slick-trader/core/pnl_calculator.hpp>
#include <slick/logger.hpp>
#include <chrono>

namespace slick::trader {

void PnLCalculator::update_mark_price(const std::string& instrument, price_t price, uint64_t timestamp_ns) {
    if (price == NULL_PRICE || price <= 0) {
        LOG_DEBUG("PnLCalculator::update_mark_price: rejected invalid mark price {} for {}", price, instrument);
        return;
    }
    // NULL_PRICE/100 ≈ 9.22e16 (integer units) = ~$9.22e8 per unit — far above any real asset price.
    // Exchange null sentinels (e.g. Coinbase INT64_MAX/1e8) will exceed this threshold.
    constexpr price_t SENTINEL_THRESHOLD = NULL_PRICE / 100;
    if (price >= SENTINEL_THRESHOLD) {
        LOG_WARN("PnLCalculator::update_mark_price: {} rejecting sentinel price={}", instrument, price);
        return;
    }
    bid_prices_[instrument] = {price, timestamp_ns};
    ask_prices_[instrument] = {price, timestamp_ns};
}

void PnLCalculator::update_top_of_book(const std::string& instrument, price_t bid, price_t ask, uint64_t timestamp_ns) {
    if (bid == NULL_PRICE || bid <= 0 || ask == NULL_PRICE || ask <= 0) {
        LOG_DEBUG("PnLCalculator::update_top_of_book: rejected invalid tob for {} bid={} ask={}", instrument, bid, ask);
        return;
    }
    if (bid != bid_prices_[instrument].price || ask != ask_prices_[instrument].price) {
        constexpr price_t SENTINEL_THRESHOLD = NULL_PRICE / 100;
        if (bid >= SENTINEL_THRESHOLD || ask >= SENTINEL_THRESHOLD) {
            LOG_WARN("PnLCalculator::update_top_of_book: {} rejecting sentinel bid={} ask={}", instrument, bid, ask);
            return;
        }
        bid_prices_[instrument] = {bid, timestamp_ns};
        ask_prices_[instrument] = {ask, timestamp_ns};
    }
}

price_t PnLCalculator::get_mark_price(const std::string& instrument) const {
    auto bid_it = bid_prices_.find(instrument);
    auto ask_it = ask_prices_.find(instrument);
    if (bid_it != bid_prices_.end() && ask_it != ask_prices_.end()) {
        return (bid_it->second.price / 2) + (ask_it->second.price / 2);
    }
    if (bid_it != bid_prices_.end()) return bid_it->second.price;
    if (ask_it != ask_prices_.end()) return ask_it->second.price;
    return 0;
}

bool PnLCalculator::has_mark_price(const std::string& instrument) const {
    return bid_prices_.find(instrument) != bid_prices_.end()
        || ask_prices_.find(instrument) != ask_prices_.end();
}

PnLSnapshot PnLCalculator::calculate_pnl(const std::string& strategy_id, uint64_t timestamp_ns) const {
    PnLSnapshot snapshot;
    snapshot.strategy_id = strategy_id;
    snapshot.timestamp_ns = timestamp_ns;

    auto positions = position_tracker_.get_all_positions(strategy_id);

    price_t total_realized = 0;
    price_t total_unrealized = 0;
    price_t total_realized_fees = 0;
    price_t total_open_fees = 0;

    for (const auto& [instrument, position] : positions) {
        // Net realized = gross realized minus fees on closed trades
        total_realized       += position.realized_pnl - position.realized_fees;
        total_realized_fees  += position.realized_fees;
        total_open_fees      += position.open_fees;

        // Net unrealized = mark-to-market minus entry fees paid for currently held lots
        if (!position.is_flat()) {
            const bool is_long = position.quantity > 0;
            const auto& price_map = is_long ? bid_prices_ : ask_prices_;
            auto mark_it = price_map.find(instrument);
            if (mark_it != price_map.end()) {
                const price_t mark_price = mark_it->second.price;
                if (mark_price == NULL_PRICE || mark_price <= 0) {
                    LOG_WARN("PnLCalculator::calculate_pnl: skipping unrealized PnL for {} — invalid mark price {}", instrument, mark_price);
                } else {
                    total_unrealized += to_price_t(position.get_unrealized_pnl(mark_price))
                                        - position.open_fees;
                }
            }
        }
    }

    snapshot.realized_pnl  = total_realized;
    snapshot.unrealized_pnl = total_unrealized;
    snapshot.total_fees     = total_realized_fees + total_open_fees;  // informational
    snapshot.total_pnl      = total_realized + total_unrealized;

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

#include <slick-trader/market_data/order_book.h>
#include <algorithm>
#include <numeric>

namespace slick::trader {

void OrderBook::apply_snapshot(const std::vector<PriceLevel>& bids,
                               const std::vector<PriceLevel>& asks,
                               uint64_t timestamp_ns,
                               uint64_t sequence) {
    clear();

    // Insert all bid levels
    for (const auto& level : bids) {
        if (level.quantity > 0) {
            bids_[level.price] = level;
        }
    }

    // Insert all ask levels
    for (const auto& level : asks) {
        if (level.quantity > 0) {
            asks_[level.price] = level;
        }
    }

    trim_depth();

    last_update_time_ = timestamp_ns;
    sequence_number_ = sequence;
}

void OrderBook::update_bid(price_t price, qty_t quantity) {
    if (quantity == 0) {
        // Remove the level
        bids_.erase(price);
    } else {
        // Update or insert
        bids_[price] = PriceLevel(price, quantity);
    }

    trim_depth();
}

void OrderBook::update_ask(price_t price, qty_t quantity) {
    if (quantity == 0) {
        // Remove the level
        asks_.erase(price);
    } else {
        // Update or insert
        asks_[price] = PriceLevel(price, quantity);
    }

    trim_depth();
}

void OrderBook::update_bids(const std::vector<PriceLevel>& updates) {
    for (const auto& level : updates) {
        update_bid(level.price, level.quantity);
    }
}

void OrderBook::update_asks(const std::vector<PriceLevel>& updates) {
    for (const auto& level : updates) {
        update_ask(level.price, level.quantity);
    }
}

bool OrderBook::get_best_bid(PriceLevel& level) const {
    if (bids_.empty()) {
        return false;
    }

    level = bids_.begin()->second;
    return true;
}

bool OrderBook::get_best_ask(PriceLevel& level) const {
    if (asks_.empty()) {
        return false;
    }

    level = asks_.begin()->second;
    return true;
}

double OrderBook::get_mid_price() const {
    PriceLevel best_bid, best_ask;

    if (!get_best_bid(best_bid) || !get_best_ask(best_ask)) {
        return 0.0;
    }

    return (from_price_t(best_bid.price) + from_price_t(best_ask.price)) / 2.0;
}

double OrderBook::get_spread() const {
    PriceLevel best_bid, best_ask;

    if (!get_best_bid(best_bid) || !get_best_ask(best_ask)) {
        return 0.0;
    }

    return from_price_t(best_ask.price) - from_price_t(best_bid.price);
}

double OrderBook::get_spread_bps() const {
    double mid = get_mid_price();
    if (mid <= 0.0) {
        return 0.0;
    }

    return (get_spread() / mid) * 10000.0;
}

std::vector<PriceLevel> OrderBook::get_top_bids(size_t n) const {
    std::vector<PriceLevel> result;
    result.reserve(std::min(n, bids_.size()));

    auto it = bids_.begin();
    for (size_t i = 0; i < n && it != bids_.end(); ++i, ++it) {
        result.push_back(it->second);
    }

    return result;
}

std::vector<PriceLevel> OrderBook::get_top_asks(size_t n) const {
    std::vector<PriceLevel> result;
    result.reserve(std::min(n, asks_.size()));

    auto it = asks_.begin();
    for (size_t i = 0; i < n && it != asks_.end(); ++i, ++it) {
        result.push_back(it->second);
    }

    return result;
}

qty_t OrderBook::get_bid_volume(size_t levels) const {
    qty_t total = 0;

    auto it = bids_.begin();
    for (size_t i = 0; i < levels && it != bids_.end(); ++i, ++it) {
        total += it->second.quantity;
    }

    return total;
}

qty_t OrderBook::get_ask_volume(size_t levels) const {
    qty_t total = 0;

    auto it = asks_.begin();
    for (size_t i = 0; i < levels && it != asks_.end(); ++i, ++it) {
        total += it->second.quantity;
    }

    return total;
}

double OrderBook::get_bid_vwap(size_t levels) const {
    double total_value = 0.0;
    double total_quantity = 0.0;

    auto it = bids_.begin();
    for (size_t i = 0; i < levels && it != bids_.end(); ++i, ++it) {
        double price = from_price_t(it->second.price);
        double quantity = from_qty_t(it->second.quantity);

        total_value += price * quantity;
        total_quantity += quantity;
    }

    if (total_quantity == 0.0) {
        return 0.0;
    }

    return total_value / total_quantity;
}

double OrderBook::get_ask_vwap(size_t levels) const {
    double total_value = 0.0;
    double total_quantity = 0.0;

    auto it = asks_.begin();
    for (size_t i = 0; i < levels && it != asks_.end(); ++i, ++it) {
        double price = from_price_t(it->second.price);
        double quantity = from_qty_t(it->second.quantity);

        total_value += price * quantity;
        total_quantity += quantity;
    }

    if (total_quantity == 0.0) {
        return 0.0;
    }

    return total_value / total_quantity;
}

double OrderBook::get_volume_imbalance(size_t levels) const {
    double bid_vol = from_qty_t(get_bid_volume(levels));
    double ask_vol = from_qty_t(get_ask_volume(levels));

    double total = bid_vol + ask_vol;
    if (total == 0.0) {
        return 0.0;
    }

    return (bid_vol - ask_vol) / total;
}

qty_t OrderBook::get_bid_quantity_at(price_t price) const {
    auto it = bids_.find(price);
    if (it != bids_.end()) {
        return it->second.quantity;
    }
    return 0;
}

qty_t OrderBook::get_ask_quantity_at(price_t price) const {
    auto it = asks_.find(price);
    if (it != asks_.end()) {
        return it->second.quantity;
    }
    return 0;
}

void OrderBook::clear() {
    bids_.clear();
    asks_.clear();
    last_update_time_ = 0;
    sequence_number_ = 0;
}

void OrderBook::trim_depth() {
    // Trim bids if exceeds max depth
    while (bids_.size() > max_depth_) {
        auto it = bids_.end();
        --it;  // Last element (lowest price in descending order)
        bids_.erase(it);
    }

    // Trim asks if exceeds max depth
    while (asks_.size() > max_depth_) {
        auto it = asks_.end();
        --it;  // Last element (highest price in ascending order)
        asks_.erase(it);
    }
}

} // namespace slick::trader

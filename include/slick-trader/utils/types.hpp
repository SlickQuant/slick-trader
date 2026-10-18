#pragma once

#include <cstdint>
#include <cmath>
#include <limits>
#include <string>
#include <vector>
#include <slick-trader/events/event.hpp>

namespace slick::trader {

// Fixed-point representation for prices and quantities
// price_t: int64_t with 8 decimal places (1e-8 precision)
// qty_t: uint64_t with 8 decimal places (1e-8 precision)
using price_t = int64_t;
using qty_t = int64_t;

constexpr price_t NULL_PRICE = std::numeric_limits<price_t>::max();
constexpr int64_t PRICE_SCALE = 100'000'000;  // 1e8
constexpr int64_t QTY_SCALE = 100'000'000;   // 1e8

// Conversion functions
inline price_t to_price_t(double value) {
    if (std::isnan(value)) return NULL_PRICE;
    constexpr double MAX_SAFE = static_cast<double>(std::numeric_limits<price_t>::max()) / PRICE_SCALE;
    constexpr double MIN_SAFE = static_cast<double>(std::numeric_limits<price_t>::min()) / PRICE_SCALE;
    if (value > MAX_SAFE || value < MIN_SAFE) return NULL_PRICE;
    return static_cast<price_t>(std::round(value * PRICE_SCALE));
}

inline qty_t to_qty_t(double value) {
    return static_cast<qty_t>(std::round(value * QTY_SCALE));
}

inline double from_price_t(price_t value) {
    return value == NULL_PRICE ? std::numeric_limits<double>::quiet_NaN() : static_cast<double>(value) / PRICE_SCALE;
}

inline double from_qty_t(qty_t value) {
    return static_cast<double>(value) / QTY_SCALE;
}

enum class SubscriptionType : uint8_t {
    TRADE,
    QUOTE,
    LEVEL2,
    ORDER_BOOK,
    MBO,            // market by order: every order of the book, for brokers whose feed carries it
};

// Subscription request from a strategy
struct SubscriptionRequest {
    BrokerId broker_id;                   // Which broker to subscribe to
    SubscriptionType type;                // Trade, Quote, Level2, OrderBook or MBO
    std::vector<std::string> instruments; // List of instruments
    int depth = 10;                       // Order book depth (for orderbook subscriptions)

    SubscriptionRequest(BrokerId broker, SubscriptionType sub_type,
                       const std::vector<std::string>& instrs, int d = 10)
        : broker_id(broker), type(sub_type), instruments(instrs), depth(d) {}
};

} // namespace slick::trader

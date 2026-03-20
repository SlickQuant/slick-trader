#pragma once

#include <cstdint>
#include <cmath>
#include <limits>

namespace slick::trader {

// Fixed-point representation for prices and quantities
// price_t: int64_t with 8 decimal places (1e-8 precision)
// qty_t: uint64_t with 8 decimal places (1e-8 precision)
using price_t = int64_t;
using qty_t = uint64_t;

constexpr price_t NULL_PRICE = std::numeric_limits<price_t>::max();
constexpr int64_t PRICE_SCALE = 100'000'000;  // 1e8
constexpr uint64_t QTY_SCALE = 100'000'000;   // 1e8

// Conversion functions
inline price_t to_price_t(double value) {
    return std::isnan(value) ? NULL_PRICE : static_cast<price_t>(std::round(value * PRICE_SCALE));
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
    ORDER_BOOK
};

} // namespace slick::trader

#pragma once

#include <cstdint>

namespace slick::trader {

enum Side : uint8_t {
    BUY = 0,
    SELL = 1,
    UNKNOWN = 2,
};

enum class OrderType : uint8_t {
    MARKET = 0,
    LIMIT = 1,
    STOP = 2,
    STOP_LIMIT = 3,
    BRACKET = 4
};

enum class TimeInForce : uint8_t {
    GTC,  // Good Till Cancel
    IOC,  // Immediate Or Cancel
    FOK,  // Fill Or Kill
    GTD   // Good Till Date
};

enum class OrderStatus : uint8_t {
    PENDING_NEW,
    PENDING_MODIFY,
    PENDING_CANCEL,
    ACKNOWLEDGED,
    PARTIALLY_FILLED,
    FILLED,
    CANCELLED,
    REJECTED
};

} // namespace slick::trader

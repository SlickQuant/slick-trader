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

inline constexpr std::string_view to_string(OrderType type) {
    switch (type) {
        case OrderType::MARKET: return "MARKET";
        case OrderType::LIMIT: return "LIMIT";
        case OrderType::STOP: return "STOP";
        case OrderType::STOP_LIMIT: return "STOP_LIMIT";
        case OrderType::BRACKET: return "BRACKET";
        default: return "UNKNOWN";
    }
}

inline constexpr std::string_view to_string(TimeInForce tif) {
    switch (tif) {
        case TimeInForce::GTC: return "GTC";
        case TimeInForce::IOC: return "IOC";
        case TimeInForce::FOK: return "FOK";
        case TimeInForce::GTD: return "GTD";
        default: return "UNKNOWN";
    }
}

inline constexpr std::string_view to_string(OrderStatus status) {
    switch (status) {
        case OrderStatus::PENDING_NEW: return "PENDING_NEW";
        case OrderStatus::PENDING_MODIFY: return "PENDING_MODIFY";
        case OrderStatus::PENDING_CANCEL: return "PENDING_CANCEL";
        case OrderStatus::ACKNOWLEDGED: return "ACKNOWLEDGED";
        case OrderStatus::PARTIALLY_FILLED: return "PARTIALLY_FILLED";
        case OrderStatus::FILLED: return "FILLED";
        case OrderStatus::CANCELLED: return "CANCELLED";
        case OrderStatus::REJECTED: return "REJECTED";
        default: return "UNKNOWN";
    }
}

} // namespace slick::trader

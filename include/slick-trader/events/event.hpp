#pragma once

#include <cstdint>
#include <chrono>
#include <cstring>
#include <string_view>
#include <algorithm>
#include <slick-trader/utils/time.hpp>

namespace slick::trader {

constexpr size_t MAX_INSTRUMENT_LENGTH = 32;

enum class BrokerId : uint8_t {
    UNKNOWN = 0,
    COINBASE,
    ALPACA,
    HYPERLIQUID,
    SIMULATED,
    CME,
    _BROKER_ID_COUNT_,
};

enum class EventType : uint8_t {
    TRADE,
    QUOTE,
    LEVEL2_UPDATE,
    ORDER_BOOK,
    CANDLE,
    NEW_ORDER,
    MODIFY_ORDER,
    CANCEL_ORDER,
    ORDER_ACK,
    ORDER_FILL,
    ORDER_REJECT,
    ORDER_CANCEL,
    POSITION_UPDATE,
    PNL_UPDATE,
    TOB,
    REGISTER,
    REGISTER_END,
    STATUS,
    SUBSCRIBE,
    UNSUBSCRIBE,
    STRATEGY_CONFIG,
    INSTRUMENT_DEFINITION,
    MBO_UPDATE,
    INSTRUMENT_STATUS,
};

// Base event header - all events must be POD/trivially copyable for lock-free queue
#pragma pack(push, 1)
struct Event {
    EventType type;
    uint64_t timestamp_ns;      // System receive time
    uint64_t event_time_ns;     // Exchange/event time
    BrokerId broker_id;
    char instrument[MAX_INSTRUMENT_LENGTH];

    Event(EventType t = EventType::TRADE)
        : type(t)
        , timestamp_ns(get_timestamp_ns())
        , event_time_ns(0)
        , broker_id(BrokerId::UNKNOWN)
        , instrument{} {}

    void set_instrument(std::string_view instr) {
        size_t len = std::min(instr.size(), MAX_INSTRUMENT_LENGTH - 1);
        std::memcpy(instrument, instr.data(), len);
        instrument[len] = '\0';
    }
};
#pragma pack(pop)

inline const char* to_string(BrokerId bid) {
    switch(bid) {
        case BrokerId::COINBASE:
            return "COINBASE";
        case BrokerId::ALPACA:
            return "ALPACA";
        case BrokerId::HYPERLIQUID:
            return "HYPERLIQUID";
        case BrokerId::SIMULATED:
            return "SIMULATED";
        case BrokerId::CME:
            return "CME";
        default:
            return "UNKNOWN";
    }
}

inline BrokerId to_broker_id(const std::string& broker) {
    if (broker == "COINBASE")           return BrokerId::COINBASE;
    else if (broker == "ALPACA")        return BrokerId::ALPACA;
    else if (broker == "HYPERLIQUID")   return BrokerId::HYPERLIQUID;
    else if (broker == "SIMULATED")     return BrokerId::SIMULATED;
    else if (broker == "CME")           return BrokerId::CME;
    else                                return BrokerId::UNKNOWN;
}

} // namespace slick::trader

#pragma once

#include <cstdint>
#include <chrono>
#include <cstring>
#include <string_view>
#include <algorithm>

namespace slick::trader {

constexpr size_t MAX_INSTRUMENT_LENGTH = 32;

enum class BrokerId : uint8_t {
    UNKNOWN = 0,
    COINBASE,
    ALPACA,
    SIMULATED
};

enum class EventType : uint8_t {
    TRADE,
    QUOTE,
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
        , timestamp_ns(std::chrono::system_clock::now().time_since_epoch().count())
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

} // namespace slick::trader

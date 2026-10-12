#pragma once

#include <slick-trader/events/event.hpp>
#include <algorithm>
#include <cstring>
#include <string_view>
#include <type_traits>

namespace slick::trader {

constexpr size_t MAX_STATUS_REASON_LENGTH = 32;

// What an instrument's market is doing, as its exchange reports it.
enum class TradingStatus : uint8_t {
    UNKNOWN,    // not reported, or something this does not model
    PRE_OPEN,   // orders are taken and nothing matches yet
    OPEN,       // continuous trading
    HALTED,     // trading is suspended
    CLOSED,     // not trading: outside its session, or expired
};

inline const char* to_string(TradingStatus status) {
    switch (status) {
        case TradingStatus::PRE_OPEN: return "PRE_OPEN";
        case TradingStatus::OPEN:     return "OPEN";
        case TradingStatus::HALTED:   return "HALTED";
        case TradingStatus::CLOSED:   return "CLOSED";
        default:                      return "UNKNOWN";
    }
}

// InstrumentStatusEvent - a change of an instrument's trading state, published by a
// broker's market data connector (for CME, from MDP 3.0's security status). StrategyManager
// forwards it to every strategy; what to do about a halt is the strategy's decision.
#pragma pack(push, 1)
struct InstrumentStatusEvent : public Event {
    TradingStatus status;
    bool cancel_allowed;    // false while the exchange refuses cancels, as before an opening cross
    char reason[MAX_STATUS_REASON_LENGTH];  // The exchange's reason, as text; may be empty

    InstrumentStatusEvent()
        : Event(EventType::INSTRUMENT_STATUS)
        , status(TradingStatus::UNKNOWN)
        , cancel_allowed(true)
        , reason{}
    {}

    void set_reason(std::string_view text) {
        size_t len = std::min(text.size(), MAX_STATUS_REASON_LENGTH - 1);
        std::memcpy(reason, text.data(), len);
        reason[len] = '\0';
    }
};
#pragma pack(pop)

static_assert(std::is_trivially_copyable_v<InstrumentStatusEvent>, "InstrumentStatusEvent must be trivially copyable");

} // namespace slick::trader
